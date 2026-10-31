// caldav_ics.cpp - ICS feed: the streaming parser. Part of the CalDAV Relay mode; include/caldav_internal.h says how the files divide the work.
#include "caldav_internal.h"

namespace CalDavRelay {

#if WIRSA_FEATURE_CALDAV_ICS
static uICAL::TZMap_ptr gCddvIcsTzmap;
static std::map<String, CalDavMasterEvent> gCddvIcsMasters;
static std::map<String, std::vector<CalDavOverrideEvent>> gCddvIcsOverrides;

// Set once per fetch (cddvFetchIcsFeed) from its rangeStart/rangeEnd. Lets cddvIcsOnComponent drop a non-repeating master the moment it is parsed if it is provably outside
// the sync window, rather than wait for cddvIcsFinishParse's VEventIter range check at the end of the feed (see the early drop below).
static uICAL::seconds_t gCddvIcsRangeStart = 0;
static uICAL::seconds_t gCddvIcsRangeEnd = 0;

// Diagnostic counters only (no logic): they show whether the early drop fires or the feed simply has many RECURRING events (which cannot be judged early) keeping their object graphs alive.
static unsigned gCddvIcsMastersDropped = 0;    // non-repeating, outside window, dropped on capture
static unsigned gCddvIcsMastersRepeating = 0;  // repeating, always kept regardless of window

// Declared here because cddvIcsHeapCritical(), below, needs it before gCddvIcsPartial.
static bool gCddvIcsOverflow = false;

// The same widened filter as parseICalAndEmit's local lambda, duplicated and not shared (see the section header in caldav_http.cpp).
bool cddvIcsUseLine(const uICAL::string parent, const uICAL::string line) {
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
}

void cddvIcsBeginParse() {
  gCddvIcsTzmap = uICAL::new_ptr<uICAL::TZMap>();
  gCddvIcsMasters.clear();
  gCddvIcsOverrides.clear();
}

// Parses ONE complete "BEGIN:VEVENT ... END:VEVENT" or VTIMEZONE block (no VCALENDAR wrapper: uICAL::VObjectStream::nextObjectName() reads whatever BEGIN comes first) and folds it
// into gCddvIcsTzmap/gCddvIcsMasters/gCddvIcsOverrides, as parseICalAndEmit's per-child loop does, one component at a time.
// Crash seen on hardware: Masters and Overrides hold a full VEvent/DateTime/TZ/RRule graph per event for the WHOLE feed (not just the window: RRULE and RECURRENCE-ID need the whole
// document), so a feed with long one-off history can fill the heap until a small `new` (e.g. a TZ object) fails, and failing to allocate the std::bad_alloc exception object forces
// std::terminate()/abort(), which no try/catch intercepts. cddvIcsHeapCritical() is a cheap per-component backstop; the real mitigation is the non-repeating early drop below.
// A feed heavy on RECURRING events can still exhaust the heap, hence the backstop.
bool cddvIcsHeapCritical() {
  static const uint32_t kMinFreeHeap = 12288;
  if (ESP.getFreeHeap() >= kMinFreeHeap) return false;
  if (!gCddvIcsOverflow) {
    gCddvIcsOverflow = true;
    LOGW("CalDAV: [ics] aborting - free heap critically low (%u bytes) mid-feed"
                                 " (%u masters kept [%u repeating], %u dropped as out-of-window)\r\n",
                                 (unsigned) ESP.getFreeHeap(), (unsigned) gCddvIcsMasters.size(),
                                 gCddvIcsMastersRepeating, gCddvIcsMastersDropped);
  }
  return true;
}

void cddvIcsOnComponent(const String &componentText) {
  yield();
  if (cddvIcsHeapCritical()) return;
  uICAL::istream_String compStream(componentText);
  uICAL::VLineStream lines(compStream);
  uICAL::VObjectStream stm(lines, cddvIcsUseLine);

  uICAL::VObject_ptr child;
  try {
    child = stm.nextObject(true);
  } catch (...) {
    LOGV("CalDAV: [ics] component parse error - skipped");
    return;
  }
  if (!child) return;

  if (child->getName() == "VTIMEZONE") {
    try {
      gCddvIcsTzmap->add(child);
      if (logOn(LOG_VERBOSE)) {
        uICAL::VLine_ptr tzIdLine = child->getPropertyByName("TZID");
        logPrintf(LOG_VERBOSE, "CalDAV: [ics] VTIMEZONE added: %s\r\n", tzIdLine ? tzIdLine->value.c_str() : "(no TZID)");
      }
    } catch (const uICAL::Error &e) {
      LOGV("CalDAV: [ics] VTIMEZONE skipped (%s)\r\n", e.message.c_str());
    } catch (...) {
      LOGV("CalDAV: [ics] VTIMEZONE skipped (non-uICAL parse error)");
    }
    return;
  }
  if (child->getName() != "VEVENT") return;

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
      vevent = uICAL::new_ptr<uICAL::VEvent>(child, gCddvIcsTzmap);
    } catch (const uICAL::Error &e) {
      LOGV("CalDAV: [ics] VEVENT skipped (%s)\r\n", e.message.c_str());
      return;
    } catch (...) {
      LOGV("CalDAV: [ics] VEVENT skipped (non-uICAL parse error)");
      return;
    }

