#include "network.h"
#include "globals.h"
#include "serial_io.h"
#include "display_menu.h"    // showMessage()/readSwitches()/waitSwitches() - waitForWiFiConnection()'s own UI
#include <Adafruit_SSD1306.h>
#include <ESPmDNS.h>

// LED_PIN/RXD2/TXD2 come from globals.h, included directly like every
// other module. RTS/CTS/DTR/DCD/DSR/RI aren't wired on this hardware and
// have no pin defines here (DCD/DSR/RI were removed - dead leftover modem/
// telnet-BBS signaling, see globals.h's own pin-definitions comment).

// Result codes (avoid R_OK conflict with system header)
#define R_MODEM_OK 0
#define R_ERROR 4
#define R_OK_STAT 200

// Forward declarations for functions still in main.cpp
extern void showMessage(String message);
extern void firmwareCheck();

// WiFi symbols for display
extern const uint8_t PROGMEM wifi_symbol[];

// Global variables (defined in main.cpp)
extern Adafruit_SSD1306 display;
extern String ssid, password;
extern MDNSResponder mdns;
extern byte serialSpeed;
extern const int bauds[];
extern HardwareSerial PhysicalSerial;

// One-time setup that must run exactly once per fresh association - called
// by maintainWiFi() and waitForWiFiConnection() alike so the two paths
// can't double-run mdns.begin()/tcpServer.begin() against each other.
static void wifiOnConnected() {
  // Disables modem-sleep: a sleeping radio can silently drop association
  // mid-handshake during a heavier network operation (e.g. TLS against a
  // remote with a larger cert chain/more round trips). Set here so it
  // re-applies on every fresh association, not just the first.
  //
  // TX power is left at the ESP32 core's own default rather than maxed -
  // max power draws more current per TX burst with no range benefit on an
  // already-strong link, and the extra draw risks a brief brownout/
  // self-interference right as a handshake bursts packets.
  WiFi.setSleep(false);

  SerialPrint("CONNECTED TO "); SerialPrintLn(WiFi.SSID());
  SerialPrint("IP ADDRESS: "); SerialPrintLn(ipToString(WiFi.localIP()));
  // A tcpServer used to bind and listen here, but nothing in the live
  // firmware ever called tcpServer.available()/hasClient()/accept() to pull
  // a connection off it - that consumption only existed in the old
  // AT-command modem/telnet-BBS logic (main.cpp.backup, not in the live
  // build). tcpServer/tcpServerPort/tcpClient/consoleClient/
  // consoleConnected and the telnet EEPROM flag (settings.cpp) have since
  // all been removed outright as dead weight, not just left unbound here.
  mdns.begin("WiRSa"); // wirsa.local - also advertises the FTP server
  mdns.addService("ftp", "tcp", 21);
  SerialPrintLn("FTP SERVER: ftp://wirsa.local (once started)");
  showWifiIcon();
  firmwareCheck();
}

// Get signal strength level (0-4) from RSSI
// Returns: 0 = no signal, 1 = weak, 2 = fair, 3 = good, 4 = excellent
int getSignalBars() {
  if (WiFi.status() != WL_CONNECTED) return 0;

  int rssi = WiFi.RSSI();
  if (rssi > -50) return 4;       // Excellent
  else if (rssi > -60) return 3;  // Good
  else if (rssi > -70) return 2;  // Fair
  else if (rssi > -80) return 1;  // Weak
  else return 0;                   // Very weak / no signal
}

// Draw a dithered (gray) bar by alternating pixels in a checkerboard pattern
void drawDitheredBar(int x, int y, int w, int h) {
  for (int py = y; py < y + h; py++) {
    for (int px = x; px < x + w; px++) {
      // Checkerboard pattern: draw pixel if (px + py) is even
      if ((px + py) % 2 == 0) {
        display.drawPixel(px, py, SSD1306_BLACK);
      }
    }
  }
}

