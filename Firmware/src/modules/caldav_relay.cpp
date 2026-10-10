// CalDAV Relay mode - see include/caldav_relay.h for the architecture
// summary. Implements CalDAV-generic, iCloud, ICS Feed, and Google
// Calendar (via a one-time OAuth handshake - see the Google OAuth section
// below).
//
// Wire protocol constants below MUST match apps/caldav_sync/Serial.h and
// apps/caldav_sync/CalDAVSync.h exactly (hackmaster_y2035 repo) - both
// sides were written against the same byte layout, not a shared header,
// so a drift here is a silent wire-incompatibility, not a compile error.
//
// Like bitwarden_relay.cpp, every WiFiClientSecure/HTTPClient call here
// was written against the real ESP32 Arduino core API (no local ESP32
// toolchain in this environment to compile-test against) - kept as close
// as possible to bitwarden_relay.cpp's own already-proven usage of the
// same classes (setInsecure()/connect()/HTTPClient::begin(client, url))
// rather than introducing new, unverified API surface.

#include "caldav_relay.h"
#include "globals.h"
#include "serial_io.h"
#include "display_menu.h"
#include "network.h"
#include "relay_proto.h"
#include <WiFi.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <WebServer.h>  // transient local listener for the Google OAuth token handoff
#include <ArduinoJson.h>  // Google token/Calendar API JSON, and the OAuth handoff POST body
#include <mbedtls/base64.h>  // cddvRawTlsDiagnostic's own Basic Auth header - same real API bitwarden_relay.cpp's bwBase64Encode already uses, not the higher-level base64:: Arduino wrapper
#include <time.h>
#include <stdarg.h>  // cddvLog's own varargs

// Real flash-budget report: with all four CalDAV server types built in,
// this one file alone is ~3000 lines and pushed a real build over its
// partition's size limit. Each of these independently disables one
// server type's own code (set from the user's own platformio.ini,
// gitignored/machine-specific, not touched here - e.g.
// -DWIRSA_FEATURE_CALDAV_GOOGLE=0) while leaving the other three intact.
// A disabled type's CDDV_TYPE_* still exists on the wire (so a saved Palm
// slot of that type doesn't corrupt anything) but SYNC_REQUEST for it
// returns a clear "not enabled in this build" SYNC_ERROR instead of
// silently failing. Generic CalDAV and iCloud can't be split from each
// other - both drive the exact same REPORT/multistatus XML machinery,
// differing only in URL shape and auth - so there's one combined flag for
// both, not two.
#ifndef WIRSA_FEATURE_CALDAV_REPORT
#define WIRSA_FEATURE_CALDAV_REPORT 1  // generic CalDAV + iCloud (shared REPORT/multistatus code)
#endif
#ifndef WIRSA_FEATURE_CALDAV_ICS
#define WIRSA_FEATURE_CALDAV_ICS 1     // plain ICS Feed (e.g. Google's "secret address in iCal format")
#endif
#ifndef WIRSA_FEATURE_CALDAV_GOOGLE
#define WIRSA_FEATURE_CALDAV_GOOGLE 1  // Google Calendar API via OAuth sign-in
#endif

// uICAL (RRULE/timezone/occurrence expansion) is needed by generic
// CalDAV/iCloud (parseICalAndEmit) and ICS Feed (cddvIcsOnComponent and
// friends) - NOT by Google, which gets server-side occurrence expansion
// from the Calendar API itself (singleEvents=true) and never touches
// uICAL at all. If both of the types that need it are disabled, this
// drops the #include entirely - PlatformIO's own dependency scanner finds
// libraries by which headers a translation unit includes, so this can
// also keep the whole uICAL library out of the build, not just this
// file's own usage of it.
#if WIRSA_FEATURE_CALDAV_REPORT || WIRSA_FEATURE_CALDAV_ICS
#include "uICAL/cppstl.h"
#include "uICAL/types.h"
#include "uICAL/error.h"
#include "uICAL/util.h"
#include "uICAL/tzmap.h"
#include "uICAL/vevent.h"
#include "uICAL/veventiter.h"
#include "uICAL/calendarentry.h"  // CalendarEntry_ptr::start()/end() need the full definition, not just VEventIter's own forward-declare
#include "uICAL/vobject.h"
#include "uICAL/vobjectstream.h"
#include "uICAL/vline.h"
#include "uICAL/vlinestream.h"
#include "uICAL/datetime.h"
#include "uICAL/rrule.h"
#endif

extern bool usbDebug;
extern HardwareSerial PhysicalSerial;

// Real flash-budget report: this file alone carries ~95 usbDebug-gated log
// call sites, and usbDebug is a RUNTIME bool - every one of those format
// string literals (plus the vsnprintf/Serial.printf call overhead at each
// site) is unconditionally compiled into the firmware regardless of
// whether usbDebug is ever true, since the compiler can't know a runtime
// variable's value at compile time. WIRSA_DEBUG_LOGGING=0 (set from the
// user's own platformio.ini, gitignored/machine-specific, not touched
// here) turns cddvLog/cddvLogLn into no-op MACROS instead of functions -
// the preprocessor then discards every call site's string-literal
// arguments before the compiler ever sees them, at zero cost to the
// existing `if (usbDebug) cddvLog(...)` call sites (they don't need
// touching - `if (usbDebug) ((void)0);` is valid and optimizes away).
// Leave it at the default (1) and behavior is completely unchanged.
#ifndef WIRSA_DEBUG_LOGGING
#define WIRSA_DEBUG_LOGGING 1
#endif

#if WIRSA_DEBUG_LOGGING
// Every debug log line in this file goes through here - a sync involves
// multiple retries/reconnects/WiFi events, and without a timestamp there's
// no way to tell "this happened right after that" from "30 seconds later"
// just by reading log order. Drop-in replacement for Serial.printf/println:
// checks usbDebug internally (existing call sites' own `if (usbDebug)`
// guards are redundant but harmless to leave in place).
static void cddvLog(const char *fmt, ...) {
  if (!usbDebug) return;
  char buf[320];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  Serial.printf("[%8lu] %s", (unsigned long) millis(), buf);
}
static void cddvLogLn(const char *msg) {
  cddvLog("%s\r\n", msg);
}
#else
#define cddvLog(...) ((void) 0)
#define cddvLogLn(...) ((void) 0)
#endif

// ============================================================================
// Wire protocol constants (apps/caldav_sync/Serial.h)
// ============================================================================

enum : uint8_t {
  CDDV_HELLO = 0x01, CDDV_HELLO_ACK = 0x02,
  CDDV_SYNC_REQUEST = 0x03, CDDV_EVENT_ITEM = 0x04, CDDV_ITEM_ACK = 0x05,
  CDDV_SYNC_DONE = 0x06, CDDV_SYNC_ERROR = 0x07,
  CDDV_OAUTH_START = 0x08, CDDV_OAUTH_INFO = 0x09, CDDV_OAUTH_TOKEN = 0x0A,
  CDDV_BYE = 0x0B
};

// apps/caldav_sync/CalDAVSync.h's CDDVServerType
enum : uint8_t {
  CDDV_TYPE_CALDAV = 0, CDDV_TYPE_ICLOUD = 1, CDDV_TYPE_GOOGLE = 2, CDDV_TYPE_ICSFEED = 3
};

static const size_t CDDV_MAX_BODY = 3192;  // apps/caldav_sync/Serial.h cddvMaxFrameBody - must match exactly
static const int    CDDV_ACK_RETRIES = 5;
static const uint32_t CDDV_ACK_RETRY_DELAY_MS = 500;
static const uint32_t CDDV_ACK_TIMEOUT_MS = 10000;
static const uint32_t CDDV_BYTE_TIMEOUT_MS = 3000;

// Baud negotiation, mirrors WiRSa's own SASD mode (sasd.cpp's
// SASD_CMD_SET_BAUD). Every fresh connection - both this relay's own
// PhysicalSerial and apps/caldav_sync/Serial.h's cddvBaudIdle on the Palm
// side - starts at this fixed, known-safe rate, since a rate can't be
// proposed reliably over a link where the two sides might already differ.
// CDDV_HELLO/CDDV_HELLO_ACK negotiate up from here before SYNC_REQUEST.
static const uint32_t CDDV_BAUD_IDLE = 9600;
// If the Palm negotiates a faster rate but never follows up with
// SYNC_REQUEST within this window, revert to CDDV_BAUD_IDLE so the next
// connection attempt isn't stuck listening at an unexpected rate.
static const uint32_t CDDV_NEGOTIATE_TIMEOUT_MS = 15000;

// past: None/1wk/2wk/1mo/2mo/3mo (cddvPastWindow); future: 2wk/1mo/3mo/6mo/1yr
// (cddvFutureWindow) - see apps/caldav_sync/CalDAVSync.h. Approximated to
// 30-day months, matching the "rough sync boundary" nature of these
// settings on the Palm side (they were never meant to be exact calendar
// months).
static const uint16_t kPastWindowDays[]   = {0, 7, 14, 30, 60, 90};
static const uint16_t kFutureWindowDays[] = {14, 30, 90, 180, 365};
static const int kPastWindowCount = sizeof(kPastWindowDays) / sizeof(kPastWindowDays[0]);
static const int kFutureWindowCount = sizeof(kFutureWindowDays) / sizeof(kFutureWindowDays[0]);

// Palm epoch (1904-01-01) is this many seconds before the Unix epoch
// (1970-01-01) - every uICAL seconds_t value is Unix-epoch, every
// CDDV_EVENT_ITEM utcStart/utcEnd on the wire is Palm-epoch (matches
// every other Palm OS time API, e.g. TimGetSeconds()/TimUTCToTimeZone()
// on the Palm side).
static const uint32_t UNIX_TO_PALM_EPOCH = 2082844800UL;

// Static (not stack) scratch - this relay processes one sync/one frame at
// a time, never concurrently, so module-level reuse is safe and avoids a
// multi-KB buffer on the loop task's stack (this project's own established
// caution - see bitwarden_relay.cpp's identical reasoning for gFrameBody).
static uint8_t gFrameBody[CDDV_MAX_BODY];
static uint8_t gAckScratch[CDDV_MAX_BODY];
static uint8_t gEventBody[CDDV_MAX_BODY];

static bool cddvWifiReady = false;

// Forward decl - real definition lives with the other OLED helpers further
// down (cddvDrawHeader/cddvDrawStatus), but cddvHttpRequestOnce (well above
// that point in the file) needs to call it to flip the on-device status
// through Connecting.../Syncing.../Completed! transitions as a sync progresses.
static void cddvDrawStatus(const char *state);
static void cddvDrawHeader();  // same forward-decl need as cddvDrawStatus above - see cddvRevertToIdleBaud below

// Tracks whether the most recently completed sync actually succeeded, so
// caldavRelayLoop can decide whether "Completed!" is honest to show before
// reverting to "Waiting..." - a failed sync goes straight back to Waiting
// instead. Reset at the top of cddvHandleSyncRequest, set true only at its
// real success exit (right before CDDV_SYNC_DONE is sent).
static bool gCddvLastSyncOk = false;

// Set when CDDV_HELLO successfully negotiates a faster rate;
// caldavRelayLoop's own tick checks this against CDDV_NEGOTIATE_TIMEOUT_MS
// to catch "negotiated but the Palm never followed up with SYNC_REQUEST"
// and revert before it's stuck listening at an unexpected rate.
static unsigned long gCddvNegotiatedAt = 0;

// Reopens PhysicalSerial at CDDV_BAUD_IDLE and redraws the header so the
// title bar's live baud readout (cddvDrawHeader's own liveBaudStr call)
// reflects it immediately - shared by the negotiation-timeout check and
// the post-sync cleanup in caldavRelayLoop, so there's exactly one place
// this sequence needs to be correct.
static void cddvRevertToIdleBaud() {
  PhysicalSerial.begin(CDDV_BAUD_IDLE, WIRSA_SERIAL_FORMAT, RXD2, TXD2);
  cddvDrawHeader();
}

// ============================================================================
// Clock - "now" for the sync window comes from the Palm's own current UTC
// estimate (SYNC_REQUEST's trailing u32 - see cddvHandleSyncRequest's own
// use of it), not this module's own NTP fetch: NTP depending on the local
// network succeeding on every sync risked a silent fallback to a near-1970
// clock and a garbage (empty, no error) window. No NTP dependency in this file.
// ============================================================================

// ============================================================================
// Small string helpers
// ============================================================================

// server + path -> one full URL. "server" already includes a scheme for
// CalDAV-generic (see CalDAVSync.h: "CalDAV: server URL") and IS the whole
// URL for an ICS feed (path is unused there). Handles the one slash
// between them so neither a trailing slash on server nor a leading slash
// on path produces "//".
// Only actually called from the generic-CalDAV/iCloud dispatch branch
// below (the ICS Feed path uses its own feedUrl param directly, unjoined).
#if WIRSA_FEATURE_CALDAV_REPORT
static String joinUrl(const String &server, const String &path) {
  if (path.length() == 0) return server;
  bool serverHasSlash = server.endsWith("/");
  bool pathHasSlash = path.startsWith("/");
  if (serverHasSlash && pathHasSlash) return server + path.substring(1);
  if (!serverHasSlash && !pathHasSlash) return server + "/" + path;
  return server + path;
}