    if (recIdLine) {
      try {
        String tzid = recIdLine->getParam("TZID");
        uICAL::DateTime recId((recIdLine->value + tzid).c_str(), gCddvIcsTzmap);
        CalDavOverrideEvent ov;
        ov.recurrenceIdEpoch = recId.toEpochSeconds();
        // Not ov.vevent = vevent (see CalDavOverrideEvent): an override is only a fields lookup by date and never expanded via VEventIter, so there is no reason to keep its DateTime/TZ/RRule graph alive for the rest of the feed.
        ov.title = icalUnescapeText(String(vevent->summary.c_str()));
        ov.startEpoch = vevent->start.toEpochSeconds();
        ov.endEpoch = vevent->end.toEpochSeconds();
        ov.allDay = vevent->start.isDateOnly();
        ov.location = location;
        ov.description = description;
        ov.cancelled = cancelled;
        gCddvIcsOverrides[uid].push_back(ov);
        // The send to the Palm (cddvSendEventItem) runs only later, in cddvIcsFinishParse, once the whole feed has streamed, so this capture-time log is the only per-event progress signal during the transfer.
        LOGV("CalDAV: [ics] VEVENT override captured: %s\r\n", uid.c_str());
      } catch (const uICAL::Error &e) {
        LOGV("CalDAV: [ics] RECURRENCE-ID override skipped (%s)\r\n", e.message.c_str());
      } catch (...) {
        LOGV("CalDAV: [ics] RECURRENCE-ID override skipped (non-uICAL parse error)");
      }
    } else {
      // Crash seen on hardware: Masters holds a full VEvent graph per event for the ENTIRE feed regardless of the window (RRULE/RECURRENCE-ID need the whole document, so it cannot be
      // filtered server-side or by per-block streaming as on the CalDAV/iCloud path). A non-repeating event is the safe case to filter early: with no RRULE, VEventIter yields only its own
      // start/end, which is what cddvIcsFinishParse's range check does at the end anyway, so an event wholly outside the window is dropped here. A repeating event cannot be judged this way
      // (its own start/end may be outside the window while occurrences fall inside), so isRepeating events are always kept. A feed with years of one-off history let per-event heap use snowball
      // into a double allocation failure and abort().
      uICAL::seconds_t evStart = 0, evEnd = 0;
      bool evAllDay = false;
      if (!isRepeating) {
        evStart = vevent->start.toEpochSeconds();
        evEnd = vevent->end.toEpochSeconds();
        evAllDay = vevent->start.isDateOnly();
        if (evEnd < gCddvIcsRangeStart || evStart > gCddvIcsRangeEnd) {
          gCddvIcsMastersDropped++;
          LOGV("CalDAV: [ics] VEVENT master dropped (non-repeating, outside window): %s\r\n", uid.c_str());
          return;
        }
      } else {
        // Three cheap, always-safe checks that rule out a REPEATING series ever landing in the window, without the full RRULE expansion (a feed can carry many series that are simply over,
        // UNTIL/COUNT long exhausted, which used to be held for nothing):
        //  1. The series' DTSTART is after the window ends: it cannot have produced an occurrence yet, whatever FREQ/COUNT/UNTIL.
        //  2. RRULE has UNTIL, an INCLUSIVE upper bound on every occurrence (RFC 5545 3.3.10): if it is before the window starts, the series is done.
        //  3. RRULE has COUNT (exclusive with UNTIL): the date of the LAST occurrence needs the real expansion (BYDAY/BYMONTHDAY pick a day WITHIN each base period but never push an occurrence
        //     past it), so a deliberately oversized period per FREQ step (e.g. 31 days for MONTHLY) gives a conservative upper bound, later than the true last occurrence; drop only if even that is before the window.
        // A truly unbounded series (neither UNTIL nor COUNT) cannot be judged early and is always kept.
        uICAL::seconds_t seriesStart = vevent->start.toEpochSeconds();
        if (seriesStart > gCddvIcsRangeEnd) {
          gCddvIcsMastersDropped++;
          LOGV("CalDAV: [ics] VEVENT master dropped (repeating, starts after window): %s\r\n", uid.c_str());
          return;
        }
        String rruleValue(rruleLine->value.c_str());
        String untilStr, countStr, freqStr, intervalStr;
        if (cddvRruleFindParam(rruleValue, "UNTIL", untilStr)) {
          try {
            uICAL::DateTime untilDt(untilStr.c_str(), gCddvIcsTzmap);
            if (untilDt.toEpochSeconds() < gCddvIcsRangeStart) {
              gCddvIcsMastersDropped++;
              LOGV("CalDAV: [ics] VEVENT master dropped (repeating, UNTIL before window): %s\r\n", uid.c_str());
              return;
            }
          } catch (...) {
            // A malformed or unparseable UNTIL: keep the series rather than risk dropping something real.
          }
        } else if (cddvRruleFindParam(rruleValue, "COUNT", countStr)) {
          int countVal = countStr.toInt();
          int intervalVal = cddvRruleFindParam(rruleValue, "INTERVAL", intervalStr) ? intervalStr.toInt() : 1;
          if (intervalVal < 1) intervalVal = 1;
          uICAL::seconds_t periodSeconds = 0;
          if (cddvRruleFindParam(rruleValue, "FREQ", freqStr)) {
            if (freqStr == "SECONDLY") periodSeconds = 1;
            else if (freqStr == "MINUTELY") periodSeconds = 60;
            else if (freqStr == "HOURLY") periodSeconds = 3600;
            else if (freqStr == "DAILY") periodSeconds = 86400;
            else if (freqStr == "WEEKLY") periodSeconds = 604800;
            else if (freqStr == "MONTHLY") periodSeconds = 2678400;   // 31 days - longest possible month
            else if (freqStr == "YEARLY") periodSeconds = 31622400;   // 366 days - longest possible year
          }
          if (countVal > 0 && periodSeconds > 0) {
            uICAL::seconds_t maxSpan = (uICAL::seconds_t) countVal * (uICAL::seconds_t) intervalVal * periodSeconds;
            if (seriesStart + maxSpan < gCddvIcsRangeStart) {
              gCddvIcsMastersDropped++;
              LOGV("CalDAV: [ics] VEVENT master dropped (repeating, COUNT-bounded, ends before window): %s\r\n", uid.c_str());
              return;
            }
          }
        }
        // EXDATE means something only against an RRULE (for a non-repeating master it is thrown away anyway, see CalDavMasterEvent), so skip the work otherwise.
        for (auto exLine : child->listProperties("EXDATE")) {
          String tzid = exLine->getParam("TZID");
          std::vector<String> parts;
          splitCommaList(String(exLine->value.c_str()), parts);
          for (auto &p : parts) {
            if (p.length() == 0) continue;
            try {
              uICAL::DateTime exDt((p + tzid).c_str(), gCddvIcsTzmap);
              vevent->rrule->exclude(exDt);
            } catch (const uICAL::Error &e) {
              LOGV("CalDAV: [ics] EXDATE value skipped (%s)\r\n", e.message.c_str());
            } catch (...) {
              LOGV("CalDAV: [ics] EXDATE value skipped (non-uICAL parse error)");
            }
          }
        }
      }
      CalDavMasterEvent m;
      if (isRepeating) {
        m.vevent = vevent;
      } else {
        // Not m.vevent = vevent (see CalDavMasterEvent): left unset, the local vevent shared_ptr and everything it owns (DateTime x2, TZ x2, RRule) is freed when this function returns,
        // not held through the feed just to read these four values in cddvIcsFinishParse.
        m.title = icalUnescapeText(String(vevent->summary.c_str()));
        m.startEpoch = evStart;
        m.endEpoch = evEnd;
        m.allDay = evAllDay;
      }
      m.location = location;
      m.description = description;
      m.cancelled = cancelled;
      m.isRepeating = isRepeating;
      gCddvIcsMasters[uid] = m;
      if (isRepeating) gCddvIcsMastersRepeating++;
      LOGV("CalDAV: [ics] VEVENT master captured (%s): %s\r\n",
                                   isRepeating ? "repeating" : "in-window", uid.c_str());
    }
  } catch (const uICAL::Error &e) {
    LOGV("CalDAV: [ics] VEVENT entirely skipped (%s)\r\n", e.message.c_str());
  } catch (...) {
    LOGV("CalDAV: [ics] VEVENT entirely skipped (non-uICAL parse error)");
  }
}