void showWifiIcon() {
  // Icon position and size (in header area which has white background)
  const int iconX = 72;
  const int iconY = 4;
  const int iconW = 11;
  const int iconH = 8;

  // Clear the icon area first (fill with white since header is white)
  display.fillRect(iconX, iconY, iconW, iconH, SSD1306_WHITE);

  if (WiFi.status() == WL_CONNECTED) {
    digitalWrite(LED_PIN, LOW);  // LED on

    int bars = getSignalBars();

    // Draw signal bars (drawing in BLACK on white header background)
    // 4 bars, each 2 pixels wide with 1 pixel gap
    // Heights: bar1=2px, bar2=4px, bar3=6px, bar4=8px (from bottom at iconY+iconH)

    int barWidth = 2;
    int gap = 1;
    int baseY = iconY + iconH;  // Bottom of icon area

    // Bar 1 (shortest, leftmost) - height 2
    if (bars >= 1) {
      display.fillRect(iconX, baseY - 2, barWidth, 2, SSD1306_BLACK);
    } else {
      drawDitheredBar(iconX, baseY - 2, barWidth, 2);
    }

    // Bar 2 - height 4
    if (bars >= 2) {
      display.fillRect(iconX + barWidth + gap, baseY - 4, barWidth, 4, SSD1306_BLACK);
    } else {
      drawDitheredBar(iconX + barWidth + gap, baseY - 4, barWidth, 4);
    }

    // Bar 3 - height 6
    if (bars >= 3) {
      display.fillRect(iconX + 2*(barWidth + gap), baseY - 6, barWidth, 6, SSD1306_BLACK);
    } else {
      drawDitheredBar(iconX + 2*(barWidth + gap), baseY - 6, barWidth, 6);
    }

    // Bar 4 (tallest, rightmost) - height 8
    if (bars >= 4) {
      display.fillRect(iconX + 3*(barWidth + gap), baseY - 8, barWidth, 8, SSD1306_BLACK);
    } else {
      drawDitheredBar(iconX + 3*(barWidth + gap), baseY - 8, barWidth, 8);
    }

  } else {
    digitalWrite(LED_PIN, HIGH); // LED off
    // Draw X for no connection
    display.drawLine(iconX + 1, iconY + 1, iconX + iconW - 2, iconY + iconH - 2, SSD1306_BLACK);
    display.drawLine(iconX + iconW - 2, iconY + 1, iconX + 1, iconY + iconH - 2, SSD1306_BLACK);
  }
  display.display();
}

// Every WiFi.disconnect() call in the firmware funnels through here - the
// one place to add/tune disconnect-time behavior (e.g. WiFi.disconnect(true)
// to also erase stored credentials, or debug logging on every disconnect)
// so a change here applies everywhere at once: retryWiFi() below, WiFi
// Setup's pre-scan and pre-test-connect disconnects, and disconnectWiFi()
// itself. Mirrors wifiBeginConnect()'s own role for WiFi.begin().
void wifiDisconnect() {
  WiFi.disconnect();
}

// Convenience wrapper for callers that also want the UI settled afterward
// (a brief pause + an immediate icon redraw showing "disconnected") rather
// than just the raw disconnect - currently no caller needs that combination
// (every existing disconnect site has its own different follow-up timing),
// kept as the natural place for one that does.
void disconnectWiFi() {
  wifiDisconnect();
  delay(500);
  showWifiIcon();
}

String ipToString(IPAddress ip) {
  char s[16];
  sprintf(s, "%d.%d.%d.%d", ip[0], ip[1], ip[2], ip[3]);
  return s;
}

// Every WiFi.begin() call in the firmware funnels through here - see the
// declaration comment in network.h for why. Currently a thin wrapper; add
// any connection-time tuning (TX power, PHY mode, channel/BSSID hints, a
// forced WiFi.mode(WIFI_STA), etc.) here and it reaches every caller.
void wifiBeginConnect(const char *connSsid, const char *connPassword) {
  WiFi.begin(connSsid, connPassword);
  WiFi.setTxPower(WIFI_POWER_15dBm);
}

void wifiReconnectToSaved() {
  if (ssid.length() == 0) return;  // matches every inline call site this
                                    // replaced - deliberately NOT also
                                    // requiring password.length()>0, since a
                                    // saved open network has an empty one
  wifiBeginConnect(ssid.c_str(), password.c_str());
}