// iCloud CalDAV endpoints are always shaped
// https://{shard}/{uuid}/calendars/{calendarName}/ - CalDAVSync.h's own
// comment on CDDVSlot documents the split ("iCloud: shard hostname" for
// server, "iCloud: calendar name" for path, plus the separate uuid field)
// - the user looks up their own shard/UUID once (via Apple's CalDAV
// principal-discovery response) and enters all three directly; this build
// does not automate that discovery. shard is entered WITHOUT a scheme
// (unlike CalDAV-generic's server field, which already includes one) -
// prepend https:// defensively in case the user included one anyway.
static String joinUrlICloud(const String &shard, const String &uuid, const String &calName) {
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

static void formatUtcBasic(time_t t, char *out, size_t outCap) {
  struct tm tmv;
  gmtime_r(&t, &tmv);
  strftime(out, outCap, "%Y%m%dT%H%M%SZ", &tmv);
}

// Un-escapes the handful of XML entities that can legitimately appear
// inside a <calendar-data> element's text content (servers vary on
// whether they escape at all vs. wrap in CDATA - callers only run this on
// the non-CDATA path).
static String xmlUnescape(const String &in) {
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

// uICAL doesn't implement RFC 5545 SS3.3.11's TEXT-value escaping - a
// VLine's own `value` (and VEvent::summary, assigned straight from it) is
// exactly the raw bytes off the wire. A CalDAV server routinely escapes a
// literal backslash/semicolon/comma/newline inside a TEXT property
// (SUMMARY/DESCRIPTION/LOCATION) as \\, \;, \,, and \n/\N respectively -
// this un-escapes exactly those 4 defined sequences back to their real
// characters. Any other backslash sequence is left alone (not a defined
// escape - safer to leave a stray backslash visible than silently eat a
// character that wasn't meant to be one).
// Shared by generic CalDAV/iCloud (parseICalAndEmit) and ICS Feed
// (cddvIcsOnComponent and friends) - neither Google (server-side
// occurrence expansion, no iCal text of its own to unescape/split) nor
// any code shared with it needs these, so they're dropped entirely if
// BOTH of the types that do need them are disabled.
#if WIRSA_FEATURE_CALDAV_REPORT || WIRSA_FEATURE_CALDAV_ICS
static String icalUnescapeText(const String &in) {
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

// Masks an account-identifying value (AppleID/email, DSID, calendar UUID)
// for debug logs, so a log can be shared for troubleshooting without
// manually redacting these every time. Shows just enough (a short prefix +
// total length) to tell two
// different values apart across log lines - e.g. distinguishing retries
// against the same account - without exposing the real value. Logging
// only; every actual network request still uses the real, unmasked value.
static String maskSensitive(const String &s) {
  if (s.length() == 0) return "(empty)";
  if (s.length() <= 4) return "****";
  return s.substring(0, 4) + "..." + String((unsigned) s.length()) + "ch";
}

// Masks any "/"-delimited URL path segment that looks like an opaque
// identifier (all-hex or all-digit, 8+ chars - e.g. iCloud's DSID/calendar
// UUID embedded directly in the request URL) before logging it, so the
// [connect]/[fetch] log lines don't leak them even though this HTTP layer
// is generic and doesn't itself know which server type's URLs carry
// sensitive path segments. Host and short/word segments ("calendars",
// scheme, etc.) are left readable.
static bool cddvLooksLikeOpaqueId(const String &seg) {
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

static String maskUrlForLog(const String &url) {
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

// extractCalendarDataBlocksIncremental (this file's own streaming sibling
// of the original one-shot scanner - the same substring-scanning logic and
// the same CDATA/entity-unescaping rules, just fed from a small, growing-
// then-trimmed accumulator instead of one whole buffered response) lives
// further down, next to the module-level multistatus streaming state it
// operates on (gCddvMultistatusAccum) and the per-block callback it calls
// (cddvOnCalendarDataBlock) - see that section's own comment.

// ============================================================================
// iCal parsing + occurrence expansion (uICAL) - see uICAL/vobject.h's own
// header comment (added for this relay) for why Calendar::load() can't be
// used directly: its useLine filter silently discards UID/LOCATION/
// STATUS/EXDATE/RECURRENCE-ID before VObject ever sees them. This mirrors
// Calendar::load()'s own structure (calendar.cpp) with a widened filter
// and its own VEVENT-processing loop instead of building a Calendar.
// ============================================================================

struct CalDavMasterEvent {
  // Only populated (non-null) for a REPEATING master - a real hardware
  // memory-exhaustion report on a feed with many one-off events found that
  // the full uICAL VEvent/DateTime/TZ/RRule object graph (several separate
  // heap allocations each, one per master, held for the ENTIRE feed
  // transfer - see cddvIcsFinishParse's own two-phase design comment) was
  // the dominant cost, and almost all of it is dead weight for a
  // non-repeating event: its single occurrence is exactly its own
  // start/end, already computed once at parse time. For a non-repeating
  // master, cddvIcsOnComponent copies out just title/startEpoch/endEpoch/
  // allDay below and lets the uICAL object graph be freed immediately
  // (never stored here) instead of held until cddvIcsFinishParse runs at
  // the very end of the whole feed.
  uICAL::VEvent_ptr vevent;
  String title;              // only used when !isRepeating - see above
  uICAL::seconds_t startEpoch = 0;  // only used when !isRepeating
  uICAL::seconds_t endEpoch = 0;    // only used when !isRepeating
  bool allDay = false;              // only used when !isRepeating
  String location;
  String description;
  bool cancelled;
  bool isRepeating;  // RRULE property was present on this VEVENT - NOT
                      // vevent->rrule's own nullability (VEvent always
                      // constructs an RRule, even a no-op one, for a
                      // non-recurring event - see vevent.cpp).
};
struct CalDavOverrideEvent {
  uICAL::seconds_t recurrenceIdEpoch;
  // Shared with parseICalAndEmit (CalDAV/iCloud path) - that function's own
  // masters/overrides are local, per-call variables (one already-complete
  // single-event <calendar-data> block at a time, discarded once emitted),
  // so it still always populates and reads this field directly, unchanged.
  // The ICS Feed path (cddvIcsOnComponent/cddvIcsFinishParse below) is the
  // one that persists gCddvIcsOverrides for the WHOLE feed transfer - same
  // real-hardware memory story as CalDavMasterEvent's own title/startEpoch/
  // endEpoch/allDay fields, added for the identical reason: an override is
  // only ever used as a lookup-by-date to override a handful of fields, it
  // never needs VEventIter/RRule/TZ, so the ICS path leaves this unset and
  // uses the lightweight fields below instead, freeing the full object
  // graph immediately rather than holding it for the rest of the feed.
  uICAL::VEvent_ptr vevent;
  String title;                     // ICS path only - see above
  uICAL::seconds_t startEpoch = 0;  // ICS path only
  uICAL::seconds_t endEpoch = 0;    // ICS path only
  bool allDay = false;              // ICS path only
  String location;
  String description;
  bool cancelled;
};

// Splits a comma-separated EXDATE (or similar) value into its parts -
// RFC 5545 allows one EXDATE line to list multiple exclusion dates.
static void splitCommaList(const String &value, std::vector<String> &out) {
  int start = 0;
  for (;;) {
    int comma = value.indexOf(',', start);
    if (comma < 0) { out.push_back(value.substring(start)); break; }
    out.push_back(value.substring(start, comma));
    start = comma + 1;
  }
}
#endif  // WIRSA_FEATURE_CALDAV_REPORT || WIRSA_FEATURE_CALDAV_ICS

// Finds one "KEY=value" component of a raw RRULE string (e.g. FREQ, COUNT,
// UNTIL, INTERVAL) without needing uICAL's own RRule class, whose freq/
// count/until/interval members are all protected (not accessible outside
// RRule/RRuleIter/ByWeekDayCounter - see rrule.h). Used by the ICS Feed's
// early-drop logic, which needs to read these BEFORE deciding whether a
// recurring master is even worth the cost of keeping.
#if WIRSA_FEATURE_CALDAV_ICS
static bool cddvRruleFindParam(const String &rrule, const char *key, String &outValue) {
  String needle = String(key) + "=";
  int pos = rrule.indexOf(needle);
  if (pos < 0) return false;
  int valStart = pos + needle.length();
  int semiPos = rrule.indexOf(';', valStart);
  outValue = (semiPos >= 0) ? rrule.substring(valStart, semiPos) : rrule.substring(valStart);
  return true;
}
#endif  // WIRSA_FEATURE_CALDAV_ICS

// Always defined regardless of which CalDAV server types are built in -
// every type's own fetch function (parseICalAndEmit, cddvFetchIcsFeed,
// cddvFetchGoogleCalendar) shares this exact sink signature.
typedef std::function<bool(const String &uid, const String &title, const String &location,
                            uint32_t utcStartPalm, uint32_t utcEndPalm, bool allDay,
                            const String &description, bool isRepeating)> CalDavEventSink;

#if WIRSA_FEATURE_CALDAV_REPORT
// Parses one VCALENDAR block of raw iCal text (either a whole ICS feed,
// or one <calendar-data> block from a multistatus REPORT response) and
// calls sink() for every occurrence within [rangeStartUnix, rangeEndUnix]
// (Unix epoch seconds), after applying EXDATE exclusions and RECURRENCE-ID
// overrides. Returns false only if sink() itself returned false (the
// caller aborted, e.g. an ack failed) - a malformed/empty calendar block
// is logged and treated as zero events, not an error (matches the Palm
// side's own "silently skip if unavailable" design for this whole
// feature).
static bool parseICalAndEmit(const String &icalText, uICAL::seconds_t rangeStartUnix,
                              uICAL::seconds_t rangeEndUnix, CalDavEventSink sink) {
  uICAL::istream_String icalStream(icalText);
  uICAL::VLineStream lines(icalStream);

  // Widened filter: everything Calendar::load()'s own useLine already
  // allows, PLUS UID/LOCATION/STATUS/EXDATE/RECURRENCE-ID for VEVENT.
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
    if (usbDebug) cddvLogLn("CalDAV: iCal parse error (root) - treating as empty");
    return true;
  }
  if (!root || root->getName() != "VCALENDAR") {
    if (usbDebug) cddvLogLn("CalDAV: iCal block did not start with VCALENDAR - skipped");
    return true;
  }

  uICAL::TZMap_ptr tzmap = uICAL::new_ptr<uICAL::TZMap>();
  std::map<String, CalDavMasterEvent> masters;
  std::map<String, std::vector<CalDavOverrideEvent>> overrides;

  for (;;) {
    // A calendar can have many VEVENTs; running the whole parse
    // synchronously with no yield point risks starving the WiFi driver's
    // own FreeRTOS task long enough to miss beacon/keepalive timing and
    // drop the AP association - a well-documented ESP32 failure class.
    // Mirrors bitwarden_relay.cpp's own yield() calls during its long
    // parsing work.
    yield();

    uICAL::VObject_ptr child;
    try {
      child = stm.nextObject(true);
    } catch (...) {
      break;  // stop at the first parse error - whatever was already collected still gets emitted
    }
    if (!child) break;

    if (child->getName() == "VTIMEZONE") {
      // tzmap->add(child) can throw on a malformed VTIMEZONE block - a
      // calendar spanning a full year can carry more than one VTIMEZONE
      // definition (DST transitions, multiple zones referenced by
      // different events), so guarding just the first one isn't enough.
      try {
        tzmap->add(child);
        // Logs the success case too, not just failures: uICAL::VObjectStream
        // reads components strictly in document order with no lookahead, so
        // a VEVENT referencing a TZID that's registered LATER in the
        // document (this line appearing after that VEVENT's own log line)
        // fails to resolve it even though VTIMEZONE registration itself
        // succeeded - distinguishing that ordering issue from any other cause.
        if (usbDebug) {
          uICAL::VLine_ptr tzIdLine = child->getPropertyByName("TZID");
          cddvLog("CalDAV: VTIMEZONE added: %s\r\n", tzIdLine ? tzIdLine->value.c_str() : "(no TZID)");
        }
      } catch (const uICAL::Error &e) {
        // uICAL's own exception types (error.h) carry a real message
        // string - surfaced here instead of collapsing every failure into
        // a generic "parse error".
        if (usbDebug) cddvLog("CalDAV: VTIMEZONE skipped (%s)\r\n", e.message.c_str());
      } catch (...) {
        if (usbDebug) cddvLogLn("CalDAV: VTIMEZONE skipped (non-uICAL parse error)");
      }
      continue;
    }
    if (child->getName() != "VEVENT") continue;

    // Wraps the whole per-VEVENT body in one outer try/catch rather than
    // guarding each individual uICAL call (getPropertyByName, DateTime
    // construction, etc.) - any exception anywhere while processing one
    // malformed VEVENT skips just that VEVENT, matching this function's
    // stated design ("logged and treated as zero events, not an error")
    // without depending on having found every possible throw site by inspection.
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
      // Same reasoning as the VTIMEZONE catch above - the actual uICAL
      // error message is what's needed to diagnose a skipped VEVENT,
      // not a guess.
      if (usbDebug) cddvLog("CalDAV: VEVENT skipped (%s)\r\n", e.message.c_str());
      continue;
    } catch (...) {
      if (usbDebug) cddvLogLn("CalDAV: VEVENT skipped (non-uICAL parse error)");
      continue;
    }

    if (recIdLine) {
      // A feed can have TZID-qualified RECURRENCE-ID values without always
      // shipping full VTIMEZONE coverage for that TZID - malformed-enough-
      // to-throw input that this function is supposed to log and skip, not
      // propagate as an uncaught exception.
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
        if (usbDebug) cddvLog("CalDAV: RECURRENCE-ID override skipped (%s)\r\n", e.message.c_str());
      } catch (...) {
        if (usbDebug) cddvLogLn("CalDAV: RECURRENCE-ID override skipped (non-uICAL parse error)");
      }
    } else {
      // EXDATE exclusions apply to the MASTER's own rrule - do this here,
      // once, rather than re-parsing on every occurrence.
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
            if (usbDebug) cddvLog("CalDAV: EXDATE value skipped (%s)\r\n", e.message.c_str());
          } catch (...) {
            if (usbDebug) cddvLogLn("CalDAV: EXDATE value skipped (non-uICAL parse error)");
          }
        }
      }
      CalDavMasterEvent m;
      m.vevent = vevent;
      m.location = location;
      m.description = description;
      m.cancelled = cancelled;
      m.isRepeating = isRepeating;
      // A UID can appear more than once if a feed is malformed - last
      // master wins, matching "most recently declared" rather than
      // silently dropping the event entirely.
      masters[uid] = m;
    }
    } catch (const uICAL::Error &e) {
      if (usbDebug) cddvLog("CalDAV: VEVENT entirely skipped (%s)\r\n", e.message.c_str());
      continue;
    } catch (...) {
      if (usbDebug) cddvLogLn("CalDAV: VEVENT entirely skipped (non-uICAL parse error)");
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

    // A malformed RRULE (bad UNTIL, etc.) or a DTSTART whose TZID has no
    // matching VTIMEZONE block can throw during expansion (VEventIter
    // construction, iter.next()/now()/entry()->end()). One bad master
    // series shouldn't take down the whole sync - skip it and keep going,
    // same as every other malformed-input case in this function.
    try {
      uICAL::VEventIter iter(m.vevent, rangeBegin, rangeEnd);
      while (iter.next()) {
        // Highest-iteration-count loop in this whole function for a real
        // recurring series (e.g. daily/weekly over a 180-day window) -
        // matches bitwarden_relay.cpp's own "once per completed item"
        // yield() cadence for exactly this reason (see this loop's own
        // outer comment above).
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

        // A recurring series can run far outside [rangeBegin, rangeEnd) -
        // VEventIter's own range is a hint, not a hard guarantee for every
        // uICAL version/pattern, so clamp here too rather than trust it
        // silently.
        if (startEpoch < (uICAL::seconds_t) rangeStartUnix || startEpoch > (uICAL::seconds_t) rangeEndUnix) continue;

        uint32_t utcStartPalm = (uint32_t) startEpoch + UNIX_TO_PALM_EPOCH;
        uint32_t utcEndPalm = (uint32_t) endEpoch + UNIX_TO_PALM_EPOCH;
        // Logs the real UTC date/time (not just the raw epoch) so an
        // event's computed start is directly checkable against the
        // [rangeStart, rangeEnd) window printed at [window].
        if (usbDebug) {
          char startBuf[24];
          formatUtcBasic((time_t) startEpoch, startBuf, sizeof(startBuf));
          cddvLog("CalDAV: [occurrence] uid=%s start=%s (epoch=%ld, allDay=%d)\r\n",
                        uid.c_str(), startBuf, (long) startEpoch, allDay ? 1 : 0);
        }
        // isRepeating reflects the MASTER's own RRULE, not the override's -
        // an overridden occurrence is still logically one instance of a
        // repeating series (that's what RECURRENCE-ID means), so the Palm
        // side's repeat icon should still show for it.
        if (!sink(uid, title, location, utcStartPalm, utcEndPalm, allDay, description, m.isRepeating)) return false;
      }
    } catch (const uICAL::Error &e) {
      if (usbDebug) cddvLog("CalDAV: master event skipped mid-expansion (%s)\r\n", e.message.c_str());
      continue;
    } catch (...) {
      if (usbDebug) cddvLogLn("CalDAV: master event skipped mid-expansion (non-uICAL parse error)");
      continue;
    }
  }

  // Known limitation (documented, not implemented this pass): an override
  // whose RECURRENCE-ID points at an occurrence that itself falls OUTSIDE
  // [rangeBegin, rangeEnd) - i.e. an event moved INTO the visible window
  // from a date that wouldn't otherwise be shown - is not independently
  // emitted. Every override that DOES correspond to a master occurrence
  // still inside the window is handled correctly above.
  return true;
}
#endif  // WIRSA_FEATURE_CALDAV_REPORT

// ============================================================================
// Network fetch
// ============================================================================

// Picks WiFiClientSecure (TLS, cert verification disabled - TOFU, matching
// bitwarden_relay.cpp's own setInsecure() convention, since this protocol
// has no cert-pinning frame equivalent to BW_TRUST) or a plain WiFiClient
// based on the URL's own scheme, then lets HTTPClient::begin(client, url)
// do the rest of the URL parsing (host/port/path) - simpler and more
// robust than hand-parsing a URL ourselves.
// One single attempt - the real entry point (cddvHttpRequest, just below)
// wraps this in a retry loop. Split out so the retry loop doesn't have to
// duplicate the tls/plain/http setup dance.
// Pulls just the hostname out of a "scheme://host[:port]/path" URL - used
// only for the DNS pre-check below, not for real URL handling (that's
// still entirely HTTPClient::begin's job).
static String cddvUrlHost(const String &url) {
  int schemeEnd = url.indexOf("://");
  int hostStart = (schemeEnd >= 0) ? schemeEnd + 3 : 0;
  int pathStart = url.indexOf('/', hostStart);
  int hostEnd = (pathStart >= 0) ? pathStart : url.length();
  String hostPort = url.substring(hostStart, hostEnd);
  int colon = hostPort.indexOf(':');
  return (colon >= 0) ? hostPort.substring(0, colon) : hostPort;
}

// cddvRawTlsDiagnostic's own port extractor. Explicit ":port" wins if
// present; otherwise default by scheme (443/80) - a hardcoded 443 would
// wrongly attempt a TLS handshake against a plain-HTTP target's real port.
static uint16_t cddvUrlPort(const String &url, bool isHttps) {
  int schemeEnd = url.indexOf("://");
  int hostStart = (schemeEnd >= 0) ? schemeEnd + 3 : 0;
  int pathStart = url.indexOf('/', hostStart);
  int hostEnd = (pathStart >= 0) ? pathStart : url.length();
  String hostPort = url.substring(hostStart, hostEnd);
  int colon = hostPort.indexOf(':');
  if (colon >= 0) return (uint16_t) hostPort.substring(colon + 1).toInt();
  return isHttps ? 443 : 80;
}

// cddvRawTlsDiagnostic's own path extractor (host-stripping only - iCloud/
// CalDAV URLs built by joinUrl()/joinUrlICloud() never carry a query
// string, so no "?" handling needed).
static String cddvUrlPath(const String &url) {
  int schemeEnd = url.indexOf("://");
  int hostStart = (schemeEnd >= 0) ? schemeEnd + 3 : 0;
  int pathStart = url.indexOf('/', hostStart);
  return (pathStart >= 0) ? url.substring(pathStart) : String("/");
}

// A caller-supplied sink for streamed response bytes - no capture (plain
// function pointer, matches this file's own no-capture convention for
// callback state, e.g. extractCalendarDataBlocks's onBlock). Each call site
// (CalDAV/iCloud multistatus, ICS feed, future Google) supplies its own
// function and does its OWN incremental accumulation/parsing from it.
// Returns false to abort the transfer (out of memory, malformed feed, sink
// downstream already gave up, etc.) - real hardware bug found without
// this: an aborted ICS/multistatus parse kept silently discarding every
// further chunk while HTTPClient kept pulling the REST of a large response
// off the socket for nothing, looking exactly like a hang from the Palm's
// side even though WiRSa had already given up minutes earlier.
typedef bool (*CddvBodyChunkFn)(const char *data, size_t len);