// The occurrence-expansion tail: parseICalAndEmit's logic, reading gCddvIcsMasters/gCddvIcsOverrides (filled by cddvIcsOnComponent over the whole streamed feed) instead of locals.
bool cddvIcsFinishParse(uICAL::seconds_t rangeStartUnix, uICAL::seconds_t rangeEndUnix, CalDavEventSink sink) {
  uICAL::DateTime rangeBegin((uICAL::seconds_t) rangeStartUnix);
  uICAL::DateTime rangeEnd((uICAL::seconds_t) rangeEndUnix);

  for (auto &kv : gCddvIcsMasters) {
    yield();
    const String &uid = kv.first;
    CalDavMasterEvent &m = kv.second;
    if (m.cancelled) continue;

    std::vector<CalDavOverrideEvent> *ovList = nullptr;
    auto ovIt = gCddvIcsOverrides.find(uid);
    if (ovIt != gCddvIcsOverrides.end()) ovList = &ovIt->second;

    // Lightweight path for a non-repeating master: m.vevent was never set (see CalDavMasterEvent), so there is no object graph to run VEventIter on; its one occurrence is the
    // (startEpoch, endEpoch, title) captured at parse time. Mirrors one iteration of the VEventIter loop below, override matching included (a RECURRENCE-ID override on a non-repeating
    // master is invalid per RFC 5545, but a malformed feed could carry one, so stay defensive).
    if (!m.isRepeating) {
      uICAL::seconds_t startEpoch = m.startEpoch;
      uICAL::seconds_t endEpoch = m.endEpoch;
      bool allDay = m.allDay;
      String title = m.title;
      String location = m.location;
      String description = m.description;
      bool skip = false;

      if (ovList) {
        for (auto &ov : *ovList) {
          if (ov.recurrenceIdEpoch == startEpoch) {
            if (ov.cancelled) { skip = true; break; }
            title = ov.title;
            location = ov.location;
            description = ov.description;
            allDay = ov.allDay;
            startEpoch = ov.startEpoch;
            endEpoch = ov.endEpoch;
            break;
          }
        }
      }

      if (!skip && startEpoch >= (uICAL::seconds_t) rangeStartUnix && startEpoch <= (uICAL::seconds_t) rangeEndUnix) {
        uint32_t utcStartPalm = (uint32_t) startEpoch + UNIX_TO_PALM_EPOCH;
        uint32_t utcEndPalm = (uint32_t) endEpoch + UNIX_TO_PALM_EPOCH;
        if (logOn(LOG_VERBOSE)) {
          char startBuf[24];
          formatUtcBasic((time_t) startEpoch, startBuf, sizeof(startBuf));
          logPrintf(LOG_VERBOSE, "CalDAV: [ics][occurrence] uid=%s start=%s (epoch=%ld, allDay=%d)\r\n",
                        uid.c_str(), startBuf, (long) startEpoch, allDay ? 1 : 0);
        }
        if (!sink(uid, title, location, utcStartPalm, utcEndPalm, allDay, description, m.isRepeating)) return false;
      }
      continue;
    }

    try {
      uICAL::VEventIter iter(m.vevent, rangeBegin, rangeEnd);
      while (iter.next()) {
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
              title = ov.title;
              location = ov.location;
              description = ov.description;
              allDay = ov.allDay;
              startEpoch = ov.startEpoch;
              endEpoch = ov.endEpoch;
              break;
            }
          }
        }
        if (skip) continue;

        if (startEpoch < (uICAL::seconds_t) rangeStartUnix || startEpoch > (uICAL::seconds_t) rangeEndUnix) continue;

        uint32_t utcStartPalm = (uint32_t) startEpoch + UNIX_TO_PALM_EPOCH;
        uint32_t utcEndPalm = (uint32_t) endEpoch + UNIX_TO_PALM_EPOCH;
        if (logOn(LOG_VERBOSE)) {
          char startBuf[24];
          formatUtcBasic((time_t) startEpoch, startBuf, sizeof(startBuf));
          logPrintf(LOG_VERBOSE, "CalDAV: [ics][occurrence] uid=%s start=%s (epoch=%ld, allDay=%d)\r\n",
                        uid.c_str(), startBuf, (long) startEpoch, allDay ? 1 : 0);
        }
        if (!sink(uid, title, location, utcStartPalm, utcEndPalm, allDay, description, m.isRepeating)) return false;
      }
    } catch (const uICAL::Error &e) {
      LOGV("CalDAV: [ics] master event skipped mid-expansion (%s)\r\n", e.message.c_str());
    } catch (...) {
      LOGV("CalDAV: [ics] master event skipped mid-expansion (non-uICAL parse error)");
    }
  }
  return true;
}

