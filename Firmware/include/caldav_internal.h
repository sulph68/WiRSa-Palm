// caldav_internal.h - what the files of CalDAV Relay mode share. Not for the rest of the firmware: that sees only caldav_relay.h.
//
//   caldav_util.cpp    scratch buffers, text helpers, URL joining, the kept TLS connection
//   caldav_ical.cpp    iCal text to events (uICAL), for CalDAV and iCloud
//   caldav_http.cpp    HTTP(S) requests and the multistatus reader (CalDAV, iCloud, CardDAV)
//   caldav_ics.cpp     ICS feed: the streaming parser
//   caldav_google.cpp  Google Calendar API and the OAuth handoff
//   caldav_diag.cpp    raw TLS diagnostic
//   caldav_sync.cpp    SYNC_REQUEST: fetch, convert, hand to the Palm
//   caldav_relay.cpp   the mode itself: how it starts, runs and ends (relay_mode.h), and the OLED
//
// Wire protocol constants below MUST match docs/protocol/Protocol-wire.md (section 10) and apps/caldav_sync/Serial.h and CalDAVSync.h exactly (hackmaster_y2035 repo): both
// sides were written against the same byte layout, not a shared header, so a drift is a silent wire incompatibility, not a compile error.
//
// One sync at a time: the scratch buffers and the per-sync state below are shared by the files and reset by cddvHandleSyncRequest.
#pragma once

#include "caldav_relay.h"
#include "globals.h"
#include "serial_io.h"
#include "display_menu.h"
#include "network.h"
#include "relay_proto.h"
#include "palm_ctl.h"
#include "mode_timer.h"
#include "relay_mode.h"
#include "relay_timing.h"
#include "relay_queue.h"
#include "carddav_card.h"  // CardDAV contacts: vCard trimming
#include "palm_text.h"      // UTF-8 to Palm Latin
#include <new>  // std::bad_alloc (a card too big for the heap is skipped)
#include <WiFi.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <WebServer.h>  // transient local listener for the Google OAuth token handoff
#include <ArduinoJson.h>  // Google token/Calendar API JSON, and the OAuth handoff POST body
#include <mbedtls/base64.h>  // cddvRawTlsDiagnostic's Basic Auth header: the API bitwarden_relay.cpp's bwBase64Encode uses, not the Arduino base64:: wrapper
#include <time.h>

// Flash budget: with all four CalDAV server types built in, the one file was ~3000 lines and pushed a build over its partition. Each switch below disables one server type's code (set from the user's
// own platformio.ini, gitignored and machine-specific, e.g. -DWIRSA_FEATURE_CALDAV_GOOGLE=0) and leaves the others. A disabled type's CDDV_TYPE_* still exists on the wire (so a saved Palm slot
// of that type corrupts nothing), but its SYNC_REQUEST gets a clear "not enabled in this build" ERR. Generic CalDAV and iCloud cannot be split (the same REPORT/multistatus machinery, differing in
// URL shape and auth), so they share one flag.
#ifndef WIRSA_FEATURE_CALDAV_REPORT
#define WIRSA_FEATURE_CALDAV_REPORT 1  // generic CalDAV + iCloud (shared REPORT/multistatus code)
#endif
#ifndef WIRSA_FEATURE_CALDAV_ICS
#define WIRSA_FEATURE_CALDAV_ICS 1     // plain ICS Feed (e.g. Google's "secret address in iCal format")
#endif
#ifndef WIRSA_FEATURE_CALDAV_GOOGLE
#define WIRSA_FEATURE_CALDAV_GOOGLE 1  // Google Calendar API via OAuth sign-in
#endif
// CardDAV contacts (iCloud Contacts, generic CardDAV) ride on the same REPORT/multistatus reader, so they need WIRSA_FEATURE_CALDAV_REPORT. 0 = those types answer "not enabled in this build".
// Design: palmos_sdk apps/carddav_sync.md.
#ifndef WIRSA_FEATURE_CARDDAV
#define WIRSA_FEATURE_CARDDAV 1
#endif
#if WIRSA_FEATURE_CARDDAV && !WIRSA_FEATURE_CALDAV_REPORT
#undef WIRSA_FEATURE_CARDDAV
#define WIRSA_FEATURE_CARDDAV 0
#endif

