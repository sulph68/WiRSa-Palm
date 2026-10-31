// caldav_http.cpp - HTTP(S) requests and the multistatus reader (CalDAV, iCloud, CardDAV). Part of the CalDAV Relay mode; include/caldav_internal.h says how the files divide the work.
#include "caldav_internal.h"

namespace CalDavRelay {

// ---- Network fetch ----

// cddvHttpRequestOnce is one attempt: WiFiClientSecure (certificate verification off, TOFU as in bitwarden_relay.cpp's setInsecure(); this protocol has no BW_TRUST
// equivalent) or a plain WiFiClient by the URL scheme, then HTTPClient::begin(client, url) parses the rest. cddvHttpRequest wraps it in the retry loop.
// The hostname of a "scheme://host[:port]/path" URL, for the DNS pre-check only (HTTPClient::begin does the real parsing).
String cddvUrlHost(const String &url) {
  int schemeEnd = url.indexOf("://");
  int hostStart = (schemeEnd >= 0) ? schemeEnd + 3 : 0;
  int pathStart = url.indexOf('/', hostStart);
  int hostEnd = (pathStart >= 0) ? pathStart : url.length();
  String hostPort = url.substring(hostStart, hostEnd);
  int colon = hostPort.indexOf(':');
  return (colon >= 0) ? hostPort.substring(0, colon) : hostPort;
}

// Port for cddvRawTlsDiagnostic: an explicit ":port" wins, else by scheme (443/80); a fixed 443 would attempt TLS against a plain-HTTP target's port.
uint16_t cddvUrlPort(const String &url, bool isHttps) {
  int schemeEnd = url.indexOf("://");
  int hostStart = (schemeEnd >= 0) ? schemeEnd + 3 : 0;
  int pathStart = url.indexOf('/', hostStart);
  int hostEnd = (pathStart >= 0) ? pathStart : url.length();
  String hostPort = url.substring(hostStart, hostEnd);
  int colon = hostPort.indexOf(':');
  if (colon >= 0) return (uint16_t) hostPort.substring(colon + 1).toInt();
  return isHttps ? 443 : 80;
}

// Path for cddvRawTlsDiagnostic (host stripped; joinUrl()/joinUrlICloud() URLs never carry a query string).
String cddvUrlPath(const String &url) {
  int schemeEnd = url.indexOf("://");
  int hostStart = (schemeEnd >= 0) ? schemeEnd + 3 : 0;
  int pathStart = url.indexOf('/', hostStart);
  return (pathStart >= 0) ? url.substring(pathStart) : String("/");
}

// A caller's sink for streamed response bytes (a plain function pointer, no capture); each call site accumulates and parses on its own. Returns false to abort the transfer
// (out of memory, malformed feed, downstream gave up). Without the abort, HTTPClient kept pulling the rest of a large response off the socket after an aborted parse, which looked
// like a hang from the Palm.

// A write-only Arduino Stream handed to HTTPClient::writeToStream(), replacing getString() so a response never has to fit in a buffer. writeToStream() already de-chunks
// Transfer-Encoding, so write() just forwards the bytes to the caller's onChunk, and it cannot fail to "grow" since it has no buffer. The read side is never called (inert).
class CddvChunkStream : public Stream {
public:
  CddvBodyChunkFn onChunk = nullptr;
  size_t totalBytes = 0;