// ============================================================================
// Background WiFi connection manager
// ============================================================================
// maintainWiFi() is called once per loop() iteration and never blocks: it
// only calls WiFi.begin() (async - returns immediately, ESP32's own WiFi
// task does the actual association) and polls WiFi.status(). This keeps
// WiFi up proactively for every WiFi-requiring feature (FTP/SLIP/
// BitWarden) rather than each one connecting on demand.
//
// Once configured (SSID+password set), this retries every WIFIMGR_RETRY_MS
// for up to one WIFIMGR_GRACE_MS window (1 minute) - if still not
// connected by then, it gives up (no more WiFi.begin() calls) and the icon
// settles into a static "not connected" glyph. It does not auto-resume
// after that; a fresh window only starts via retryWiFi() (Utilities'
// "Retry WiFi" item) or a WiFi-requiring service's own
// waitForWiFiConnection() call, both below. Retrying indefinitely instead
// doesn't help - the ESP32 WiFi driver stops actually attempting
// association after about a minute of repeated WiFi.begin() calls
// regardless, so an explicit stop+manual-retry model matches real behavior.
extern int menuMode;

static bool wifiMgrWasConnected = false;      // last-known status, for edge detection
static bool wifiMgrTrying = false;            // currently in an unbroken "not connected" stretch
static bool wifiMgrGaveUp = false;            // stopped retrying after one full grace window
static unsigned long wifiMgrTryingSinceMs = 0;
static unsigned long wifiMgrLastBeginMs = 0;
static const unsigned long WIFIMGR_RETRY_MS = 20000;   // re-issue WiFi.begin() this often while trying
static const unsigned long WIFIMGR_GRACE_MS = 90000;   // retry window before giving up / showing "not found"

// Listens for the ESP-IDF WiFi driver's own disconnect event, which
// carries the real reason code (esp_wifi_types.h's wifi_err_reason_t) and
// RSSI at the exact moment it happens, rather than inferring anything
// after the fact from WiFi.status() checked later.
static const char *wifiReasonName(uint8_t r) {
  switch (r) {
    case 1: return "UNSPECIFIED";
    case 2: return "AUTH_EXPIRE";
    case 3: return "AUTH_LEAVE";
    case 4: return "ASSOC_EXPIRE";
    case 6: return "NOT_AUTHED";
    case 7: return "NOT_ASSOCED";
    case 8: return "ASSOC_LEAVE (AP or station requested disassociation)";
    case 15: return "4WAY_HANDSHAKE_TIMEOUT";
    case 34: return "MISSING_ACKS";
    case 35: return "EXCEEDED_TXOP";
    case 39: return "TIMEOUT";
    case 200: return "BEACON_TIMEOUT (missed AP beacons - RF/power-save related)";
    case 201: return "NO_AP_FOUND";
    case 202: return "AUTH_FAIL";
    case 203: return "ASSOC_FAIL";
    case 204: return "HANDSHAKE_TIMEOUT";
    case 205: return "CONNECTION_FAIL";
    default: return "(see esp_wifi_types.h wifi_err_reason_t for this code)";
  }
}

static void wifiDisconnectEventHandler(arduino_event_id_t event, arduino_event_info_t info) {
  if (event != ARDUINO_EVENT_WIFI_STA_DISCONNECTED) return;
  uint8_t reason = info.wifi_sta_disconnected.reason;
  int8_t rssi = info.wifi_sta_disconnected.rssi;
  // Timestamped with millis() so this can be lined up against
  // caldav_relay.cpp's own [%8lu]-prefixed logs - this event fires from a
  // different task/context than that module's request loop.
  Serial.printf("[%8lu] WiFi: STA_DISCONNECTED event - reason=%u (%s) rssi=%d heap=%u\r\n",
                (unsigned long) millis(), reason, wifiReasonName(reason), (int) rssi, (unsigned) ESP.getFreeHeap());
}