// Generic write-only Arduino Stream sink handed to HTTPClient::writeToStream(),
// replacing getString()'s buffer-the-whole-response approach - applies
// uniformly to CalDAV/iCloud/ICS/future Google, since a response can
// exceed available buffer space regardless of server type.
// writeToStream() (HTTPClient.cpp) already transparently de-chunks a
// chunked-Transfer-Encoding response internally (writeToStreamDataBlock)
// before ever calling write() here, so this class never needs to know or
// care whether the server used chunked or identity encoding - it just
// forwards whatever complete, already-dechunked bytes arrive to the
// caller's own onChunk, and unlike StreamString (getString()'s own backing
// store) this write() can never fail to "grow" since it has no buffer of
// its own to grow - it only ever forwards. Read side (available/read/peek)
// is never called by writeToStream() - stubbed inert.
class CddvChunkStream : public Stream {
public:
  CddvBodyChunkFn onChunk = nullptr;
  size_t totalBytes = 0;

  int available() override { return 0; }
  int read() override { return -1; }
  int peek() override { return -1; }
  size_t write(uint8_t b) override { return write(&b, 1); }
  size_t write(const uint8_t *buffer, size_t size) override {
    // A short return (less than size) here is HTTPClient's own signal to
    // abort the transfer (writeToStreamDataBlock treats bytesWrite !=
    // bytesRead as HTTPC_ERROR_STREAM_WRITE and stops reading immediately)
    // - this is what actually stops the socket read, not just our own
    // onChunk() silently no-opping on every later call.
    bool keepGoing = onChunk ? onChunk((const char *) buffer, size) : true;
    totalBytes += size;
    if (!keepGoing) return 0;
    // A response streams through here as many small write() calls -
    // same "keep WiFi/watchdog serviced" reasoning as this file's other
    // yield() calls.
    yield();
    return size;
  }
};

static bool cddvHttpRequestOnce(const String &url, const char *method, const String &body,
                                 const char *user, const char *pass,
                                 const char *contentType, const char *extraHeaderName,
                                 const char *extraHeaderValue, bool browserHeaders,
                                 CddvBodyChunkFn onBodyChunk,
                                 int &codeOut, bool &anyBodyDeliveredOut, String &errOut) {
  anyBodyDeliveredOut = false;
  // HTTPClient's own errorToString(-1) collapses DNS failure, network-path
  // block, and local resource exhaustion into the same generic "connection
  // refused" string, indistinguishable from a real HTTP 401 credential
  // problem. Resolving the host ourselves first turns "can't even connect"
  // into two distinguishable outcomes: "DNS lookup failed" vs. an actual
  // TCP/TLS-level refusal from a host that did resolve - either way, never
  // a credential problem, since nothing has been sent to the server yet.
  {
    String host = cddvUrlHost(url);
    IPAddress ip;
    if (usbDebug) cddvLog("CalDAV: [dns] resolving %s\r\n", host.c_str());
    if (host.length() > 0 && !WiFi.hostByName(host.c_str(), ip)) {
      codeOut = 0;
      errOut = String("DNS lookup failed for ") + host + " - check the hostname, and that WiRSa has working internet";
      return false;
    }
    if (usbDebug) cddvLog("CalDAV: [dns] resolved %s -> %s\r\n", host.c_str(), ip.toString().c_str());
  }

  HTTPClient http;
  bool isHttps = url.startsWith("https://");
  bool ok;

  // The mbedTLS SSL record buffer is fixed at 16384 bytes in EACH direction
  // in this core's own sdkconfig - two SEPARATE allocations (in, out), not
  // one combined block. Logged here purely for diagnostics.
  //
  // A previous version of this function used this reading to REJECT the
  // connection attempt outright below a 40KB "largest contiguous block"
  // threshold, on the theory that mbedtls_ssl_setup() needs one ~32KB
  // contiguous allocation and would otherwise fail silently deep inside
  // WiFiClientSecure::connect(). That theory was wrong, confirmed by real
  // iCloud sync testing (and again here: a sync failed this pre-flight
  // check at largest_free_block=36852 - already comfortably above either
  // 16KB buffer on its own - and there was never any evidence the REAL
  // connect() would have failed at that level). Since the two SSL buffers
  // are separate 16KB-each allocations, not one 32-40KB block, a
  // moderately fragmented heap that still has two ~16-20KB holes handles
  // this fine - this function was rejecting connections that would have
  // succeeded. Removed: let the real WiFiClientSecure::connect() attempt
  // below be the actual arbiter - if IT fails (mbedtls_ssl_setup()
  // returning an error, surfaced by HTTPClient as a generic connect
  // failure), that's handled by the ordinary connection-failure path
  // already below, with no need to guess about it beforehand.
  if (isHttps && usbDebug) {
    cddvLog("CalDAV: heap before TLS connect: free=%u largest_free_block=%u\r\n",
                  (unsigned) ESP.getFreeHeap(), (unsigned) ESP.getMaxAllocHeap());
  }
  // tls/plain must outlive every http.* call below, not just begin() -
  // HTTPClient::begin(client, url) stores a reference to whichever object
  // is passed, it does not copy or take ownership; declaring these inside
  // an if/else block would destroy the socket the instant begin() returns,
  // leaving http.GET()/sendRequest() operating on a dangling reference.
  // Matches bitwarden_relay.cpp's own convention (its WiFiClientSecure is
  // declared at the caller's scope and passed by reference for the same reason).
  WiFiClientSecure tls;
  WiFiClient plain;
  if (usbDebug) cddvLog("CalDAV: [connect] %s: %s\r\n", isHttps ? "TLS" : "plain", maskUrlForLog(url).c_str());
  if (isHttps) {
    tls.setInsecure();
    tls.setTimeout(60);  // seconds (WiFiClientSecure::setTimeout takes seconds, not ms)
    // The TLS handshake itself is governed by a separate field,
    // sslclient_context::handshake_timeout, which defaults to 120000ms and
    // is only changed by setHandshakeTimeout() - tls.setTimeout() above
    // only sets WiFiClientSecure's own _timeout (TCP-connect select() and
    // handshake read/write socket_timeout), not the handshake's overall
    // ceiling. Without this call, a handshake that has to retransmit a few
    // times can legitimately run for up to 2 minutes, well past every
    // other timeout in this function.
    tls.setHandshakeTimeout(60);  // seconds
    // HTTPClient.h declares `_client` as `WiFiClient*`, and WiFiClient.h's
    // ESPLwIPClient base declares `virtual int setTimeout(uint32_t) = 0` -
    // HTTPClient's own internal `_client->setTimeout(...)` call therefore
    // dispatches to WiFiClientSecure::setTimeout(), the same shadowed
    // _timeout, not Stream::_timeout. Every Stream::readStringUntil()
    // HTTPClient does internally (status line, headers, chunk-size lines)
    // is capped at Stream's own hardcoded 1000ms default unless
    // Stream::_timeout is set explicitly here, via this upcast, before
    // handing the object to HTTPClient - HTTPClient's own later call only
    // re-touches WiFiClientSecure's shadowed copy and cannot undo this.
    ((Stream &) tls).setTimeout(60000);  // milliseconds - Stream::setTimeout's own unit, not seconds
    ok = http.begin(tls, url);
  } else {
    ((Stream &) plain).setTimeout(60000);  // same _timeout-shadowing risk applies to plain WiFiClient
    ok = http.begin(plain, url);
  }
  if (!ok) { errOut = "HTTP begin() failed (bad URL?)"; return false; }

  // HTTPClient::connect() (HTTPClient.cpp) passes its own internal
  // _connectTimeout field to the underlying socket connect() call - a
  // separate field from both http.setTimeout() below (_tcpTimeout, used
  // for post-connect reads) and tls.setTimeout() above (not what gets
  // passed into this specific 3-arg connect(host, port, timeout) call at
  // all). This library's default for _connectTimeout is
  // HTTPCLIENT_DEFAULT_TCP_TIMEOUT = 5000ms unless overridden by
  // setConnectTimeout() - a TLS handshake against a remote with more round
  // trips than a lightweight self-hosted server can exceed that, reported
  // as a generic "connection refused" rather than a timeout. 60s gives
  // real margin over a slow REPORT query's execution time on the remote
  // server, which is the actual long pole here, not anything WiRSa controls.
  http.setConnectTimeout(60000);
  http.setTimeout(60000);
  if (browserHeaders) {
    // A realistic Chrome-on-Windows header set - some public ICS-feed
    // hosts front the file with a CDN/WAF whose bot mitigation can
    // silently drop/reset a request that doesn't look like a real
    // browser, rather than returning a clean HTTP error. Deliberately not
    // claiming Accept-Encoding: gzip/br - ESP32 HTTPClient doesn't
    // decompress, so claiming compression support a server might honor
    // would hand back binary garbage instead of parseable ICS text;
    // "identity" is what a real Chrome request falls back to requesting.
    // ICS feed only (browserHeaders=false everywhere else, see caller) -
    // no real browser ever issues a WebDAV REPORT, so a request claiming
    // "Chrome" on a method+Accept shape no browser would produce is
    // exactly the kind of mismatch a security-conscious WAF flags as
    // anomalous. Genuine CalDAV/iCloud REPORT requests send an honest,
    // minimal client identifier instead (below) rather than impersonating a browser.
    //
    // HTTPClient::addHeader() (HTTPClient.cpp) silently rejects
    // "Connection", "User-Agent", and "Host" - they're handled internally
    // via dedicated setters, so User-Agent must be set via setUserAgent()
    // (below), not addHeader().
    http.addHeader("Accept", "text/calendar,text/plain,application/xml,*/*");
    http.addHeader("Accept-Language", "en-US,en;q=0.9");
    http.addHeader("Accept-Encoding", "identity");
    http.setUserAgent("Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36");
  } else {
    // Matches the headers Evolution Data Server (libedataserver) sends for
    // a genuine WebDAV client (e-soup-session.c's
    // e_soup_session_preset_message()): User-Agent "Evolution/" VERSION,
    // Connection: close, Cache-Control: no-cache, and Pragma: no-cache
    // (the latter two implement RFC 4918 SS10.4.5's cache-disabling
    // requirement for WebDAV). No explicit Accept header. Content-Type's
    // exact charset quoting is matched too at the call site below
    // (E_WEBDAV_CONTENT_TYPE_XML is a quoted "utf-8", not bare utf-8).
    //
    // setReuse(false) is HTTPClient's actual mechanism for a
    // "Connection: close" request (controls the internal _reuse flag that
    // decides which value it sends itself) - addHeader() can't set
    // Connection directly (see the browserHeaders=true branch's own comment).
    http.setUserAgent("Evolution/3.52.3");
    http.setReuse(false);
    http.addHeader("Cache-Control", "no-cache");
    http.addHeader("Pragma", "no-cache");
  }
  if (contentType) http.addHeader("Content-Type", contentType);
  if (extraHeaderName) http.addHeader(extraHeaderName, extraHeaderValue);
  if (user && pass && user[0]) http.setAuthorization(user, pass);

  if (usbDebug) {
    cddvLog("CalDAV: [send] %s (%u byte body, auth=%s, browserHeaders=%s)\r\n",
                  method, (unsigned) body.length(), (user && pass && user[0]) ? "yes" : "no",
                  browserHeaders ? "yes" : "no");
  }
  // Connect/TLS handshake is already done by this point (http.begin() above
  // succeeded) - flip the on-device status from "Connecting..." to
  // "Syncing..." right as the actual request goes out, covering both the
  // CalDAV/iCloud REPORT path and the ICS-feed GET path (cddvFetchIcsFeed
  // routes through this same function) uniformly.
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
    // Includes the raw HTTPClient error code alongside the human-readable
    // string - e.g. "connection refused (code=-1)" vs "connection lost
    // (code=-5)" are two different HTTPC_ERROR_* values worth telling
    // apart at a glance.
    errOut = String("HTTP request failed: ") + http.errorToString(code) + " (code=" + String(code) + ")";
    // Confirms whether the WiFi association itself is already gone at the
    // exact moment a request fails, vs. just this one TCP/TLS session
    // having been reset by the remote end while WiFi is still fine.
    // WL_CONNECTED == 3 in this core.
    if (usbDebug) cddvLog("CalDAV: [response] connect/send failed: %s (WiFi.status()=%d, %s)\r\n",
                                 errOut.c_str(), (int) WiFi.status(),
                                 (WiFi.status() == WL_CONNECTED) ? "still connected" : "NOT connected");
    http.end();
    return false;
  }
  // HTTPClient::getString() discards writeToStream()'s return value - if
  // the internal StreamString's buffer failed to grow far enough into a
  // large response (heap too fragmented for the needed contiguous block),
  // it silently returns whatever partial data was captured, with no error
  // surfaced. Streaming directly into CddvChunkStream below means there's
  // no growing buffer that can fail to grow - what's left to check is a
  // genuine mid-stream socket death, which writeToStreamDataBlock does
  // correctly report as a negative HTTPC_ERROR_* (its own
  // bytesWritten/leftBytes and chunk-header read-timeout checks), checked
  // here instead of silently discarded.
  CddvChunkStream sink;
  sink.onChunk = onBodyChunk;
  int written = http.writeToStream(&sink);
  anyBodyDeliveredOut = (sink.totalBytes > 0);
  if (written < 0) {
    errOut = String("HTTP response read failed: ") + http.errorToString(written) + " (code=" + String(written) + ")";
    if (usbDebug) cddvLog("CalDAV: [response] %s\r\n", errOut.c_str());
    codeOut = 0;  // treat as a connection-level failure so the retry loop above retries it
    http.end();
    return false;
  }
  if (usbDebug) {
    cddvLog("CalDAV: [response] HTTP %d, %u byte body (streamed)\r\n", code, (unsigned) sink.totalBytes);
  }
  http.end();
  return true;
}

