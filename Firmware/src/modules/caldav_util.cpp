// caldav_util.cpp - text helpers, URL joining, the kept TLS connection. Part of the CalDAV Relay mode; include/caldav_internal.h says how the files divide the work.
#include "caldav_internal.h"

namespace CalDavRelay {

// Static, not stack, scratch: the relay does one sync and one frame at a time, so module-level reuse is safe and keeps a multi-KB buffer off the loop task's stack (as bitwarden_relay.cpp's gFrameBody).
uint8_t gFrameBody[CDDV_MAX_BODY];
uint8_t gAckScratch[CDDV_MAX_BODY];
uint8_t gEventBody[CDDV_MAX_BODY];

bool cddvWifiReady = false;

// Forward declaration: the definition is with the OLED helpers (cddvDrawHeader/cddvDrawStatus) further down, but cddvHttpRequestOnce, far above, calls it to move the status through
// Connecting.../Syncing.../Completed!.

// Whether the last sync succeeded, so caldavRelayLoop knows if "Completed!" is honest before reverting to "Waiting..." (a failed sync goes straight to Waiting). Reset at the top of
// cddvHandleSyncRequest, set true only at its real success exit (just before CDDV_SYNC_DONE is sent).
bool gCddvLastSyncOk = false;

// Set when CDDV_HELLO negotiates a faster rate; caldavRelayLoop's tick compares it with RelayTiming::NEGOTIATE_TIMEOUT_MS to catch "negotiated but no SYNC_REQUEST followed" and revert before it is
// stuck listening at an unexpected rate.
unsigned long gCddvNegotiatedAt = 0;

// ---- TLS keep-alive between SYNC_REQUESTs ----
// "Sync All" sends one SYNC_REQUEST per calendar or address book, usually to one host, and each did a fresh DNS, TCP and TLS handshake. On boards with PSRAM (WIRSA_CDDV_KEEPALIVE) the REPORT path
// keeps its TLS connection for the next request: one persistent WiFiClientSecure and HTTPClient (the HTTPClient destructor would close the socket, so it must outlive each call). Closed on any failure,
// after 20 s idle, on a different host, on the Palm's BYE and when the mode exits; nothing is kept past the session.
static WiFiClientSecure *gKeepTls = nullptr;
HTTPClient *gKeepHttp = nullptr;
static String gKeepHost;
static uint16_t gKeepPort = 0;
unsigned long gKeepLastUsed = 0;
static const unsigned long kKeepIdleMs = 20000;

void cddvKeepClose() {
  if (gKeepTls) gKeepTls->stop();
  delete gKeepHttp; gKeepHttp = nullptr;
  delete gKeepTls; gKeepTls = nullptr;
  gKeepHost = String(); gKeepPort = 0;
}

// The kept client for host:port, ready for HTTPClient::begin(): reused if still open, recently used and for the same host, else stopped (the next connect is a fresh handshake).
WiFiClientSecure *cddvKeepClient(const String &host, uint16_t port, bool &reused) {
  if (!gKeepTls) gKeepTls = new WiFiClientSecure();
  if (!gKeepHttp) gKeepHttp = new HTTPClient();
  bool same = gKeepHost == host && gKeepPort == port && gKeepTls->connected() &&
              (millis() - gKeepLastUsed) < kKeepIdleMs;
  if (!same) gKeepTls->stop();
  reused = same;
  gKeepHost = host; gKeepPort = port;
  return gKeepTls;
}

// Reopens PhysicalSerial at RelayTiming::BAUD_IDLE and redraws the header so the live baud readout (cddvDrawHeader's liveBaudStr) follows at once. Shared by the negotiation-timeout check and
// the post-sync cleanup in caldavRelayLoop, so the sequence is right in one place.
void cddvRevertToIdleBaud() {
  cddvKeepClose();
  PhysicalSerial.begin(RelayTiming::BAUD_IDLE, WIRSA_SERIAL_FORMAT, RXD2, TXD2);
  cddvDrawHeader();
}

// ---- Clock ----
// "Now" for the sync window comes from the Palm's UTC estimate (SYNC_REQUEST's trailing u32, see cddvHandleSyncRequest), not this module's own NTP fetch: depending on NTP succeeding at every sync
// risked a silent fallback to a near-1970 clock and an empty window with no error. No NTP dependency here.

// ---- Small string helpers ----

// server + path -> one full URL. "server" already has a scheme for CalDAV-generic (CalDAVSync.h "CalDAV: server URL") and IS the whole URL for an ICS feed (path unused). Handles the one slash
// between them so neither a trailing slash on server nor a leading one on path gives "//". Called only from the generic-CalDAV/iCloud branch (the ICS path uses its feedUrl unjoined).
#if WIRSA_FEATURE_CALDAV_REPORT
String joinUrl(const String &server, const String &path) {
  if (path.length() == 0) return server;
  bool serverHasSlash = server.endsWith("/");
  bool pathHasSlash = path.startsWith("/");
  if (serverHasSlash && pathHasSlash) return server + path.substring(1);
  if (!serverHasSlash && !pathHasSlash) return server + "/" + path;
  return server + path;
}

// iCloud CalDAV endpoints are always https://{shard}/{uuid}/calendars/{calendarName}/ (CalDAVSync.h's CDDVSlot comment: "iCloud: shard hostname" for server, "iCloud: calendar name" for path, plus the
// uuid field). The user looks up their shard and UUID once (Apple's CalDAV principal discovery) and enters all three; this build does not automate that. shard is entered WITHOUT a scheme (unlike
// CalDAV-generic's server): https:// is prepended defensively in case the user included one.
String joinUrlICloud(const String &shard, const String &uuid, const String &calName) {
  String host = shard;
  if (!host.startsWith("https://") && !host.startsWith("http://")) host = "https://" + host;
  if (!host.endsWith("/")) host += "/";

  String u = uuid;
  while (u.startsWith("/")) u = u.substring(1);
  while (u.endsWith("/")) u = u.substring(0, u.length() - 1);

  String c = calName;
  while (c.startsWith("/")) c = c.substring(1);
  while (c.endsWith("/")) c = c.substring(0, c.length() - 1);

  return host + u + "/calendars/" + c + "/";
}
#endif  // WIRSA_FEATURE_CALDAV_REPORT

void formatUtcBasic(time_t t, char *out, size_t outCap) {
  struct tm tmv;
  gmtime_r(&t, &tmv);
  strftime(out, outCap, "%Y%m%dT%H%M%SZ", &tmv);
}

// Un-escapes the few XML entities that can appear in a <calendar-data> element's text (servers vary on escaping versus CDATA); callers use it only on the non-CDATA path.
String xmlUnescape(const String &in) {
  String out;
  out.reserve(in.length());
  for (size_t i = 0; i < in.length(); i++) {
    if (in[i] == '&') {
      if (in.startsWith("&lt;", i)) { out += '<'; i += 3; continue; }
      if (in.startsWith("&gt;", i)) { out += '>'; i += 3; continue; }
      if (in.startsWith("&amp;", i)) { out += '&'; i += 4; continue; }
      if (in.startsWith("&quot;", i)) { out += '"'; i += 5; continue; }
      if (in.startsWith("&apos;", i)) { out += '\''; i += 5; continue; }
      if (in.startsWith("&#13;", i)) { out += '\r'; i += 4; continue; }
      if (in.startsWith("&#10;", i)) { out += '\n'; i += 4; continue; }
    }
    out += in[i];
  }
  return out;
}

// uICAL does not implement RFC 5545 3.3.11's TEXT escaping: a VLine's value (and VEvent::summary, assigned from it) is the raw bytes off the wire. A CalDAV server routinely escapes a literal
// backslash, semicolon, comma and newline in a TEXT property (SUMMARY/DESCRIPTION/LOCATION) as \\, \;, \, and \n/\N; this restores exactly those 4 defined sequences. Any other backslash
// sequence is left alone (not a defined escape: better a stray visible backslash than a silently eaten character).
// Shared by generic CalDAV/iCloud (parseICalAndEmit) and ICS Feed (cddvIcsOnComponent and friends). Google (server-side expansion, no iCal text of its own) and the code shared with it do not need
// these, so they are dropped if BOTH types that need them are disabled.
#if WIRSA_FEATURE_CALDAV_REPORT || WIRSA_FEATURE_CALDAV_ICS
String icalUnescapeText(const String &in) {
  String out;
  out.reserve(in.length());
  for (size_t i = 0; i < in.length(); i++) {
    if (in[i] == '\\' && i + 1 < in.length()) {
      char n = in[i + 1];
      if (n == '\\') { out += '\\'; i++; continue; }
      if (n == ';')  { out += ';';  i++; continue; }
      if (n == ',')  { out += ',';  i++; continue; }
      if (n == 'n' || n == 'N') { out += '\n'; i++; continue; }
    }
    out += in[i];
  }
  return out;
}
#endif  // WIRSA_FEATURE_CALDAV_REPORT || WIRSA_FEATURE_CALDAV_ICS

// Masks an account-identifying value (AppleID/email, DSID, calendar UUID) for debug logs, so a log can be shared without redacting. A short prefix and the total length tell two values apart across
// log lines (e.g. retries on one account) without exposing the value. Logging only; requests use the real value.
String maskSensitive(const String &s) {
  if (s.length() == 0) return "(empty)";
  if (s.length() <= 4) return "****";
  return s.substring(0, 4) + "..." + String((unsigned) s.length()) + "ch";
}

// Masks any "/"-delimited URL path segment that looks like an opaque identifier (all hex or all digits, 8+ chars, e.g. iCloud's DSID or calendar UUID in the URL) before logging, so the
// [connect]/[fetch] lines do not leak them although this HTTP layer does not know which server type's URLs carry sensitive segments. The host and short or word segments ("calendars", scheme) stay readable.
bool cddvLooksLikeOpaqueId(const String &seg) {
  if (seg.length() < 8) return false;
  bool allHex = true, allDigit = true;
  for (unsigned i = 0; i < seg.length(); i++) {
    char c = seg[i];
    bool isDigit = (c >= '0' && c <= '9');
    bool isHex = isDigit || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
    if (!isHex) allHex = false;
    if (!isDigit) allDigit = false;
  }
  return allHex || allDigit;
}

String maskUrlForLog(const String &url) {
  String out;
  int start = 0;
  for (;;) {
    int slash = url.indexOf('/', start);
    int end = (slash < 0) ? (int) url.length() : slash;
    String seg = url.substring(start, end);
    out += cddvLooksLikeOpaqueId(seg) ? maskSensitive(seg) : seg;
    if (slash < 0) break;
    out += '/';
    start = slash + 1;
  }
  return out;
}

// extractCalendarDataBlocksIncremental (the streaming sibling of the original one-shot scanner: the same substring scanning and CDATA/entity unescaping, fed from a small accumulator that grows and is
// trimmed) is further down, beside the multistatus streaming state it works on (gCddvMultistatusAccum) and its callback (cddvOnCalendarDataBlock).

// ---- iCal parsing and occurrence expansion (uICAL) ----
// See uICAL/vobject.h's header comment (added for this relay) for why Calendar::load() cannot be used: its useLine filter silently discards UID/LOCATION/STATUS/EXDATE/RECURRENCE-ID before VObject
// sees them. This mirrors Calendar::load()'s structure (calendar.cpp) with a widened filter and its own VEVENT loop instead of building a Calendar.


#if WIRSA_FEATURE_CALDAV_REPORT || WIRSA_FEATURE_CALDAV_ICS
// Splits a comma-separated EXDATE (or similar) value: RFC 5545 allows one EXDATE line to list several exclusion dates.
void splitCommaList(const String &value, std::vector<String> &out) {
  int start = 0;
  for (;;) {
    int comma = value.indexOf(',', start);
    if (comma < 0) { out.push_back(value.substring(start)); break; }
    out.push_back(value.substring(start, comma));
    start = comma + 1;
  }
}
#endif  // WIRSA_FEATURE_CALDAV_REPORT || WIRSA_FEATURE_CALDAV_ICS

// Finds one "KEY=value" part of a raw RRULE string (FREQ, COUNT, UNTIL, INTERVAL) without uICAL's RRule, whose freq/count/until/interval are protected (rrule.h). Used by the ICS Feed's early drop,
// which needs them BEFORE deciding whether a recurring master is worth keeping.
#if WIRSA_FEATURE_CALDAV_ICS
bool cddvRruleFindParam(const String &rrule, const char *key, String &outValue) {
  String needle = String(key) + "=";
  int pos = rrule.indexOf(needle);
  if (pos < 0) return false;
  int valStart = pos + needle.length();
  int semiPos = rrule.indexOf(';', valStart);
  outValue = (semiPos >= 0) ? rrule.substring(valStart, semiPos) : rrule.substring(valStart);
  return true;
}
#endif  // WIRSA_FEATURE_CALDAV_ICS

}  // namespace CalDavRelay
