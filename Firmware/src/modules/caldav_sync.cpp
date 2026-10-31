// caldav_sync.cpp - SYNC_REQUEST: fetch, convert, hand to the Palm. Part of the CalDAV Relay mode; include/caldav_internal.h says how the files divide the work.
#include "caldav_internal.h"

namespace CalDavRelay {

// ---- SYNC_REQUEST handling ----

void cddvSendSyncError(const char *msg, uint8_t code) {
  // The Palm's error alert holds about this much text; a longer message is cut at a word and ends with "..." (the log has it in full).
  char text[CDDV_ERR_TEXT_MAX + 1];
  size_t n = strlen(msg);
  if (n <= CDDV_ERR_TEXT_MAX) {
    memcpy(text, msg, n + 1);
  } else {
    size_t cut = CDDV_ERR_TEXT_MAX - 3;
    while (cut > 0 && msg[cut] != ' ') cut--;
    if (cut == 0) cut = CDDV_ERR_TEXT_MAX - 3;
    memcpy(text, msg, cut);
    memcpy(text + cut, "...", 4);
  }
  RelayProto::Writer w(gEventBody, sizeof(gEventBody));
  w.u8(code);
  w.str(String(text));
  RelayQueue::emitFinal(CDDV_ERR, gEventBody, w.len);
}

// Event text is UTF-8 from the server; the Palm shows Palm Latin (PalmText::fromUtf8: accents kept where Palm Latin has them, Chinese and kana and Hangul romanised). uid is never converted.
// Chinese expands (a character becomes ~5), so the Palm-side field caps cut it first.

bool cddvSendEventItem(const String &uid, const String &title, const String &location,
                               uint32_t utcStartPalm, uint32_t utcEndPalm, bool allDay,
                               const String &description, bool isRepeating) {
  RelayProto::Writer w(gEventBody, sizeof(gEventBody));
  w.str(uid);
  w.str(PalmText::fromUtf8(title));
  w.str(PalmText::fromUtf8(location));
  w.u32(utcStartPalm);
  w.u32(utcEndPalm);
  w.u8(allDay ? 1 : 0);
  // New fields are appended, not interleaved, matching apps/caldav_sync/SyncEngine.c's crStr/cr8 read order; change both together.
  w.str(PalmText::fromUtf8(description));
  w.u8(isRepeating ? 1 : 0);

  // Queued when the download-ahead queue is on (relay_queue.h), else sent and acked here. false = the Palm is gone.
  bool acked = RelayQueue::send(CDDV_EVENT_ITEM, gEventBody, w.len, title.c_str());
  if (acked) {
    gCddvEventsSent++;
    LOGV("CalDAV: [emit] #%u %s (uid=%s, title=%s)\r\n",
                                 gCddvEventsSent, RelayQueue::active() ? "queued" : "sent", uid.c_str(), title.c_str());
  } else if (logOn(LOG_WARN)) {
    logPrintf(LOG_WARN, "CalDAV: EVENT_ITEM not sent (uid=%s) - the Palm stopped acking\r\n", uid.c_str());
  }
  return acked;
}

#if WIRSA_FEATURE_CARDDAV
// ---- CardDAV contacts ----
// One CONTACT_ITEM per contact and a GROUP_ITEM per group card, both acked as EVENT_ITEM. Bodies are in Protocol-wire.md section 10 (read by CalDAV Sync; change both together):
//  CONTACT_ITEM: uid str, displayName str, sortName str, nDates u8, {year u16 (0 = unknown), month u8, day u8, kind u8 (0 birthday, 1 engagement, 2 remembrance, 3 wedding, 4 work, 5 other), name str}*nDates,
//                nCategories u8, {category str}*nCategories, text str (trimmed vCard lines, '\n'-separated)
//  GROUP_ITEM:   uid str, name str, nMembers u8, {member uid str}*nMembers; a big group comes as several frames with the same uid.
// All text is Palm Latin (CardDav::toPalmLatin), never UTF-8.

String cddvStr(const std::string &s) {
  String r;
  if (s.size()) r.concat((const uint8_t *) s.data(), (unsigned int) s.size());
  return r;
}

bool cddvSendAcked(uint8_t type, size_t len, const char *what, const std::string &uid) {
  bool acked = RelayQueue::send(type, gEventBody, len, what);
  if (acked) {
    gCddvEventsSent++;
  } else if (logOn(LOG_WARN)) {
    logPrintf(LOG_WARN, "CardDAV: %s not sent (uid=%s) - the Palm stopped acking\r\n", what, maskSensitive(cddvStr(uid)).c_str());
  }
  return acked;
}

bool cddvSendContact(const CardDav::Card &c) {
  RelayProto::Writer w(gEventBody, sizeof(gEventBody));
  w.str(cddvStr(c.uid));
  w.str(cddvStr(c.displayName));
  w.str(cddvStr(c.sortName));
  uint8_t nd = c.dates.size() > 8 ? 8 : (uint8_t) c.dates.size();
  w.u8(nd);
  for (uint8_t i = 0; i < nd; i++) {
    const CardDav::CardDate &d = c.dates[i];
    w.u16(d.year); w.u8(d.month); w.u8(d.day); w.u8(d.kind);
    w.str(cddvStr(d.name));
  }
  uint8_t nc = c.categories.size() > 16 ? 16 : (uint8_t) c.categories.size();
  w.u8(nc);
  for (uint8_t i = 0; i < nc; i++) w.str(cddvStr(c.categories[i]));
  // The text goes last; a card too big for one frame (none in a real 850-card iCloud book, the largest 1.3 KB) is cut at a line break.
  std::string text = c.text;
  size_t room = (w.len + 2 < sizeof(gEventBody)) ? sizeof(gEventBody) - w.len - 2 : 0;
  if (text.size() > room) {
    size_t cut = text.rfind('\n', room ? room - 1 : 0);
    text.resize(cut == std::string::npos ? 0 : cut + 1);
    LOGW("CardDAV: card cut to %u bytes to fit one frame\r\n", (unsigned) text.size());
  }
  w.str(cddvStr(text));
  return cddvSendAcked(CDDV_CONTACT_ITEM, w.len, "CONTACT_ITEM", c.uid);
}

bool cddvSendGroup(const CardDav::Card &c) {
  size_t i = 0;
  do {   // at least one frame, even for a group with no members
    RelayProto::Writer w(gEventBody, sizeof(gEventBody));
    w.str(cddvStr(c.uid));
    w.str(cddvStr(c.displayName));
    size_t countAt = w.len;
    w.u8(0);
    uint8_t n = 0;
    while (i < c.members.size() && n < 255 &&
           w.len + 2 + c.members[i].size() + 16 < sizeof(gEventBody)) {
      w.str(cddvStr(c.members[i]));
      i++; n++;
    }
    gEventBody[countAt] = n;
    if (!cddvSendAcked(CDDV_GROUP_ITEM, w.len, "GROUP_ITEM", c.uid)) return false;
  } while (i < c.members.size());
  return true;
}

// gCddvOnBlock for an <address-data> block: trim, convert, send.
static unsigned gCddvContacts = 0, gCddvGroups = 0, gCddvSkipped = 0;
void cddvOnAddressDataBlock(const String &block) {
  if (gSinkAborted) return;
  CardDav::Card c;
  bool ok;
  try {   // a card too big for the heap is skipped, not a crash
    std::string v(block.c_str(), block.length());
    if (!CardDav::trimCard(v, c)) { gCddvSkipped++; return; }
    ok = c.isGroup ? cddvSendGroup(c) : cddvSendContact(c);
  } catch (const std::bad_alloc &) {
    gCddvSkipped++;
    return;
  }
  if (!ok) { gSinkAborted = true; return; }
  if (c.isGroup) gCddvGroups++; else gCddvContacts++;
  yield();
}

// iCloud contacts: https://{host}/{DSID}/carddavhome/{book}/ - host as entered in the slot (pNN-contacts.icloud.com), book "card" unless the slot names another.
String joinUrlICloudContacts(const String &shard, const String &dsid, const String &book) {
  String host = shard;
  if (!host.startsWith("https://") && !host.startsWith("http://")) host = "https://" + host;
  if (!host.endsWith("/")) host += "/";
  String u = dsid;
  while (u.startsWith("/")) u = u.substring(1);
  while (u.endsWith("/")) u = u.substring(0, u.length() - 1);
  String b = book.length() ? book : String("card");
  while (b.startsWith("/")) b = b.substring(1);
  while (b.endsWith("/")) b = b.substring(0, b.length() - 1);
  return host + u + "/carddavhome/" + b + "/";
}
#endif  // WIRSA_FEATURE_CARDDAV

void cddvHandleSyncRequest(const uint8_t *body, size_t bodyLen) {
  gCddvLastSyncOk = false;  // only flipped true at this function's own real success exit, below
  RelayProto::Reader r(body, bodyLen);
  uint8_t serverType = r.u8();
  String server = r.str();
  String path = r.str();
  String uuid = r.str();
  String user = r.str();
  String secret = r.str();
  uint8_t pastWin = r.u8();
  uint8_t futureWin = r.u8();
  // See SyncEngine.c's matching comment. Not compatible with a Palm build that predates this field: Reader::u8()/u32() set r.ok = false past the end of a short body (checked below, as every field),
  // so a mismatched pair gets "Malformed SYNC_REQUEST", not a bogus 0. That fits how the project is used: the Palm app and WiRSa firmware are always rebuilt and reflashed together.
  uint32_t palmUtcNow = r.u32();
  // Google's OAuth Client ID and Secret live on the Palm only (CDDVSlot) and are sent in every SYNC_REQUEST, so WiRSa holds no resident account-identifying value. Empty for every other server type.
  String googleClientId = r.str();
  String googleClientSecret = r.str();

  if (!r.ok) {
    cddvSendSyncError("Malformed SYNC_REQUEST", CDDV_ERR_BAD_REQUEST);
    return;
  }

  bool contacts = serverType == CDDV_TYPE_ICLOUD_CONTACTS || serverType == CDDV_TYPE_CARDDAV;
  if (serverType != CDDV_TYPE_CALDAV && serverType != CDDV_TYPE_ICLOUD &&
      serverType != CDDV_TYPE_ICSFEED && serverType != CDDV_TYPE_GOOGLE && !contacts) {
    cddvSendSyncError("Unknown server type", CDDV_ERR_BAD_REQUEST);
    return;
  }

  // The Palm's clock is the primary source for "now" (see the Clock section above); fall back to WiRSa's system clock only if the Palm sent something that is not a real timestamp (0, or an old
  // Palm build that predates this field and decodes as 0).
  time_t now = (time_t) palmUtcNow;
  bool usedPalmClock = (now > 1577836800L);  // sane post-2020 UTC time
  if (!usedPalmClock) time(&now);

  if (pastWin >= kPastWindowCount) pastWin = 0;
  if (futureWin >= kFutureWindowCount) futureWin = 0;
  time_t rangeStart = now - (time_t) kPastWindowDays[pastWin] * 86400L;
  time_t rangeEnd = now + (time_t) kFutureWindowDays[futureWin] * 86400L;

  // Masks account-identifying fields (email, DSID, calendar UUID) in debug logs so a log can be shared without redaction. The request below uses the real values; only logging is masked.
  String userLog = maskSensitive(user);
  String uuidLog = maskSensitive(uuid);
  String pathLog = maskSensitive(path);

  if (logOn(LOG_VERBOSE)) {
    logPrintf(LOG_VERBOSE, "CalDAV: SYNC_REQUEST type=%u server=%s path=%s uuid=%s user=%s pastWin=%u futureWin=%u\r\n",
                   serverType, server.c_str(), pathLog.c_str(), uuidLog.c_str(), userLog.c_str(), pastWin, futureWin);
    logPrintf(LOG_VERBOSE, "CalDAV: [window] now=%ld range=[%ld .. %ld] (clock source: %s)\r\n",
                   (long) now, (long) rangeStart, (long) rangeEnd, usedPalmClock ? "Palm" : "WiRSa system clock");
    if (serverType == CDDV_TYPE_GOOGLE) {
      // First 4 characters and the length only (maskSensitive's convention): enough to spot a truncated, mistyped or empty value without printing the real Client ID/Secret to a log the user might share.
      logPrintf(LOG_VERBOSE, "CalDAV: [google] clientId=%s clientSecret=%s\r\n",
                    maskSensitive(googleClientId).c_str(), maskSensitive(googleClientSecret).c_str());
    }
  }

  gSinkAborted = false;
  gCddvProdOk = false;
  gCddvEventsSent = 0;
  gCddvBlocksExtracted = 0;
  gCddvMultistatusAccum = "";
#if WIRSA_FEATURE_CALDAV_REPORT
  gCddvDataTag = "calendar-data";          // the reader's default: calendars
  gCddvOnBlock = cddvOnCalendarDataBlock;
#endif
  CalDavEventSink sink = cddvSendEventItem;
  gActiveSink = &sink;
  String err;
  bool ok;

  if (serverType == CDDV_TYPE_CALDAV || serverType == CDDV_TYPE_ICLOUD) {
#if WIRSA_FEATURE_CALDAV_REPORT
    // iCloud's CalDAV server (with the right shard, UUID and calendar path, see joinUrlICloud) is a normal RFC 4791 endpoint: the same REPORT body, Basic Auth and 207/200 handling as generic
    // CalDAV works unmodified; only the URL construction differs.
    gCddvRangeStart = rangeStart;
    gCddvRangeEnd = rangeEnd;
    char startBuf[24], endBuf[24];
    formatUtcBasic(rangeStart, startBuf, sizeof(startBuf));
    formatUtcBasic(rangeEnd, endBuf, sizeof(endBuf));
    // A one-sided (open-ended future) time-range is valid per RFC 4791 and is what EDS's cbdav_add_limit_days_filter() sends with no future limit, but it is costlier for a server (indefinitely
    // recurring series are reasoned about against "start to infinity"). The bounded two-sided filter is kept for a predictable request against a server under load.
    String reportBody =
      "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
      "<C:calendar-query xmlns:D=\"DAV:\" xmlns:C=\"urn:ietf:params:xml:ns:caldav\">"
        "<D:prop><D:getetag/><C:calendar-data/></D:prop>"
        "<C:filter><C:comp-filter name=\"VCALENDAR\"><C:comp-filter name=\"VEVENT\">"
          "<C:time-range start=\"" + String(startBuf) + "\" end=\"" + String(endBuf) + "\"/>"
        "</C:comp-filter></C:comp-filter></C:filter>"
      "</C:calendar-query>";
    String url = (serverType == CDDV_TYPE_ICLOUD)
      ? joinUrlICloud(server, uuid, path)
      : joinUrl(server, path);
    LOGV("CalDAV: [fetch] %s REPORT: %s\r\n",
                                 (serverType == CDDV_TYPE_ICLOUD) ? "iCloud" : "CalDAV", maskUrlForLog(url).c_str());
    int code;
    // browserHeaders=false: an authenticated WebDAV REPORT identifies itself honestly, not as Chrome (claiming Chrome on a method and Accept no browser produces risks a WAF). The Content-Type charset is
    // quoted as EDS's E_WEBDAV_CONTENT_TYPE_XML (e-webdav-session.h: "application/xml; charset=\"utf-8\"").
    // cddvOnMultistatusChunk extracts and emits each <calendar-data> block as its bytes arrive, via gCddvMultistatusAccum, and never buffers the whole chunked response of unknown size.
    if (CDDV_DIAGNOSTIC_RAW_TLS) {
      // Bypasses cddvHttpRequest/HTTPClient (see cddvRawTlsDiagnostic). Always reports as an ERR (never a real sync), so the result (success or failure, timing, byte counts) shows on the Palm without reading serial logs.
      cddvRawTlsDiagnostic(url, user.c_str(), secret.c_str(), reportBody, err);
      ok = false;
    } else {
      ok = cddvHttpRequest(url, "REPORT", reportBody, user.c_str(), secret.c_str(),
                            "application/xml; charset=\"utf-8\"", "Depth", "1", false,
                            cddvOnMultistatusChunk, code, err);
      if (ok) {
        if (code != 207 && code != 200) {
          ok = false;
          err = String("CalDAV server returned HTTP ") + code;
          // Apple rejects the regular Apple ID password outright for CalDAV once 2FA is on (Basic Auth cannot complete a second factor). A 401 here is very likely that, not a wrong shard, UUID or
          // calendar path, so say so rather than make the user guess.
          if (serverType == CDDV_TYPE_ICLOUD && code == 401)
            err += " - use an app-specific password, not your Apple ID password";
        } else {
          LOGV("CalDAV: [parse] extracted %u calendar-data block(s) (streamed)\r\n",
                                       gCddvBlocksExtracted);
          ok = !gSinkAborted;
          if (!ok && err.length() == 0) err = "Lost contact with the Palm mid-sync";
        }
      }
    }
#else
    ok = false;
    err = "Generic CalDAV/iCloud sync is not enabled in this WiRSa build";
#endif  // WIRSA_FEATURE_CALDAV_REPORT
  } else if (contacts) {
#if WIRSA_FEATURE_CARDDAV
    // CardDAV: one REPORT addressbook-query for the whole address book (iCloud and Radicale both answer it, apps/carddav_sync.md section 2.2); each <address-data> card is trimmed and sent as it streams
    // in. The sync window fields are not used.
    String url = (serverType == CDDV_TYPE_ICLOUD_CONTACTS)
      ? joinUrlICloudContacts(server, uuid, path)
      : joinUrl(server, path);
    String reportBody =
      "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
      "<C:addressbook-query xmlns:D=\"DAV:\" xmlns:C=\"urn:ietf:params:xml:ns:carddav\">"
        "<D:prop><D:getetag/><C:address-data/></D:prop>"
      "</C:addressbook-query>";
    LOGV("CardDAV: [fetch] %s REPORT: %s\r\n",
                          (serverType == CDDV_TYPE_ICLOUD_CONTACTS) ? "iCloud" : "CardDAV",
                          maskUrlForLog(url).c_str());
    gCddvContacts = gCddvGroups = gCddvSkipped = 0;
    gCddvDataTag = "address-data";
    gCddvOnBlock = cddvOnAddressDataBlock;
    int code;
    ok = cddvHttpRequest(url, "REPORT", reportBody, user.c_str(), secret.c_str(),
                          "application/xml; charset=\"utf-8\"", "Depth", "1", false,
                          cddvOnMultistatusChunk, code, err);
    gCddvDataTag = "calendar-data";
    gCddvOnBlock = cddvOnCalendarDataBlock;
    if (ok) {
      if (code != 207 && code != 200) {
        ok = false;
        err = String("CardDAV server returned HTTP ") + code;
        if (serverType == CDDV_TYPE_ICLOUD_CONTACTS && code == 401)
          err += " - use an app-specific password, not your Apple ID password";
      } else {
        LOGV("CardDAV: [done] %u contact(s), %u group(s) sent, %u card(s) skipped\r\n",
                              gCddvContacts, gCddvGroups, gCddvSkipped);
        ok = !gSinkAborted;
        if (!ok && err.length() == 0) err = "Lost contact with the Palm mid-sync";
      }
    }
#else
    ok = false;
    err = "Contacts sync is not enabled in this WiRSa build";
#endif  // WIRSA_FEATURE_CARDDAV
  } else if (serverType == CDDV_TYPE_GOOGLE) {
#if WIRSA_FEATURE_CALDAV_GOOGLE
    // path = calendar ID, secret = the Sec35-decrypted refresh token, googleClientId/googleClientSecret = the Palm-side fields; all sent fresh in this SYNC_REQUEST (see SyncEngine.c's field mapping).
    // WiRSa holds none of them resident.
    LOGV("CalDAV: [fetch] Google Calendar");
    if (googleClientId.length() == 0 || googleClientSecret.length() == 0) {
      ok = false;
      err = "No Google OAuth Client ID/Secret set for this calendar - enter them in Edit on the Palm";
    } else if (secret.length() == 0) {
      ok = false;
      err = "Not signed in to Google yet - use \"Sign in with Google\" in Edit first";
    } else {
      ok = cddvFetchGoogleCalendar(secret, googleClientId, googleClientSecret, path, rangeStart, rangeEnd, sink, err);
    }
#else
    ok = false;
    err = "Google Calendar sync is not enabled in this WiRSa build";
#endif  // WIRSA_FEATURE_CALDAV_GOOGLE
  } else {
#if WIRSA_FEATURE_CALDAV_ICS
    LOGV("CalDAV: [fetch] ICS feed: %s\r\n", maskUrlForLog(server).c_str());
    ok = cddvFetchIcsFeed(server, rangeStart, rangeEnd, sink, err);
#else
    ok = false;
    err = "ICS Feed sync is not enabled in this WiRSa build";
#endif  // WIRSA_FEATURE_CALDAV_ICS
  }

  gActiveSink = nullptr;

  if (!ok) {
    gCddvSessFail++;
    LOGW("CalDAV: sync failed: %s (after %u event(s) sent)\r\n",
                                 err.c_str(), gCddvEventsSent);
    if (!gSinkAborted) cddvSendSyncError(err.length() ? err.c_str() : "Sync failed");
    // If gSinkAborted, the Palm has already lost contact or sent BYE: no point sending an ERR it likely will not see.
    return;
  }

  // What was fetched and handed over. Whether the Palm took it is known only once the queue has drained (cddvReportSyncDelivery, after consume()).
  gCddvProdOk = true;
  gCddvProdContacts = contacts;
#if WIRSA_FEATURE_CARDDAV
  gCddvProdCount = contacts ? gCddvContacts : gCddvEventsSent;
#else
  gCddvProdCount = gCddvEventsSent;   // no contacts without CardDAV
#endif
  LOGV("CalDAV: fetched %u %s(s)\r\n", gCddvProdCount, contacts ? "contact" : "event");
  gCddvLastSyncOk = true;
  RelayQueue::emitFinal(CDDV_SYNC_DONE, nullptr, 0);
  RelayQueue::emitFinal(CDDV_BYE, nullptr, 0);
}

// cddvHandleSyncRequest is stack-hungry on the default loop() task's 8 KB: a TLS handshake (mbedTLS crypto needs depth), then a full C++ object graph (shared_ptr/map/vector VObject/VLine tree,
// RRULE/VEventIter expansion) via uICAL. A stack overflow does not always fault at once: it can smash adjacent memory (the WiFi/LWIP task's state included, since ESP32 tasks share the heap) and
// give non-deterministic instability instead of a clean crash. So it runs on a dedicated FreeRTOS task with a bigger stack, as bitwarden_relay.cpp's bwHandleSyncBigStack.
// 64 KB, not 32 KB: the ICS Feed path (cddvFetchIcsFeed) runs the uICAL parse of each VEVENT/VTIMEZONE (cddvIcsOnComponent) from CddvChunkStream::write(), which is called INSIDE HTTPClient::writeToStream()'s
// socket/TLS read chain (writeToStreamDataBlock -> WiFiClientSecure::read -> mbedTLS record decrypt). Unlike the multistatus path (parseICalAndEmit runs after the request has returned, at a shallow
// post-TLS depth), the whole uICAL graph build nests beneath the TLS/HTTPClient read stack, so the two peaks add. A hardware crash (Guru Meditation / LoadProhibited, EXCVADDR=0x00000000) a few
// VTIMEZONE components into a sync fits that combined peak overflowing the old 32 KB; a typical sync has 100 KB+ of free heap, so a bigger stack is cheap.
static SemaphoreHandle_t gCddvSyncTaskDone = nullptr;
static const uint8_t *gCddvSyncTaskBody = nullptr;
static size_t gCddvSyncTaskBodyLen = 0;

// After the whole slot has gone to the Palm (queue drained, or the inline path returned): log what arrived and add it to the session totals. A failed fetch was counted already.
void cddvReportSyncDelivery(bool queued) {
  if (!gCddvProdOk) return;
  uint32_t lost = queued ? RelayQueue::stats().lost : 0;
  bool cut = queued && RelayQueue::cancelled();
  const char *what = gCddvProdContacts ? "contact" : "event";
  unsigned delivered = gCddvProdCount > lost ? gCddvProdCount - lost : 0;
  if (!cut && lost == 0) {
    gCddvSessOk++;
    LOGI("CalDAV: synced %u %s(s) to the Palm\r\n", gCddvProdCount, what);
  } else {
    gCddvSessFail++;
    LOGW("CalDAV: sync incomplete - %u of %u %s(s) reached the Palm%s\r\n", delivered, gCddvProdCount, what,
         cut ? " (the Palm stopped answering)" : "");
  }
  if (gCddvProdContacts) gCddvSessContacts += delivered; else gCddvSessEvents += delivered;
}

static const uint32_t CDDV_SYNC_STACK = 16384;   // measured peak 5.9 KB over events and 813 contacts

void cddvHandleSyncRequestTask(void *) {
  cddvHandleSyncRequest(gCddvSyncTaskBody, gCddvSyncTaskBodyLen);
  LOGI("CalDAV: sync task stack used at most %u of %u bytes\r\n",
       (unsigned) (CDDV_SYNC_STACK - uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t)), (unsigned) CDDV_SYNC_STACK);
  RelayQueue::end();                       // wakes the consumer (no-op without the queue)
  xSemaphoreGive(gCddvSyncTaskDone);
  vTaskDelete(NULL);
}

