// BitWarden Relay mode: the mode itself (how it starts, runs and ends, as the shared relay skeleton, relay_mode.h, sees it). include/bitwarden_relay.h has the architecture,
// include/bitwarden_internal.h how the files divide the work.
#include "bitwarden_internal.h"

namespace BitWardenRelay {

// Set when BW_HELLO negotiates a faster rate; bitwardenRelayLoop's tick compares it with RelayTiming::NEGOTIATE_TIMEOUT_MS.
unsigned long gBwNegotiatedAt = 0;
// Totals for the BYE summary, reset when the mode is entered.
unsigned gBwSessOk = 0, gBwSessFail = 0, gBwSessItems = 0;
// The last fetch, reported once the Palm has taken (or missed) it: bwReportDelivery.
bool gBwProdOk = false;
unsigned gBwProdItems = 0, gBwProdFolders = 0, gBwProdOrgs = 0, gBwSkippedItems = 0;
// Forward declaration: the definition is with the OLED helpers further down, but bwHandleHello (well above) needs it for the live baud readout.

// Static, not stack, scratch buffers: the relay processes one frame or field at a time, so module-level reuse is safe and keeps multi-KB buffers off the loop task's stack (that pattern caused real
// stack-overflow crashes elsewhere, see the lazyFTP auto-reconnect history).
uint8_t gFrameBody[BW_MAX_BODY];
uint8_t gScratchCt[BW_MAX_BODY];
uint8_t gScratchPt[BW_MAX_BODY];
uint8_t gScratchMacInput[16 + BW_MAX_BODY];

// ---- OLED header and status: the boxed header and plain black status of hotsync.cpp's hsDrawHeader()/hsDrawStatus(). Status is a one-line toggle between "Waiting..." and "Syncing...": enough to show
// it is alive. ----

void bwDrawHeader() {
  display.fillRect(0, 0, 128, 16, SSD1306_WHITE);
  display.drawLine(1, 1, 126, 1, SSD1306_BLACK);
  display.drawLine(1, 14, 126, 14, SSD1306_BLACK);
  display.drawLine(1, 1, 1, 14, SSD1306_BLACK);
  display.drawLine(126, 1, 126, 14, SSD1306_BLACK);

  display.setCursor(4, 4);
  display.setTextColor(SSD1306_BLACK);
  // "Bitwarden" leaves room for the WiFi status icon. The live baud (not the configured value) is top right, as hotsync.cpp's hsDrawHeader() and sasd.cpp's sasdDrawStatusLines().
  {
    String line = "Bitwarden";
    display.print(line);
    display.print(padLeft(liveBaudStr(PhysicalSerial.baudRate()), 20 - line.length()));
  }
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 17);
  display.display();
  drawWifiStatusIcon();  // WiFi status on every title bar
}

void bwDrawStatus(const char *state) {
  display.fillRect(0, 17, 128, 47, SSD1306_BLACK);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 19);
  display.print(state);
  display.display();
}

// Reopens PhysicalSerial at RelayTiming::BAUD_IDLE and redraws the header so the live baud readout follows at once. Shared by the negotiation-timeout check and the post-sync cleanup in
// bitwardenRelayLoop, so the sequence is right in one place.
void bwRevertToIdleBaud() {
  PhysicalSerial.begin(RelayTiming::BAUD_IDLE, WIRSA_SERIAL_FORMAT, RXD2, TXD2);
  bwDrawHeader();
}


}  // namespace BitWardenRelay

using namespace BitWardenRelay;

static bool bwWifiReady = false;

// ---- Mode entry, exit and loop: the shape of SASD (enterSasdMode/exitSasdMode/sasdLoop): entered from the main menu, it services what the Palm sends until BACK or the +++ USB-serial escape. See
// bitwarden_relay.h for why it cannot be an always-pumped background mode like the FTP Server (one shared physical UART). ----

// Back to the main menu on the shared Exit Timers (mode_timer.h).
static ModeExitTimer bwTimer;