// Bounded and small: only ONE in-flight VEVENT/VTIMEZONE block (typically under 1 KB) plus some filler, never the whole feed. Capped anyway: a malformed feed that never sends a matching END: would grow it without limit.
static String gCddvIcsPartial;
static bool gCddvIcsSawVCalendar = false;

// kCddvIcsPartialCap is well UNDER 65536: without BOARD_HAS_PSRAM a single Arduino String cannot exceed 65535 bytes whatever the free heap (WString.h CAPACITY_MAX, the cap that broke
// the old whole-feed buffering). A pathological component (a VTIMEZONE listing decades of DST rules) near that limit would fail as String::concat() quietly returning false; this named cap
// catches it first with a clear log line.
static const size_t kCddvIcsPartialCap = 49152;

// Persists ACROSS calls. Without it, one large component (often Google's VTIMEZONE, tens of KB of DST rules) that arrives in many small TCP/TLS chunks had its indexOf() restarted at
// its byte 0 on EVERY chunk: O(chunks x component size) instead of one linear scan, which looked like a hung sync (the log stops right after the GET), not a network problem.
// gCddvIcsPendingBegin < 0 means "not inside a known component"; reset once per fetch in cddvFetchIcsFeed.
static int gCddvIcsPendingBegin = -1;
static const char *gCddvIcsPendingEndTag = NULL;
static int gCddvIcsScannedUpTo = 0;  // how far the END-tag search has already confirmed clean, for the CURRENT pending component

