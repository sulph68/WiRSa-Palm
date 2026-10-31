// caldav_ical.cpp - iCal text to events (uICAL), for CalDAV and iCloud. Part of the CalDAV Relay mode; include/caldav_internal.h says how the files divide the work.
#include "caldav_internal.h"

namespace CalDavRelay {

#if WIRSA_FEATURE_CALDAV_REPORT
// Parses one VCALENDAR block of raw iCal text (a whole ICS feed, or one <calendar-data> block of a multistatus REPORT) and calls sink() for every occurrence within [rangeStartUnix, rangeEndUnix]
// (Unix seconds), after EXDATE exclusions and RECURRENCE-ID overrides. False only if sink() returned false (the caller aborted, e.g. an ack failed); a malformed or empty block is logged and gives zero
// events, not an error (the Palm's "silently skip if unavailable" design for this feature).
bool parseICalAndEmit(const String &icalText, uICAL::seconds_t rangeStartUnix,
                              uICAL::seconds_t rangeEndUnix, CalDavEventSink sink) {
  uICAL::istream_String icalStream(icalText);
  uICAL::VLineStream lines(icalStream);

  // Widened filter: everything Calendar::load()'s useLine allows, PLUS UID/LOCATION/STATUS/EXDATE/RECURRENCE-ID for VEVENT.
  uICAL::VObjectStream::lineP_t useLine = [](const uICAL::string parent, const uICAL::string line) {
    if (parent == "VCALENDAR") {
      if (line.empty()) return true;
    } else if (parent == "VTIMEZONE") {
      if (line.empty()) return true;
      if (line == "TZID") return true;
    } else if (parent == "STANDARD" || parent == "DAYLIGHT") {
      if (line.empty()) return true;
      if (line == "TZOFFSETFROM") return true;
      if (line == "TZOFFSETTO") return true;
      if (line == "TZNAME") return true;
      if (line == "DTSTART") return true;
      if (line == "RRULE") return true;
    } else if (parent == "VEVENT") {
      if (line.empty()) return true;
      if (line == "SUMMARY") return true;
      if (line == "DTSTAMP") return true;
      if (line == "DTSTART") return true;
      if (line == "DTEND") return true;
      if (line == "RRULE") return true;
      if (line == "UID") return true;
      if (line == "LOCATION") return true;
      if (line == "STATUS") return true;
      if (line == "EXDATE") return true;
      if (line == "RECURRENCE-ID") return true;
      if (line == "DESCRIPTION") return true;
    }
    return false;
  };

  uICAL::VObjectStream stm(lines, useLine);

  uICAL::VObject_ptr root;
  try {
    root = stm.nextObject(false);
  } catch (...) {
    LOGW("CalDAV: iCal parse error (root) - treating as empty");
    return true;
  }
  if (!root || root->getName() != "VCALENDAR") {
    LOGW("CalDAV: iCal block did not start with VCALENDAR - skipped");
    return true;
  }

  uICAL::TZMap_ptr tzmap = uICAL::new_ptr<uICAL::TZMap>();
  std::map<String, CalDavMasterEvent> masters;
  std::map<String, std::vector<CalDavOverrideEvent>> overrides;

  for (;;) {
    // A calendar can have many VEVENTs, and a parse with no yield point can starve the WiFi driver's task long enough to miss beacon/keepalive timing and drop the AP association (a known ESP32 failure
    // class). As bitwarden_relay.cpp's yield() calls in its long parsing.
    yield();

    uICAL::VObject_ptr child;
    try {
      child = stm.nextObject(true);
    } catch (...) {
      break;  // stop at the first parse error; what was already collected is still emitted
    }
    if (!child) break;

    if (child->getName() == "VTIMEZONE") {
      // tzmap->add(child) can throw on a malformed VTIMEZONE; a year-long calendar can carry several (DST transitions, several zones), so guarding only the first is not enough.
      try {
        tzmap->add(child);
        // Logs success too: uICAL::VObjectStream reads components strictly in document order with no lookahead, so a VEVENT whose TZID is registered LATER in the document fails to resolve although the
        // VTIMEZONE registration itself succeeded; the log order shows that ordering issue apart from other causes.
        if (logOn(LOG_VERBOSE)) {
          uICAL::VLine_ptr tzIdLine = child->getPropertyByName("TZID");
          logPrintf(LOG_VERBOSE, "CalDAV: VTIMEZONE added: %s\r\n", tzIdLine ? tzIdLine->value.c_str() : "(no TZID)");
        }
      } catch (const uICAL::Error &e) {
        // uICAL's exception types (error.h) carry a message: surface it rather than a generic "parse error".
        LOGV("CalDAV: VTIMEZONE skipped (%s)\r\n", e.message.c_str());
      } catch (...) {
        LOGV("CalDAV: VTIMEZONE skipped (non-uICAL parse error)");
      }
      continue;
    }
    if (child->getName() != "VEVENT") continue;

    // One outer try/catch around the whole per-VEVENT body, not around each uICAL call (getPropertyByName, DateTime construction, ...): an exception anywhere skips just that VEVENT ("logged and
    // zero events, not an error"), without having to find every throw site by inspection.
    try {
    uICAL::VLine_ptr uidLine = child->getPropertyByName("UID");
    uICAL::VLine_ptr recIdLine = child->getPropertyByName("RECURRENCE-ID");
    uICAL::VLine_ptr statusLine = child->getPropertyByName("STATUS");
    uICAL::VLine_ptr locLine = child->getPropertyByName("LOCATION");
    uICAL::VLine_ptr descLine = child->getPropertyByName("DESCRIPTION");
    uICAL::VLine_ptr rruleLine = child->getPropertyByName("RRULE");

    String uid = uidLine ? String(uidLine->value.c_str()) : String("");
    String location = locLine ? icalUnescapeText(String(locLine->value.c_str())) : String("");
    String description = descLine ? icalUnescapeText(String(descLine->value.c_str())) : String("");
    bool cancelled = statusLine && String(statusLine->value.c_str()).equalsIgnoreCase("CANCELLED");
    bool isRepeating = (rruleLine != nullptr);

    uICAL::VEvent_ptr vevent;
    try {
      vevent = uICAL::new_ptr<uICAL::VEvent>(child, tzmap);
    } catch (const uICAL::Error &e) {
      // As the VTIMEZONE catch above: the uICAL message is what diagnoses a skipped VEVENT.
      LOGV("CalDAV: VEVENT skipped (%s)\r\n", e.message.c_str());
      continue;
    } catch (...) {
      LOGV("CalDAV: VEVENT skipped (non-uICAL parse error)");
      continue;
    }

    if (recIdLine) {
      // A feed can have TZID-qualified RECURRENCE-ID values without full VTIMEZONE coverage for that TZID: input malformed enough to throw, which this function logs and skips, not propagates.
      try {
        String tzid = recIdLine->getParam("TZID");
        uICAL::DateTime recId((recIdLine->value + tzid).c_str(), tzmap);
        CalDavOverrideEvent ov;
        ov.recurrenceIdEpoch = recId.toEpochSeconds();
        ov.vevent = vevent;
        ov.location = location;
        ov.description = description;
        ov.cancelled = cancelled;
        overrides[uid].push_back(ov);
      } catch (const uICAL::Error &e) {
        LOGV("CalDAV: RECURRENCE-ID override skipped (%s)\r\n", e.message.c_str());
      } catch (...) {
        LOGV("CalDAV: RECURRENCE-ID override skipped (non-uICAL parse error)");
      }
    } else {
      // EXDATE exclusions apply to the MASTER's rrule: do it once here, not on every occurrence.
      for (auto exLine : child->listProperties("EXDATE")) {
        String tzid = exLine->getParam("TZID");
        std::vector<String> parts;
        splitCommaList(String(exLine->value.c_str()), parts);
        for (auto &p : parts) {
          if (p.length() == 0) continue;
          try {
            uICAL::DateTime exDt((p + tzid).c_str(), tzmap);
            vevent->rrule->exclude(exDt);
          } catch (const uICAL::Error &e) {
            LOGV("CalDAV: EXDATE value skipped (%s)\r\n", e.message.c_str());
          } catch (...) {
            LOGV("CalDAV: EXDATE value skipped (non-uICAL parse error)");
          }
        }
      }
      CalDavMasterEvent m;
      m.vevent = vevent;
      m.location = location;
      m.description = description;
      m.cancelled = cancelled;
      m.isRepeating = isRepeating;
      // A UID can appear twice in a malformed feed: the last master wins ("most recently declared"), not a silently dropped event.
      masters[uid] = m;
    }
    } catch (const uICAL::Error &e) {
      LOGV("CalDAV: VEVENT entirely skipped (%s)\r\n", e.message.c_str());
      continue;
    } catch (...) {
      LOGV("CalDAV: VEVENT entirely skipped (non-uICAL parse error)");
      continue;
    }
  }

  uICAL::DateTime rangeBegin((uICAL::seconds_t) rangeStartUnix);
  uICAL::DateTime rangeEnd((uICAL::seconds_t) rangeEndUnix);

  for (auto &kv : masters) {
    yield();  // once per master series - see the loop below's own comment
    const String &uid = kv.first;
    CalDavMasterEvent &m = kv.second;
    if (m.cancelled) continue;

    std::vector<CalDavOverrideEvent> *ovList = nullptr;
    auto ovIt = overrides.find(uid);
    if (ovIt != overrides.end()) ovList = &ovIt->second;

    // A malformed RRULE (bad UNTIL, ...) or a DTSTART whose TZID has no VTIMEZONE can throw during expansion (VEventIter construction, iter.next()/now()/entry()->end()). One bad master series must not
    // take down the sync: skip it, as every other malformed input here.
    try {
      uICAL::VEventIter iter(m.vevent, rangeBegin, rangeEnd);
      while (iter.next()) {
        // The highest-iteration loop here for a real recurring series (daily or weekly over a 180-day window): yield() as bitwarden_relay.cpp's once per completed item (see the outer comment above).
        yield();
        uICAL::DateTime occStart = iter.now();
        uICAL::CalendarEntry_ptr entry = iter.entry();
        uICAL::DateTime occEnd = entry->end();

        String title = icalUnescapeText(String(m.vevent->summary.c_str()));
        String location = m.location;
        String description = m.description;
        bool allDay = occStart.isDateOnly();
        uICAL::seconds_t startEpoch = occStart.toEpochSeconds();
        uICAL::seconds_t endEpoch = occEnd.toEpochSeconds();
        bool skip = false;

        if (ovList) {
          for (auto &ov : *ovList) {
            if (ov.recurrenceIdEpoch == startEpoch) {
              if (ov.cancelled) { skip = true; break; }
              title = icalUnescapeText(String(ov.vevent->summary.c_str()));
              location = ov.location;
              description = ov.description;
              allDay = ov.vevent->start.isDateOnly();
              startEpoch = ov.vevent->start.toEpochSeconds();
              endEpoch = ov.vevent->end.toEpochSeconds();
              break;
            }
          }
        }
        if (skip) continue;

        // A recurring series can run far outside [rangeBegin, rangeEnd); VEventIter's range is a hint, not a guarantee for every uICAL version or pattern, so clamp here too.
        if (startEpoch < (uICAL::seconds_t) rangeStartUnix || startEpoch > (uICAL::seconds_t) rangeEndUnix) continue;

        uint32_t utcStartPalm = (uint32_t) startEpoch + UNIX_TO_PALM_EPOCH;
        uint32_t utcEndPalm = (uint32_t) endEpoch + UNIX_TO_PALM_EPOCH;
        // Logs the real UTC date and time (not only the epoch) so an event's computed start can be checked against the [rangeStart, rangeEnd) window printed at [window].
        if (logOn(LOG_VERBOSE)) {
          char startBuf[24];
          formatUtcBasic((time_t) startEpoch, startBuf, sizeof(startBuf));
          logPrintf(LOG_VERBOSE, "CalDAV: [occurrence] uid=%s start=%s (epoch=%ld, allDay=%d)\r\n",
                        uid.c_str(), startBuf, (long) startEpoch, allDay ? 1 : 0);
        }
        // isRepeating reflects the MASTER's RRULE, not the override's: an overridden occurrence is still one instance of a repeating series (that is what RECURRENCE-ID means), so the Palm's repeat icon still shows.
        if (!sink(uid, title, location, utcStartPalm, utcEndPalm, allDay, description, m.isRepeating)) return false;
      }
    } catch (const uICAL::Error &e) {
      LOGV("CalDAV: master event skipped mid-expansion (%s)\r\n", e.message.c_str());
      continue;
    } catch (...) {
      LOGV("CalDAV: master event skipped mid-expansion (non-uICAL parse error)");
      continue;
    }
  }

  // Known limitation: an override whose RECURRENCE-ID points at an occurrence OUTSIDE [rangeBegin, rangeEnd) (an event moved INTO the window from a date that would not otherwise show) is not emitted on
  // its own. Every override that matches a master occurrence inside the window is handled above.
  return true;
}
#endif  // WIRSA_FEATURE_CALDAV_REPORT

}  // namespace CalDavRelay
