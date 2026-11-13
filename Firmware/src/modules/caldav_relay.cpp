// CalDAV Relay mode: the mode itself (how it starts, runs and ends, as the shared relay skeleton, relay_mode.h, sees it). include/caldav_relay.h has the architecture, include/caldav_internal.h how
// the files divide the work.
#include "caldav_internal.h"

namespace CalDavRelay {

// ---- OLED header and status: the shape of bwDrawHeader()/bwDrawStatus() (bitwarden_relay.cpp): a boxed header with live baud and WiFi icon, a plain one-line status below ----

void cddvDrawHeader() {
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

void cddvDrawStatus(const char *state) {
  display.fillRect(0, 17, 128, 47, SSD1306_BLACK);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 19);
  display.print(state);
  display.display();
}

}  // namespace CalDavRelay

using namespace CalDavRelay;


// ---- Mode entry, exit and loop: the shape of bitwarden_relay.cpp's enterBitwardenRelayMode/exitBitwardenRelayMode/bitwardenRelayLoop ----

// Back to the main menu on the shared Exit Timers (mode_timer.h).
static ModeExitTimer cddvTimer;

// What is CalDAV's own in the skeleton (relay_mode.h): its counters at entry, its kept connection and OAuth listener on the way out.
static void cddvOnEnter() { gCddvSessOk = gCddvSessFail = gCddvSessEvents = gCddvSessContacts = 0; }
static void cddvOnLeave() {
  cddvKeepClose();
  cddvOAuthStop();  // don't leave the OAuth listener open past this mode's own lifetime
}

static RelayMode::Spec cddvMode = {
  "CalDAV Relay", "CalDAV", MODE_CALDAV_RELAY, 'A', RelayTiming::BAUD_IDLE, CDDV_MAX_PALM_TYPE, /*needsWifi*/ true, &cddvTimer, RelayTiming::NEGOTIATE_TIMEOUT_MS,
  &cddvWifiReady, &gCddvNegotiatedAt,
  cddvDrawHeader, cddvDrawStatus, cddvRevertToIdleBaud, cddvOnEnter, /*beforeLeave*/ nullptr, cddvOnLeave, /*onFrame*/ nullptr, /*drainAtEntry*/ true,
  CDDV_HELLO_ACK, /*helloVersion*/ CDDV_PROTOCOL_VERSION, "WiRSa-DAV/2",
};

void enterCalDavRelayMode() {
  cddvMode.helloConfirm = true;   // the confirmed switch (relay_mode.h): the port moves only when the Palm has read the HELLO reply
  RelayMode::enter(cddvMode);
}
void exitCalDavRelayMode() { RelayMode::leave(cddvMode); }

// The frames the Palm sends and what answers each (RelayMode::dispatch warns about a type not in the table).
static void cddvOnHello(const uint8_t *body, size_t len) { RelayMode::handleHello(cddvMode, body, len); }

static void cddvOnSyncRequest(const uint8_t *body, size_t len) {
  // "Syncing..." is shown mid-call (cddvHttpRequestOnce, once the request goes out). gCddvLastSyncOk (set at cddvHandleSyncRequest's real success exit) decides whether a brief "Completed!" is honest;
  // a failed sync goes straight back to "Waiting...".
  cddvDrawStatus("Connecting...");
  cddvHandleSyncRequestBigStack(body, len);
  if (gCddvLastSyncOk) {
    cddvDrawStatus("Completed!");
    delay(1500);
  }
  // Does not revert the baud here: Sync All runs several SYNC_REQUESTs back to back on the same negotiated connection (apps/caldav_sync's SyncEngineRunOne keepOpen), and reverting after each would desync
  // the Palm's next request. It reverts on a real CDDV_BYE (below) or the negotiate-timeout safety net if the Palm goes silent.
  // gCddvNegotiatedAt is refreshed here, not left at its negotiation-time value: cddvHandleSyncRequestBigStack just blocked for the whole slot (one iCloud fetch alone can take 30-90+ s), almost
  // certainly longer than RelayTiming::NEGOTIATE_TIMEOUT_MS, so the next tick would see an expired window and force a premature revert mid-Sync-All.
  gCddvNegotiatedAt = millis();
  cddvDrawStatus("Waiting...");
}