// The same fix for the other half of the scan: a sync once went silent (no crash, yield() still ran so the watchdog stayed quiet) after a component parse error. Bytes with no
// BEGIN:VEVENT/BEGIN:VTIMEZONE yet (leftover text between components) were rescanned from byte 0 on every chunk while waiting for the next component to start: the same O(chunks x gap
// size) blowup that gCddvIcsScannedUpTo fixed for the "inside a pending component" case.
static int gCddvIcsNoBeginScannedUpTo = 0;

// Scans gCddvIcsPartial for complete BEGIN:VEVENT/BEGIN:VTIMEZONE ... END:... blocks (a VEVENT never nests a VEVENT, a VTIMEZONE never a VTIMEZONE, so the FIRST matching END: is
// right, with none of the CDATA/self-closing ambiguity of the XML path's extractCalendarDataBlocksIncremental), feeds each to cddvIcsOnComponent, and trims the consumed prefix.
void cddvIcsExtractComponents() {
  int consumedUpTo = 0;
  for (;;) {
    if (gCddvIcsPendingBegin < 0) {
      // Resume where the last no-match scan stopped, minus an overlap that still catches a BEGIN split across a chunk boundary ("BEGIN:VTIMEZONE", the longer, is 16 chars); see gCddvIcsNoBeginScannedUpTo.
      int searchFrom = gCddvIcsNoBeginScannedUpTo - 15;
      if (searchFrom < consumedUpTo) searchFrom = consumedUpTo;
      int posV = gCddvIcsPartial.indexOf("BEGIN:VEVENT", searchFrom);
      int posT = gCddvIcsPartial.indexOf("BEGIN:VTIMEZONE", searchFrom);
      if (posV < 0 && posT < 0) {
        gCddvIcsNoBeginScannedUpTo = gCddvIcsPartial.length();
        break;  // no more components starting yet - wait for more bytes
      }
      if (posV >= 0 && (posT < 0 || posV < posT)) {
        gCddvIcsPendingBegin = posV;
        gCddvIcsPendingEndTag = "END:VEVENT";
      } else {
        gCddvIcsPendingBegin = posT;
        gCddvIcsPendingEndTag = "END:VTIMEZONE";
      }
      gCddvIcsScannedUpTo = gCddvIcsPendingBegin;
    }

    {
      int tagLen = (int) strlen(gCddvIcsPendingEndTag);
      // Resume just short of where the previous scan stopped (overlap enough for a tag split at a chunk boundary) rather than rescanning the component from its start every time; this is the fix described at gCddvIcsPendingBegin.
      int resumeFrom = gCddvIcsScannedUpTo - (tagLen - 1);
      if (resumeFrom < gCddvIcsPendingBegin) resumeFrom = gCddvIcsPendingBegin;
      int endPos = gCddvIcsPartial.indexOf(gCddvIcsPendingEndTag, resumeFrom);
      if (endPos < 0) {
        gCddvIcsScannedUpTo = gCddvIcsPartial.length();
        break;  // still incomplete - wait for more bytes, resume from here next call
      }
      int blockEnd = endPos + tagLen;

      // Heap kept draining to the critical floor although the early drop was firing on almost every component (1425 dropped, 2 kept on one feed). If dropping a component's data does not
      // recover its memory, the parse-then-discard cycle itself leaks. Measured once per component (no heap-tracing hooks here) to see whether and how much.
      uint32_t heapBeforeComponent = ESP.getFreeHeap();
      cddvIcsOnComponent(gCddvIcsPartial.substring(gCddvIcsPendingBegin, blockEnd));
      uint32_t heapAfterComponent = ESP.getFreeHeap();
      if (logOn(LOG_VERBOSE) && heapAfterComponent + 256 < heapBeforeComponent) {
        logPrintf(LOG_VERBOSE, "CalDAV: [ics] heap dropped %ld bytes processing one component (%lu -> %lu)\r\n",
                      (long) (heapBeforeComponent - heapAfterComponent),
                      (unsigned long) heapBeforeComponent, (unsigned long) heapAfterComponent);
      }

      consumedUpTo = blockEnd;
      gCddvIcsPendingBegin = -1;
      gCddvIcsPendingEndTag = NULL;
      gCddvIcsScannedUpTo = 0;
    }
  }
  if (consumedUpTo > 0) {
    gCddvIcsPartial.remove(0, consumedUpTo);
    // A NEW component may have been found (pending, waiting for its END:) in the same call that completed earlier ones; its offsets are relative to the pre-trim buffer, so shift them down by what was removed.
    if (gCddvIcsPendingBegin >= 0) {
      gCddvIcsPendingBegin -= consumedUpTo;
      gCddvIcsScannedUpTo -= consumedUpTo;
    }
    gCddvIcsNoBeginScannedUpTo -= consumedUpTo;
    if (gCddvIcsNoBeginScannedUpTo < 0) gCddvIcsNoBeginScannedUpTo = 0;
  }
}

