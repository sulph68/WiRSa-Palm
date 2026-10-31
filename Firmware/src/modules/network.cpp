#include "network.h"
#include "modes.h"
#include "led.h"
#include "globals.h"
#include "serial_io.h"
#if WIRSA_FEATURE_MESH
#include "mesh_ble.h"
#endif
#include "display_menu.h"  // showMessage()/readSwitches()/waitSwitches(): waitForWiFiConnection()'s UI
#include <Adafruit_SSD1306.h>
#include <ESPmDNS.h>

// LED_PIN, RXD2 and TXD2 come from globals.h. RTS/CTS/DTR/DCD/DSR/RI are not wired and have no pin defines (see globals.h).

// Result codes (R_OK would conflict with a system header)
#define R_MODEM_OK 0
#define R_ERROR 4
#define R_OK_STAT 200

// Forward declarations for main.cpp functions
extern void showMessage(String message);
extern void firmwareCheck();

extern const uint8_t PROGMEM wifi_symbol[];

// Globals defined in main.cpp
extern WirsaDisplay display;
extern String ssid, password;
extern MDNSResponder mdns;
extern byte serialSpeed;
extern const int bauds[];
extern HardwareSerial PhysicalSerial;

// One-time setup per fresh association, called by maintainWiFi() and waitForWiFiConnection() so the two paths cannot double-run mdns.begin().
static void wifiOnConnected() {
  // Modem-sleep off: a sleeping radio can drop the association mid-handshake during a heavier operation (e.g. TLS with a larger cert chain). Set here so it applies on every fresh association.
  // TX power stays at the core's default, not maxed: maximum draws more per TX burst with no range benefit on a strong link, and risks a brief brownout or self-interference as a handshake bursts packets.
#if WIRSA_FEATURE_MESH
  // While Bluetooth is on (Mesh mode, or a background check at the main menu) WiFi must keep modem sleep, or the coexistence code aborts.
  WiFi.setSleep(menuMode == MODE_MESH || MeshBle::active());
#else
  WiFi.setSleep(false);
#endif

  SerialPrint("CONNECTED TO "); SerialPrintLn(WiFi.SSID());
  SerialPrint("IP ADDRESS: "); SerialPrintLn(ipToString(WiFi.localIP()));
  // (A tcpServer used to listen here; nothing in the live firmware accepted from it, only the removed AT-command modem and telnet BBS logic did, so it and its settings are gone.)
  mdns.begin("WiRSa"); // wirsa.local - also advertises the FTP server
  mdns.addService("ftp", "tcp", 21);
  SerialPrintLn("FTP SERVER: ftp://wirsa.local (once started)");
  showWifiIcon();
  firmwareCheck();
}

// Signal level from RSSI: 0 none, 1 weak, 2 fair, 3 good, 4 excellent
int getSignalBars() {
  if (WiFi.status() != WL_CONNECTED) return 0;

  int rssi = WiFi.RSSI();
  if (rssi > -50) return 4;       // Excellent
  else if (rssi > -60) return 3;  // Good
  else if (rssi > -70) return 2;  // Fair
  else if (rssi > -80) return 1;  // Weak
  else return 0;                   // Very weak / no signal
}

// A dithered (gray) bar: a checkerboard of pixels
void drawDitheredBar(int x, int y, int w, int h) {
  for (int py = y; py < y + h; py++) {
    for (int px = x; px < x + w; px++) {
      if ((px + py) % 2 == 0) {
        display.drawPixel(px, py, SSD1306_BLACK);
      }
    }
  }
}