// uICAL (RRULE, timezone and occurrence expansion) is needed by generic CalDAV/iCloud (parseICalAndEmit) and ICS Feed (cddvIcsOnComponent and friends), NOT by Google, whose Calendar API expands
// occurrences server-side (singleEvents=true). If both types that need it are disabled the #include goes, and since PlatformIO finds libraries by the headers a translation unit includes, the whole
// uICAL library stays out of the build.
#if WIRSA_FEATURE_CALDAV_REPORT || WIRSA_FEATURE_CALDAV_ICS
#include "uICAL/cppstl.h"
#include "uICAL/types.h"
#include "uICAL/error.h"
#include "uICAL/util.h"
#include "uICAL/tzmap.h"
#include "uICAL/vevent.h"
#include "uICAL/veventiter.h"
#include "uICAL/calendarentry.h"  // CalendarEntry_ptr::start()/end() need the full definition, not VEventIter's forward declaration
#include "uICAL/vobject.h"
#include "uICAL/vobjectstream.h"
#include "uICAL/vline.h"
#include "uICAL/vlinestream.h"
#include "uICAL/datetime.h"
#include "uICAL/rrule.h"
#endif

extern bool usbDebug;
extern HardwareSerial PhysicalSerial;

// Keep the TLS connection between SYNC_REQUESTs (caldav_util.cpp)
#ifndef WIRSA_CDDV_KEEPALIVE
#define WIRSA_CDDV_KEEPALIVE 1
#endif