// body must outlive the task: gFrameBody, the only caller's buffer, is static and fully consumed here before caldavRelayLoop() reads another frame into it (the caller blocks until done), as bwHandleSyncBigStack.
void cddvHandleSyncRequestBigStack(const uint8_t *body, size_t bodyLen) {
  if (!gCddvSyncTaskDone) gCddvSyncTaskDone = xSemaphoreCreateBinary();
  gCddvSyncTaskBody = body;
  gCddvSyncTaskBodyLen = bodyLen;
  TaskHandle_t h = nullptr;
  RelayQueue::Config qc = { CDDV_ACK, CDDV_BYE, gAckScratch, sizeof(gAckScratch),
                            RelayTiming::CDDV_ACK_ATTEMPTS, RelayTiming::ACK_RETRY_DELAY_MS, RelayTiming::ITEM_ACK_TIMEOUT_MS, RelayTiming::BYTE_TIMEOUT_MS,
                            CDDV_SYNC_DONE };
  bool queued = RelayQueue::begin(qc);
#if !WIRSA_CDDV_QUEUE
  RelayQueue::disable(); queued = false;
#endif
  LOGV("CalDAV: download-ahead queue %s\r\n", queued ? "ON (PSRAM)" : "OFF");
  // Internal RAM when the stack is wanted (a task stack must be one internal block).
  LOGI("CalDAV: sync start, internal heap free %u, largest block %u (stack needs %u); PSRAM free %u\r\n",
       (unsigned) heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
       (unsigned) heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL), (unsigned) CDDV_SYNC_STACK,
       (unsigned) ESP.getFreePsram());
  BaseType_t created = xTaskCreate(cddvHandleSyncRequestTask, "cddvSync", CDDV_SYNC_STACK, nullptr, 1, &h);
  if (created != pdPASS) {
    LOGW("CalDAV: failed to create sync task (out of memory?) - running inline\r\n");
    RelayQueue::disable();                 // one thread: nothing could drain a queue
    cddvHandleSyncRequest(body, bodyLen);
    cddvReportSyncDelivery(false);
    return;
  }
  RelayQueue::consume();                   // sends the queued frames while the task fetches
  xSemaphoreTake(gCddvSyncTaskDone, portMAX_DELAY);
  if (RelayQueue::cancelled()) gCddvLastSyncOk = false;   // the Palm never got all of it
  cddvReportSyncDelivery(queued);
}

}  // namespace CalDavRelay