void maintainWiFi() {
  if (ssid == "" || password == "") return;  // not configured - nothing to maintain

  static bool disconnectHandlerRegistered = false;
  if (!disconnectHandlerRegistered) {
    WiFi.onEvent(wifiDisconnectEventHandler, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
    disconnectHandlerRegistered = true;
  }

  bool connected = (WiFi.status() == WL_CONNECTED);
  unsigned long now = millis();

  // Pause new reconnect attempts while the physical serial port is in
  // active use for a Palm<->WiRSa data protocol (HotSync/SASD/BitWarden
  // Relay) - WiFi.begin() can briefly stall the whole sketch while the
  // radio driver resets, disrupting these timing-sensitive transfers. SLIP
  // Gateway is deliberately not included here - it depends on WiFi itself
  // to function, so reconnecting while it's active is wanted. Doesn't
  // affect waitForWiFiConnection() below, which drives its own WiFi.begin()
  // calls directly rather than going through this background manager.
  bool transferActive = (menuMode == MODE_FILE || menuMode == MODE_PLAYBACK || menuMode == MODE_BITWARDEN || menuMode == MODE_CALDAV_RELAY);

  if (connected) {
    if (!wifiMgrWasConnected) {
      // Fresh association (boot, or recovered from a drop) - run the
      // one-time on-connect setup (mDNS, firmware check).
      wifiOnConnected();
    }
    wifiMgrTrying = false;
  } else {
    if (wifiMgrWasConnected || !wifiMgrTrying) {
      // Either just dropped, or this is the very first attempt this boot -
      // start (or restart) an unbroken "trying" stretch and force an
      // immediate fresh WiFi.begin() rather than waiting out the retry
      // interval (unless a transfer is active - see transferActive above).
      wifiMgrTrying = true;
      wifiMgrTryingSinceMs = now;
      wifiMgrLastBeginMs = 0;
      wifiMgrGaveUp = false;
    }
    if (now - wifiMgrTryingSinceMs > WIFIMGR_GRACE_MS) {
      wifiMgrGaveUp = true;
    }
    if (!wifiMgrGaveUp && !transferActive && (wifiMgrLastBeginMs == 0 || (now - wifiMgrLastBeginMs) > WIFIMGR_RETRY_MS)) {
      wifiBeginConnect(ssid.c_str(), password.c_str());
      wifiMgrLastBeginMs = now;
    }
  }
  wifiMgrWasConnected = connected;

  // Icon draws in whatever header is currently on screen, not just MAIN's -
  // every showHeader()-based menu screen calls this from showHeader()
  // itself, and HotSync/SASD/SLIP/BitWarden's own custom headers call it
  // directly too, so this periodic tick fires regardless of menuMode to
  // keep the "searching" animation advancing on whichever one is showing.
  static unsigned long lastIconDrawMs = 0;
  static bool lastIconConnected = false;
  static bool lastIconAnimating = false;
  bool animating = wifiMgrTrying && (now - wifiMgrTryingSinceMs <= WIFIMGR_GRACE_MS);
  bool stateChanged = (connected != lastIconConnected) || (animating != lastIconAnimating);
  bool animFrameDue = animating && (now - lastIconDrawMs > 400);
  if (stateChanged || animFrameDue || lastIconDrawMs == 0) {
    drawWifiStatusIcon();
    lastIconDrawMs = now;
    lastIconConnected = connected;
    lastIconAnimating = animating;
  }
}

void retryWiFi() {
  if (ssid == "" || password == "") return;  // nothing saved to retry
  // Drop any stale association first so the WiFi.begin() below actually
  // re-associates instead of being a no-op if we're still (weakly)
  // connected. Resetting these statics is exactly what maintainWiFi() itself
  // does on a fresh drop - wifiMgrLastBeginMs=0 makes its very next tick
  // re-issue WiFi.begin() immediately rather than waiting out
  // WIFIMGR_RETRY_MS, wifiMgrGaveUp=false lets it retry again at all, and
  // wifiMgrTryingSinceMs=now restarts both the icon's animate-vs-"not found"
  // window and the new give-up window.
  wifiDisconnect();
  wifiMgrWasConnected = false;
  wifiMgrTrying = true;
  wifiMgrGaveUp = false;
  wifiMgrTryingSinceMs = millis();
  wifiMgrLastBeginMs = 0;
}

// Blocking wait used by WiFi-requiring services (FTP Server/SLIP Gateway/
// BitWarden Relay) at entry: shows "Waiting for WiFi..." while retrying,
// and if still not connected after one WIFIMGR_GRACE_MS window, "WiFi
// unavailable. Retry?" with ENTER retrying (another window) and BACK
// cancelling (caller must then abandon its own startup). Drives its own
// WiFi.begin() calls directly instead of going through maintainWiFi() -
// that background manager is deliberately paused (transferActive, above)
// during some of these same modes, which would otherwise deadlock this wait.
bool waitForWiFiConnection() {
  if (ssid == "" || password == "") {
    showMessage("CONFIGURE SSID\nAND PASSWORD\nOVER SERIAL");
    delay(1500);
    return false;
  }

  while (true) {
    if (WiFi.status() == WL_CONNECTED) return true;

    showMessage("Waiting for\nWiFi...");
    unsigned long windowStart = millis();
    unsigned long lastBeginMs = 0;
    while (WiFi.status() != WL_CONNECTED && (millis() - windowStart) < WIFIMGR_GRACE_MS) {
      if (lastBeginMs == 0 || (millis() - lastBeginMs) > WIFIMGR_RETRY_MS) {
        wifiBeginConnect(ssid.c_str(), password.c_str());
        lastBeginMs = millis();
      }
      delay(100);
    }

    if (WiFi.status() == WL_CONNECTED) {
      // Keep the background manager's own state in sync so its next tick
      // doesn't think it's still trying (and re-run wifiOnConnected() again).
      wifiOnConnected();
      wifiMgrWasConnected = true;
      wifiMgrTrying = false;
      wifiMgrGaveUp = false;
      return true;
    }

    showMessage("WiFi unavailable.\n\nENTER: Retry\nBACK: Exit");
    while (true) {
      readSwitches();
      if (BTNEN) { waitSwitches(); break; }        // retry - loop the outer while(true)
      if (BTNBK) { waitSwitches(); return false; } // give up - caller returns to its own previous menu
      delay(100);
    }
  }
}

// 3 vertical bars (mobile-signal-strength style), drawn in the same header
// icon slot showWifiIcon() already uses (iconX=72,iconY=4 - inside the
// white header bar every showHeader() call paints). Three distinct visual
// states:
//   - Connected: 0-3 bars lit solid, mapped from getSignalBars()'s existing
//     0-4 RSSI scale (collapsed to 0-3 - see the mapping below).
//   - Connecting/searching, still within WIFIMGR_GRACE_MS: bars animate,
//     climbing 0->1->2->3 then resetting, like a phone "searching" glyph -
//     advanced by maintainWiFi()'s own timer, not redrawn every call.
//   - Not connected, past the grace window (or WiFi never configured at
//     all): static X, same glyph showWifiIcon() already uses for "no link".
void drawWifiStatusIcon() {
  const int iconX = 72;
  const int iconY = 4;
  const int iconW = 11;
  const int iconH = 8;
  const int barWidth = 2;
  const int gap = 1;
  const int baseY = iconY + iconH;

  display.fillRect(iconX, iconY, iconW, iconH, SSD1306_WHITE);

  bool connected = (WiFi.status() == WL_CONNECTED);

  if (!connected && !wifiMgrTrying) {
    // Not configured, or a stretch hasn't started yet (e.g. this is called
    // before the first maintainWiFi() tick) - same "no link" X as
    // showWifiIcon()'s own disconnected state.
    display.drawLine(iconX + 1, iconY + 1, iconX + iconW - 2, iconY + iconH - 2, SSD1306_BLACK);
    display.drawLine(iconX + iconW - 2, iconY + 1, iconX + 1, iconY + iconH - 2, SSD1306_BLACK);
    display.display();
    return;
  }

  int litBars;  // 0-3
  if (connected) {
    // Collapse the existing 0-4 RSSI scale down to 0-3 for a 3-bar glyph.
    int bars4 = getSignalBars();
    litBars = (bars4 * 3 + 2) / 4;  // rounds 0->0,1->1,2->2,3->2,4->3
    if (litBars > 3) litBars = 3;
  } else if (millis() - wifiMgrTryingSinceMs > WIFIMGR_GRACE_MS) {
    // Past the grace window and still not connected - static "not found",
    // same X glyph as the disconnected case above.
    display.drawLine(iconX + 1, iconY + 1, iconX + iconW - 2, iconY + iconH - 2, SSD1306_BLACK);
    display.drawLine(iconX + iconW - 2, iconY + 1, iconX + 1, iconY + iconH - 2, SSD1306_BLACK);
    display.display();
    return;
  } else {
    // Still within the grace window - animate a climbing-bars "searching"
    // pulse (0,1,2,3 bars lit, then back to 0) driven by wall-clock time so
    // it advances smoothly regardless of exactly when maintainWiFi() last
    // happened to be called.
    litBars = (int) ((millis() / 400) % 4);
  }

  // 3 bars, each 2px wide with 1px gap, heights 3/5/8px (bottom-aligned).
  int heights[3] = {3, 5, 8};
  for (int b = 0; b < 3; b++) {
    int bx = iconX + b * (barWidth + gap);
    int bh = heights[b];
    if (b < litBars) {
      display.fillRect(bx, baseY - bh, barWidth, bh, SSD1306_BLACK);
    } else {
      drawDitheredBar(bx, baseY - bh, barWidth, bh);
    }
  }
  display.display();
}