  int available() override { return 0; }
  int read() override { return -1; }
  int peek() override { return -1; }
  size_t write(uint8_t b) override { return write(&b, 1); }
  size_t write(const uint8_t *buffer, size_t size) override {
    // A short return here is HTTPClient's signal to abort (writeToStreamDataBlock treats bytesWrite != bytesRead as HTTPC_ERROR_STREAM_WRITE and stops reading); that stops the
    // socket read, not just our onChunk() no-opping.
    bool keepGoing = onChunk ? onChunk((const char *) buffer, size) : true;
    totalBytes += size;
    if (!keepGoing) return 0;
    // a response arrives as many small write() calls: yield() to keep WiFi and the watchdog serviced
    yield();
    return size;
  }
};

bool cddvHttpRequestOnce(const String &url, const char *method, const String &body,
                                 const char *user, const char *pass,
                                 const char *contentType, const char *extraHeaderName,
                                 const char *extraHeaderValue, bool browserHeaders,
                                 CddvBodyChunkFn onBodyChunk,
                                 int &codeOut, bool &anyBodyDeliveredOut, String &errOut) {
  anyBodyDeliveredOut = false;
  // HTTPClient's errorToString(-1) lumps DNS failure, a blocked network path and local exhaustion into "connection refused", which looks like a credential problem.
  // Resolving the host first separates "DNS lookup failed" from a TCP/TLS refusal by a host that did resolve; neither is a credential problem, as nothing was sent yet.
  {
    String host = cddvUrlHost(url);
    IPAddress ip;
    LOGV("CalDAV: [dns] resolving %s\r\n", host.c_str());
    if (host.length() > 0 && !WiFi.hostByName(host.c_str(), ip)) {
      codeOut = 0;
      errOut = String("DNS lookup failed for ") + host + " - check the hostname, and that WiRSa has working internet.";
      return false;
    }
    LOGV("CalDAV: [dns] resolved %s -> %s\r\n", host.c_str(), ip.toString().c_str());
  }

  bool isHttps = url.startsWith("https://");
  bool ok;
  // Keep-alive: REPORT over TLS on a PSRAM board keeps its connection (see the keep-alive block near the top); everything else is one connection per request.
  bool keep = WIRSA_CDDV_KEEPALIVE && isHttps && !browserHeaders && strcmp(method, "REPORT") == 0 && psramFound();
  bool keptReused = false;
  HTTPClient localHttp;
  HTTPClient *httpPtr = &localHttp;
  WiFiClientSecure localTls;
  WiFiClientSecure *tlsPtr = &localTls;
  if (keep) {
    tlsPtr = cddvKeepClient(cddvUrlHost(url), cddvUrlPort(url, true), keptReused);
    httpPtr = gKeepHttp;
  }
  HTTPClient &http = *httpPtr;
  WiFiClientSecure &tls = *tlsPtr;

  // The mbedTLS SSL record buffers are 16384 bytes each way in this core's sdkconfig: two separate allocations, not one block. Logged for diagnostics only.
  // An earlier version rejected the connection below a 40 KB largest-free-block, assuming one ~32 KB contiguous allocation. That was wrong (a sync failed the check at 36852 with no
  // sign connect() would fail); two 16-20 KB holes are enough. Removed: the real connect() decides, and a failure takes the ordinary connection-failure path.
  if (isHttps && logOn(LOG_VERBOSE)) {
    logPrintf(LOG_VERBOSE, "CalDAV: heap before TLS connect: free=%u largest_free_block=%u\r\n",
                  (unsigned) ESP.getFreeHeap(), (unsigned) ESP.getMaxAllocHeap());
  }
  // tls/plain must outlive every http.* call, not just begin(): HTTPClient::begin(client, url) keeps a reference, so objects declared inside an if/else would be destroyed
  // when begin() returns, leaving a dangling reference. Same convention as bitwarden_relay.cpp.
  WiFiClient plain;
  LOGV("CalDAV: [connect] %s: %s%s\r\n", isHttps ? "TLS" : "plain", maskUrlForLog(url).c_str(),
                        keptReused ? " (reusing the open connection)" : "");
  if (isHttps) {
    tls.setInsecure();
    tls.setTimeout(60);  // seconds (WiFiClientSecure::setTimeout takes seconds, not ms)
    // The TLS handshake has its own ceiling, sslclient_context::handshake_timeout (default 120 s, set only by setHandshakeTimeout()); tls.setTimeout() sets only the TCP-connect and
    // socket read/write timeouts. Without this a handshake that retransmits a few times can run for two minutes.
    tls.setHandshakeTimeout(60);  // seconds
    // HTTPClient calls _client->setTimeout(), which dispatches to WiFiClientSecure::setTimeout(), a shadowed _timeout, not Stream::_timeout. Every Stream::readStringUntil()
    // HTTPClient does (status line, headers, chunk sizes) stays capped at Stream's 1000 ms default unless Stream::_timeout is set here by the upcast; HTTPClient's later call cannot undo it.
    ((Stream &) tls).setTimeout(60000);  // milliseconds - Stream::setTimeout's own unit, not seconds
    ok = http.begin(tls, url);
  } else {
    ((Stream &) plain).setTimeout(60000);  // same _timeout-shadowing risk applies to plain WiFiClient
    ok = http.begin(plain, url);
  }
  if (!ok) { errOut = "HTTP begin() failed (bad URL?)"; return false; }

  // HTTPClient::connect() passes its _connectTimeout (default HTTPCLIENT_DEFAULT_TCP_TIMEOUT = 5000 ms) to the 3-argument socket connect(), a different field from
  // http.setTimeout() (_tcpTimeout, post-connect reads) and tls.setTimeout(). A TLS handshake with more round trips than a light self-hosted server can exceed 5 s and is reported as
  // "connection refused". 60 s covers a slow REPORT on the remote server, which is the real long pole.
  http.setConnectTimeout(60000);
  http.setTimeout(60000);
  if (browserHeaders) {
    // A realistic Chrome-on-Windows header set, for the ICS feed only: some public hosts front the file with a CDN/WAF whose bot mitigation silently drops or resets a request
    // that does not look like a browser. No Accept-Encoding gzip/br: HTTPClient does not decompress, so "identity" (what Chrome falls back to) avoids binary garbage instead of ICS.
    // Not for CalDAV/iCloud REPORT: no browser issues a WebDAV REPORT, so claiming Chrome on that method and Accept is exactly the mismatch a WAF flags; those send an honest, minimal
    // client identifier (below). HTTPClient::addHeader() silently rejects Connection, User-Agent and Host, so User-Agent goes through setUserAgent().
    http.addHeader("Accept", "text/calendar,text/plain,application/xml,*/*");
    http.addHeader("Accept-Language", "en-US,en;q=0.9");
    http.addHeader("Accept-Encoding", "identity");
    http.setUserAgent("Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36");
  } else {
    // The headers Evolution Data Server (libedataserver, e-soup-session.c e_soup_session_preset_message()) sends as a WebDAV client: User-Agent "Evolution/" VERSION,
    // Connection: close, Cache-Control: no-cache and Pragma: no-cache (RFC 4918 10.4.5), no Accept. The Content-Type charset quoting is matched at the call site below
    // (E_WEBDAV_CONTENT_TYPE_XML is a quoted "utf-8"). setReuse(false) is how HTTPClient sends "Connection: close", as addHeader() cannot set it.
    http.setUserAgent("Evolution/3.52.3");
    http.setReuse(keep);
    http.addHeader("Cache-Control", "no-cache");
    http.addHeader("Pragma", "no-cache");
  }
  if (contentType) http.addHeader("Content-Type", contentType);
  if (extraHeaderName) http.addHeader(extraHeaderName, extraHeaderValue);
  if (user && pass && user[0]) http.setAuthorization(user, pass);

  if (logOn(LOG_VERBOSE)) {
    logPrintf(LOG_VERBOSE, "CalDAV: [send] %s (%u byte body, auth=%s, browserHeaders=%s)\r\n",
                  method, (unsigned) body.length(), (user && pass && user[0]) ? "yes" : "no",
                  browserHeaders ? "yes" : "no");
  }
  // The connect and TLS handshake are done (http.begin() succeeded): the on-device status goes from "Connecting..." to "Syncing..." as the request goes out, for REPORT and the ICS GET alike.
  cddvDrawStatus("Syncing...");

  int code;
  if (body.length() > 0) {
    code = http.sendRequest(method, body);
  } else if (strcmp(method, "GET") == 0) {
    code = http.GET();
  } else {
    code = http.sendRequest(method);
  }
  codeOut = code;
  if (code <= 0) {
    // Includes the raw HTTPClient code beside the text, e.g. "connection refused (code=-1)" vs "connection lost (code=-5)", two different HTTPC_ERROR_* values.
    errOut = String("HTTP request failed: ") + http.errorToString(code) + " (code=" + String(code) + ")";
    // Shows whether the WiFi association itself is gone when a request fails, or only this TCP/TLS session was reset by the remote end. WL_CONNECTED == 3 in this core.
    LOGV("CalDAV: [response] connect/send failed: %s (WiFi.status()=%d, %s)\r\n",
                                 errOut.c_str(), (int) WiFi.status(),
                                 (WiFi.status() == WL_CONNECTED) ? "still connected" : "NOT connected");
    http.end();
    if (keep) cddvKeepClose();
    return false;
  }
  // HTTPClient::getString() ignores writeToStream()'s result: if its StreamString failed to grow in a large response (fragmented heap) it returns the partial data with no error.
  // Streaming into CddvChunkStream has no buffer to fail to grow; what remains is a real mid-stream socket death, which writeToStreamDataBlock reports as a negative HTTPC_ERROR_*, checked here.
  CddvChunkStream sink;
  sink.onChunk = onBodyChunk;
  int written = http.writeToStream(&sink);
  anyBodyDeliveredOut = (sink.totalBytes > 0);
  if (written < 0) {
    errOut = String("HTTP response read failed: ") + http.errorToString(written) + " (code=" + String(written) + ")";
    LOGV("CalDAV: [response] %s\r\n", errOut.c_str());
    codeOut = 0;  // treat as a connection-level failure so the retry loop above retries it
    http.end();
    if (keep) cddvKeepClose();
    return false;
  }
  if (logOn(LOG_VERBOSE)) {
    logPrintf(LOG_VERBOSE, "CalDAV: [response] HTTP %d, %u byte body (streamed)\r\n", code, (unsigned) sink.totalBytes);
  }
  http.end();
  if (keep) gKeepLastUsed = millis();
  return true;
}

// Retries only a connection-level failure (code <= 0: DNS, TCP, TLS, a mid-transfer drop), each on a fresh socket and TLS handshake (the tls/http objects are function-local).
// A well-formed HTTP error (4xx/5xx) is a real answer and not retried; rapid retries would not help and look more like abuse to a WAF. Covers transient WiFi/DNS/server hiccups (as
// SyncEngine.c's SendAckWithRetry) and sometimes a WAF that dropped the first attempt only; it cannot help against a deterministic TLS-fingerprint block.
bool cddvHttpRequest(const String &url, const char *method, const String &body,
                             const char *user, const char *pass,
                             const char *contentType, const char *extraHeaderName,
                             const char *extraHeaderValue, bool browserHeaders,
                             CddvBodyChunkFn onBodyChunk,
                             int &codeOut, String &errOut) {
  static const int kMaxAttempts = 3;
  for (int attempt = 1; attempt <= kMaxAttempts; attempt++) {
    bool anyBodyDelivered = false;
    if (cddvHttpRequestOnce(url, method, body, user, pass, contentType,
                             extraHeaderName, extraHeaderValue, browserHeaders,
                             onBodyChunk, codeOut, anyBodyDelivered, errOut)) {
      return true;
    }
    if (codeOut > 0) return false;  // real HTTP error - don't retry
    // onBodyChunk may already have handed some of the response to the caller before a mid-stream death; on the CalDAV/iCloud path those events may be on the wire to the Palm
    // (cddvSendEventItem), and a retry restarts at byte 0 and would send them twice. A failure before any body (DNS, connect, send, a dead connection before the first body byte) is
    // safe to retry; one after body delivery began must fail hard instead.
    if (anyBodyDelivered) {
      LOGW("CalDAV: [retry] response already partially streamed - not retrying (would duplicate already-sent events)");
      return false;
    }
    if (attempt < kMaxAttempts) {
      LOGW("CalDAV: [retry] attempt %d/%d failed (%s), retrying...\r\n",
                                   attempt, kMaxAttempts, errOut.c_str());
      // network.cpp's maintainWiFi() skips MODE_CALDAV_RELAY (transferActive) so it does not pull the connection from under a transfer, so nothing brings WiFi back once it is
      // dead in this mode. Retrying against dead WiFi wastes the whole retry budget: reconnect and wait (bounded) before the next attempt.
      if (WiFi.status() != WL_CONNECTED) {
        LOGV("CalDAV: [retry] WiFi is down - reconnecting before next attempt...");
        WiFi.reconnect();
        unsigned long waitStart = millis();
        while (WiFi.status() != WL_CONNECTED && (millis() - waitStart) < 8000) {
          delay(200);
          yield();
        }
        if (logOn(LOG_INFO)) {
          logPrintf(LOG_INFO, "CalDAV: [retry] WiFi %s after %lums\r\n",
                        (WiFi.status() == WL_CONNECTED) ? "reconnected" : "still down",
                        (unsigned long) (millis() - waitStart));
        }
      } else {
        delay(700);
      }
    }
  }
  return false;
}

// extractCalendarDataBlocks takes a plain function pointer, so the sink, abort and range state of the REPORT path is threaded through these statics for one
// cddvHandleSyncRequest (single-threaded, one sync at a time, like every static scratch buffer here).
CalDavEventSink *gActiveSink = nullptr;
bool gSinkAborted = false;
// Totals for the BYE summary, reset when the mode is entered.
unsigned gCddvSessOk = 0, gCddvSessFail = 0, gCddvSessEvents = 0, gCddvSessContacts = 0;
// Result of the last fetch (producer side), reported once the Palm has had it all.
bool gCddvProdOk = false, gCddvProdContacts = false;
unsigned gCddvProdCount = 0;
unsigned gCddvEventsSent = 0;  // verbose-logging stage counter, reset per SYNC_REQUEST
// gCddvRangeStart/End, gCddvMultistatusAccum and gCddvBlocksExtracted stay declared whatever WIRSA_FEATURE_CALDAV_REPORT is (a few bytes each); only the REPORT-parsing code is conditional.
time_t gCddvRangeStart = 0, gCddvRangeEnd = 0;

#if WIRSA_FEATURE_CALDAV_REPORT

// What the multistatus reader cuts from a REPORT reply and where each block goes: <calendar-data> to the iCalendar parser (the default, set at the start of every sync) or
// <address-data> to the CardDAV contact sender (cddvOnAddressDataBlock, WIRSA_FEATURE_CARDDAV).
const char *gCddvDataTag = "calendar-data";
void (*gCddvOnBlock)(const String &block) = cddvOnCalendarDataBlock;

void cddvOnCalendarDataBlock(const String &block) {
  if (gSinkAborted || !gActiveSink) return;
  if (!parseICalAndEmit(block, (uICAL::seconds_t) gCddvRangeStart, (uICAL::seconds_t) gCddvRangeEnd, *gActiveSink)) {
    gSinkAborted = true;
  }
}
#endif  // WIRSA_FEATURE_CALDAV_REPORT

// Streaming state for the CalDAV/iCloud/Google REPORT path: an accumulator holding only one in-flight partial <calendar-data> block (plus XML filler), not the whole
// tens-of-KB chunked response. Reset per SYNC_REQUEST, like gActiveSink and gCddvRangeStart.
String gCddvMultistatusAccum;
unsigned gCddvBlocksExtracted = 0;

// Incremental block scanner (substring scanning, CDATA and entity unescaping; it matches the literal "calendar-data", namespace-prefix-agnostic and safe in this response
// shape). Scans gCddvMultistatusAccum from the start, calls cddvOnCalendarDataBlock() for every COMPLETE block and trims the accumulator to the unconsumed tail. "No closing tag yet"
// is the normal state while chunks still arrive, so it waits for the next chunk without logging an error.
#if WIRSA_FEATURE_CALDAV_REPORT
void extractCalendarDataBlocksIncremental() {
  int searchFrom = 0;
  int consumedUpTo = 0;
  for (;;) {
    int tagLen = (int) strlen(gCddvDataTag);
    int tagPos = gCddvMultistatusAccum.indexOf(gCddvDataTag, searchFrom);
    if (tagPos < 0) break;
    int ltPos = gCddvMultistatusAccum.lastIndexOf('<', tagPos);
    if (ltPos < 0) { searchFrom = tagPos + tagLen; continue; }
    int gtPos = gCddvMultistatusAccum.indexOf('>', tagPos);
    if (gtPos < 0) break;  // opening tag not fully arrived yet - wait for more bytes
    bool selfClosing = (gtPos > 0 && gCddvMultistatusAccum[gtPos - 1] == '/');
    if (selfClosing) {
      searchFrom = gtPos + 1;
      consumedUpTo = gtPos + 1;
      continue;
    }
    int contentStart = gtPos + 1;
    int closeTagPos = gCddvMultistatusAccum.indexOf(gCddvDataTag, contentStart);
    if (closeTagPos < 0) break;  // closing tag not fully arrived yet - wait for more bytes
    int closeGt = gCddvMultistatusAccum.indexOf('>', closeTagPos);
    if (closeGt < 0) break;

    int closeLt = gCddvMultistatusAccum.lastIndexOf("</", closeTagPos);
    if (closeLt < contentStart) closeLt = closeTagPos;

    // The block's text: inside a CDATA section, or the raw (escaped) XML.
    int from = contentStart, to = closeLt;
    bool cdata = false;
    int cdataStart = gCddvMultistatusAccum.indexOf("<![CDATA[", contentStart);
    if (cdataStart >= 0 && cdataStart < closeLt) {
      int cdataEnd = gCddvMultistatusAccum.indexOf("]]>", cdataStart);
      from = cdataStart + 9;
      to = (cdataEnd >= 0 && cdataEnd < closeLt) ? cdataEnd : closeLt;
      cdata = true;
    }
    if (to < from) to = from;

    // Copied out with checked allocations, not substring() and assignment: when the heap cannot satisfy substring() it returns an invalid String (null buffer) and
    // String::move() then memmoves from address 0 into the target's small buffer, a LoadProhibited crash seen late in a long Sync All. Out of memory now ends the parse with an error the Palm gets as ERR.
    String block;
    if (!block.reserve((unsigned) (to - from)) ||
        !block.concat((const uint8_t *) gCddvMultistatusAccum.c_str() + from, (unsigned) (to - from))) {
      gSinkAborted = true;
      LOGW("CalDAV: [parse] out of memory copying a calendar-data block (%d bytes, heap %u, max alloc %u)\r\n",
                            to - from, (unsigned) ESP.getFreeHeap(), (unsigned) ESP.getMaxAllocHeap());
      break;
    }
    gCddvBlocksExtracted++;
    if (cdata) {
      gCddvOnBlock(block);
    } else {
      String unescaped = xmlUnescape(block);   // move-constructed: safe even if invalid
      gCddvOnBlock(unescaped);
    }
    searchFrom = closeGt + 1;
    consumedUpTo = closeGt + 1;
    if (gSinkAborted) break;
    yield();
  }
  if (consumedUpTo > 0) gCddvMultistatusAccum.remove(0, consumedUpTo);
}

// The chunk callback of the REPORT path's onBodyChunk: a binary-safe, checked append (String::concat's bool return) and then an incremental extraction pass.
bool cddvOnMultistatusChunk(const char *data, size_t len) {
  if (gSinkAborted) return false;
  if (!gCddvMultistatusAccum.concat((const uint8_t *) data, (unsigned int) len)) {
    gSinkAborted = true;
    LOGW("CalDAV: [parse] out of memory accumulating response chunk");
    return false;
  }
  extractCalendarDataBlocksIncremental();
  return !gSinkAborted;
}
#endif  // WIRSA_FEATURE_CALDAV_REPORT

// ICS Feed: a true incremental parse that never holds the whole feed, with the same small-accumulator, extract-and-discard design as the multistatus path, scanning plain
// BEGIN:VEVENT/BEGIN:VTIMEZONE ... END: boundaries (a raw ICS has no XML wrapper).
// Why: the earlier design buffered the whole feed in one Arduino String and hit an unconditional ~64 KB ceiling on boards without BOARD_HAS_PSRAM (WString.h CAPACITY_MAX: 65535
// without PSRAM, 3 MB with; capacity() is protected so it cannot be probed). A real Google Calendar export failed the first 64 KB growth step with 100 KB+ heap free: nothing to do
// with the heap, so no cap raise or defragmentation could help.
// Unlike the multistatus <calendar-data> blocks (each a complete one-event VCALENDAR), one ICS feed's VEVENTs share a VTIMEZONE/tzmap and can reference each other by RECURRENCE-ID,
// so it is two-phase: phase 1 (streamed here) parses each VTIMEZONE/VEVENT, bounded by one component (typically under 1 KB), into the masters/overrides maps parseICalAndEmit also builds;
// phase 2 (cddvIcsFinishParse, once after the feed) expands the occurrences from those maps as parseICalAndEmit's tail does. The code is deliberately duplicated, not shared with
// parseICalAndEmit: that is the CalDAV/iCloud path's hardware-confirmed implementation, and refactoring it risks that path.

}  // namespace CalDavRelay