void showWifiIcon() {
  // Icon position and size (in the header area, white background)
  const int iconX = 72;
  const int iconY = 4;
  const int iconW = 11;
  const int iconH = 8;

  // Clear the icon area (white, as the header)
  display.fillRect(iconX, iconY, iconW, iconH, SSD1306_WHITE);

  if (WiFi.status() == WL_CONNECTED) {
    ledSet(true);  // LED on

    int bars = getSignalBars();

    // Signal bars in BLACK on the white header: 4 bars, 2 pixels wide with a 1-pixel gap, heights 2/4/6/8 px from the bottom at iconY+iconH

    int barWidth = 2;
    int gap = 1;
    int baseY = iconY + iconH;  // Bottom of icon area

    if (bars >= 1) {
      display.fillRect(iconX, baseY - 2, barWidth, 2, SSD1306_BLACK);
    } else {
      drawDitheredBar(iconX, baseY - 2, barWidth, 2);
    }

    if (bars >= 2) {
      display.fillRect(iconX + barWidth + gap, baseY - 4, barWidth, 4, SSD1306_BLACK);
    } else {
      drawDitheredBar(iconX + barWidth + gap, baseY - 4, barWidth, 4);
    }

    if (bars >= 3) {
      display.fillRect(iconX + 2*(barWidth + gap), baseY - 6, barWidth, 6, SSD1306_BLACK);
    } else {
      drawDitheredBar(iconX + 2*(barWidth + gap), baseY - 6, barWidth, 6);
    }

    if (bars >= 4) {
      display.fillRect(iconX + 3*(barWidth + gap), baseY - 8, barWidth, 8, SSD1306_BLACK);
    } else {
      drawDitheredBar(iconX + 3*(barWidth + gap), baseY - 8, barWidth, 8);
    }

  } else {
    ledSet(false); // LED off
    // X: no connection
    display.drawLine(iconX + 1, iconY + 1, iconX + iconW - 2, iconY + iconH - 2, SSD1306_BLACK);
    display.drawLine(iconX + iconW - 2, iconY + 1, iconX + 1, iconY + iconH - 2, SSD1306_BLACK);
  }
  display.display();
}

// Every WiFi.disconnect() funnels through here, the one place to tune disconnect-time behaviour (e.g. WiFi.disconnect(true) to erase credentials, or logging): retryWiFi(),
// WiFi Setup's pre-scan and pre-test disconnects, and disconnectWiFi(). The counterpart of wifiBeginConnect() for WiFi.begin().
void wifiDisconnect() {
  WiFi.disconnect();
}

// For a caller that also wants the UI settled (a brief pause and an immediate "disconnected" icon). No caller needs that today (each disconnect site has its own follow-up timing); kept as the place for one.
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