namespace CalDavRelay {


// ---- Wire protocol constants (apps/caldav_sync/Serial.h) ----

// The type numbers follow Protocol-wire.md section 6: the common frames at 0x01-0x06, CalDAV's own in 0x20-0x2F, the frames the Palm sends first (the junk filter's bound, CDDV_MAX_PALM_TYPE, is the highest).
enum : uint8_t {
  CDDV_HELLO = 0x01, CDDV_HELLO_ACK = 0x02, CDDV_BYE = 0x03,
  CDDV_ACK = 0x04,
  CDDV_SYNC_DONE = 0x05,
  CDDV_ERR = 0x06,         // WiRSa -> Palm: code u8, message str
  CDDV_SYNC_REQUEST = 0x20, CDDV_OAUTH_START = 0x21,
  CDDV_EVENT_ITEM = 0x22,
  // CardDAV contacts (WIRSA_FEATURE_CARDDAV), WiRSa -> Palm, each acked with CDDV_ACK like CDDV_EVENT_ITEM. Bodies: Protocol-wire.md section 10.
  CDDV_CONTACT_ITEM = 0x23, CDDV_GROUP_ITEM = 0x24,
  CDDV_OAUTH_INFO = 0x25, CDDV_OAUTH_TOKEN = 0x26
};
static const uint8_t CDDV_MAX_PALM_TYPE = CDDV_OAUTH_START;
static const uint8_t CDDV_PROTOCOL_VERSION = 2;   // 2: the type numbers of Protocol-wire.md section 6 (the Palm app refuses a WiRSa that reports less)

// The code of an ERR frame (0 is never used)
enum : uint8_t { CDDV_ERR_BAD_REQUEST = 1, CDDV_ERR_FAILED = 2 };
static const size_t CDDV_ERR_TEXT_MAX = 96;   // the longest message the Palm's error alert shows (5 lines); cddvSendSyncError cuts a longer one

// apps/caldav_sync/CalDAVSync.h's CDDVServerType
enum : uint8_t {
  CDDV_TYPE_CALDAV = 0, CDDV_TYPE_ICLOUD = 1, CDDV_TYPE_GOOGLE = 2, CDDV_TYPE_ICSFEED = 3,
  CDDV_TYPE_ICLOUD_CONTACTS = 4, CDDV_TYPE_CARDDAV = 5   // contacts (WIRSA_FEATURE_CARDDAV)
};

static const size_t CDDV_MAX_BODY = 3192;  // apps/caldav_sync/Serial.h cddvMaxFrameBody - must match exactly
// Download-ahead (relay_queue.h): on by default, effective only with PSRAM. 0 = frames are sent with their ack wait as produced (the old path).
#ifndef WIRSA_CDDV_QUEUE
#define WIRSA_CDDV_QUEUE 1
#endif

// Baud negotiation, as SASD (sasd.cpp's SASD_CMD_SET_BAUD). Every fresh connection (this relay's PhysicalSerial and the Palm's cddvBaudIdle in apps/caldav_sync/Serial.h) starts at this fixed, safe
// rate, since a rate cannot be proposed reliably when the two sides might already differ; CDDV_HELLO/CDDV_HELLO_ACK negotiate up from it before SYNC_REQUEST. If the Palm negotiates a faster rate but
// sends no SYNC_REQUEST within this window, revert to RelayTiming::BAUD_IDLE so the next connection is not stuck listening at an unexpected rate.

// past: None/1wk/2wk/1mo/2mo/3mo (cddvPastWindow); future: 2wk/1mo/3mo/6mo/1yr (cddvFutureWindow), see apps/caldav_sync/CalDAVSync.h. Months are approximated as 30 days: these are rough sync
// boundaries on the Palm side, not exact calendar months.
static const uint16_t kPastWindowDays[]   = {0, 7, 14, 30, 60, 90};
static const uint16_t kFutureWindowDays[] = {14, 30, 90, 180, 365};
static const int kPastWindowCount = sizeof(kPastWindowDays) / sizeof(kPastWindowDays[0]);
static const int kFutureWindowCount = sizeof(kFutureWindowDays) / sizeof(kFutureWindowDays[0]);

// The Palm epoch (1904-01-01) is this many seconds before the Unix epoch (1970-01-01). Every uICAL seconds_t is Unix-epoch; every CDDV_EVENT_ITEM utcStart/utcEnd on the wire is Palm-epoch (as
// Palm OS time APIs, e.g. TimGetSeconds()/TimUTCToTimeZone()).
static const uint32_t UNIX_TO_PALM_EPOCH = 2082844800UL;
static const uint16_t CDDV_OAUTH_PORT = 8090;                 // the Google sign-in listener (caldav_google.cpp)
static const bool CDDV_DIAGNOSTIC_RAW_TLS = false;            // see caldav_diag.cpp

// structures and callback types the files exchange
#if WIRSA_FEATURE_CALDAV_REPORT || WIRSA_FEATURE_CALDAV_ICS
struct CalDavMasterEvent {
  // Set only for a REPEATING master. On a feed with many one-off events the full uICAL VEvent/DateTime/TZ/RRule graph (several heap allocations each, one per master, held for the ENTIRE feed, see
  // cddvIcsFinishParse's two-phase design) was the dominant memory cost and is dead weight for a non-repeating event, whose one occurrence is its own start/end, computed once at parse time. For those,
  // cddvIcsOnComponent copies out title/startEpoch/endEpoch/allDay below and frees the graph at once, rather than hold it until cddvIcsFinishParse at the end of the feed.
  uICAL::VEvent_ptr vevent;
  String title;              // only used when !isRepeating - see above
  uICAL::seconds_t startEpoch = 0;  // only used when !isRepeating
  uICAL::seconds_t endEpoch = 0;    // only used when !isRepeating
  bool allDay = false;              // only used when !isRepeating
  String location;
  String description;
  bool cancelled;
  bool isRepeating;  // RRULE property was present on this VEVENT - NOT
                      // vevent->rrule's own nullability (VEvent always builds an RRule, even a no-op one, for a non-recurring event; see vevent.cpp).
};
struct CalDavOverrideEvent {
  uICAL::seconds_t recurrenceIdEpoch;
  // Shared with parseICalAndEmit (CalDAV/iCloud): its masters and overrides are per-call locals (one complete single-event <calendar-data> block at a time, discarded once emitted), so it always populates
  // and reads this field. The ICS Feed path (cddvIcsOnComponent/cddvIcsFinishParse) persists gCddvIcsOverrides for the WHOLE feed, for the same memory reason as CalDavMasterEvent's title/startEpoch/
  // endEpoch/allDay: an override is only a lookup by date for a few fields and never needs VEventIter/RRule/TZ, so the ICS path leaves this unset and uses the light fields below, freeing the graph at once.
  uICAL::VEvent_ptr vevent;
  String title;                     // ICS path only - see above
  uICAL::seconds_t startEpoch = 0;  // ICS path only
  uICAL::seconds_t endEpoch = 0;    // ICS path only
  bool allDay = false;              // ICS path only
  String location;
  String description;
  bool cancelled;
};
#endif


// Always defined, whatever server types are built: every type's fetch (parseICalAndEmit, cddvFetchIcsFeed, cddvFetchGoogleCalendar) shares this sink signature.
typedef std::function<bool(const String &uid, const String &title, const String &location,
                            uint32_t utcStartPalm, uint32_t utcEndPalm, bool allDay,
                            const String &description, bool isRepeating)> CalDavEventSink;

typedef bool (*CddvBodyChunkFn)(const char *data, size_t len);

// ---- Shared state (defined in the file that owns it; a sync resets the per-sync part) ----
extern uint8_t gFrameBody[CDDV_MAX_BODY];
extern uint8_t gAckScratch[CDDV_MAX_BODY];
extern uint8_t gEventBody[CDDV_MAX_BODY];
extern bool cddvWifiReady;
extern bool gCddvLastSyncOk;
extern unsigned long gCddvNegotiatedAt;
extern HTTPClient *gKeepHttp;
extern unsigned long gKeepLastUsed;
extern CalDavEventSink *gActiveSink;
extern bool gSinkAborted;
extern unsigned gCddvSessOk, gCddvSessFail, gCddvSessEvents, gCddvSessContacts;
extern bool gCddvProdOk, gCddvProdContacts;
extern unsigned gCddvProdCount;
extern unsigned gCddvEventsSent;  // verbose-logging stage counter, reset per SYNC_REQUEST
extern time_t gCddvRangeStart, gCddvRangeEnd;
#if WIRSA_FEATURE_CALDAV_REPORT
extern const char *gCddvDataTag;
extern void (*gCddvOnBlock)(const String &block);
#endif

extern String gCddvMultistatusAccum;
extern unsigned gCddvBlocksExtracted;
#if WIRSA_FEATURE_CALDAV_GOOGLE
extern WebServer gCddvOAuthServer;
extern bool gCddvOAuthActive;
extern char gCddvOAuthPin[7];  // 6 digits + NUL
extern String gCddvOAuthRefreshToken;
extern bool gCddvOAuthTokenReady;
extern unsigned long gCddvOAuthDeadline;
#endif


// ---- Functions the files share (each defined in the file named) ----

// ---- caldav_util.cpp ----
void cddvKeepClose();
WiFiClientSecure *cddvKeepClient(const String &host, uint16_t port, bool &reused);
void cddvRevertToIdleBaud();
#if WIRSA_FEATURE_CALDAV_REPORT
String joinUrl(const String &server, const String &path);
String joinUrlICloud(const String &shard, const String &uuid, const String &calName);
#endif

void formatUtcBasic(time_t t, char *out, size_t outCap);
String xmlUnescape(const String &in);
#if WIRSA_FEATURE_CALDAV_REPORT || WIRSA_FEATURE_CALDAV_ICS
String icalUnescapeText(const String &in);
#endif
String maskSensitive(const String &s);
bool cddvLooksLikeOpaqueId(const String &seg);
String maskUrlForLog(const String &url);
#if WIRSA_FEATURE_CALDAV_REPORT || WIRSA_FEATURE_CALDAV_ICS
void splitCommaList(const String &value, std::vector<String> &out);
#endif

#if WIRSA_FEATURE_CALDAV_ICS
bool cddvRruleFindParam(const String &rrule, const char *key, String &outValue);
#endif


// ---- caldav_ical.cpp ----
#if WIRSA_FEATURE_CALDAV_REPORT
bool parseICalAndEmit(const String &icalText, uICAL::seconds_t rangeStartUnix,
                              uICAL::seconds_t rangeEndUnix, CalDavEventSink sink);
#endif


// ---- caldav_http.cpp ----
String cddvUrlHost(const String &url);
uint16_t cddvUrlPort(const String &url, bool isHttps);
String cddvUrlPath(const String &url);
bool cddvHttpRequestOnce(const String &url, const char *method, const String &body,
                                 const char *user, const char *pass,
                                 const char *contentType, const char *extraHeaderName,
                                 const char *extraHeaderValue, bool browserHeaders,
                                 CddvBodyChunkFn onBodyChunk,
                                 int &codeOut, bool &anyBodyDeliveredOut, String &errOut);
bool cddvHttpRequest(const String &url, const char *method, const String &body,
                             const char *user, const char *pass,
                             const char *contentType, const char *extraHeaderName,
                             const char *extraHeaderValue, bool browserHeaders,
                             CddvBodyChunkFn onBodyChunk,
                             int &codeOut, String &errOut);
#if WIRSA_FEATURE_CALDAV_REPORT
void cddvOnCalendarDataBlock(const String &block);
void extractCalendarDataBlocksIncremental();
bool cddvOnMultistatusChunk(const char *data, size_t len);
#endif


// ---- caldav_ics.cpp ----
#if WIRSA_FEATURE_CALDAV_ICS
bool cddvIcsUseLine(const uICAL::string parent, const uICAL::string line);
void cddvIcsBeginParse();
bool cddvIcsHeapCritical();
void cddvIcsOnComponent(const String &componentText);
bool cddvIcsFinishParse(uICAL::seconds_t rangeStartUnix, uICAL::seconds_t rangeEndUnix, CalDavEventSink sink);
void cddvIcsExtractComponents();
bool cddvOnIcsChunk(const char *data, size_t len);
bool cddvFetchIcsFeed(const String &feedUrl, time_t rangeStart, time_t rangeEnd,
                              CalDavEventSink sink, String &errOut);
#endif


// ---- caldav_google.cpp ----
#if WIRSA_FEATURE_CALDAV_GOOGLE
String cddvUrlEncode(const String &s);
void cddvFormatRfc3339(time_t t, char *out, size_t outCap);
int64_t cddvDaysFromCivil(int y, int m, int d);
bool cddvParseRfc3339(const String &s, time_t &outEpoch);
bool cddvOnGoogleChunk(const char *data, size_t len);
bool cddvGoogleRefreshAccessToken(const String &refreshToken, const String &clientId,
                                          const String &clientSecret,
                                          String &outAccessToken, String &errOut);
bool cddvFetchGoogleCalendar(const String &refreshToken, const String &clientId, const String &clientSecret,
                                     const String &calendarId, time_t rangeStart, time_t rangeEnd,
                                     CalDavEventSink sink, String &errOut);
void cddvOAuthStop();
void cddvOAuthHandlePost();
void cddvOAuthPump();
#endif

#if !(WIRSA_FEATURE_CALDAV_GOOGLE)
void cddvOAuthStop();
void cddvOAuthPump();
#endif


// ---- caldav_diag.cpp ----
bool cddvRawTlsDiagnostic(const String &url, const char *user, const char *pass,
                                  const String &reportBody, String &resultOut);

// ---- caldav_sync.cpp ----
void cddvSendSyncError(const char *msg, uint8_t code = CDDV_ERR_FAILED);
bool cddvSendEventItem(const String &uid, const String &title, const String &location,
                               uint32_t utcStartPalm, uint32_t utcEndPalm, bool allDay,
                               const String &description, bool isRepeating);
#if WIRSA_FEATURE_CARDDAV
String cddvStr(const std::string &s);
bool cddvSendAcked(uint8_t type, size_t len, const char *what, const std::string &uid);
bool cddvSendContact(const CardDav::Card &c);
bool cddvSendGroup(const CardDav::Card &c);
void cddvOnAddressDataBlock(const String &block);
String joinUrlICloudContacts(const String &shard, const String &dsid, const String &book);
#endif

void cddvHandleSyncRequest(const uint8_t *body, size_t bodyLen);
void cddvReportSyncDelivery(bool queued);
void cddvHandleSyncRequestTask(void *);
void cddvHandleSyncRequestBigStack(const uint8_t *body, size_t bodyLen);


// ---- caldav_relay.cpp ----
void cddvDrawHeader();
void cddvDrawStatus(const char *state);

}  // namespace CalDavRelay