// Retries only a connection-level failure (code <= 0 - DNS/TCP/TLS/mid-
// transfer drop) with a fresh connection each time; a real, well-formed
// HTTP error response (4xx/5xx) is NOT retried - that's a legitimate
// answer from the server, retrying it rapidly wouldn't help and could
// look even more like abuse to whatever WAF is in front of it. Each
// retry is a genuinely NEW socket+TLS handshake (cddvHttpRequestOnce's
// own tls/http objects are function-local), not a reused/stale
// connection - covers real transient WiFi/DNS/server hiccups (same
// reasoning as SyncEngine.c's own SendAckWithRetry), and empirically
// sometimes succeeds against a WAF that dropped the FIRST attempt but
// not a second, even though it can't help against a deterministic
// TLS-fingerprint block that fails identically every time.
static bool cddvHttpRequest(const String &url, const char *method, const String &body,
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
    // onBodyChunk may have already handed some response bytes to the
    // caller before a mid-stream connection death - for the CalDAV/iCloud
    // path that means some events may already be on the wire to the Palm
    // (cddvSendEventItem). Retrying with a fresh connection restarts the
    // response from byte 0, which would re-extract and re-send those same
    // events a second time. A pre-body failure (DNS/connect/send, or a
    // dead connection before the first body byte arrived) is still fully
    // safe to retry - only a failure after body delivery has begun needs
    // to become a hard failure instead of a silent duplicate-emitting retry.
    if (anyBodyDelivered) {
      if (usbDebug) cddvLogLn("CalDAV: [retry] response already partially streamed - not retrying (would duplicate already-sent events)");
      return false;
    }
    if (attempt < kMaxAttempts) {
      if (usbDebug) cddvLog("CalDAV: [retry] attempt %d/%d failed (%s), retrying...\r\n",
                                   attempt, kMaxAttempts, errOut.c_str());
      // network.cpp's maintainWiFi() background reconnect logic
      // deliberately skips MODE_CALDAV_RELAY (transferActive) so it
      // doesn't yank the connection out from under an active transfer -
      // fine while the connection is still good, but nothing else brings
      // WiFi back once it's already dead while this mode is active.
      // Retrying against dead WiFi wastes the whole retry budget for
      // nothing - explicitly reconnect and wait (bounded) before the next
      // attempt instead.
      if (WiFi.status() != WL_CONNECTED) {
        if (usbDebug) cddvLogLn("CalDAV: [retry] WiFi is down - reconnecting before next attempt...");
        WiFi.reconnect();
        unsigned long waitStart = millis();
        while (WiFi.status() != WL_CONNECTED && (millis() - waitStart) < 8000) {
          delay(200);
          yield();
        }
        if (usbDebug) {
          cddvLog("CalDAV: [retry] WiFi %s after %lums\r\n",
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

// extractCalendarDataBlocks takes a plain function pointer (no capture),
// so the sink/abort/range state for the CalDAV REPORT path (below) is
// threaded through these module-level statics for the duration of one
// cddvHandleSyncRequest call - safe (single-threaded, one sync at a time,
// same reasoning as every other static scratch buffer in this file).
static CalDavEventSink *gActiveSink = nullptr;
static bool gSinkAborted = false;
static unsigned gCddvEventsSent = 0;  // verbose-logging stage counter, reset per SYNC_REQUEST
// gCddvRangeStart/End/gCddvMultistatusAccum/gCddvBlocksExtracted below are
// left declared unconditionally regardless of WIRSA_FEATURE_CALDAV_REPORT
// (a handful of bytes each, not worth the complexity of guarding a shared-
// state global whose only writers/readers are the functions already
// individually guarded below) - only the actual REPORT-parsing CODE that
// uses them is conditional.
static time_t gCddvRangeStart = 0, gCddvRangeEnd = 0;

#if WIRSA_FEATURE_CALDAV_REPORT
static void cddvOnCalendarDataBlock(const String &block) {
  if (gSinkAborted || !gActiveSink) return;
  if (!parseICalAndEmit(block, (uICAL::seconds_t) gCddvRangeStart, (uICAL::seconds_t) gCddvRangeEnd, *gActiveSink)) {
    gSinkAborted = true;
  }
}
#endif  // WIRSA_FEATURE_CALDAV_REPORT

// Streaming state for the CalDAV/iCloud/Google multistatus REPORT path - a
// small accumulator that only ever holds one in-flight partial
// <calendar-data> block (plus a little between-blocks XML filler) rather
// than the whole, potentially tens-of-KB, chunked-Transfer-Encoding
// response. Reset per SYNC_REQUEST, same as gActiveSink/gCddvRangeStart above.
static String gCddvMultistatusAccum;
static unsigned gCddvBlocksExtracted = 0;

// Incremental sibling of the original one-shot block scanner (same
// substring-scanning/CDATA/entity-unescaping logic, matching on the
// literal substring "calendar-data" as a tag name - namespace-prefix-
// agnostic and safe in this narrow response shape, per the original's own
// comment). Scans gCddvMultistatusAccum from the start, calls
// cddvOnCalendarDataBlock() for every COMPLETE block found, then trims the
// accumulator down to just the unconsumed tail. Unlike the original,
// "no closing tag yet" is the NORMAL steady state while more chunks are
// still arriving - not malformed - so it simply stops and waits for the
// next chunk instead of logging an error.
#if WIRSA_FEATURE_CALDAV_REPORT
static void extractCalendarDataBlocksIncremental() {
  int searchFrom = 0;
  int consumedUpTo = 0;
  for (;;) {
    int tagPos = gCddvMultistatusAccum.indexOf("calendar-data", searchFrom);
    if (tagPos < 0) break;
    int ltPos = gCddvMultistatusAccum.lastIndexOf('<', tagPos);
    if (ltPos < 0) { searchFrom = tagPos + 13; continue; }
    int gtPos = gCddvMultistatusAccum.indexOf('>', tagPos);
    if (gtPos < 0) break;  // opening tag not fully arrived yet - wait for more bytes
    bool selfClosing = (gtPos > 0 && gCddvMultistatusAccum[gtPos - 1] == '/');
    if (selfClosing) {
      searchFrom = gtPos + 1;
      consumedUpTo = gtPos + 1;
      continue;
    }
    int contentStart = gtPos + 1;
    int closeTagPos = gCddvMultistatusAccum.indexOf("calendar-data", contentStart);
    if (closeTagPos < 0) break;  // closing tag not fully arrived yet - wait for more bytes
    int closeGt = gCddvMultistatusAccum.indexOf('>', closeTagPos);
    if (closeGt < 0) break;

    int closeLt = gCddvMultistatusAccum.lastIndexOf("</", closeTagPos);
    if (closeLt < contentStart) closeLt = closeTagPos;
    String raw = gCddvMultistatusAccum.substring(contentStart, closeLt);

    String content;
    int cdataStart = raw.indexOf("<![CDATA[");
    if (cdataStart >= 0) {
      int cdataEnd = raw.indexOf("]]>", cdataStart);
      content = (cdataEnd >= 0) ? raw.substring(cdataStart + 9, cdataEnd) : raw.substring(cdataStart + 9);
    } else {
      content = xmlUnescape(raw);
    }
    gCddvBlocksExtracted++;
    cddvOnCalendarDataBlock(content);
    searchFrom = closeGt + 1;
    consumedUpTo = closeGt + 1;
    if (gSinkAborted) break;
    yield();
  }
  if (consumedUpTo > 0) gCddvMultistatusAccum.remove(0, consumedUpTo);
}

// Chunk callback wired as the CalDAV/iCloud REPORT path's onBodyChunk -
// binary-safe, checked append (String::concat's own bool return, unlike
// the library's own internal discard-the-error pattern this whole redesign
// is replacing) followed by an incremental extraction pass.
static bool cddvOnMultistatusChunk(const char *data, size_t len) {
  if (gSinkAborted) return false;
  if (!gCddvMultistatusAccum.concat((const uint8_t *) data, (unsigned int) len)) {
    gSinkAborted = true;
    if (usbDebug) cddvLogLn("CalDAV: [parse] out of memory accumulating response chunk");
    return false;
  }
  extractCalendarDataBlocksIncremental();
  return !gSinkAborted;
}
#endif  // WIRSA_FEATURE_CALDAV_REPORT

// ICS Feed: true incremental/streaming parse - never buffers the whole feed
// in memory, same "small bounded accumulator, extract-and-discard" design
// as the multistatus path above (gCddvMultistatusAccum/
// extractCalendarDataBlocksIncremental), just scanning for plain
// BEGIN:VEVENT/BEGIN:VTIMEZONE ... END:... boundaries instead of XML
// <calendar-data> tags (a raw ICS feed has no XML wrapper at all).
//
// Real motivation, not theoretical: the PREVIOUS design buffered the whole
// feed into one Arduino String before parsing it, which turned out to hit
// a hard, unconditional ~64KB ceiling on boards without BOARD_HAS_PSRAM
// (WString.h's own CAPACITY_MAX enum: 65535 without PSRAM vs 3MB with it -
// String::capacity() is even a PROTECTED member, so this couldn't be
// probed at runtime either). Confirmed on real hardware: a real Google
// Calendar export failed the very FIRST 64KB growth step even with 100KB+
// of free heap reported, because the failure had nothing to do with heap
// at all - raising the cap or fixing fragmentation could never have helped.
//
// Unlike the multistatus path's own <calendar-data> blocks (each already a
// complete, independent one-event VCALENDAR from the server), one raw ICS
// feed's VEVENTs share a common VTIMEZONE/tzmap and can forward- or
// backward-reference each other via RECURRENCE-ID, so this still needs a
// two-phase design: phase 1 (streamed, below) parses each VTIMEZONE/VEVENT
// component as its own small, independent String the moment it completes
// (bounded by ONE component's own size - typically well under 1KB, never
// the whole feed) into the same masters/overrides maps parseICalAndEmit
// itself builds; phase 2 (cddvIcsFinishParse, called once after the whole
// feed has streamed through) expands occurrences from those already-
// parsed, memory-cheap maps exactly like parseICalAndEmit's own tail does.
// Deliberately NOT sharing code with parseICalAndEmit (duplicated instead)
// - that function is the CalDAV/iCloud path's own real-hardware-confirmed
// implementation, and refactoring it to be reusable risks regressing an
// already-working path for the sake of this one.
#if WIRSA_FEATURE_CALDAV_ICS
static uICAL::TZMap_ptr gCddvIcsTzmap;
static std::map<String, CalDavMasterEvent> gCddvIcsMasters;
static std::map<String, std::vector<CalDavOverrideEvent>> gCddvIcsOverrides;

// Set once per fetch (cddvFetchIcsFeed) from its own rangeStart/rangeEnd
// params - lets cddvIcsOnComponent drop a non-repeating master the instant
// it's parsed if it's already provably outside the sync window, instead of
// waiting for cddvIcsFinishParse's own VEventIter range check at the very
// end of the whole feed. See that early-drop's own comment for why.
static uICAL::seconds_t gCddvIcsRangeStart = 0;
static uICAL::seconds_t gCddvIcsRangeEnd = 0;

// Diagnostic counters only (not used for any logic) - a real report asked
// "are you actually discarding old events?" with no way to tell from the
// log alone whether the early-drop was firing or whether the feed simply
// has a lot of genuinely RECURRING events (which can't be judged early -
// see the early-drop's own comment) keeping their full object graph
// alive. These make that visible instead of guessed at.
static unsigned gCddvIcsMastersDropped = 0;    // non-repeating, outside window, dropped on capture
static unsigned gCddvIcsMastersRepeating = 0;  // repeating, always kept regardless of window

// Declared up here (moved from its previous spot further down, alongside
// gCddvIcsPartial) because cddvIcsHeapCritical(), defined below, needs it
// before that point in the file.
static bool gCddvIcsOverflow = false;

// Same widened filter as parseICalAndEmit's own local lambda - duplicated,
// not shared, see this section's own header comment.
static bool cddvIcsUseLine(const uICAL::string parent, const uICAL::string line) {
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

static void cddvIcsBeginParse() {
  gCddvIcsTzmap = uICAL::new_ptr<uICAL::TZMap>();
  gCddvIcsMasters.clear();
  gCddvIcsOverrides.clear();
}

// Parses ONE already-complete "BEGIN:VEVENT ... END:VEVENT" or
// "BEGIN:VTIMEZONE ... END:VTIMEZONE" block (no VCALENDAR wrapper needed -
// uICAL::VObjectStream::nextObjectName() just reads whatever BEGIN it sees
// first) and folds it into gCddvIcsTzmap/gCddvIcsMasters/gCddvIcsOverrides -
// exactly what parseICalAndEmit's own per-child loop body does, just fed
// one component at a time instead of walking a whole buffered document.
// Real-hardware crash found: gCddvIcsMasters/gCddvIcsOverrides hold a full
// VEvent/DateTime/TZ/RRule object graph per event for the WHOLE feed (not
// just the sync window - RRULE/RECURRENCE-ID need full-document context),
// so a feed with a long history of one-off events can accumulate enough
// live heap allocations that a later small `new` (e.g. a TZ object) fails
// - and C++ failing to even allocate the resulting std::bad_alloc's own
// exception object forces an unconditional std::terminate()/abort(), which
// no try/catch here can intercept. Checked once per component (cheap) as a
// backstop - this alone doesn't fix the underlying growth, see the
// non-repeating-master early-drop further down in this function for the
// actual mitigation; a feed heavy on RECURRING events (which can't be
// judged early that way) can still exhaust heap on its own, hence this.
static bool cddvIcsHeapCritical() {
  static const uint32_t kMinFreeHeap = 12288;
  if (ESP.getFreeHeap() >= kMinFreeHeap) return false;
  if (!gCddvIcsOverflow) {
    gCddvIcsOverflow = true;
    if (usbDebug) cddvLog("CalDAV: [ics] aborting - free heap critically low (%u bytes) mid-feed"
                                 " (%u masters kept [%u repeating], %u dropped as out-of-window)\r\n",
                                 (unsigned) ESP.getFreeHeap(), (unsigned) gCddvIcsMasters.size(),
                                 gCddvIcsMastersRepeating, gCddvIcsMastersDropped);
  }
  return true;
}

static void cddvIcsOnComponent(const String &componentText) {
  yield();
  if (cddvIcsHeapCritical()) return;
  uICAL::istream_String compStream(componentText);
  uICAL::VLineStream lines(compStream);
  uICAL::VObjectStream stm(lines, cddvIcsUseLine);

  uICAL::VObject_ptr child;
  try {
    child = stm.nextObject(true);
  } catch (...) {
    if (usbDebug) cddvLogLn("CalDAV: [ics] component parse error - skipped");
    return;
  }
  if (!child) return;

  if (child->getName() == "VTIMEZONE") {
    try {
      gCddvIcsTzmap->add(child);
      if (usbDebug) {
        uICAL::VLine_ptr tzIdLine = child->getPropertyByName("TZID");
        cddvLog("CalDAV: [ics] VTIMEZONE added: %s\r\n", tzIdLine ? tzIdLine->value.c_str() : "(no TZID)");
      }
    } catch (const uICAL::Error &e) {
      if (usbDebug) cddvLog("CalDAV: [ics] VTIMEZONE skipped (%s)\r\n", e.message.c_str());
    } catch (...) {
      if (usbDebug) cddvLogLn("CalDAV: [ics] VTIMEZONE skipped (non-uICAL parse error)");
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
      if (usbDebug) cddvLog("CalDAV: [ics] VEVENT skipped (%s)\r\n", e.message.c_str());
      return;
    } catch (...) {
      if (usbDebug) cddvLogLn("CalDAV: [ics] VEVENT skipped (non-uICAL parse error)");
      return;
    }

    if (recIdLine) {
      try {
        String tzid = recIdLine->getParam("TZID");
        uICAL::DateTime recId((recIdLine->value + tzid).c_str(), gCddvIcsTzmap);
        CalDavOverrideEvent ov;
        ov.recurrenceIdEpoch = recId.toEpochSeconds();
        // Deliberately NOT ov.vevent = vevent - see CalDavOverrideEvent's
        // own comment. An override is only ever consulted as a fields
        // lookup keyed by date, never expanded via VEventIter, so there's
        // no reason to keep the full DateTime/TZ/RRule graph alive for the
        // rest of the feed just to read these same four values back out
        // of it later.
        ov.title = icalUnescapeText(String(vevent->summary.c_str()));
        ov.startEpoch = vevent->start.toEpochSeconds();
        ov.endEpoch = vevent->end.toEpochSeconds();
        ov.allDay = vevent->start.isDateOnly();
        ov.location = location;
        ov.description = description;
        ov.cancelled = cancelled;
        gCddvIcsOverrides[uid].push_back(ov);
        // Matches the VTIMEZONE success log's own verbosity - the send-to-
        // Palm phase (cddvSendEventItem) only runs later, in
        // cddvIcsFinishParse, once the whole feed has streamed through
        // (see that function's own comment for why), so this capture-time
        // log is the only per-event progress signal during the transfer.
        if (usbDebug) cddvLog("CalDAV: [ics] VEVENT override captured: %s\r\n", uid.c_str());
      } catch (const uICAL::Error &e) {
        if (usbDebug) cddvLog("CalDAV: [ics] RECURRENCE-ID override skipped (%s)\r\n", e.message.c_str());
      } catch (...) {
        if (usbDebug) cddvLogLn("CalDAV: [ics] RECURRENCE-ID override skipped (non-uICAL parse error)");
      }
    } else {
      // Real-hardware crash found: gCddvIcsMasters holds a full VEvent
      // object graph per event for the ENTIRE feed regardless of the sync
      // window (RRULE/RECURRENCE-ID need full-document context, so this
      // can't be filtered server-side or via the usual per-block streaming
      // the CalDAV/iCloud path uses). A non-repeating event is the one
      // case that's safe to filter early: with no RRULE, VEventIter can
      // only ever yield ITS OWN start/end as a single occurrence - exactly
      // what cddvIcsFinishParse's own range check does at the very end of
      // the whole feed anyway - so an event entirely outside the window
      // can never contribute and is dropped here instead of held for the
      // rest of a potentially long-running feed. (A repeating event can't
      // be judged this way - its OWN start/end may be outside the window
      // while individual occurrences fall inside it - so isRepeating
      // events are always kept, same as before.) A feed with years of
      // one-off history is exactly what let per-event heap use snowball
      // into a double allocation failure -> abort() on real hardware.
      uICAL::seconds_t evStart = 0, evEnd = 0;
      bool evAllDay = false;
      if (!isRepeating) {
        evStart = vevent->start.toEpochSeconds();
        evEnd = vevent->end.toEpochSeconds();
        evAllDay = vevent->start.isDateOnly();
        if (evEnd < gCddvIcsRangeStart || evStart > gCddvIcsRangeEnd) {
          gCddvIcsMastersDropped++;
          if (usbDebug) cddvLog("CalDAV: [ics] VEVENT master dropped (non-repeating, outside window): %s\r\n", uid.c_str());
          return;
        }
      } else {
        // Three cheap, always-safe checks that can rule out a REPEATING
        // series ever landing in the sync window, without running the
        // full RRULE expansion (real report: a feed can carry many
        // recurring series that are simply over - RRULE:UNTIL/COUNT long
        // exhausted - which this file wasn't checking at all before,
        // holding their full object graph for the whole feed for
        // nothing):
        //  1. The series' own DTSTART is after the window ends - by
        //     definition it can't have produced an occurrence yet,
        //     regardless of FREQ/COUNT/UNTIL.
        //  2. RRULE has an explicit UNTIL - RFC 5545 3.3.10 makes it an
        //     INCLUSIVE upper bound on every occurrence in the series - if
        //     that's before the window starts, the whole series is done.
        //  3. RRULE has an explicit COUNT (mutually exclusive with UNTIL
        //     per RFC 5545) - the exact date of the LAST occurrence needs
        //     the real RRULE expansion (BYDAY/BYMONTHDAY/etc. select which
        //     day WITHIN each base period, but never push a occurrence
        //     past it), so a generous, deliberately-oversized period
        //     length per FREQ step (e.g. 31 days for MONTHLY, the longest
        //     possible month) gives a conservative upper bound that can
        //     only ever be LATER than the true last occurrence - safe to
        //     drop only if even THAT bound is still before the window.
        // A truly unbounded series (neither UNTIL nor COUNT) can't be
        // judged early at all, and is always kept, same as before.
        uICAL::seconds_t seriesStart = vevent->start.toEpochSeconds();
        if (seriesStart > gCddvIcsRangeEnd) {
          gCddvIcsMastersDropped++;
          if (usbDebug) cddvLog("CalDAV: [ics] VEVENT master dropped (repeating, starts after window): %s\r\n", uid.c_str());
          return;
        }
        String rruleValue(rruleLine->value.c_str());
        String untilStr, countStr, freqStr, intervalStr;
        if (cddvRruleFindParam(rruleValue, "UNTIL", untilStr)) {
          try {
            uICAL::DateTime untilDt(untilStr.c_str(), gCddvIcsTzmap);
            if (untilDt.toEpochSeconds() < gCddvIcsRangeStart) {
              gCddvIcsMastersDropped++;
              if (usbDebug) cddvLog("CalDAV: [ics] VEVENT master dropped (repeating, UNTIL before window): %s\r\n", uid.c_str());
              return;
            }
          } catch (...) {
            // Malformed/unparseable UNTIL value - fall through and keep
            // the series rather than risk dropping something real.
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
              if (usbDebug) cddvLog("CalDAV: [ics] VEVENT master dropped (repeating, COUNT-bounded, ends before window): %s\r\n", uid.c_str());
              return;
            }
          }
        }
        // EXDATE only means anything against an RRULE - meaningless (and,
        // for a non-repeating master, about to be thrown away anyway per
        // CalDavMasterEvent's own comment) otherwise, so skip the work.
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
              if (usbDebug) cddvLog("CalDAV: [ics] EXDATE value skipped (%s)\r\n", e.message.c_str());
            } catch (...) {
              if (usbDebug) cddvLogLn("CalDAV: [ics] EXDATE value skipped (non-uICAL parse error)");
            }
          }
        }
      }
      CalDavMasterEvent m;
      if (isRepeating) {
        m.vevent = vevent;
      } else {
        // Deliberately NOT m.vevent = vevent - see CalDavMasterEvent's own
        // comment. Leaving it unset lets the local `vevent` shared_ptr
        // (and everything it owns: DateTime x2, TZ x2, RRule) be freed the
        // moment this function returns, instead of held for the rest of
        // the feed transfer just to read these same four values back out
        // of it later in cddvIcsFinishParse.
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
      if (usbDebug) cddvLog("CalDAV: [ics] VEVENT master captured (%s): %s\r\n",
                                   isRepeating ? "repeating" : "in-window", uid.c_str());
    }
  } catch (const uICAL::Error &e) {
    if (usbDebug) cddvLog("CalDAV: [ics] VEVENT entirely skipped (%s)\r\n", e.message.c_str());
  } catch (...) {
    if (usbDebug) cddvLogLn("CalDAV: [ics] VEVENT entirely skipped (non-uICAL parse error)");
  }
}

// Occurrence-expansion tail - identical logic to parseICalAndEmit's own,
// reading gCddvIcsMasters/gCddvIcsOverrides (already fully populated by
// cddvIcsOnComponent across the whole streamed feed) instead of locals.
static bool cddvIcsFinishParse(uICAL::seconds_t rangeStartUnix, uICAL::seconds_t rangeEndUnix, CalDavEventSink sink) {
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

    // Lightweight path for a non-repeating master - m.vevent was never
    // populated for one (see CalDavMasterEvent's own comment), so there's
    // no uICAL object graph left to run VEventIter against; its one and
    // only "occurrence" is exactly the (startEpoch, endEpoch, title)
    // captured at parse time. Mirrors the VEventIter loop's own body
    // below for a single iteration, override-matching included (a
    // RECURRENCE-ID override against a non-repeating master isn't valid
    // per RFC 5545, but a malformed feed could still produce one, so this
    // stays defensive rather than assuming it can't happen).
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
        if (usbDebug) {
          char startBuf[24];
          formatUtcBasic((time_t) startEpoch, startBuf, sizeof(startBuf));
          cddvLog("CalDAV: [ics][occurrence] uid=%s start=%s (epoch=%ld, allDay=%d)\r\n",
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
        if (usbDebug) {
          char startBuf[24];
          formatUtcBasic((time_t) startEpoch, startBuf, sizeof(startBuf));
          cddvLog("CalDAV: [ics][occurrence] uid=%s start=%s (epoch=%ld, allDay=%d)\r\n",
                        uid.c_str(), startBuf, (long) startEpoch, allDay ? 1 : 0);
        }
        if (!sink(uid, title, location, utcStartPalm, utcEndPalm, allDay, description, m.isRepeating)) return false;
      }
    } catch (const uICAL::Error &e) {
      if (usbDebug) cddvLog("CalDAV: [ics] master event skipped mid-expansion (%s)\r\n", e.message.c_str());
    } catch (...) {
      if (usbDebug) cddvLogLn("CalDAV: [ics] master event skipped mid-expansion (non-uICAL parse error)");
    }
  }
  return true;
}

// Bounded, small - only ever holds ONE in-flight VEVENT/VTIMEZONE block
// (typically well under 1KB) plus a little between-components filler, never
// the whole feed. Still capped defensively - a hostile/malformed feed that
// never sends a matching END: would otherwise grow this unboundedly.
static String gCddvIcsPartial;
static bool gCddvIcsSawVCalendar = false;

// kCddvIcsPartialCap is deliberately well UNDER 65536, not just "generous" -
// on a board without BOARD_HAS_PSRAM, a single Arduino String can never
// hold more than 65535 bytes total regardless of free heap (WString.h's own
// CAPACITY_MAX enum - the exact hard cap that broke the OLD whole-feed-
// buffering design this file used to have). A single pathological
// component (in practice: a VTIMEZONE listing many decades of DST
// transition rules) approaching that limit would otherwise fail via
// String::concat() silently returning false deep inside this same
// accumulator, which is a much less obvious symptom than this explicit,
// named cap catching it first with a clear log line.
static const size_t kCddvIcsPartialCap = 49152;

// Persists ACROSS calls (real hardware bug found: without this, a single
// large component - most often Google's own VTIMEZONE block, which can
// carry many years of DST transition rules and run to tens of KB - that
// takes many small TCP/TLS chunks to fully arrive gets its own indexOf()
// search restarted from byte 0 of that component on EVERY chunk, turning
// what should be one linear scan of the whole feed into O(chunks x
// component size) - looked exactly like a hung sync on real hardware
// (WiRSa's own log stops dead right after the GET is sent), not a
// network problem. gCddvIcsPendingBegin < 0 means "not currently inside a
// known component" - reset once per fetch, in cddvFetchIcsFeed.
static int gCddvIcsPendingBegin = -1;
static const char *gCddvIcsPendingEndTag = NULL;
static int gCddvIcsScannedUpTo = 0;  // how far the END-tag search has already confirmed clean, for the CURRENT pending component

// Same fix, same reasoning, for the OTHER half of the scan: real-hardware
// hang found where a sync went totally silent (no crash, no further log
// lines - yield() still ran, so the watchdog never tripped) after a
// component parse error. Root cause: a run of buffered bytes containing
// no BEGIN:VEVENT/BEGIN:VTIMEZONE yet (e.g. leftover text between two
// components) was being re-scanned with indexOf() from byte 0 on every
// single incoming chunk while waiting for the next one to start, same
// O(chunks x gap size) blowup as gCddvIcsScannedUpTo already fixed for
// the "inside a pending component" case - just left unfixed here.
static int gCddvIcsNoBeginScannedUpTo = 0;

// Scans gCddvIcsPartial for complete BEGIN:VEVENT/BEGIN:VTIMEZONE ... END:...
// blocks (VEVENT never nests another VEVENT, VTIMEZONE never nests another
// VTIMEZONE, so matching the FIRST corresponding END: is always correct -
// no CDATA/self-closing-tag ambiguity to handle the way the XML multistatus
// path's own extractCalendarDataBlocksIncremental has to), feeds each one
// to cddvIcsOnComponent, then trims the consumed prefix.
static void cddvIcsExtractComponents() {
  int consumedUpTo = 0;
  for (;;) {
    if (gCddvIcsPendingBegin < 0) {
      // Resume from where the last no-match scan left off, minus enough
      // overlap to still catch a BEGIN tag split across a chunk boundary
      // ("BEGIN:VTIMEZONE" is the longer of the two, 16 chars) - see
      // gCddvIcsNoBeginScannedUpTo's own comment.
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
      // Resume just shy of where the previous scan left off (enough
      // overlap to catch the tag if it was split right at a chunk
      // boundary), instead of re-scanning the whole component from its
      // own start every time - this is the actual fix, see this
      // function's own header comment.
      int resumeFrom = gCddvIcsScannedUpTo - (tagLen - 1);
      if (resumeFrom < gCddvIcsPendingBegin) resumeFrom = gCddvIcsPendingBegin;
      int endPos = gCddvIcsPartial.indexOf(gCddvIcsPendingEndTag, resumeFrom);
      if (endPos < 0) {
        gCddvIcsScannedUpTo = gCddvIcsPartial.length();
        break;  // still incomplete - wait for more bytes, resume from here next call
      }
      int blockEnd = endPos + tagLen;

      // Real report: heap kept draining to the critical floor even though
      // the early-drop logic above was correctly firing on almost every
      // component (1425 dropped, only 2 kept, on one real feed) - if
      // dropping (never storing) a component's parsed data isn't actually
      // recovering its memory, something in the parse-then-discard cycle
      // itself is leaking, not the design of what gets kept. Measured
      // once per component (not per allocation - no heap-tracing hooks
      // available in this environment) to find out whether that's really
      // happening and how much, instead of guessing further.
      uint32_t heapBeforeComponent = ESP.getFreeHeap();
      cddvIcsOnComponent(gCddvIcsPartial.substring(gCddvIcsPendingBegin, blockEnd));
      uint32_t heapAfterComponent = ESP.getFreeHeap();
      if (usbDebug && heapAfterComponent + 256 < heapBeforeComponent) {
        cddvLog("CalDAV: [ics] heap dropped %ld bytes processing one component (%lu -> %lu)\r\n",
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
    // A NEW component may have been found (and left pending, waiting on
    // its own END:) in the same call that also completed earlier ones -
    // its offsets are relative to the pre-trim buffer, so they need to
    // shift down by whatever was just removed.
    if (gCddvIcsPendingBegin >= 0) {
      gCddvIcsPendingBegin -= consumedUpTo;
      gCddvIcsScannedUpTo -= consumedUpTo;
    }
    gCddvIcsNoBeginScannedUpTo -= consumedUpTo;
    if (gCddvIcsNoBeginScannedUpTo < 0) gCddvIcsNoBeginScannedUpTo = 0;
  }
}

// Streaming here (unlike CalDAV/iCloud's parseICalAndEmit, which sends each
// event to the Palm the moment its own single-event <calendar-data> block
// finishes) never calls cddvSendEventItem until the WHOLE feed has arrived
// and cddvIcsFinishParse runs - RECURRENCE-ID overrides can reference a
// master VEVENT appearing either earlier or later in the document, so
// nothing can be emitted early. That means usbDebug's usual per-item log
// line goes completely silent for the ENTIRE body transfer, which looks
// identical to "stuck" whether it actually is or not. This heartbeat (one
// line per ~16KB received) is the only forward-progress signal available
// during that phase - a real stall shows the same byte count repeating
// forever; genuine (if slow) progress shows it climbing.
static size_t gCddvIcsTotalBytes = 0;
static size_t gCddvIcsLastHeartbeatBytes = 0;

// Tracks gCddvIcsPartial's own currently-reserved capacity - kept manually
// because String::capacity() is a PROTECTED member on this Arduino core
// (confirmed via a real build error earlier this session), so it can't be
// queried directly to decide whether a reserve() call is actually needed.
// Updated only right after a successful reserve().
static size_t gCddvIcsPartialReservedTo = 0;

// Grow in fixed steps instead of reserving the full kCddvIcsPartialCap (48KB)
// up front for the whole fetch - real hardware report: a feed whose actual
// components run a few KB each (typical) was aborting on a "free heap
// critically low" bail after barely a dozen events, because a flat 48KB
// reservation - sized for the WORST case, a single huge VTIMEZONE - was
// permanently pinned for the ENTIRE fetch regardless of what this feed's
// components actually needed. Growing in ~8KB steps still avoids
// reallocating gCddvIcsPartial on every single small TCP/TLS chunk (the
// original O(chunks x size) concern), while a typical few-KB component
// only ever pins a few KB, not 48.
static const size_t kCddvIcsGrowStep = 8192;

static bool cddvOnIcsChunk(const char *data, size_t len) {
  if (gCddvIcsOverflow) return false;
  size_t neededLen = gCddvIcsPartial.length() + len;
  if (neededLen > kCddvIcsPartialCap) {
    gCddvIcsOverflow = true;
    if (usbDebug) cddvLogLn("CalDAV: [ics] a single component exceeded the accumulator cap - malformed feed (missing END:?)");
    return false;
  }
  if (neededLen > gCddvIcsPartialReservedTo) {
    size_t target = neededLen + kCddvIcsGrowStep;
    if (target > kCddvIcsPartialCap) target = kCddvIcsPartialCap;
    if (gCddvIcsPartial.reserve(target)) gCddvIcsPartialReservedTo = target;
    // If reserve() fails here, gCddvIcsPartialReservedTo is left as-is (too
    // small) - the concat() below still does its own checked, safe growth
    // as a fallback, same as before this optimization existed.
  }
  if (!gCddvIcsPartial.concat((const uint8_t *) data, (unsigned int) len)) {
    gCddvIcsOverflow = true;
    if (usbDebug) cddvLogLn("CalDAV: [ics] out of memory accumulating response chunk");
    return false;
  }
  gCddvIcsTotalBytes += len;
  if (usbDebug && gCddvIcsTotalBytes - gCddvIcsLastHeartbeatBytes >= 16384) {
    gCddvIcsLastHeartbeatBytes = gCddvIcsTotalBytes;
    cddvLog("CalDAV: [ics] ...%u bytes received so far (%u masters kept [%u repeating], %u dropped as out-of-window, %u override uids buffered, %u bytes free heap)\r\n",
                  (unsigned) gCddvIcsTotalBytes, (unsigned) gCddvIcsMasters.size(), gCddvIcsMastersRepeating,
                  gCddvIcsMastersDropped, (unsigned) gCddvIcsOverrides.size(), (unsigned) ESP.getFreeHeap());
  }
  // A 200 response whose body isn't actually iCalendar data (most often an
  // HTML bot-challenge/CAPTCHA page from whatever CDN/WAF fronts the real
  // feed) would otherwise silently parse as zero VEVENTs and look
  // identical to "the feed is just empty" - checked once, cheaply, on the
  // still-small partial buffer rather than needing the whole body.
  if (!gCddvIcsSawVCalendar && gCddvIcsPartial.indexOf("BEGIN:VCALENDAR") >= 0) gCddvIcsSawVCalendar = true;
  cddvIcsExtractComponents();
  return !gCddvIcsOverflow;
}

// ICS Feed: a plain unauthenticated GET of the whole feed, streamed and
// clamped to the same [rangeStart, rangeEnd) window locally (a plain ICS
// feed has no server-side filtering at all).
static bool cddvFetchIcsFeed(const String &feedUrl, time_t rangeStart, time_t rangeEnd,
                              CalDavEventSink sink, String &errOut) {
  int code;
  gCddvIcsPartial = "";
  // Reset the capacity tracker, not a flat reserve() to the full cap - see
  // gCddvIcsPartialReservedTo/kCddvIcsGrowStep's own comments. A real
  // hardware sync aborted on "free heap critically low" after barely a
  // dozen events because reserving the full 48KB cap up front, unconditionally,
  // for the WHOLE fetch turned out to cost far more than this design's
  // actual per-component memory need in the common case.
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
  // ICS Feed is the one path that actually needs the browser-spoofed
  // headers (public feed hosts behind a consumer WAF) - see
  // cddvHttpRequestOnce's own comment for why CalDAV/iCloud REPORT
  // requests deliberately do NOT get this treatment.
  bool ok = cddvHttpRequest(feedUrl, "GET", String(), nullptr, nullptr, nullptr, nullptr, nullptr, true,
                             cddvOnIcsChunk, code, errOut);
  if (!ok) {
    // code <= 0 here (cddvHttpRequest already retried 3x and still never
    // got a real HTTP response at all) - a connection actively dropped
    // mid-handshake/mid-transfer, as opposed to a clean error status, is
    // consistent with a CDN/WAF (Cloudflare, Imperva/Incapsula) resetting
    // the connection for a client it doesn't like the look of, rather
    // than answering with an HTTP-level rejection. Spelled out here since
    // "connection lost" alone gives the user nothing actionable to chase.
    if (code <= 0)
      errOut += " (repeated connection failures on an HTTPS feed can mean"
                " the feed's CDN is blocking this device - try a mirror"
                " not fronted by one, or a plain HTTP feed if available)";
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
    errOut = "ICS feed returned HTTP 200 but the body wasn't iCalendar data"
             " (possibly a bot-protection/CAPTCHA page from the feed's CDN)";
    return false;
  }

  return cddvIcsFinishParse((uICAL::seconds_t) rangeStart, (uICAL::seconds_t) rangeEnd, sink);
}
#endif  // WIRSA_FEATURE_CALDAV_ICS

// ============================================================================
// Google Calendar: no uICAL involved on this path at all - the Calendar
// API's events.list expands recurring events and drops cancelled
// instances server-side (singleEvents=true), so there's no client-side
// RRULE/EXDATE/RECURRENCE-ID handling to do, unlike the CalDAV/iCloud path.
// ============================================================================
#if WIRSA_FEATURE_CALDAV_GOOGLE

// RFC 3986 percent-encoding for a URL query/path component - same
// alnum+"-_.~" allowlist bitwarden_relay.cpp's own urlEncode() uses for
// Bitwarden's OAuth2-style POST bodies.
static String cddvUrlEncode(const String &s) {
  String out;
  out.reserve(s.length() * 3);
  static const char *hex = "0123456789ABCDEF";
  for (size_t i = 0; i < s.length(); i++) {
    uint8_t c = (uint8_t) s[i];
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') out += (char) c;
    else { out += '%'; out += hex[c >> 4]; out += hex[c & 0xF]; }
  }
  return out;
}

static void cddvFormatRfc3339(time_t t, char *out, size_t outCap) {
  struct tm tmv;
  gmtime_r(&t, &tmv);
  strftime(out, outCap, "%Y-%m-%dT%H:%M:%SZ", &tmv);
}

// Days since the Unix epoch for a proleptic-Gregorian y/m/d - Howard
// Hinnant's well-known days_from_civil algorithm. Used instead of
// mktime()/timegm() so this doesn't depend on how (or whether) this
// specific toolchain's libc handles UTC conversion/timezone state.
static int64_t cddvDaysFromCivil(int y, int m, int d) {
  y -= m <= 2;
  int64_t era = (y >= 0 ? y : y - 399) / 400;
  unsigned yoe = (unsigned) (y - era * 400);
  unsigned doy = (153 * (unsigned) (m + (m > 2 ? -3 : 9)) + 2) / 5 + (unsigned) d - 1;
  unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + (int64_t) doe - 719468;
}

// Parses an RFC 3339 timestamp as the Google Calendar API sends it: either
// a timed "2026-09-25T09:00:00-07:00" / "...Z" dateTime, or a bare
// "2026-09-25" all-day date (midnight UTC, matching how this relay's own
// CalDAV/iCloud path already treats an iCal all-day DATE value).
//
// siscanf, not sscanf: newlib's integer-only scanner (already linked for
// tzset). These were the firmware's only sscanf calls; plain sscanf pulled
// in the floating-point scanner + strtod for nothing (~16KB of flash).
static bool cddvParseRfc3339(const String &s, time_t &outEpoch) {
  int y, mo, d, h = 0, mi = 0, sec = 0;
  char offSign = 0; int offH = 0, offM = 0;
  int n = (int) s.length();
  if (n == 10) {
    if (siscanf(s.c_str(), "%d-%d-%d", &y, &mo, &d) != 3) return false;
  } else if (n >= 19) {
    if (siscanf(s.c_str(), "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &sec) != 6) return false;
    if (n > 19) {
      char tail = s[19];
      if ((tail == '+' || tail == '-') && n >= 25) {
        offSign = tail;
        siscanf(s.c_str() + 20, "%d:%d", &offH, &offM);
      }
      // 'Z' (or anything else) falls through as UTC (offSign left 0) -
      // Google always sends one of 'Z' or a numeric offset in practice.
    }
  } else {
    return false;
  }
  int64_t secs = cddvDaysFromCivil(y, mo, d) * 86400 + h * 3600 + mi * 60 + sec;
  // local = UTC + offset, so UTC = local - offset.
  if (offSign == '+') secs -= (int64_t) offH * 3600 + offM * 60;
  else if (offSign == '-') secs += (int64_t) offH * 3600 + offM * 60;
  outEpoch = (time_t) secs;
  return true;
}

static String gCddvGoogleAccum;
static bool gCddvGoogleOverflow = false;
static const size_t kCddvGoogleAccumCap = 512 * 1024;

static bool cddvOnGoogleChunk(const char *data, size_t len) {
  if (gCddvGoogleOverflow) return false;
  if (gCddvGoogleAccum.length() + len > kCddvGoogleAccumCap ||
      !gCddvGoogleAccum.concat((const uint8_t *) data, (unsigned int) len)) {
    gCddvGoogleOverflow = true;
    if (usbDebug) cddvLogLn("CalDAV: [google] response body exceeded accumulator cap or ran out of memory");
    return false;
  }
  return true;
}

// Exchanges the long-lived refresh token (stored Sec35-encrypted on the
// Palm, sent as `secret` in every SYNC_REQUEST) for a short-lived access
// token - done fresh on every sync, nothing cached on WiRSa across syncs,
// same pattern as every other credential this relay handles.
//
// clientSecret is required here despite Google's own docs saying it's
// optional for "Desktop app" clients - confirmed against a real client,
// the token endpoint rejects both this grant and the authorization_code
// grant with "client_secret is missing" otherwise.
static bool cddvGoogleRefreshAccessToken(const String &refreshToken, const String &clientId,
                                          const String &clientSecret,
                                          String &outAccessToken, String &errOut) {
  String form = "client_id=" + cddvUrlEncode(clientId) +
                "&client_secret=" + cddvUrlEncode(clientSecret) +
                "&refresh_token=" + cddvUrlEncode(refreshToken) +
                "&grant_type=refresh_token";
  gCddvGoogleAccum = "";
  gCddvGoogleOverflow = false;
  int code;
  bool ok = cddvHttpRequest("https://oauth2.googleapis.com/token", "POST", form,
                             nullptr, nullptr, "application/x-www-form-urlencoded",
                             nullptr, nullptr, false, cddvOnGoogleChunk, code, errOut);
  if (!ok) return false;
  if (code != 200) {
    // A revoked/expired refresh token (invalid_grant) needs the user to
    // sign in again - worth saying plainly rather than a bare HTTP code.
    JsonDocument errDoc;
    if (!deserializeJson(errDoc, gCddvGoogleAccum)) {
      const char *err = errDoc["error"] | "";
      if (err[0]) {
        const char *desc = errDoc["error_description"] | "";
        errOut = String("Google token refresh failed: ") + err + (desc[0] ? (String(" - ") + desc) : String());
        if (strcmp(err, "invalid_grant") == 0)
          errOut += " - sign in with Google again for this calendar";
        return false;
      }
    }
    errOut = String("Google token endpoint returned HTTP ") + code;
    return false;
  }
  JsonDocument doc;
  if (deserializeJson(doc, gCddvGoogleAccum)) {
    errOut = "Malformed JSON from Google token endpoint";
    return false;
  }
  outAccessToken = String((const char *) (doc["access_token"] | (const char *) nullptr));
  if (outAccessToken.length() == 0) {
    errOut = "Google token endpoint response had no access_token";
    return false;
  }
  return true;
}

static bool cddvFetchGoogleCalendar(const String &refreshToken, const String &clientId, const String &clientSecret,
                                     const String &calendarId, time_t rangeStart, time_t rangeEnd,
                                     CalDavEventSink sink, String &errOut) {
  String accessToken;
  if (!cddvGoogleRefreshAccessToken(refreshToken, clientId, clientSecret, accessToken, errOut)) return false;
  String authHeader = "Bearer " + accessToken;

  char timeMinBuf[24], timeMaxBuf[24];
  cddvFormatRfc3339(rangeStart, timeMinBuf, sizeof(timeMinBuf));
  cddvFormatRfc3339(rangeEnd, timeMaxBuf, sizeof(timeMaxBuf));

  // Bounded to a sane worst case (250/page default x 20 pages = 5000
  // events) rather than looping on nextPageToken indefinitely.
  static const int kCddvGoogleMaxPages = 20;
  String pageToken;

  for (int page = 0; page < kCddvGoogleMaxPages; page++) {
    String url = "https://www.googleapis.com/calendar/v3/calendars/" + cddvUrlEncode(calendarId) +
                 "/events?singleEvents=true&orderBy=startTime&maxResults=250" +
                 "&timeMin=" + cddvUrlEncode(String(timeMinBuf)) +
                 "&timeMax=" + cddvUrlEncode(String(timeMaxBuf));
    if (pageToken.length()) url += "&pageToken=" + cddvUrlEncode(pageToken);

    if (usbDebug) cddvLog("CalDAV: [google] fetch page %d: %s\r\n", page, maskUrlForLog(url).c_str());
    gCddvGoogleAccum = "";
    gCddvGoogleOverflow = false;
    int code;
    bool ok = cddvHttpRequest(url, "GET", String(), nullptr, nullptr, nullptr,
                               "Authorization", authHeader.c_str(), false,
                               cddvOnGoogleChunk, code, errOut);
    if (!ok) return false;
    if (code != 200) {
      // Calendar API (and most Google REST APIs) nest the real reason in
      // error.message - a different shape than the token endpoint's flat
      // error/error_description, so this needs its own parse. Common
      // causes: the Calendar API not enabled for this Cloud project, or
      // the calendar ID not shared with/visible to this account.
      JsonDocument errDoc;
      if (!deserializeJson(errDoc, gCddvGoogleAccum)) {
        const char *msg = errDoc["error"]["message"] | (const char *) nullptr;
        if (msg) {
          errOut = String("Google Calendar API error: ") + msg;
          return false;
        }
      }
      errOut = String("Google Calendar API returned HTTP ") + code;
      return false;
    }
    if (gCddvGoogleOverflow) {
      errOut = "Google Calendar response too large or ran out of memory while reading it";
      return false;
    }

    JsonDocument doc;
    if (deserializeJson(doc, gCddvGoogleAccum)) {
      errOut = "Malformed JSON from Google Calendar API";
      return false;
    }

    for (JsonObject item : doc["items"].as<JsonArray>()) {
      const char *status = item["status"] | "confirmed";
      if (strcmp(status, "cancelled") == 0) continue;

      // No icalUnescapeText here (unlike the CalDAV/iCloud path) - these
      // are plain JSON string fields, not iCal TEXT values, so there's no
      // backslash-escaping to undo.
      String uid = String((const char *) (item["id"] | (const char *) nullptr));
      String title = String((const char *) (item["summary"] | (const char *) ""));
      String location = String((const char *) (item["location"] | (const char *) ""));
      String description = String((const char *) (item["description"] | (const char *) ""));
      const char *recurringEventId = item["recurringEventId"] | (const char *) nullptr;
      bool isRepeating = (recurringEventId != nullptr);

      const char *startDate = item["start"]["date"] | (const char *) nullptr;
      const char *startDateTime = item["start"]["dateTime"] | (const char *) nullptr;
      const char *endDate = item["end"]["date"] | (const char *) nullptr;
      const char *endDateTime = item["end"]["dateTime"] | (const char *) nullptr;

      bool allDay;
      time_t startEpoch, endEpoch;
      if (startDate && endDate) {
        allDay = true;
        if (!cddvParseRfc3339(String(startDate), startEpoch)) continue;
        if (!cddvParseRfc3339(String(endDate), endEpoch)) continue;
      } else if (startDateTime && endDateTime) {
        allDay = false;
        if (!cddvParseRfc3339(String(startDateTime), startEpoch)) continue;
        if (!cddvParseRfc3339(String(endDateTime), endEpoch)) continue;
      } else {
        continue;  // malformed item - skip rather than abort the whole sync
      }

      uint32_t utcStartPalm = (uint32_t) startEpoch + UNIX_TO_PALM_EPOCH;
      uint32_t utcEndPalm = (uint32_t) endEpoch + UNIX_TO_PALM_EPOCH;

      if (!sink(uid, title, location, utcStartPalm, utcEndPalm, allDay, description, isRepeating)) {
        errOut = "Lost contact with the Palm mid-sync";
        return false;
      }
    }

    const char *nextPageToken = doc["nextPageToken"] | (const char *) nullptr;
    if (!nextPageToken) break;
    pageToken = String(nextPageToken);
  }
  return true;
}
#endif  // WIRSA_FEATURE_CALDAV_GOOGLE

// ============================================================================
// Diagnostic path: mirrors BitWarden's own raw-socket pattern (talking
// directly to WiFiClientSecure instead of through HTTPClient) to isolate
// whether a connectivity issue originates in HTTPClient's own wrapper
// behavior versus the underlying WiFiClientSecure/mbedTLS handshake.
// Deliberately never touches the real cddvHttpRequestOnce/cddvHttpRequest
// path - gated by CDDV_DIAGNOSTIC_RAW_TLS below (its own call site), and
// does not extract/emit any real events (diagnosis only). Left in place
// for reuse if an isolated connectivity test is needed again.
static const bool CDDV_DIAGNOSTIC_RAW_TLS = false;

static bool cddvRawTlsDiagnostic(const String &url, const char *user, const char *pass,
                                  const String &reportBody, String &resultOut) {
  String host = cddvUrlHost(url);
  String path = cddvUrlPath(url);
  unsigned long t0 = millis();

  // network.cpp's maintainWiFi() deliberately treats the entire time
  // menuMode==MODE_CALDAV_RELAY as transferActive, so its own background
  // reconnect logic is disabled for as long as this mode's screen is
  // showing. cddvHttpRequest's retry loop compensates by calling
  // WiFi.reconnect() itself on failure - mirrored here.
  if (WiFi.status() != WL_CONNECTED) {
    cddvLog("CalDAV: [raw-diag] WiFi is down - reconnecting before attempt...\r\n");
    WiFi.reconnect();
    unsigned long waitStart = millis();
    while (WiFi.status() != WL_CONNECTED && (millis() - waitStart) < 8000) {
      delay(200);
      yield();
    }
    cddvLog("CalDAV: [raw-diag] WiFi %s after %lums\r\n",
            (WiFi.status() == WL_CONNECTED) ? "reconnected" : "still down",
            (unsigned long) (millis() - waitStart));
    if (WiFi.status() != WL_CONNECTED) {
      resultOut = "raw-diag: WiFi was down and did not reconnect within 8s";
      return false;
    }
  }

  IPAddress ip;
  if (!WiFi.hostByName(host.c_str(), ip)) {
    resultOut = "raw-diag: DNS lookup failed for " + host;
    return false;
  }
  if (usbDebug) cddvLog("CalDAV: [raw-diag] resolved %s -> %s, heap free=%u rssi=%d\r\n",
                               host.c_str(), ip.toString().c_str(),
                               (unsigned) ESP.getFreeHeap(), (int) WiFi.RSSI());

  // Both client objects are declared here so whichever is used stays alive
  // for the rest of the function; `client` is used for everything
  // scheme-agnostic below (connect/print/read/stop all come from the
  // common Client/Stream interface either way) - a plain-http target must
  // never attempt a TLS connect against port 443.
  bool isHttps = url.startsWith("https://");
  uint16_t port = cddvUrlPort(url, isHttps);
  WiFiClientSecure tls;
  WiFiClient plain;
  Client *client;
  if (isHttps) {
    tls.setInsecure();
    tls.setTimeout(60);  // seconds - matches cddvHttpRequestOnce's own budget
    tls.setHandshakeTimeout(60);  // seconds - see cddvHttpRequestOnce's own comment on this real, separate field
    // WiFiClientSecure/WiFiClient each declare their own private
    // `int _timeout`, shadowing the inherited, protected `Stream::_timeout`
    // (hardcoded to 1000ms - what Stream::readStringUntil()/timedRead()
    // actually read). Setting Stream::_timeout explicitly via this upcast,
    // same fix as cddvHttpRequestOnce's own equivalent.
    ((Stream &) tls).setTimeout(60000);  // milliseconds - Stream::setTimeout's own unit, not seconds
    client = &tls;
  } else {
    ((Stream &) plain).setTimeout(60000);  // same shadowing risk applies to plain WiFiClient too - set explicitly
    client = &plain;
  }

  unsigned long tConnectStart = millis();
  bool connected = client->connect(ip, port);
  unsigned long connectMs = millis() - tConnectStart;
  if (!connected) {
    resultOut = "raw-diag: connect() failed after " + String(connectMs) +
                "ms (" + (isHttps ? "TLS" : "plain") + ", port " + String(port) +
                ", WiFi.status()=" + String((int) WiFi.status()) + ")";
    if (usbDebug) cddvLog("CalDAV: [raw-diag] %s\r\n", resultOut.c_str());
    return false;
  }
  if (usbDebug) cddvLog("CalDAV: [raw-diag] %s connected (port %u) in %lums, heap free=%u rssi=%d\r\n",
                               isHttps ? "TLS" : "plain", (unsigned) port,
                               connectMs, (unsigned) ESP.getFreeHeap(), (int) WiFi.RSSI());

  // Basic Auth header - mbedtls_base64_encode directly (bitwarden_relay.cpp's
  // own bwBase64Encode uses the exact same real API), not HTTPClient's
  // internal encoder, since the whole point here is zero HTTPClient
  // involvement.
  String credPlain = String(user) + ":" + String(pass);
  size_t b64Needed = 0;
  mbedtls_base64_encode(nullptr, 0, &b64Needed, (const unsigned char *) credPlain.c_str(), credPlain.length());
  uint8_t *b64Buf = (uint8_t *) malloc(b64Needed + 1);
  String credB64;
  if (b64Buf) {
    size_t b64Actual = 0;
    mbedtls_base64_encode(b64Buf, b64Needed, &b64Actual,
                           (const unsigned char *) credPlain.c_str(), credPlain.length());
    b64Buf[b64Actual] = 0;
    credB64 = String((char *) b64Buf);
    free(b64Buf);
  }

  // Same header set as cddvHttpRequestOnce's browserHeaders=false branch
  // (EDS/Evolution-matching), sent as one single write - mirrors
  // bitwarden_relay.cpp's own request-building shape exactly.
  String req = "REPORT " + path + " HTTP/1.1\r\n";
  req += "Host: " + host + "\r\n";
  req += "User-Agent: Evolution/3.52.3\r\n";
  req += "Connection: close\r\n";
  req += "Cache-Control: no-cache\r\n";
  req += "Pragma: no-cache\r\n";
  req += "Content-Type: application/xml; charset=\"utf-8\"\r\n";
  req += "Depth: 1\r\n";
  req += "Authorization: Basic " + credB64 + "\r\n";
  req += "Content-Length: " + String(reportBody.length()) + "\r\n";
  req += "\r\n";
  req += reportBody;

  unsigned long tSendStart = millis();
  size_t written = client->print(req);
  unsigned long sendMs = millis() - tSendStart;
  if (usbDebug) cddvLog("CalDAV: [raw-diag] wrote %u/%u request bytes in %lums (WiFi.status()=%d)\r\n",
                               (unsigned) written, (unsigned) req.length(), sendMs, (int) WiFi.status());
  if (WiFi.status() != WL_CONNECTED) {
    resultOut = "raw-diag: WiFi dropped during/after request write (wrote " + String((unsigned) written) +
                "/" + String((unsigned) req.length()) + " bytes, sent in " + String(sendMs) + "ms)";
    client->stop();
    return false;
  }

  String statusLine = client->readStringUntil('\n');
  if (statusLine.length() == 0) {
    resultOut = "raw-diag: no response after " + String(millis() - t0) +
                "ms total (WiFi.status()=" + String((int) WiFi.status()) + ")";
    client->stop();
    return false;
  }

  // Drain headers, then enough of the body to confirm real CalDAV XML came
  // back - a connectivity/stability probe, not a real sync, so no need to
  // fully buffer or parse it.
  bool inHeaders = true;
  unsigned bodyBytes = 0;
  bool sawMultistatus = false;
  unsigned long tDrainStart = millis();
  while (millis() - tDrainStart < 5000) {
    if (inHeaders) {
      String line = client->readStringUntil('\n');
      if (line.length() == 0 && !client->connected() && !client->available()) break;
      if (line.length() <= 1) inHeaders = false;
      continue;
    }
    int avail = client->available();
    if (avail <= 0) {
      if (!client->connected()) break;
      yield();
      continue;
    }
    uint8_t buf[256];
    int n = client->read(buf, sizeof(buf));
    if (n <= 0) break;
    bodyBytes += (unsigned) n;
    if (!sawMultistatus && String((char *) buf, (unsigned) n).indexOf("multistatus") >= 0) sawMultistatus = true;
    yield();
  }
  client->stop();

  statusLine.trim();
  resultOut = "raw-diag OK: " + statusLine + ", " + String(bodyBytes) +
              " body bytes, multistatus=" + (sawMultistatus ? "yes" : "no") +
              ", total " + String(millis() - t0) + "ms";
  if (usbDebug) cddvLog("CalDAV: [raw-diag] %s\r\n", resultOut.c_str());
  return true;
}

// ============================================================================
// SYNC_REQUEST handling
// ============================================================================

static void cddvSendSyncError(const char *msg) {
  RelayProto::Writer w(gEventBody, sizeof(gEventBody));
  w.str(String(msg));
  RelayProto::writeFrame(PhysicalSerial, CDDV_SYNC_ERROR, gEventBody, w.len);
}

static bool cddvSendEventItem(const String &uid, const String &title, const String &location,
                               uint32_t utcStartPalm, uint32_t utcEndPalm, bool allDay,
                               const String &description, bool isRepeating) {
  RelayProto::Writer w(gEventBody, sizeof(gEventBody));
  w.str(uid);
  w.str(title);
  w.str(location);
  w.u32(utcStartPalm);
  w.u32(utcEndPalm);
  w.u8(allDay ? 1 : 0);
  // New fields appended at the end (not interleaved with the original
  // ones) - matches apps/caldav_sync/SyncEngine.c's own crStr/cr8 read
  // order exactly; must be changed in lockstep with that file.
  w.str(description);
  w.u8(isRepeating ? 1 : 0);

  bool peerBye = false;
  bool acked = RelayProto::writeFrameAcked(PhysicalSerial, CDDV_EVENT_ITEM, gEventBody, w.len,
                                            CDDV_ITEM_ACK, CDDV_BYE,
                                            gAckScratch, sizeof(gAckScratch),
                                            CDDV_ACK_RETRIES, CDDV_ACK_RETRY_DELAY_MS,
                                            CDDV_ACK_TIMEOUT_MS, CDDV_BYTE_TIMEOUT_MS,
                                            &peerBye);
  if (acked) {
    gCddvEventsSent++;
    if (usbDebug) cddvLog("CalDAV: [emit] #%u sent (uid=%s, title=%s)\r\n",
                                 gCddvEventsSent, uid.c_str(), title.c_str());
  } else if (usbDebug) {
    cddvLog("CalDAV: EVENT_ITEM ack failed (uid=%s)%s\r\n", uid.c_str(),
                  peerBye ? " - Palm said BYE" : "");
  }
  return acked;
}

static void cddvHandleSyncRequest(const uint8_t *body, size_t bodyLen) {
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
  // See SyncEngine.c's own matching comment. Not backward-compatible with
  // a Palm build that predates this field: Reader::u8()/u32() set
  // r.ok=false on reading past the end of a short body (checked below,
  // same as every other field on this wire format), so a mismatched pair
  // correctly gets "Malformed SYNC_REQUEST" rather than silently decoding
  // a bogus 0 - matches how this project is used (Palm app + WiRSa
  // firmware always rebuilt/reflashed together).
  uint32_t palmUtcNow = r.u32();
  // Google's OAuth Client ID/Secret - live on the Palm only (CDDVSlot),
  // sent fresh in every SYNC_REQUEST so WiRSa never holds a resident,
  // account-identifying value of its own. Empty for every other server type.
  String googleClientId = r.str();
  String googleClientSecret = r.str();

  if (!r.ok) {
    cddvSendSyncError("Malformed SYNC_REQUEST");
    return;
  }

  if (serverType != CDDV_TYPE_CALDAV && serverType != CDDV_TYPE_ICLOUD &&
      serverType != CDDV_TYPE_ICSFEED && serverType != CDDV_TYPE_GOOGLE) {
    cddvSendSyncError("Unknown server type");
    return;
  }

  // The Palm's own clock is the primary source for "now" (see this file's
  // Clock section above); only fall back to WiRSa's system clock if the
  // Palm sent something clearly not a real timestamp (0, or an old Palm
  // build that predates this field and decodes it as 0).
  time_t now = (time_t) palmUtcNow;
  bool usedPalmClock = (now > 1577836800L);  // sane post-2020 UTC time
  if (!usedPalmClock) time(&now);

  if (pastWin >= kPastWindowCount) pastWin = 0;
  if (futureWin >= kFutureWindowCount) futureWin = 0;
  time_t rangeStart = now - (time_t) kPastWindowDays[pastWin] * 86400L;
  time_t rangeEnd = now + (time_t) kFutureWindowDays[futureWin] * 86400L;

  // Masks account-identifying fields (email, DSID, calendar UUID) in debug
  // logs so a log can be shared for troubleshooting without manual
  // redaction. The actual request below still uses the real, unmasked
  // values - only logging is affected.
  String userLog = maskSensitive(user);
  String uuidLog = maskSensitive(uuid);
  String pathLog = maskSensitive(path);

  if (usbDebug) {
    cddvLog("CalDAV: SYNC_REQUEST type=%u server=%s path=%s uuid=%s user=%s pastWin=%u futureWin=%u\r\n",
                   serverType, server.c_str(), pathLog.c_str(), uuidLog.c_str(), userLog.c_str(), pastWin, futureWin);
    cddvLog("CalDAV: [window] now=%ld range=[%ld .. %ld] (clock source: %s)\r\n",
                   (long) now, (long) rangeStart, (long) rangeEnd, usedPalmClock ? "Palm" : "WiRSa system clock");
    if (serverType == CDDV_TYPE_GOOGLE) {
      // First-4-chars+length only (maskSensitive's own convention) - enough
      // to spot a truncated/mistyped/empty value without ever printing the
      // real Client ID/Secret to a log the user might paste/share.
      cddvLog("CalDAV: [google] clientId=%s clientSecret=%s\r\n",
                    maskSensitive(googleClientId).c_str(), maskSensitive(googleClientSecret).c_str());
    }
  }

  gSinkAborted = false;
  gCddvEventsSent = 0;
  gCddvBlocksExtracted = 0;
  gCddvMultistatusAccum = "";
  CalDavEventSink sink = cddvSendEventItem;
  gActiveSink = &sink;
  String err;
  bool ok;

  if (serverType == CDDV_TYPE_CALDAV || serverType == CDDV_TYPE_ICLOUD) {
#if WIRSA_FEATURE_CALDAV_REPORT
    // Db35/WiRSa: iCloud's CalDAV server (once you have the right shard/
    // UUID/calendar path - see joinUrlICloud's own comment) is a normal,
    // RFC-4791-compliant CalDAV endpoint - the exact same REPORT body,
    // Basic Auth, and 207/200 handling as the generic CalDAV path works
    // unmodified; only the URL construction differs.
    gCddvRangeStart = rangeStart;
    gCddvRangeEnd = rangeEnd;
    char startBuf[24], endBuf[24];
    formatUtcBasic(rangeStart, startBuf, sizeof(startBuf));
    formatUtcBasic(rangeEnd, endBuf, sizeof(endBuf));
    // A one-sided (open-ended future) time-range is valid per RFC 4791 and
    // is what EDS's own cbdav_add_limit_days_filter() sends when no future
    // limit is configured, but it's more expensive for a server to
    // evaluate (indefinitely-recurring series must be reasoned about
    // against "start to infinity") - keeping the bounded two-sided filter
    // here avoids trading a predictable request for a slower, less
    // predictable one against a server under real load.
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
    if (usbDebug) cddvLog("CalDAV: [fetch] %s REPORT: %s\r\n",
                                 (serverType == CDDV_TYPE_ICLOUD) ? "iCloud" : "CalDAV", maskUrlForLog(url).c_str());
    int code;
    // browserHeaders=false: a real authenticated WebDAV REPORT identifies
    // itself honestly, not impersonating Chrome - a request claiming
    // "Chrome" while using a method+Accept shape no browser produces risks
    // tripping a security-conscious WAF. Content-Type charset quoted to
    // match EDS's own E_WEBDAV_CONTENT_TYPE_XML exactly (e-webdav-session.h:
    // "application/xml; charset=\"utf-8\"").
    //
    // cddvOnMultistatusChunk extracts and emits each <calendar-data> block
    // as its bytes arrive, via gCddvMultistatusAccum - never buffers the
    // whole (chunked, size-unknown) response into one String.
    if (CDDV_DIAGNOSTIC_RAW_TLS) {
      // Bypasses cddvHttpRequest/HTTPClient entirely; see
      // cddvRawTlsDiagnostic's own header comment. Always reports back as
      // a SYNC_ERROR (never a real sync) so the result - success/failure,
      // timing, byte counts - shows up directly on the Palm's screen
      // without needing to read serial logs.
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
          // Apple rejects the regular Apple ID password outright for CalDAV
          // once 2FA is on (Basic Auth can't complete an interactive second
          // factor) - a 401 here is very likely that, not a wrong
          // shard/UUID/calendar path, so say so directly rather than making
          // the user guess.
          if (serverType == CDDV_TYPE_ICLOUD && code == 401)
            err += " - use an app-specific password (appleid.apple.com), not your regular Apple ID password";
        } else {
          if (usbDebug) cddvLog("CalDAV: [parse] extracted %u calendar-data block(s) (streamed)\r\n",
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
  } else if (serverType == CDDV_TYPE_GOOGLE) {
#if WIRSA_FEATURE_CALDAV_GOOGLE
    // path = calendar ID, secret = the Sec35-decrypted refresh token,
    // googleClientId/googleClientSecret = the Palm-side Client ID/Secret
    // fields - all sent fresh in this SYNC_REQUEST (see
    // apps/caldav_sync/SyncEngine.c's own field mapping comment). WiRSa
    // holds none of these resident.
    if (usbDebug) cddvLogLn("CalDAV: [fetch] Google Calendar");
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
    if (usbDebug) cddvLog("CalDAV: [fetch] ICS feed: %s\r\n", maskUrlForLog(server).c_str());
    ok = cddvFetchIcsFeed(server, rangeStart, rangeEnd, sink, err);
#else
    ok = false;
    err = "ICS Feed sync is not enabled in this WiRSa build";
#endif  // WIRSA_FEATURE_CALDAV_ICS
  }

  gActiveSink = nullptr;

  if (!ok) {
    if (usbDebug) cddvLog("CalDAV: sync failed: %s (after %u event(s) sent)\r\n",
                                 err.c_str(), gCddvEventsSent);
    if (!gSinkAborted) cddvSendSyncError(err.length() ? err.c_str() : "Sync failed");
    // If gSinkAborted, the Palm side has already lost contact / sent BYE -
    // no point sending it a SYNC_ERROR it likely won't see either.
    return;
  }

  if (usbDebug) cddvLog("CalDAV: [done] sync succeeded, %u event(s) sent\r\n", gCddvEventsSent);
  gCddvLastSyncOk = true;
  RelayProto::writeFrame(PhysicalSerial, CDDV_SYNC_DONE, nullptr, 0);
  RelayProto::writeFrame(PhysicalSerial, CDDV_BYE, nullptr, 0);
}

// cddvHandleSyncRequest does deep, stack-hungry work on the default
// loop() task's undersized 8KB stack: a TLS handshake (mbedTLS crypto
// needs real stack depth) followed by building a full C++ object graph
// (std::shared_ptr/map/vector-based VObject/VLine tree, then
// RRULE/VEventIter occurrence expansion) via uICAL. A stack overflow
// doesn't always trip a hard fault immediately - it can smash adjacent
// memory instead (including the WiFi/LWIP task's own state, since ESP32
// tasks share the same heap), producing non-deterministic instability
// rather than a clean crash. Wrapped on a dedicated FreeRTOS task with an
// enlarged stack instead, same shape as bitwarden_relay.cpp's own
// bwHandleSyncBigStack.
//
// 64KB, not 32KB: the ICS Feed path (cddvFetchIcsFeed) streams the uICAL
// parse of each VEVENT/VTIMEZONE component (cddvIcsOnComponent) directly
// from CddvChunkStream::write(), which is itself called from INSIDE
// HTTPClient::writeToStream()'s own socket/TLS record-read call chain
// (writeToStreamDataBlock -> WiFiClientSecure::read -> mbedTLS record
// decrypt). Unlike the CalDAV/iCloud multistatus path (parseICalAndEmit
// runs only AFTER the HTTP request has fully returned, at a shallow,
// post-TLS stack depth), this nests the ENTIRE uICAL object-graph build
// several frames beneath the TLS/HTTPClient read stack rather than after
// it - the two peaks are additive, not sequential. A real-hardware crash
// (Guru Meditation / LoadProhibited, EXCVADDR=0x00000000) reproduced
// reliably a few VTIMEZONE components into a sync is consistent with
// this combined peak overflowing the old 32KB budget; there's 100KB+ of
// free heap margin on a typical sync per the diagnostic logging above,
// so paying for a larger dedicated stack here is cheap.
static SemaphoreHandle_t gCddvSyncTaskDone = nullptr;
static const uint8_t *gCddvSyncTaskBody = nullptr;
static size_t gCddvSyncTaskBodyLen = 0;

static void cddvHandleSyncRequestTask(void *) {
  cddvHandleSyncRequest(gCddvSyncTaskBody, gCddvSyncTaskBodyLen);
  xSemaphoreGive(gCddvSyncTaskDone);
  vTaskDelete(NULL);
}

// body must point at storage that outlives the task (gFrameBody, the only
// real caller, is a static file-scope buffer - safe; its contents are
// fully consumed here before caldavRelayLoop() reads another frame into
// it, since the caller blocks until this task signals done) - identical
// reasoning to bwHandleSyncBigStack's own comment.
static void cddvHandleSyncRequestBigStack(const uint8_t *body, size_t bodyLen) {
  if (!gCddvSyncTaskDone) gCddvSyncTaskDone = xSemaphoreCreateBinary();
  gCddvSyncTaskBody = body;
  gCddvSyncTaskBodyLen = bodyLen;
  TaskHandle_t h = nullptr;
  BaseType_t created = xTaskCreate(cddvHandleSyncRequestTask, "cddvSync", 65536, nullptr, 1, &h);
  if (created != pdPASS) {
    if (usbDebug) cddvLogLn("CalDAV: failed to create sync task (out of memory?) - running inline");
    cddvHandleSyncRequest(body, bodyLen);
    return;
  }
  xSemaphoreTake(gCddvSyncTaskDone, portMAX_DELAY);
}

// ============================================================================
// Google OAuth handoff (CDDV_OAUTH_START/INFO/TOKEN). google_auth.py, run
// once by the user on their own computer, does the actual Google OAuth
// dance and POSTs the resulting refresh token to a transient, PIN-secured
// local HTTP listener here, which relays it to the Palm over serial and
// tears itself down. WiRSa never persists the token itself - the Palm
// stores it (Sec35-encrypted) and re-sends it as `secret` in every
// SYNC_REQUEST, same as a CalDAV/iCloud password.
// ============================================================================

// cddvOAuthStop()/cddvOAuthPump() are called unconditionally from the
// shared mode-exit and mode-loop code further down regardless of which
// CalDAV types are built in, so a no-op stub pair covers the
// WIRSA_FEATURE_CALDAV_GOOGLE=0 case without needing to guard those call
// sites individually. Everything else in this section (including the
// CDDV_OAUTH_START frame handler down in the switch below) is Google-only.
#if WIRSA_FEATURE_CALDAV_GOOGLE
static const uint16_t CDDV_OAUTH_PORT = 8090;
// Generous enough to cover an unfamiliar-app consent-screen click-through
// in a real browser, not just the network round trip itself.
static const uint32_t CDDV_OAUTH_TIMEOUT_MS = 300000;

static WebServer gCddvOAuthServer(CDDV_OAUTH_PORT);
static bool gCddvOAuthActive = false;
static char gCddvOAuthPin[7];  // 6 digits + NUL
static String gCddvOAuthRefreshToken;
static bool gCddvOAuthTokenReady = false;
static unsigned long gCddvOAuthDeadline = 0;

static void cddvOAuthStop() {
  if (gCddvOAuthActive) {
    gCddvOAuthServer.stop();
    gCddvOAuthActive = false;
  }
  gCddvOAuthPin[0] = '\0';
  gCddvOAuthRefreshToken = "";
  gCddvOAuthTokenReady = false;
  gCddvOAuthDeadline = 0;
}

// POST body: {"pin": "123456", "refresh_token": "..."} - see
// apps/caldav_sync/tools/google_auth.py's own matching POST.
static void cddvOAuthHandlePost() {
  String body = gCddvOAuthServer.arg("plain");
  JsonDocument doc;
  if (deserializeJson(doc, body)) {
    gCddvOAuthServer.send(400, "application/json", "{\"error\":\"bad json\"}");
    return;
  }
  const char *pin = doc["pin"] | "";
  const char *refreshToken = doc["refresh_token"] | "";
  if (strcmp(pin, gCddvOAuthPin) != 0) {
    if (usbDebug) cddvLogLn("CalDAV: OAuth callback PIN mismatch - ignored");
    gCddvOAuthServer.send(403, "application/json", "{\"error\":\"pin mismatch\"}");
    return;
  }
  if (!refreshToken[0]) {
    gCddvOAuthServer.send(400, "application/json", "{\"error\":\"missing refresh_token\"}");
    return;
  }
  gCddvOAuthRefreshToken = refreshToken;
  gCddvOAuthTokenReady = true;
  gCddvOAuthServer.send(200, "application/json", "{\"ok\":true}");
  if (usbDebug) cddvLogLn("CalDAV: OAuth refresh token received from google_auth.py");
}

// Called every caldavRelayLoop tick while a session is active: pumps the
// listener, relays the token to the Palm once it arrives, and tears the
// session down on delivery or timeout so the port isn't left open
// indefinitely.
static void cddvOAuthPump() {
  if (!gCddvOAuthActive) return;
  gCddvOAuthServer.handleClient();

  if (gCddvOAuthTokenReady) {
    RelayProto::Writer w(gEventBody, sizeof(gEventBody));
    w.str(gCddvOAuthRefreshToken);
    RelayProto::writeFrame(PhysicalSerial, CDDV_OAUTH_TOKEN, gEventBody, w.len);
    if (usbDebug) cddvLogLn("CalDAV: OAuth token relayed to Palm");
    cddvOAuthStop();
    cddvDrawStatus("Waiting...");
    return;
  }
  if (millis() > gCddvOAuthDeadline) {
    if (usbDebug) cddvLogLn("CalDAV: OAuth session timed out waiting for google_auth.py");
    cddvOAuthStop();
    cddvDrawStatus("Waiting...");
  }
}
#else
static void cddvOAuthStop() {}
static void cddvOAuthPump() {}
#endif  // WIRSA_FEATURE_CALDAV_GOOGLE

// ============================================================================
// OLED header/status - same shape as bwDrawHeader()/bwDrawStatus()
// (bitwarden_relay.cpp) - boxed header with live baud + WiFi icon, plain
// one-line status below.
// ============================================================================

static void cddvDrawHeader() {
  display.fillRect(0, 0, 128, 16, SSD1306_WHITE);
  display.drawLine(1, 1, 126, 1, SSD1306_BLACK);
  display.drawLine(1, 14, 126, 14, SSD1306_BLACK);
  display.drawLine(1, 1, 1, 14, SSD1306_BLACK);
  display.drawLine(126, 1, 126, 14, SSD1306_BLACK);

  display.setCursor(4, 4);
  display.setTextColor(SSD1306_BLACK);
  {
    String line = "CalDAV";
    display.print(line);
    display.print(padLeft(liveBaudStr(PhysicalSerial.baudRate()), 20 - line.length()));
  }
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 17);
  display.display();
  drawWifiStatusIcon();
}

static void cddvDrawStatus(const char *state) {
  display.fillRect(0, 17, 128, 47, SSD1306_BLACK);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 19);
  display.print(state);
  display.display();
}

// ============================================================================
// Mode entry/exit/loop - same shape as bitwarden_relay.cpp's own
// enterBitwardenRelayMode/exitBitwardenRelayMode/bitwardenRelayLoop.
// ============================================================================

void enterCalDavRelayMode() {
  menuMode = MODE_CALDAV_RELAY;
  setBinaryMode(true);

  SerialPrintLn("\r\nEntering CalDAV Relay Mode...");

  if (WiFi.status() != WL_CONNECTED) {
    if (!waitForWiFiConnection()) {
      SerialPrintLn("CalDAV Relay: WiFi not available - cancelled.");
      setBinaryMode(false);
      mainMenu(false);
      return;
    }
  }
  cddvWifiReady = true;

  if (usbDebug) cddvLogLn("CalDAV: mode entered - WiFi connected");

  // Start every session at the fixed idle rate rather than inheriting
  // whatever bauds[serialSpeed] the global setup() left PhysicalSerial at -
  // CDDV_HELLO negotiates up from here once the Palm connects. Same
  // pattern as sasd.cpp's own mode entry.
  PhysicalSerial.begin(CDDV_BAUD_IDLE, WIRSA_SERIAL_FORMAT, RXD2, TXD2);

  // The CalDAV Sync app can have bytes already sitting on PhysicalSerial
  // before WiRSa enters this mode (app already open, or a previous relay
  // session left something mid-frame), which RelayProto::readFrame would
  // otherwise try to interpret as a real frame header once
  // caldavRelayLoop starts polling - producing checksum-mismatch/dropped-
  // oversized-frame noise and possibly never resynchronizing. Discard
  // anything already buffered, same as the Palm's own CDDVSerialOpen()
  // does on its end (SrmReceiveFlush).
  {
    int drained = 0;
    while (PhysicalSerial.available()) { PhysicalSerial.read(); drained++; }
    if (usbDebug && drained > 0) cddvLog("CalDAV: drained %d stale byte(s) from serial at mode entry\r\n", drained);
  }

  SerialPrintLn("CalDAV Relay Active - waiting for the Palm");
  SerialPrintLn("Press +++ or BACK button to exit");
  cddvDrawHeader();
  // Serial-link key pair, generated once per boot (relay_proto.h).
  cddvDrawStatus("Preparing...");
  RelayProto::secureBegin();
  cddvDrawStatus("Waiting...");
}

void exitCalDavRelayMode() {
  setBinaryMode(false);
  RelayProto::secureReset();
  cddvOAuthStop();  // don't leave the OAuth listener open past this mode's own lifetime
  // If BACK/+++ is pressed while a negotiated (non-idle) rate is still
  // active - before CDDV_BYE arrives or the negotiate-timeout fires - the
  // mode's own revert-to-idle tick check never gets a chance to run, since
  // leaving the mode stops caldavRelayLoop from being called at all.
  // Restore the configured interactive rate explicitly instead (same as
  // sasd.cpp's own exit path), so PhysicalSerial doesn't stay at whatever
  // CalDAV Relay last negotiated and desync the next mode that assumes
  // bauds[serialSpeed] on this port (FTP Server, SASD, HotSync).
  PhysicalSerial.begin(bauds[serialSpeed], WIRSA_SERIAL_FORMAT, RXD2, TXD2);
  SerialPrintLn("\r\nExiting CalDAV Relay Mode...");
  if (usbDebug) cddvLogLn("CalDAV: mode exit");
}

void caldavRelayLoop() {
  readSwitches();
  if (BTNBK) {
    waitSwitches();
    exitCalDavRelayMode();
    msgFlag = true;
    mainMenu(false);
    return;
  }
  waitSwitches();

  // +++ escape on USB Serial only - PhysicalSerial carries the binary
  // protocol (same convention as every other WiRSa serial mode).
  static char escapeBuffer[4] = {0};
  static int escapePos = 0;
  static unsigned long lastEscapeTime = 0;
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '+') {
      if (millis() - lastEscapeTime > 1000) escapePos = 0;
      escapeBuffer[escapePos++] = c;
      lastEscapeTime = millis();
      if (escapePos >= 3) {
        SerialPrintLn("\r\nOK");
        exitCalDavRelayMode();
        msgFlag = true; mainMenu(false);
        return;
      }
    } else {
      escapePos = 0;
    }
  }

  if (!cddvWifiReady) {
    if (WiFi.status() == WL_CONNECTED) {
      cddvWifiReady = true;
      if (usbDebug) cddvLogLn("CalDAV: WiFi came up");
      SerialPrintLn("CalDAV Relay: WiFi connected - now servicing requests.");
      cddvDrawStatus("Waiting...");
    } else {
      return;  // nothing to service yet
    }
  }

  // Pumps the transient Google OAuth listener (if a session is active) -
  // runs every tick regardless of whether a serial frame is waiting, since
  // the token arrives over WiFi/HTTP, not the serial line.
  cddvOAuthPump();

  // If CDDV_HELLO negotiated a faster rate but the Palm never followed up
  // with SYNC_REQUEST, don't stay stuck listening at a rate a future
  // connection attempt won't know to expect. Runs every tick regardless of
  // whether data is currently available.
  if (PhysicalSerial.baudRate() != CDDV_BAUD_IDLE &&
      gCddvNegotiatedAt != 0 && (millis() - gCddvNegotiatedAt) > CDDV_NEGOTIATE_TIMEOUT_MS) {
    if (usbDebug) cddvLogLn("CalDAV: negotiated baud timed out waiting for sync request, reverting to idle rate");
    gCddvNegotiatedAt = 0;
    cddvRevertToIdleBaud();
    cddvDrawStatus("Waiting...");
  }

  if (!PhysicalSerial.available()) return;

  uint8_t type; size_t bodyLen;
  if (!RelayProto::readFrame(PhysicalSerial, type, gFrameBody, sizeof(gFrameBody), bodyLen,
                              60000 /* idle */, CDDV_BYTE_TIMEOUT_MS)) return;
  // Serial-link key exchange - always first on a new connection.
  if (RelayProto::handleKex(PhysicalSerial, type, gFrameBody, bodyLen)) return;

  switch (type) {
    case CDDV_SYNC_REQUEST:
      // "Syncing..." is shown mid-call (cddvHttpRequestOnce, once the
      // request actually goes out) - gCddvLastSyncOk (set at
      // cddvHandleSyncRequest's own real success exit) decides whether a
      // brief "Completed!" is honest to show before reverting; a failed
      // sync goes straight back to "Waiting..." instead.
      cddvDrawStatus("Connecting...");
      cddvHandleSyncRequestBigStack(gFrameBody, bodyLen);
      if (gCddvLastSyncOk) {
        cddvDrawStatus("Completed!");
        delay(1500);
      }
      // Does not revert baud here - Sync All runs multiple SYNC_REQUESTs
      // back-to-back over the same negotiated connection (apps/caldav_sync's
      // SyncEngineRunOne keepOpen parameter), so reverting after every one
      // would desync the Palm's next request. The connection only reverts
      // on a real CDDV_BYE (below) or the negotiate-timeout safety net if
      // the Palm goes silent.
      //
      // gCddvNegotiatedAt is refreshed here rather than left at its
      // original negotiation-time value: cddvHandleSyncRequestBigStack just
      // blocked for this whole slot's sync (a single iCloud fetch alone
      // can take 30-90+ seconds), almost certainly longer than
      // CDDV_NEGOTIATE_TIMEOUT_MS on its own - without this refresh, the
      // very next tick's timeout check would see an already-expired window
      // and force a premature revert mid-Sync-All.
      gCddvNegotiatedAt = millis();
      cddvDrawStatus("Waiting...");
      break;
    case CDDV_BYE:
      if (usbDebug) cddvLogLn("CalDAV: BYE received");
      // The only place a successful multi-sync session reverts to the
      // idle baud - the Palm sends a real BYE only after its own true
      // last sync (SyncEngineRunOne's keepOpen parameter), whether that
      // was a single "Sync Now" or the final slot of "Sync All".
      if (PhysicalSerial.baudRate() != CDDV_BAUD_IDLE) {
        gCddvNegotiatedAt = 0;
        cddvRevertToIdleBaud();
        cddvDrawStatus("Waiting...");
      }
      break;
    case CDDV_HELLO: {
      // Mirrors sasd.cpp's own SASD_CMD_SET_BAUD. Body: Palm's offered
      // rate (its own configured target, CDDVGetBaud on that side) as a
      // big-endian uint32. Capped at bauds[serialSpeed] (WiRSa's own
      // configured max), never higher.
      RelayProto::Reader r(gFrameBody, bodyLen);
      uint32_t offered = r.u32();
      if (!r.ok) {
        if (usbDebug) cddvLogLn("CalDAV: malformed HELLO (short body) - ignored");
        break;
      }
      uint32_t negotiated = offered;
      uint32_t maxAllowed = (uint32_t) bauds[serialSpeed];
      if (negotiated > maxAllowed) negotiated = maxAllowed;
      if (negotiated == 0) negotiated = CDDV_BAUD_IDLE;

      if (usbDebug) {
        cddvLog("CalDAV: baud negotiate - Palm offered %lu, capped to %lu (WiRSa config max)\r\n",
                      (unsigned long) offered, (unsigned long) negotiated);
      }

      RelayProto::Writer w(gEventBody, sizeof(gEventBody));
      w.u32(negotiated);
      // writeFrame flushes internally before returning (relay_proto.h's own
      // guarantee) - the ack is fully on the wire, at the OLD rate the Palm
      // is still listening at, before we touch PhysicalSerial below. Same
      // ordering sasd.cpp's own CMD_SET_BAUD handler uses.
      RelayProto::writeFrame(PhysicalSerial, CDDV_HELLO_ACK, gEventBody, w.len);

      PhysicalSerial.begin(negotiated, WIRSA_SERIAL_FORMAT, RXD2, TXD2);
      gCddvNegotiatedAt = millis();
      cddvDrawHeader();  // live baud readout - title bar reflects the negotiated rate
      break;
    }
#if WIRSA_FEATURE_CALDAV_GOOGLE
    case CDDV_OAUTH_START: {
      // No baud negotiation needed for this exchange - OAuth traffic is a
      // handful of short strings, not a calendar sync, so it runs at
      // whatever rate the connection is already at.
      cddvOAuthStop();  // clear any stale prior session before starting a new one
      // No Client ID check here - WiRSa doesn't need one at this stage.
      // google_auth.py carries its own copy for the browser-based auth
      // exchange; WiRSa only needs the Client ID later, at actual sync
      // time, read fresh off the wire (see cddvHandleSyncRequest).
      snprintf(gCddvOAuthPin, sizeof(gCddvOAuthPin), "%06ld", (long) random(0, 1000000));
      gCddvOAuthServer.on("/", HTTP_POST, cddvOAuthHandlePost);
      gCddvOAuthServer.begin();
      gCddvOAuthActive = true;
      gCddvOAuthDeadline = millis() + CDDV_OAUTH_TIMEOUT_MS;
      cddvDrawStatus("Google sign-in...");

      RelayProto::Writer w(gEventBody, sizeof(gEventBody));
      w.str(ipToString(WiFi.localIP()));
      w.u16(CDDV_OAUTH_PORT);
      w.str(String(gCddvOAuthPin));
      RelayProto::writeFrame(PhysicalSerial, CDDV_OAUTH_INFO, gEventBody, w.len);
      if (usbDebug) cddvLog("CalDAV: OAuth session started, pin=%s port=%u\r\n", gCddvOAuthPin, CDDV_OAUTH_PORT);
      break;
    }
#endif  // WIRSA_FEATURE_CALDAV_GOOGLE
    default:
      if (usbDebug) cddvLog("CalDAV: unknown/unsupported frame type 0x%02X ignored\r\n", type);
      break;
  }
}