// Unlike CalDAV/iCloud's parseICalAndEmit (which sends each event when its single-event <calendar-data> block ends), streaming here sends nothing until the WHOLE feed has arrived and
// cddvIcsFinishParse runs: RECURRENCE-ID overrides can reference a master earlier or later in the document. So the per-item log goes silent for the whole body transfer, which looks like "stuck".
// This heartbeat (one line per ~16 KB received) is the only progress signal: a real stall repeats the byte count, slow progress shows it climbing.
static size_t gCddvIcsTotalBytes = 0;
static size_t gCddvIcsLastHeartbeatBytes = 0;

// Tracks gCddvIcsPartial's reserved capacity by hand: String::capacity() is PROTECTED on this core (a build error showed it), so it cannot be queried to decide whether a reserve() is needed. Updated only after a successful reserve().
static size_t gCddvIcsPartialReservedTo = 0;

// Grow in fixed ~8 KB steps, not by reserving the full kCddvIcsPartialCap (48 KB) up front: a feed whose components run a few KB (typical) aborted on "free heap critically low"
// after a dozen events because a flat 48 KB reservation, sized for the worst case (one huge VTIMEZONE), stayed pinned for the whole fetch. Steps still avoid reallocating on every small TCP/TLS
// chunk (the original O(chunks x size) concern), while a typical component pins a few KB.
static const size_t kCddvIcsGrowStep = 8192;