// Every WiFi.begin() funnels through here (see network.h): add connection-time tuning (TX power, PHY mode, channel/BSSID hints, a forced WIFI_STA) here and it reaches every caller.
static void wifiDisconnectEventHandler(arduino_event_id_t event, arduino_event_info_t info);
// Registered on the first WiFi.begin() of any kind (WiFi Setup's test connect too), not only from maintainWiFi(), which returns early until credentials are saved: first-time setup failures were silent.
static void wifiRegisterDisconnectLog() {
  static bool done = false;
  if (done) return;
  done = true;
  WiFi.onEvent(wifiDisconnectEventHandler, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
}

static const char *wifiStatusName(wl_status_t st) {
  switch (st) {
    case WL_IDLE_STATUS:     return "IDLE";
    case WL_NO_SSID_AVAIL:   return "NO_SSID_AVAIL (SSID not seen)";
    case WL_SCAN_COMPLETED:  return "SCAN_COMPLETED";
    case WL_CONNECTED:       return "CONNECTED";
    case WL_CONNECT_FAILED:  return "CONNECT_FAILED (wrong password / auth)";
    case WL_CONNECTION_LOST: return "CONNECTION_LOST";
    case WL_DISCONNECTED:    return "DISCONNECTED";
    default:                 return "NO_SHIELD / unknown";
  }
}

void wifiBeginConnect(const char *connSsid, const char *connPassword) {
  wifiRegisterDisconnectLog();
  wl_status_t r = WiFi.begin(connSsid, connPassword);
#if !defined(WIRSA_BOARD_T7S3)
  WiFi.setTxPower(WIFI_POWER_15dBm);
#endif   // T7-S3: the core's default TX power connects; 15 dBm did not (tested)
  Serial.printf("[%8lu] WiFi: begin \"%s\" (password %u chars) -> %s\r\n",
                (unsigned long) millis(), connSsid, (unsigned) strlen(connPassword), wifiStatusName(r));
}

void wifiReconnectToSaved() {
  if (ssid.length() == 0) return;  // matches every inline call site this
                                    // not also requiring password.length() > 0: a saved open network has an empty one
  wifiBeginConnect(ssid.c_str(), password.c_str());
}

// ---- Background WiFi connection manager ----
// maintainWiFi() runs once per loop() pass and never blocks: it only calls WiFi.begin() (async; the ESP32's WiFi task associates) and polls WiFi.status(), so WiFi stays up for every feature
// that needs it (FTP, SLIP, BitWarden) instead of each connecting on demand.
// Once configured (SSID and password set) it retries every WIFIMGR_RETRY_MS for one WIFIMGR_GRACE_MS window (1 minute); if still not connected it stops calling WiFi.begin() and the icon settles
// to a static "not connected" glyph. It does not resume by itself: a fresh window starts only via retryWiFi() (Utilities' "Retry WiFi") or a service's waitForWiFiConnection(). Retrying
// forever does not help, because the ESP32 WiFi driver stops attempting association after about a minute of repeated WiFi.begin() calls, so an explicit stop and manual retry matches reality.
extern int menuMode;

static bool wifiMgrWasConnected = false;      // last-known status, for edge detection
static bool wifiMgrTrying = false;            // currently in an unbroken "not connected" stretch
static bool wifiMgrGaveUp = false;            // stopped retrying after one full grace window
static unsigned long wifiMgrTryingSinceMs = 0;
static unsigned long wifiMgrLastBeginMs = 0;
static const unsigned long WIFIMGR_RETRY_MS = 20000;   // re-issue WiFi.begin() this often while trying
static const unsigned long WIFIMGR_GRACE_MS = 90000;   // retry window before giving up / showing "not found"

// The ESP-IDF driver's disconnect event carries the real reason code (esp_wifi_types.h wifi_err_reason_t) and RSSI at that moment, rather than inferring from WiFi.status() later.
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
  // Timestamped with millis() to line up with caldav_relay.cpp's [%8lu] logs; this event fires from another task than that module's request loop.
  Serial.printf("[%8lu] WiFi: STA_DISCONNECTED event - reason=%u (%s) rssi=%d heap=%u\r\n",
                (unsigned long) millis(), reason, wifiReasonName(reason), (int) rssi, (unsigned) ESP.getFreeHeap());
}

void maintainWiFi() {
  if (ssid == "" || password == "") return;  // not configured - nothing to maintain

  wifiRegisterDisconnectLog();

  bool connected = (WiFi.status() == WL_CONNECTED);
  unsigned long now = millis();

  // Pause new reconnects while the serial port serves a Palm data protocol (HotSync/SASD/BitWarden Relay): WiFi.begin() can stall the whole sketch while the radio driver resets, disrupting
  // timing-sensitive transfers. SLIP Gateway is not included: it needs WiFi, so reconnecting is wanted. waitForWiFiConnection() is unaffected, as it drives its own WiFi.begin().
  bool transferActive = Modes::isTransfer(menuMode);

  if (connected) {
    if (!wifiMgrWasConnected) {
      // Fresh association (boot, or recovered from a drop): run the one-time on-connect setup (mDNS, firmware check).
      wifiOnConnected();
    }
    wifiMgrTrying = false;
  } else {
    if (wifiMgrWasConnected || !wifiMgrTrying) {
      // Just dropped, or the first attempt this boot: start (or restart) an unbroken "trying" stretch and force a fresh WiFi.begin() at once, not after the retry interval (unless a transfer is active).
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

  // While trying, report the driver's status every 5 s so a silent failure (no disconnect event) shows on the USB log.
  static unsigned long lastWifiLogMs = 0;
  if (wifiMgrTrying && !wifiMgrGaveUp && now - lastWifiLogMs > 5000) {
    lastWifiLogMs = now;
    Serial.printf("[%8lu] WiFi: waiting, status=%s\r\n", (unsigned long) now, wifiStatusName(WiFi.status()));
  }

  // The icon draws in whatever header is on screen: showHeader() calls this, and HotSync/SASD/SLIP/BitWarden's custom headers call it too, so this tick fires regardless of menuMode to keep the "searching" animation moving.
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
  // Drop any stale association first so the WiFi.begin() below re-associates and is not a no-op while still (weakly) connected. Resetting these statics is what maintainWiFi() does on a fresh
  // drop: wifiMgrLastBeginMs = 0 makes its next tick re-issue WiFi.begin() at once, wifiMgrGaveUp = false lets it retry, and wifiMgrTryingSinceMs = now restarts the icon's animate-or-"not found" window and the give-up window.
  wifiDisconnect();
  wifiMgrWasConnected = false;
  wifiMgrTrying = true;
  wifiMgrGaveUp = false;
  wifiMgrTryingSinceMs = millis();
  wifiMgrLastBeginMs = 0;
}

// Blocking wait used by WiFi-requiring services (FTP Server, SLIP Gateway, BitWarden Relay) at entry: shows "Waiting for WiFi..." while retrying, and after one WIFIMGR_GRACE_MS window
// "WiFi unavailable. Retry?" with ENTER retrying (another window) and BACK cancelling (the caller then abandons its startup). It drives its own WiFi.begin(): the background manager is paused
// (transferActive) during some of these modes, which would otherwise deadlock this wait.
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
      // Keep the background manager's state in sync so its next tick does not think it is still trying (and run wifiOnConnected() again).
      wifiOnConnected();
      wifiMgrWasConnected = true;
      wifiMgrTrying = false;
      wifiMgrGaveUp = false;
      return true;
    }

#if !WIRSA_HAS_BUTTONS
    return false;   // screenless: nobody to press Retry - the caller gives up
#else
    showMessage("WiFi unavailable.\n\nENTER: Retry\nBACK: Exit");
    while (true) {
      readSwitches();
      if (BTNEN) { waitSwitches(); break; }        // retry - loop the outer while(true)
      if (BTNBK) { waitSwitches(); return false; } // give up - caller returns to its own previous menu
      delay(100);
    }
#endif
  }
}