static void cddvOnBye(const uint8_t *, size_t) {
  LOGI("CalDAV: done - %u sync(s) ok, %u failed, %u event(s), %u contact(s)\r\n",
       gCddvSessOk, gCddvSessFail, gCddvSessEvents, gCddvSessContacts);
  cddvKeepClose();   // the Palm's last request is done: drop the kept connection
  // The only place a successful multi-sync session reverts to the idle baud: the Palm sends a real BYE only after its true last sync (SyncEngineRunOne's keepOpen), a single "Sync Now" or the final
  // slot of "Sync All".
  if (PhysicalSerial.baudRate() != RelayTiming::BAUD_IDLE) {
    gCddvNegotiatedAt = 0;
    cddvRevertToIdleBaud();
    cddvDrawStatus("Waiting...");
  }
  modeTimerSessionEnded(cddvTimer);
}

#if WIRSA_FEATURE_CALDAV_GOOGLE
static void cddvOnOAuthStart(const uint8_t *, size_t) {
  // No baud negotiation here: OAuth traffic is a handful of short strings, not a calendar sync, so it runs at whatever rate the connection is at.
  cddvOAuthStop();  // clear any stale prior session before starting a new one
  // No Client ID check: WiRSa does not need one at this stage. google_auth.py carries its own for the browser exchange; WiRSa needs it only at sync time, read fresh off the wire (see cddvHandleSyncRequest).
  snprintf(gCddvOAuthPin, sizeof(gCddvOAuthPin), "%06ld", (long) random(0, 1000000));
  gCddvOAuthServer.on("/", HTTP_POST, cddvOAuthHandlePost);
  gCddvOAuthServer.begin();
  gCddvOAuthActive = true;
  gCddvOAuthDeadline = millis() + RelayTiming::CDDV_OAUTH_TIMEOUT_MS;
  cddvDrawStatus("Google sign-in...");

  RelayProto::Writer w(gEventBody, sizeof(gEventBody));
  w.str(ipToString(WiFi.localIP()));
  w.u16(CDDV_OAUTH_PORT);
  w.str(String(gCddvOAuthPin));
  RelayProto::writeFrame(PhysicalSerial, CDDV_OAUTH_INFO, gEventBody, w.len);
  LOGV("CalDAV: OAuth session started, pin=%s port=%u\r\n", gCddvOAuthPin, CDDV_OAUTH_PORT);
}
#endif  // WIRSA_FEATURE_CALDAV_GOOGLE

static const RelayMode::Frame kCddvFrames[] = {
  { CDDV_SYNC_REQUEST, "SYNC_REQUEST", cddvOnSyncRequest },
  { CDDV_BYE,          "BYE",          cddvOnBye },
  { CDDV_HELLO,        "HELLO",        cddvOnHello },
#if WIRSA_FEATURE_CALDAV_GOOGLE
  { CDDV_OAUTH_START,  "OAUTH_START",  cddvOnOAuthStart },
#endif
};

void caldavRelayLoop() {
  if (RelayMode::tick(cddvMode) != RelayMode::Tick::Go) return;

  uint8_t type; size_t bodyLen;
  if (!RelayMode::readFrame(cddvMode, type, gFrameBody, sizeof(gFrameBody), bodyLen, RelayTiming::FRAME_IDLE_TIMEOUT_MS, RelayTiming::BYTE_TIMEOUT_MS)) return;

  RelayMode::dispatch(cddvMode, kCddvFrames, sizeof kCddvFrames / sizeof kCddvFrames[0], type, gFrameBody, bodyLen);
}