bool cddvOnIcsChunk(const char *data, size_t len) {
  if (gCddvIcsOverflow) return false;
  size_t neededLen = gCddvIcsPartial.length() + len;
  if (neededLen > kCddvIcsPartialCap) {
    gCddvIcsOverflow = true;
    LOGW("CalDAV: [ics] a single component exceeded the accumulator cap - malformed feed (missing END:?)");
    return false;
  }
  if (neededLen > gCddvIcsPartialReservedTo) {
    size_t target = neededLen + kCddvIcsGrowStep;
    if (target > kCddvIcsPartialCap) target = kCddvIcsPartialCap;
    if (gCddvIcsPartial.reserve(target)) gCddvIcsPartialReservedTo = target;
    // If reserve() fails, gCddvIcsPartialReservedTo stays too small and the concat() below still grows with its own checked fallback, as before this optimisation.
  }
  if (!gCddvIcsPartial.concat((const uint8_t *) data, (unsigned int) len)) {
    gCddvIcsOverflow = true;
    LOGW("CalDAV: [ics] out of memory accumulating response chunk");
    return false;
  }
  gCddvIcsTotalBytes += len;
  if (logOn(LOG_VERBOSE) && gCddvIcsTotalBytes - gCddvIcsLastHeartbeatBytes >= 16384) {
    gCddvIcsLastHeartbeatBytes = gCddvIcsTotalBytes;
    logPrintf(LOG_VERBOSE, "CalDAV: [ics] ...%u bytes received so far (%u masters kept [%u repeating], %u dropped as out-of-window, %u override uids buffered, %u bytes free heap)\r\n",
                  (unsigned) gCddvIcsTotalBytes, (unsigned) gCddvIcsMasters.size(), gCddvIcsMastersRepeating,
                  gCddvIcsMastersDropped, (unsigned) gCddvIcsOverrides.size(), (unsigned) ESP.getFreeHeap());
  }
  // A 200 whose body is not iCalendar (usually an HTML bot-challenge or CAPTCHA page from the feed's CDN/WAF) would parse as zero VEVENTs and look like an empty feed; checked once on the small partial buffer.
  if (!gCddvIcsSawVCalendar && gCddvIcsPartial.indexOf("BEGIN:VCALENDAR") >= 0) gCddvIcsSawVCalendar = true;
  cddvIcsExtractComponents();
  return !gCddvIcsOverflow;
}