// Control link STATUS (palm_ctl.cpp): 0 no network set up, 1 connecting, 2 connected, 3 gave up retrying.
uint8_t wifiStateCode() {
  if (WiFi.status() == WL_CONNECTED) return 2;
  if (ssid == "") return 0;
  return wifiMgrGaveUp ? 3 : 1;
}

// 3 vertical bars (mobile signal style) in the header icon slot showWifiIcon() uses (iconX=72, iconY=4, inside the white header bar). Three states:
//  - Connected: 0-3 bars lit solid, from getSignalBars()'s 0-4 scale (collapsed to 0-3).
//  - Connecting, within WIFIMGR_GRACE_MS: the bars animate 0->1->2->3 and reset, like a phone "searching" glyph, advanced by maintainWiFi()'s timer, not redrawn every call.
//  - Not connected past the grace window (or never configured): a static X, as showWifiIcon() uses for "no link".
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
    // Not configured, or no stretch started yet (called before the first maintainWiFi() tick): the same "no link" X.
    display.drawLine(iconX + 1, iconY + 1, iconX + iconW - 2, iconY + iconH - 2, SSD1306_BLACK);
    display.drawLine(iconX + iconW - 2, iconY + 1, iconX + 1, iconY + iconH - 2, SSD1306_BLACK);
    display.display();
    return;
  }

  int litBars;  // 0-3
  if (connected) {
    // collapse the 0-4 scale to 0-3 for a 3-bar glyph
    int bars4 = getSignalBars();
    litBars = (bars4 * 3 + 2) / 4;  // rounds 0->0,1->1,2->2,3->2,4->3
    if (litBars > 3) litBars = 3;
  } else if (millis() - wifiMgrTryingSinceMs > WIFIMGR_GRACE_MS) {
    // Past the grace window and not connected: the static "not found" X.
    display.drawLine(iconX + 1, iconY + 1, iconX + iconW - 2, iconY + iconH - 2, SSD1306_BLACK);
    display.drawLine(iconX + iconW - 2, iconY + 1, iconX + 1, iconY + iconH - 2, SSD1306_BLACK);
    display.display();
    return;
  } else {
    // Within the grace window: a climbing-bars "searching" pulse (0,1,2,3 lit, then 0) driven by wall-clock time, so it advances smoothly whenever maintainWiFi() last ran.
    litBars = (int) ((millis() / 400) % 4);
  }

  // 3 bars, 2 px wide with a 1 px gap, heights 3/5/8 px, bottom-aligned
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