static void bwOnEnter() { gBwSessOk = gBwSessFail = gBwSessItems = 0; }

static RelayMode::Spec bwMode = {
  "BitWarden Relay", "BW", MODE_BITWARDEN, 'B', RelayTiming::BAUD_IDLE, BW_MAX_PALM_TYPE, /*needsWifi*/ true, &bwTimer, RelayTiming::NEGOTIATE_TIMEOUT_MS,
  &bwWifiReady, &gBwNegotiatedAt,
  bwDrawHeader, bwDrawStatus, bwRevertToIdleBaud, bwOnEnter, /*beforeLeave*/ nullptr, /*onLeave*/ nullptr, /*onFrame*/ nullptr, /*drainAtEntry*/ false,
  BW_HELLO_ACK, BW_PROTOCOL_VERSION, "WiRSa-BW/2",
};

void enterBitwardenRelayMode() { RelayMode::enter(bwMode); }
void exitBitwardenRelayMode() { RelayMode::leave(bwMode); }

// The frames the Palm sends and what answers each (RelayMode::dispatch warns about a type not in the table).
static void bwOnHello(const uint8_t *body, size_t len) { RelayMode::handleHello(bwMode, body, len); }

static void bwOnTrust(const uint8_t *body, size_t len) { bwHandleTrust(body, len); }

static void bwOnSync(const uint8_t *body, size_t len) {
  bwDrawStatus("Syncing...");
  bwHandleSyncBigStack(body, len);
  // Does not revert the baud here. After bwHandleSyncBigStack sends its SYNC_DONE, the Palm's wind-down (BwSyncRun's trailing BW_BYE, best-effort before it closes its port) still goes out at the
  // negotiated rate; reverting to RelayTiming::BAUD_IDLE first would listen at the wrong rate and read garbage right after the sync. Revert only on that real BW_BYE (below) or the negotiate-timeout
  // safety net if the Palm goes silent.
  // gBwNegotiatedAt is refreshed here, not left at its negotiation-time value: bwHandleSyncBigStack just blocked for the whole sync (hundreds of items), almost certainly longer than
  // RelayTiming::NEGOTIATE_TIMEOUT_MS, so the next tick would see an expired window and force a premature revert, racing the Palm's trailing BYE.
  gBwNegotiatedAt = millis();
  bwDrawStatus("Waiting...");
}

static void bwOnBye(const uint8_t *, size_t) {
  LOGI("BW: done - %u sync(s) ok, %u failed, %u item(s)\r\n", gBwSessOk, gBwSessFail, gBwSessItems);
  // The only place a session reverts to the idle baud: the Palm's BW_BYE arrives at the negotiated rate, so switching away only once it is received avoids garbling it.
  if (PhysicalSerial.baudRate() != RelayTiming::BAUD_IDLE) {
    gBwNegotiatedAt = 0;
    bwRevertToIdleBaud();
    bwDrawStatus("Waiting...");
  }
  modeTimerSessionEnded(bwTimer);
}

static const RelayMode::Frame kBwFrames[] = {
  { BW_HELLO, "HELLO", bwOnHello },
  { BW_TRUST, "TRUST", bwOnTrust },
  { BW_SYNC,  "SYNC",  bwOnSync },
  { BW_BYE,   "BYE",   bwOnBye },
};

void bitwardenRelayLoop() {
  if (RelayMode::tick(bwMode) != RelayMode::Tick::Go) return;

  uint8_t type; size_t bodyLen;
  if (!RelayMode::readFrame(bwMode, type, gFrameBody, sizeof(gFrameBody), bodyLen, RelayTiming::FRAME_IDLE_TIMEOUT_MS, RelayTiming::BYTE_TIMEOUT_MS)) return;

  RelayMode::dispatch(bwMode, kBwFrames, sizeof kBwFrames / sizeof kBwFrames[0], type, gFrameBody, bodyLen);
}