// ICS Feed: a plain unauthenticated GET of the whole feed, streamed and clamped locally to [rangeStart, rangeEnd) (a plain ICS feed has no server-side filtering).
bool cddvFetchIcsFeed(const String &feedUrl, time_t rangeStart, time_t rangeEnd,
                              CalDavEventSink sink, String &errOut) {
  int code;
  gCddvIcsPartial = "";
  // Reset the capacity tracker, not a flat reserve() to the full cap (see gCddvIcsPartialReservedTo and kCddvIcsGrowStep): reserving 48 KB for the whole fetch cost far more than a typical component needs.
  gCddvIcsPartialReservedTo = 0;
  gCddvIcsOverflow = false;
  gCddvIcsSawVCalendar = false;
  gCddvIcsPendingBegin = -1;
  gCddvIcsPendingEndTag = NULL;
  gCddvIcsScannedUpTo = 0;
  gCddvIcsNoBeginScannedUpTo = 0;
  gCddvIcsTotalBytes = 0;
  gCddvIcsLastHeartbeatBytes = 0;
  gCddvIcsRangeStart = (uICAL::seconds_t) rangeStart;
  gCddvIcsRangeEnd = (uICAL::seconds_t) rangeEnd;
  gCddvIcsMastersDropped = 0;
  gCddvIcsMastersRepeating = 0;
  cddvIcsBeginParse();
  // ICS Feed is the one path that needs the browser-like headers (public hosts behind a consumer WAF); see cddvHttpRequestOnce for why CalDAV/iCloud REPORT does not get them.
  bool ok = cddvHttpRequest(feedUrl, "GET", String(), nullptr, nullptr, nullptr, nullptr, nullptr, true,
                             cddvOnIcsChunk, code, errOut);
  if (!ok) {
    // code <= 0 after cddvHttpRequest's 3 retries: a connection dropped mid-handshake or mid-transfer, not a clean error status, fits a CDN/WAF (Cloudflare, Imperva/Incapsula) resetting a
    // client it dislikes. Spelled out because "connection lost" alone gives the user nothing to act on. Not for a failed DNS lookup: it has its own complete message, and the Palm's alert box is too small for both.
    if (code <= 0 && !errOut.startsWith("DNS lookup failed"))
      errOut += " - a CDN may be blocking WiRSa; try a mirror";
    return false;
  }
  if (code != 200) {
    errOut = String("ICS feed server returned HTTP ") + code;
    return false;
  }
  if (gCddvIcsOverflow) {
    errOut = "ICS feed body too large or ran out of memory while reading it";
    return false;
  }
  if (!gCddvIcsSawVCalendar) {
    errOut = "ICS feed is not iCalendar data (maybe a bot-protection page from the feed's CDN)";
    return false;
  }

  return cddvIcsFinishParse((uICAL::seconds_t) rangeStart, (uICAL::seconds_t) rangeEnd, sink);
}
#endif  // WIRSA_FEATURE_CALDAV_ICS

}  // namespace CalDavRelay
