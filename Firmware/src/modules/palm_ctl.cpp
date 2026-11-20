// palm_ctl.cpp - Palm control link: WiRSaCtl (palmos_sdk apps/WiRSaControl/) drives WiRSa over the Palm line. Wire: docs/protocol/Protocol-wire.md (sections 7 and 12).
// Every frame is 0x16 (RelayProto::CTL_LEAD) + a RelayProto frame, plaintext except the password commands (WIFI_SET, SECRETS_SET), which the Palm sends after a key exchange; the session is dropped
// after each one. Requests are 0x40-0x4F, a reply is the request + 0x20, or ERR (code u8, message str).
// In the menus menuKey() (every single-letter menu loop) spots the 0x16 and handles the frame. In CalDAV/BitWarden/Mesh/HotSync-waiting palmCtlModeHook() answers PING/STATUS and EXIT. SLIP and SASD have no hook: a reset ends them.

#include "log.h"
#include "palm_ctl.h"
#include "sd_mount.h"
#include "vdisk.h"
#include "vdfs.h"

#if WIRSA_FEATURE_PALMCTL

#include "relay_proto.h"
#include "display_menu.h"
#include "diagnostics.h"
#include "network.h"
#include "settings.h"
#include "modes.h"
#include "autobaud.h"
#include "battery.h"
#include "sd_usage.h"
#include "sd_format.h"
#include "wifi_setup.h"
#include <WiFi.h>
#include <SD.h>
#include <EEPROM.h>
#include <esp_partition.h>
#include <esp_system.h>
#if WIRSA_FEATURE_FTPSERVER
#include "ftp_server.h"
#endif
#if WIRSA_FEATURE_SLIP
#include "slip_mode.h"
#endif

extern bool usbDebug;   // main.cpp

using RelayProto::Writer;
using RelayProto::Reader;

namespace {

enum : uint8_t {
  CTL_PING = 0x40, CTL_STATUS = 0x41, CTL_WIFI_SCAN = 0x42, CTL_WIFI_SET = 0x43,
  CTL_CFG_GET = 0x44, CTL_CFG_SET = 0x45, CTL_SECRETS = 0x46, CTL_FTP = 0x47,
  CTL_ENTER = 0x48, CTL_EXIT = 0x49, CTL_REBOOT = 0x4A, CTL_FACTORY = 0x4B,
  CTL_SPEED = 0x4C, CTL_LOG = 0x4D, CTL_SYSINFO = 0x4E, CTL_FORMAT_SD = 0x4F,
  CTL_ERR = 0x5F
};
enum : uint8_t {
  ERR_NO_WIFI = 1, ERR_NO_SD = 2, ERR_NOT_BUILT = 3, ERR_NOT_A_MODE = 4,
  ERR_BUSY = 5, ERR_BAD_REQUEST = 6, ERR_FAILED = 7
};
enum : uint16_t {
  FLAG_SCREENLESS = 0x0001,
  FLAG_SPEED = 0x0080,  // SPEED supported (dual-rate menu)
  // the flags of the modes that are built (FTP, HotSync, SASD, SLIP, BitWarden, CalDAV, Mesh) come from the mode table: modes.h
};

const uint8_t PROTO_VERSION = 1;
const uint32_t BYTE_TIMEOUT_MS = 100;   // inside a frame
const uint32_t GUARD_MS = 100;          // menu burst guard
const int MAX_SCAN = 16;
const uint8_t NUM_BAUDS = 7;           // bauds[] in main.cpp
// The Palm link numbers the rates as when the table began at 300 (4 = 9600, 5 = 19200, 6 = 38400, 7 = 57600, 8 = 115200), so a WiRSaCtl from before the cut keeps working; 9 = 14400 and 10 = 28800 came later.
// Indexed by the bauds[] index; a wire value that is none of these (0..3 = 300..4800) becomes 9600.
const uint8_t kWireBaud[NUM_BAUDS] = { 4, 9, 5, 10, 6, 7, 8 };
uint8_t baudFromWire(uint8_t wire) {
  for (uint8_t i = 0; i < NUM_BAUDS; i++)
    if (kWireBaud[i] == wire) return i;
  return 0;
}

uint8_t gBody[1024];   // request body (static: too big for the loop task's stack)
uint8_t gOut[640];     // reply body
char gPendingKey = 0;  // mode letter from ENTER_MODE, handed to the main menu
int8_t gSdState = -1;  // last SD probe: -1 not tried, 0 no card, 1 card
unsigned long gSdProbeMs = 0;
const unsigned long SD_REPROBE_MS = 3000;    // a STATUS retries a missing card at most this often
unsigned long gLastPhysRx = 0;

// Session rate (WIRSA_FEATURE_DUAL_BAUD): after every boot the menu runs at HOME_BAUD; WiRSaCtl's SPEED moves it to the Palm's rate (the configured baud only caps the modes' own
// negotiation), and a frame or key heard by the second receiver moves it there. Never saved. 0 = home.
const uint32_t HOME_BAUD = 9600;   // a rate every Palm can reach
uint32_t gSessionBaud = 0;

uint32_t maxBaud() { return (uint32_t) bauds[serialSpeed]; }

uint32_t homeBaud() {
#if WIRSA_FEATURE_DUAL_BAUD
  return maxBaud() < HOME_BAUD ? maxBaud() : HOME_BAUD;
#else
  return maxBaud();   // off: the menu runs at the configured baud, as before
#endif
}

#if WIRSA_FEATURE_DUAL_BAUD
// Receive-only (no TX pin: UART1's default TX on the S3 is GPIO17, the Palm TX line), on the same RX pin as PhysicalSerial, in the menus only.
HardwareSerial ListenSerial(1);
bool gListenOn = false;
uint32_t gListenBaud = 0;
unsigned long gLastListenRx = 0;

// A login script's rate (SETBAUD / BR): the menu keeps it for the script's next step (the S key, a mode), then returns to gScriptPrev, the rate before the script. Left at the script's rate, the menu
// would stop listening for the next script (autobaud and the second receiver work from the home rate).
bool gScript = false;
uint32_t gScriptPrev = 0;
unsigned long gScriptT = 0;
bool gScriptLeft = false;            // a mode was entered while the script's rate was held
const unsigned long SCRIPT_HOLD_MS = 10000;

void scriptHold() {
  if (!gScript) { gScript = true; gScriptPrev = menuBaud(); gScriptLeft = false; }
  gScriptT = millis();
}

// The second receiver's rate: 9600 while the menu runs at any other, so every Palm can get back in. At 9600 it is off (returns the menu's rate): only SPEED moves the menu up. Listening at the
// maximum let a Palm's rate sweep move the menu to a rate that Palm could send but not receive (seen: a 57600 Palm's 115200 PING).
uint32_t otherBaud() {
  return menuBaud() != HOME_BAUD ? HOME_BAUD : menuBaud();
}

// The menu screens (same list as boot_button.cpp's): the modes set their
// own rate on the Palm line, so the second receiver is off in them.
bool inMenus() {
  switch (menuMode) {
    case MODE_MAIN: case MODE_SETTINGS: case MODE_SETBAUD: case MODE_ORIENTATION:
    case MODE_WIFI_SETUP: case MODE_WIFI_PASSWORD: case MODE_DIAGNOSTICS: case MODE_FTPSERVER:
      return true;
    default:
      return false;
  }
}

// Second receiver on in the menus (when there is another rate to hear),
// off in the modes, and at otherBaud().
void listenSync() {
  bool want = inMenus() && otherBaud() != menuBaud();
  if (want && gListenOn && gListenBaud != otherBaud()) {
    gListenBaud = otherBaud();
    ListenSerial.updateBaudRate(gListenBaud);
  }
  if (want == gListenOn) return;
  if (want) {
    gListenBaud = otherBaud();
    ListenSerial.setRxBufferSize(512);
    ListenSerial.begin(gListenBaud, WIRSA_SERIAL_FORMAT, RXD2, -1);   // RX only
    gLastListenRx = millis();
  } else {
    ListenSerial.end();
  }
  gListenOn = want;
}

// Move the menu (PhysicalSerial) to rate for this power-on.
void setMenuRate(uint32_t rate) {
  gSessionBaud = rate == homeBaud() ? 0 : rate;
  PhysicalSerial.flush();
  PhysicalSerial.updateBaudRate(menuBaud());
  while (PhysicalSerial.available()) PhysicalSerial.read();   // half-received at the old rate
  gLastPhysRx = millis();
  listenSync();
  // The header's top-right rate shows menuBaud(): repaint the main menu
  // now (other menu screens show it the next time they are drawn).
  if (menuMode == MODE_MAIN) {
    msgFlag = true;
    mainMenu(false);
  }
}

// The second receiver decoded a frame or key: the Palm talks at its rate.
void heardOnListener() {
  setMenuRate(gListenBaud);
  LOGV("PalmCtl: menu now at %lu baud (heard on the second receiver)\r\n",
                              (unsigned long) menuBaud());
}

#if WIRSA_FEATURE_AUTOBAUD
// Speed handshake (README-Palm.md "Setting the speed from a login script"):
//  1. The Palm sends the line SETBAUD (no CR needed: a line also ends after a pause). Heard as text (at 9600 or the menu's speed) it is read as text; at any other speed the pin-interrupt autobaud
//     measures the speed from the 'S' and the menu moves there once the line is over. Either way WiRSa answers OK at the speed the Palm uses.
//  2. The Palm sends BR<rate>, e.g. BR115200 (CR optional). The UART decodes it at the speed now in use, so reading it proves the speed. After autobaud the rate must equal the measured one, then OK;
//     else ERROR and the menu returns to 9600. Heard as text: any rate in the table is accepted, answered OK, and the menu moves to it.
// No BR line within 5 s of a speed change by autobaud: back to 9600.
const char kPhrase[] = "SETBAUD";
uint8_t gAw = 0;               // 0 idle, 1 waiting for BR (menu not moved), 2 waiting for BR (menu moved)
uint32_t gAwRate = 0;          // the measured speed (2)
unsigned long gAwDeadline = 0;
unsigned long gAwLastRx = 0;     // last byte of the BR line (a line also ends after a pause)
char gAwBuf[16];
uint8_t gAwLen = 0;
const unsigned long BR_PAUSE_MS = 200;

// The speeds a login script can ask for (SETBAUD / BR): the rates this Palm offers (9600 14400 19200 28800 38400 57600 115200), not only the five of bauds[] (the Max baud choices). Each is at least
// 25% from the next, so a measured bit time (within 8%) names one.
const uint32_t kScriptRates[] = { 9600, 14400, 19200, 28800, 38400, 57600, 115200 };
const uint8_t NUM_SCRIPT_RATES = sizeof(kScriptRates) / sizeof(kScriptRates[0]);

bool atRateOk(uint32_t r) {
  for (uint8_t i = 0; i < NUM_SCRIPT_RATES; i++)
    if (kScriptRates[i] == r) return true;
  return false;
}

void replyLine(const char *text) {
  PhysicalSerial.print("\r\n");
  PhysicalSerial.print(text);
  PhysicalSerial.print("\r\n");
  PhysicalSerial.flush();
}

void awaitBr(uint8_t kind, uint32_t rate) {
  gAw = kind;
  gAwRate = rate;
  gAwLen = 0;
  gAwLastRx = millis();
  gAwDeadline = millis() + 5000;
}

// The Palm's BR line is complete in gAwBuf. Every outcome is logged on the USB console regardless of USB debug: these are rare and the only way to see why a script failed.
void brLineDone() {
  uint32_t n = 0;
  bool shape = gAwLen > 2 && gAwBuf[0] == 'B' && gAwBuf[1] == 'R';
  bool ok = shape;
  char shown[sizeof(gAwBuf) + 1];
  for (uint8_t i = 0; i < gAwLen; i++) shown[i] = (gAwBuf[i] >= 0x20 && gAwBuf[i] < 0x7F) ? gAwBuf[i] : '?';
  shown[gAwLen] = '\0';
  for (uint8_t i = 2; ok && i < gAwLen; i++) {
    if (gAwBuf[i] < '0' || gAwBuf[i] > '9') ok = false;
    else n = n * 10 + (uint32_t) (gAwBuf[i] - '0');
    if (n > 1000000UL) ok = false;
  }
  bool known = ok && atRateOk(n);
  uint8_t kind = gAw;
  gAw = 0;
  if (kind == 2) {
    if (known && n == gAwRate) {
      replyLine("OK");
      Serial.printf("SetBaud: BR line \"%s\" matches the measured %lu baud - speed confirmed\r\n", shown, (unsigned long) n);
      scriptHold();
    } else {
      replyLine("ERROR");
      if (!shape)       Serial.printf("SetBaud: ERROR - expected BR<rate>, got \"%s\" (measured %lu baud) - back to %lu baud\r\n", shown, (unsigned long) gAwRate, (unsigned long) HOME_BAUD);
      else if (!ok)     Serial.printf("SetBaud: ERROR - \"%s\" is not a number (measured %lu baud) - back to %lu baud\r\n", shown, (unsigned long) gAwRate, (unsigned long) HOME_BAUD);
      else if (!known)  Serial.printf("SetBaud: ERROR - %lu is not a rate a script can ask for (9600..115200); measured %lu baud - back to %lu baud\r\n", (unsigned long) n, (unsigned long) gAwRate, (unsigned long) HOME_BAUD);
      else              Serial.printf("SetBaud: ERROR - wrong baud: the script says BR%lu but the line measured %lu baud - back to %lu baud\r\n", (unsigned long) n, (unsigned long) gAwRate, (unsigned long) HOME_BAUD);
      gScript = false;
      setMenuRate(HOME_BAUD);
    }
  } else if (known) {
    replyLine("OK");
    Serial.printf("SetBaud: BR line \"%s\" - menu %s %lu baud\r\n", shown, n != menuBaud() ? "moves to" : "stays at", (unsigned long) n);
    if (n != menuBaud()) { scriptHold(); setMenuRate(n); }
  } else {
    replyLine("ERROR");
    Serial.printf("SetBaud: ERROR - \"%s\" is not BR<rate> with a rate from the Palm list (9600 14400 19200 28800 38400 57600 115200)\r\n", shown);
  }
}

// Once per pass while a BR line is awaited (palmCtlPoll). menuKey stays off the line meanwhile.
void awaitService() {
  if (!gAw) return;
  while (PhysicalSerial.available()) {
    int x = PhysicalSerial.read();
    gAwLastRx = millis();
    if (x >= 'a' && x <= 'z') x -= 32;
    if (x == '\r' || x == '\n') {
      if (gAwLen) { brLineDone(); return; }
      continue;
    }
    if (gAwLen < sizeof(gAwBuf)) gAwBuf[gAwLen++] = (char) x;
    else gAwLen = (uint8_t) sizeof(gAwBuf);       // too long: it won't parse
  }
  // No CR needed: the line is over once the Palm has paused.
  if (gAwLen && (millis() - gAwLastRx) >= BR_PAUSE_MS) { brLineDone(); return; }
  if ((long) (millis() - gAwDeadline) >= 0) {
    if (gAw == 2) {
      Serial.printf("SetBaud: no BR line within 5 s after measuring %lu baud - back to %lu baud\r\n", (unsigned long) gAwRate, (unsigned long) HOME_BAUD);
      gScript = false;
      setMenuRate(HOME_BAUD);
    } else {
      Serial.printf("SetBaud: no BR line within 5 s of SETBAUD\r\n");
    }
    gAw = 0;
  }
}

// The line SETBAUD, whose first character c was just read from io. True if the phrase was read (consumed and answered); false if c does not start it (nothing else is read). An 'S' that is a plain
// key (SLIP) is untouched: the 'E' must follow within 30 ms. A line that starts like the phrase but is not it is dropped.
bool phraseCommand(HardwareSerial &io, int c) {
  if (c != 'S') return false;
  unsigned long t0 = millis();
  while (!io.available() && millis() - t0 < 30) yield();
  if (!io.available() || io.peek() != 'E') return false;
  uint8_t i = 1;
  bool match = true;
  t0 = millis();
  for (;;) {
    while (!io.available() && millis() - t0 < 100) yield();
    if (!io.available()) break;
    int x = io.read();
    t0 = millis();
    if (x == '\r' || x == '\n') break;
    if (x >= 'a' && x <= 'z') x -= 32;
    if (i < sizeof(kPhrase) - 1 && x == kPhrase[i]) i++;
    else match = false;
  }
  if (!match || i != sizeof(kPhrase) - 1) {                 // junk that looked like it
    Serial.printf("SetBaud: ignored a line that starts like SETBAUD but isn't\r\n");
    return true;
  }
  if (&io != &PhysicalSerial) { scriptHold(); heardOnListener(); }           // the Palm is at 9600: the menu comes down to it
  replyLine("OK");
  awaitBr(1, 0);
  Serial.printf("SetBaud: SETBAUD read as text at %lu baud - waiting for BR<rate>\r\n", (unsigned long) menuBaud());
  return true;
}

// Autobaud (autobaud.h): watch the Palm RX pin while the menu is in a menu screen, at any rate (a Palm preferring 57600 leaves the menu there, and its script can ask for 38400). A burst that
// starts with an 'S' at another standard rate moves the menu there, answers OK at that rate and waits for the BR line.
void autobaudService() {
  if (gAw == 1 || !inMenus()) {
    if (gAw != 1) autobaudDisarm();
    return;
  }
  autobaudArm(RXD2);
  uint32_t e[AUTOBAUD_EDGES];
  if (!autobaudTake(e)) return;
  int rates[NUM_SCRIPT_RATES];
  uint8_t n = 0;
  for (uint8_t i = 0; i < NUM_SCRIPT_RATES; i++) rates[n++] = (int) kScriptRates[i];
  uint32_t r = autobaudDetect(e, (uint32_t) getCpuFrequencyMhz() * 1000000UL, rates, n);
  if (r == 0 || r == menuBaud()) {
    LOGV("PalmCtl: autobaud burst -> %lu\r\n", (unsigned long) r);
    if (r == 0) {
      uint32_t mhz = getCpuFrequencyMhz();
      LOGV("PalmCtl: autobaud edges (us from the first): %lu %lu %lu %lu %lu %lu %lu %lu\r\n",
           0UL, (unsigned long) ((e[1] - e[0]) / mhz), (unsigned long) ((e[2] - e[0]) / mhz), (unsigned long) ((e[3] - e[0]) / mhz),
           (unsigned long) ((e[4] - e[0]) / mhz), (unsigned long) ((e[5] - e[0]) / mhz), (unsigned long) ((e[6] - e[0]) / mhz),
           (unsigned long) ((e[7] - e[0]) / mhz));
    }
    return;
  }
  Serial.printf("SetBaud: measured %lu baud from the first character - menu moved, waiting for BR%lu\r\n", (unsigned long) r, (unsigned long) r);
  scriptHold();
  setMenuRate(r);
  replyLine("OK");
  awaitBr(2, r);
}

// The script's rate is held while its next step comes (a mode is entered, or the line stays quiet for
// SCRIPT_HOLD_MS), then the menu goes back to the rate it had before the script.
void scriptService() {
  if (!gScript || gAw) return;
  if (! inMenus()) { gScriptLeft = true; return; }
  bool quiet = (millis() - gScriptT) >= SCRIPT_HOLD_MS && (millis() - gLastPhysRx) >= SCRIPT_HOLD_MS && ! PhysicalSerial.available();
  if (! gScriptLeft && ! quiet) return;
  gScript = false;
  Serial.printf("SetBaud: script over - menu back to %lu baud\r\n", (unsigned long) gScriptPrev);
  if (gScriptPrev != menuBaud()) setMenuRate(gScriptPrev);
}
#endif

#endif

char currentModeLetter() { return Modes::ctlLetter(menuMode); }

uint16_t buildFlags() {
  uint16_t f = Modes::builtFlags();
#if WIRSA_SCREENLESS
  f |= FLAG_SCREENLESS;
#endif
#if WIRSA_FEATURE_DUAL_BAUD
  f |= FLAG_SPEED;
#endif
  return f;
}

// A letter WiRSaCtl can start that is built into this WiRSa.
bool modeBuilt(char m) {
  const Modes::Info *i = Modes::byKey(m);
  return i && i->ctlEnter;
}

void reply(uint8_t type, const uint8_t *body, size_t len) {
  RelayProto::writeFrame(PhysicalSerial, type, body, len);
}

void replyOk(uint8_t req, bool ok = true) {
  uint8_t b = ok ? 1 : 0;
  reply((uint8_t) (req + 0x20), &b, 1);
}

void replyErr(uint8_t code, const char *msg) {
  Writer w(gOut, sizeof(gOut));
  w.u8(code);
  w.str(String(msg));
  reply(CTL_ERR, gOut, w.len);
}

// The rest of a control frame after its 0x16. Resync rule (Protocol-wire.md section 7): a further 0x16 is the real start (the first was stray); an unknown type ends the attempt at once.
// Returns the type with 0x80 cleared, and whether the frame came encrypted.
bool readCtlFrame(HardwareSerial &io, uint8_t &type, size_t &len, bool &enc) {
  uint8_t t;
  do {
    if (!RelayProto::readByte(io, t, BYTE_TIMEOUT_MS)) return false;
  } while (t == RelayProto::CTL_LEAD);
  uint8_t base = t & 0x7F;
  if (!((base >= 0x40 && base <= 0x4F) || (base >= 0x70 && base <= 0x74))) {
    LOGV("PalmCtl: bad type 0x%02X after lead byte - ignored\r\n", t);
    return false;
  }
  enc = (t & RelayProto::ENC_FLAG) != 0;
  type = t;
  if (!RelayProto::readFrameRest(io, type, gBody, sizeof(gBody), len, BYTE_TIMEOUT_MS))
    return false;
  LOGV("PalmCtl: <- 0x%02X len=%u%s\r\n", type, (unsigned) len, enc ? " (enc)" : "");
  return true;
}

void sendPing(const uint8_t *req, size_t len, char mode) {
  if (len < 4) { replyErr(ERR_BAD_REQUEST, "Bad PING"); return; }
  Writer w(gOut, sizeof(gOut));
  w.bytesRaw(req, 4);   // nonce back
  w.u8(PROTO_VERSION);
  w.str(build);
  w.u16(buildFlags());
  w.u8((uint8_t) mode);
  w.u32(menuBaud());   // where the menu listens now, for the Palm's way back
  reply((uint8_t) (CTL_PING + 0x20), gOut, w.len);
}

void sendStatus(char mode) {
  bool up = WiFi.status() == WL_CONNECTED;
  Writer w(gOut, sizeof(gOut));
  w.u8((uint8_t) mode);
  w.u8(wifiStateCode());
  w.str(up ? WiFi.SSID() : ssid);
  w.u8((uint8_t) (int8_t) (up ? WiFi.RSSI() : 0));
  w.str(up ? ipToString(WiFi.localIP()) : String(""));
  // 1 card, 2 no card, 0 not known yet (see sdProbeAfterReply), 3 the virtual card is the SASD disk
  // (chosen, or Auto with no SD card answering) - the SD text is then the virtual card's.
  if (menuMode == MODE_MAIN && SD.cardType() != CARD_NONE && !sdAlive()) gSdState = 0;   // the card was pulled
  uint8_t sdv = SD.cardType() != CARD_NONE ? 1 : gSdState == 0 ? 2 : gSdState == 1 ? 1 : 0;
  bool vp = vdiskPresent();
  if (sasdDiskMode == SASD_DISK_VIRTUAL) sdv = vp ? 3 : 2;
  else if (sasdDiskMode == SASD_DISK_AUTO && sdv == 2 && vp) sdv = 3;
  bool virt = sdv == 3;
  w.u8(sdv);
#if WIRSA_FEATURE_FTPSERVER
  w.u8(ftpServerRunning ? 1 : 0);
#else
  w.u8(0);
#endif
  w.u8(0);   // FTP client count: not tracked
  w.u32(millis() / 1000);
  // Battery, appended after uptime (a Palm app that predates it stops at uptime): present u8, millivolts u16, percent u8. Absent on boards without a battery input, or with nothing connected.
  uint32_t battMv = batteryAvailable() ? batteryMilliVolts() : 0;
  w.u8(battMv ? 1 : 0);
  w.u16((uint16_t) battMv);
  w.u8((uint8_t) (battMv ? batteryPercent(battMv) : 0));
  // SD used/total in SASD's short units, appended after the battery ("" = not known yet).
  if (virt) {
    uint64_t used = 0, total = 0;
    bool ok = vdiskUsage(&used, &total);
    if (!vdfsMounted()) vdiskClose();   // HotSync or the FTP server may hold it open
    w.str(ok ? sdHumanBytes(used, 3) + "/" + sdHumanBytes(total, 1) : String("blank"));
  } else {
    w.str(sdUsageText());
  }
  w.u8(sasdDiskMode);   // SASD disk setting (0 auto, 1 SD card, 2 virtual), after the SD text
  reply((uint8_t) (CTL_STATUS + 0x20), gOut, w.len);
}

// After a STATUS answered from the menus: if nothing has mounted the SD card yet, try now so the next STATUS can say whether there is one. After the reply so it is never held up, and never inside a
// mode (SASD/HotSync/FTP own the card there).
void sdProbeAfterReply() {
  if (SD.cardType() != CARD_NONE) {
    if (menuMode != MODE_MAIN || sdAlive()) { gSdState = 1; return; }     // inside a mode the card is not touched
    gSdState = 0;
  }
  if (gSdState == 0 && millis() - gSdProbeMs < SD_REPROBE_MS) return;
  gSdProbeMs = millis();
  gSdState = sdMount() ? 1 : 0;
  LOGV("PalmCtl: SD probe - %s (CS %d SCK %d MOSI %d MISO %d, %lu Hz)\r\n",
                               gSdState ? "card present" : "no card", (int) SD_CS, (int) SPI_SCK,
                               (int) SPI_MOSI, (int) SPI_MISO, (unsigned long) WIRSA_SD_HZ);
}

void sendScan() {
  int n = WiFi.scanNetworks();
  if (n < 0) n = 0;
  if (n > MAX_SCAN) n = MAX_SCAN;
  Writer w(gOut, sizeof(gOut));
  w.u8((uint8_t) n);
  for (int i = 0; i < n; i++) {
    String s = WiFi.SSID(i);
    if (s.length() > 32) s = s.substring(0, 32);
    w.str(s);
    w.u8((uint8_t) (int8_t) WiFi.RSSI(i));
    w.u8(WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? 1 : 0);
  }
  WiFi.scanDelete();
  reply((uint8_t) (CTL_WIFI_SCAN + 0x20), gOut, w.len);
}

void wifiSet(size_t len) {
  Reader r(gBody, len);
  String newSsid = r.str();
  String newPass = r.str();
  if (!r.ok || newSsid.length() == 0 || newSsid.length() > 32 || newPass.length() > 63) {
    replyErr(ERR_BAD_REQUEST, "Bad WiFi details");
    return;
  }
  bool ok = wifiTestConnection(newSsid.c_str(), newPass.c_str());
  if (ok) wifiSaveCredentials(newSsid.c_str(), newPass.c_str());
  else wifiReconnectToSaved();   // back to the network that was there before
  newPass = "";
  replyOk(CTL_WIFI_SET, ok);
}

// Log level on the wire: 0 off, 1 info (an older app's "USB debug on"), 2 warn, 3 verbose.
uint8_t logLevelToWire(uint8_t l) { return l == LOG_WARN ? 2 : l == LOG_INFO ? 1 : l == LOG_VERBOSE ? 3 : 0; }
uint8_t logLevelFromWire(uint8_t w) { return w == 2 ? LOG_WARN : w == 3 ? LOG_VERBOSE : w == 1 ? LOG_INFO : LOG_OFF; }

void cfgGet() {
  Writer w(gOut, sizeof(gOut));
  w.u8(kWireBaud[serialSpeed < NUM_BAUDS ? serialSpeed : 0]);   // the Palm link's rate numbers (see kWireBaud)
  w.u8(logLevelToWire(gLogLevel));
  w.u8(dispOrientation);
#if WIRSA_FEATURE_FTPSERVER
  w.str(ftpUser);
  w.str(apSsid);
  uint8_t passSet = (ftpPass.length() ? 1 : 0) | (apPass.length() ? 2 : 0);
#else
  w.str(String(""));
  w.str(String(""));
  uint8_t passSet = 0;
#endif
#if WIRSA_FEATURE_SLIP
  loadSlipSettings();
  const IPAddress *ips[4] = { &slipModeCtx.config.slipIP, &slipModeCtx.config.clientIP,
                              &slipModeCtx.config.subnetMask, &slipModeCtx.config.dnsServer };
  for (int i = 0; i < 4; i++)
    for (int j = 0; j < 4; j++) w.u8((*ips[i])[j]);
#else
  for (int i = 0; i < 16; i++) w.u8(0);
#endif
  w.u8(passSet);
  w.u8(modeUnusedExitSec);   // Exit Timers - optional tail, older apps stop reading before it
  w.u8(modeAfterExitSec);
  w.u8(slipTimeoutMin);      // SLIP timeout, minutes - optional tail too
  w.u8(sasdDiskMode);        // SASD disk: 0 auto, 1 SD card, 2 virtual card - and this one
  w.u8(deepSleepMin);        // deep sleep delay, minutes (0 off) - and this one
  w.u8(sasdTimeoutMin);      // SASD timeout, minutes (3, 5, 10, 15) - and this one
  w.u8(wifiPulse);           // WiFi pulse: 0 always on, 1 15 s, 2 30 s, 3 1 min, 4 never - and this one
  reply((uint8_t) (CTL_CFG_GET + 0x20), gOut, w.len);
}

void cfgSet(size_t len) {
  Reader r(gBody, len);
  uint8_t wireBaud = r.u8();
  uint8_t baudIdx = baudFromWire(wireBaud);   // an old 300..4800 becomes 9600
  uint8_t usb = r.u8();
  uint8_t orient = r.u8();
  String user = r.str();
  String ap = r.str();
  uint8_t ip[16];
  bool ipOk = r.bytesRaw(ip, sizeof(ip));
  if (!r.ok || !ipOk || baudIdx >= NUM_BAUDS || orient > 1) {
    replyErr(ERR_BAD_REQUEST, "Bad settings");
    return;
  }
  if (r.pos + 2 <= r.len) {   // Exit Timers - optional tail (older apps don't send it)
    modeUnusedExitSec = r.u8();
    modeAfterExitSec = r.u8();
  }
  if (r.pos + 1 <= r.len) {   // SLIP timeout, minutes - optional tail
    uint8_t m = r.u8();
    if (slipTimeoutValid(m)) slipTimeoutMin = m;
  }
  if (r.pos + 1 <= r.len) {   // SASD disk - optional tail
    uint8_t d = r.u8();
    if (d <= SASD_DISK_VIRTUAL) sasdDiskMode = d;
  }
  if (r.pos + 1 <= r.len) {   // deep sleep delay - optional tail
    uint8_t m = r.u8();
    if (deepSleepValid(m)) deepSleepMin = m;
  }
  if (r.pos + 1 <= r.len) {   // SASD timeout, minutes - optional tail
    uint8_t m = r.u8();
    if (sasdTimeoutValid(m)) sasdTimeoutMin = m;
  }
  if (r.pos + 1 <= r.len) {   // WiFi pulse - optional tail
    uint8_t m = r.u8();
    if (wifiPulseValid(m)) wifiPulse = m;
  }

  logSetLevel(logLevelFromWire(usb));
  dispOrientation = orient;
  display.setRotation(dispOrientation == 0 ? 0 : 2);   // 0 = D_NORMAL (main.cpp)
#if WIRSA_FEATURE_FTPSERVER
  ftpUser = user.substring(0, FTP_USER_MAX);
  apSsid = ap.substring(0, AP_SSID_MAX);
#endif
#if WIRSA_FEATURE_SLIP
  slipModeCtx.config.slipIP = IPAddress(ip[0], ip[1], ip[2], ip[3]);
  slipModeCtx.config.clientIP = IPAddress(ip[4], ip[5], ip[6], ip[7]);
  slipModeCtx.config.subnetMask = IPAddress(ip[8], ip[9], ip[10], ip[11]);
  slipModeCtx.config.dnsServer = IPAddress(ip[12], ip[13], ip[14], ip[15]);
  saveSlipSettings();
#endif
  bool newBaud = baudIdx != serialSpeed;
  serialSpeed = baudIdx;
  writeSettings();
  replyOk(CTL_CFG_SET);

  // The menu (and this link) move to the new rate once the reply is out -
  // same restart as baudLoop's.
  if (newBaud) {
    PhysicalSerial.flush();
    PhysicalSerial.end();
    delay(200);
    menuBaudReset();
    PhysicalSerial.begin(menuBaud(), WIRSA_SERIAL_FORMAT, RXD2, TXD2);
  }
}

void secretsSet(size_t len) {
  Reader r(gBody, len);
  String fp = r.str();
  String ap = r.str();
  if (!r.ok) { replyErr(ERR_BAD_REQUEST, "Bad passwords"); return; }
#if WIRSA_FEATURE_FTPSERVER
  if (fp.length()) ftpPass = fp.substring(0, FTP_PASS_MAX);
  if (ap.length()) apPass = ap.substring(0, AP_PASS_MAX);
  writeSettings();
#endif
  fp = ""; ap = "";
  replyOk(CTL_SECRETS);
}

// ENTER_MODE: check WiFi first for the modes that need it (their own entry would stop at a prompt), answer at once, then hand the letter to the main menu, which starts the mode as if typed.
// The SD card is checked only from the last STATUS probe (below): a fresh probe before the reply cost the reply on hardware.
void enterMode(size_t len) {
  if (len < 1) { replyErr(ERR_BAD_REQUEST, "Bad ENTER"); return; }
  char m = (char) gBody[0];
  if (!Modes::knownCtlLetter(m)) {
    replyErr(ERR_NOT_A_MODE, "Not a mode");
    return;
  }
  if (!modeBuilt(m)) { replyErr(ERR_NOT_BUILT, "Mode not built into this WiRSa"); return; }
  const Modes::Info *mode = Modes::byKey(m);
  if (mode->needsWifi && WiFi.status() != WL_CONNECTED) {
    replyErr(ERR_NO_WIFI, "WiRSa is not connected to WiFi");
    return;
  }
  // HotSync ('H') and SASD ('D') need the card unless the virtual card can stand in. Probing here cost the reply on hardware, so only what the last STATUS probe found is used (gSdState); never probed =
  // not blocked, and the menu's own check still applies. The probe is retried after the reply, so inserting a card and asking again works.
  if (mode->needsCard && (sasdDiskMode == SASD_DISK_SD || !vdiskPresent()) &&
      SD.cardType() == CARD_NONE && gSdState == 0) {
    replyErr(ERR_NO_SD, "No SD card detected");
    PhysicalSerial.flush();
    sdProbeAfterReply();
    return;
  }
  replyOk(CTL_ENTER);
  PhysicalSerial.flush();
  gPendingKey = m;
  // A second (flags) byte may follow - ignored: every mode now goes back
  // to the main menu on the Exit Timers (mode_timer.h) by itself.
  if (menuMode != MODE_MAIN) {
    msgFlag = true;
    mainMenu(false);
  }
}

#if WIRSA_FEATURE_DUAL_BAUD
// SPEED: WiRSaCtl asks for its Palm's Preferred Baud. The reply names the rate the menu will use (the fastest in bauds[] not above the request; the ESP32 can run any) and goes out at the old rate;
// then the menu moves.
void setSpeed(size_t len) {
  if (len < 4) { replyErr(ERR_BAD_REQUEST, "Bad SPEED"); return; }
  Reader r(gBody, len);
  uint32_t want = r.u32();
  uint32_t rate = (uint32_t) bauds[0];
  for (int i = 0; i < NUM_BAUDS; i++)
    if ((uint32_t) bauds[i] <= want) rate = (uint32_t) bauds[i];
  Writer w(gOut, sizeof(gOut));
  w.u8(1);
  w.u32(rate);
  reply((uint8_t) (CTL_SPEED + 0x20), gOut, w.len);
  gScript = false;
  setMenuRate(rate);
  LOGV("PalmCtl: SPEED %lu asked - menu now at %lu baud\r\n",
                              (unsigned long) want, (unsigned long) menuBaud());
}
#endif

// Session log, a page at a time. Request: op u8 (0 read, 1 clear), offset u32.
// Reply: total u32, offset u32, n u16, n text bytes (oldest first).
void sendLog(size_t len) {
  Reader r(gBody, len);
  uint8_t op = r.u8();
  uint32_t off = r.u32();
  if (!r.ok) { replyErr(ERR_BAD_REQUEST, "Bad LOG"); return; }
  if (op == 1) logClear();
  const size_t kPage = 480;
  uint8_t page[kPage];
  size_t total = logSize();
  size_t n = off < total ? logRead(off, page, kPage) : 0;
  Writer w(gOut, sizeof(gOut));
  w.u32((uint32_t) total);
  w.u32(off);
  w.u16((uint16_t) n);
  for (size_t i = 0; i < n; i++) w.u8(page[i]);
  reply((uint8_t) (CTL_LOG + 0x20), gOut, w.len);
}

// System page, a page at a time (same reply shape as LOG). Request: op u8 (unused), offset u32.
// The text is built on the first page (offset 0) and kept only until the next request for it.
// Lines are kept short: the Palm shows them in a 140-pixel field.
static const char *sysResetName(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_POWERON:   return "power on";
    case ESP_RST_SW:        return "software";
    case ESP_RST_PANIC:     return "CRASH (panic)";
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:       return "CRASH (watchdog)";
    case ESP_RST_BROWNOUT:  return "BROWNOUT";
    case ESP_RST_DEEPSLEEP: return "deep sleep";
    case ESP_RST_EXT:       return "reset button";
    default:                return "unknown";
  }
}

static const char *sysPartSub(uint8_t type, uint8_t sub) {
  static char b[12];
  if (type == ESP_PARTITION_TYPE_APP) {
    if (sub == ESP_PARTITION_SUBTYPE_APP_FACTORY) return "factory";
    if (sub >= ESP_PARTITION_SUBTYPE_APP_OTA_MIN && sub < ESP_PARTITION_SUBTYPE_APP_OTA_MIN + 16) {
      snprintf(b, sizeof(b), "ota_%u", (unsigned) (sub - ESP_PARTITION_SUBTYPE_APP_OTA_MIN));
      return b;
    }
  } else {
    switch (sub) {
      case ESP_PARTITION_SUBTYPE_DATA_OTA:      return "ota";
      case ESP_PARTITION_SUBTYPE_DATA_NVS:      return "nvs";
      case ESP_PARTITION_SUBTYPE_DATA_COREDUMP: return "coredump";
      case ESP_PARTITION_SUBTYPE_DATA_FAT:      return "fat";
      case ESP_PARTITION_SUBTYPE_DATA_SPIFFS:   return "spiffs";
      default: break;
    }
  }
  snprintf(b, sizeof(b), "0x%02X", (unsigned) sub);
  return b;
}

static size_t buildSysText(char *o, size_t cap) {
  size_t n = 0;
#define SYS(...) do { if (n < cap) n += snprintf(o + n, cap - n, __VA_ARGS__); } while (0)
  const char *fm = "?";
  switch (ESP.getFlashChipMode()) {
    case FM_QIO: fm = "QIO"; break;
    case FM_QOUT: fm = "QOUT"; break;
    case FM_DIO: fm = "DIO"; break;
    case FM_DOUT: fm = "DOUT"; break;
    case FM_FAST_READ: fm = "FAST"; break;
    case FM_SLOW_READ: fm = "SLOW"; break;
    default: break;
  }
  SYS("%s rev %u, %u cores\n", ESP.getChipModel(), (unsigned) ESP.getChipRevision(), (unsigned) ESP.getChipCores());
  SYS("CPU %u MHz, core %.0f C\n", (unsigned) ESP.getCpuFreqMHz(), (double) temperatureRead());
  SYS("Flash %u MB, %s, %u MHz\n", (unsigned) (ESP.getFlashChipSize() >> 20), fm, (unsigned) (ESP.getFlashChipSpeed() / 1000000));
  SYS("Firmware %u KB of %u KB\n", (unsigned) (ESP.getSketchSize() >> 10),
      (unsigned) ((ESP.getSketchSize() + ESP.getFreeSketchSpace()) >> 10));
  SYS("\nRAM %u KB\n used %u, free %u KB\n low %u, block %u KB\n",
      (unsigned) (ESP.getHeapSize() >> 10), (unsigned) ((ESP.getHeapSize() - ESP.getFreeHeap()) >> 10),
      (unsigned) (ESP.getFreeHeap() >> 10), (unsigned) (ESP.getMinFreeHeap() >> 10),
      (unsigned) (ESP.getMaxAllocHeap() >> 10));
  if (psramFound()) {
    SYS("\nPSRAM %u KB\n used %u, free %u KB\n low %u, block %u KB\n",
        (unsigned) (ESP.getPsramSize() >> 10), (unsigned) ((ESP.getPsramSize() - ESP.getFreePsram()) >> 10),
        (unsigned) (ESP.getFreePsram() >> 10), (unsigned) (ESP.getMinFreePsram() >> 10),
        (unsigned) (ESP.getMaxAllocPsram() >> 10));
  } else {
    SYS("\nPSRAM none\n");
  }
  uint32_t up = millis() / 1000;
  SYS("\nUp %lud %luh %lum\n", (unsigned long) (up / 86400), (unsigned long) (up / 3600 % 24), (unsigned long) (up / 60 % 60));
  SYS("Reset: %s\n", sysResetName(esp_reset_reason()));
  SYS("IDF %s\n", ESP.getSdkVersion());
  if (vdiskPresent()) {
    SYS("\nVirtual card\n %u KB, %s\n", (unsigned) (vdiskSectors() >> 1),
        vdiskIsFormatted() ? "FAT16" : "blank (not formatted)");
    if (!vdfsMounted()) vdiskClose();   // HotSync or the FTP server may hold it open
  } else {
    SYS("\nVirtual card: none\n");
  }
  SYS("\nPartitions\n");
  esp_partition_iterator_t it = esp_partition_find(ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, NULL);
  for (; it; it = esp_partition_next(it)) {
    const esp_partition_t *p = esp_partition_get(it);
    SYS(" %s %s\n  @0x%X %uK\n", p->label, sysPartSub((uint8_t) p->type, (uint8_t) p->subtype),
        (unsigned) p->address, (unsigned) (p->size >> 10));
  }
  esp_partition_iterator_release(it);
#undef SYS
  return n < cap ? n : cap;
}

void sendSys(size_t len) {
  Reader r(gBody, len);
  (void) r.u8();
  uint32_t off = r.u32();
  if (!r.ok) { replyErr(ERR_BAD_REQUEST, "Bad SYSINFO"); return; }
  static char text[2000];
  static size_t total = 0;
  if (off == 0) total = buildSysText(text, sizeof(text));
  const size_t kPage = 480;
  size_t n = off < total ? (total - off < kPage ? total - off : kPage) : 0;
  Writer w(gOut, sizeof(gOut));
  w.u32((uint32_t) total);
  w.u32(off);
  w.u16((uint16_t) n);
  for (size_t i = 0; i < n; i++) w.u8((uint8_t) text[off + i]);
  reply((uint8_t) (CTL_SYSINFO + 0x20), gOut, w.len);
}

// FORMAT_SD: "FORMAT" (6 bytes), copy u8. Erases the SD card (sd_format.h); with copy set, then copies the virtual card's
// SASD install set onto it. Reply: ok u8 (1), fat u8 (16 or 32), copied u8, total u8, note str ("" or why the copy failed).
void formatSd(size_t len) {
  if (len < 7 || memcmp(gBody, "FORMAT", 6) != 0) { replyErr(ERR_BAD_REQUEST, "Bad FORMAT_SD"); return; }
  bool copy = gBody[6] != 0;
#if WIRSA_FEATURE_FTPSERVER
  if (ftpServerRunning) { replyErr(ERR_BUSY, "Stop the FTP server first."); return; }
#endif
  String err;
  uint8_t fat = 0;
  if (!sdFormatCard(&fat, err)) {
    gSdState = SD.cardType() != CARD_NONE ? 1 : 0;
    replyErr(err == "No SD card" ? ERR_NO_SD : ERR_FAILED, err.c_str());
    return;
  }
  gSdState = 1;
  uint8_t copied = 0, total = 0;
  String note;
  if (copy && !sdCopySasdFromVirtual(&copied, &total, err)) note = err;
  Writer w(gOut, sizeof(gOut));
  w.u8(1);
  w.u8(fat);
  w.u8(copied);
  w.u8(total);
  w.str(note);
  reply((uint8_t) (CTL_FORMAT_SD + 0x20), gOut, w.len);
}

// A frame read in a menu (main menu or any menu screen).
void handleMenuFrame(uint8_t type, size_t len, bool enc) {
  if (RelayProto::handleKex(PhysicalSerial, type, gBody, len)) return;

  // Only the password commands may (and must) arrive encrypted. A plain frame while a session is up means the Palm gave up on it: drop it so the reply goes back in plaintext.
  bool secret = type == CTL_WIFI_SET || type == CTL_SECRETS;
  if (!enc && RelayProto::isSecure()) RelayProto::secureReset();
  if (secret != enc) {
    replyErr(ERR_BAD_REQUEST, secret ? "Must be sent encrypted" : "Must be sent plain");
    RelayProto::secureReset();
    return;
  }

  bool changed = false;
  switch (type) {
    case CTL_PING:      sendPing(gBody, len, currentModeLetter()); break;
    case CTL_STATUS:
      sendStatus(currentModeLetter());
      PhysicalSerial.flush();
      sdProbeAfterReply();
      break;
    case CTL_WIFI_SCAN: sendScan(); break;
    case CTL_WIFI_SET:  wifiSet(len); changed = true; break;
    case CTL_CFG_GET:   cfgGet(); break;
    case CTL_CFG_SET:   cfgSet(len); changed = true; break;
    case CTL_SECRETS:   secretsSet(len); changed = true; break;
    case CTL_FTP:
#if WIRSA_FEATURE_FTPSERVER
      if (len >= 1 && gBody[0]) ftpServerStart(false);
      else ftpServerStop();
      replyOk(CTL_FTP);
      changed = true;
#else
      replyErr(ERR_NOT_BUILT, "FTP not built into this WiRSa");
#endif
      break;
    case CTL_ENTER:     enterMode(len); break;
    case CTL_EXIT:      replyOk(CTL_EXIT); break;   // already in the menus
    case CTL_REBOOT:
      replyOk(CTL_REBOOT);
      PhysicalSerial.flush();
      delay(100);
      ESP.restart();
      break;
#if WIRSA_FEATURE_DUAL_BAUD
    case CTL_SPEED:     setSpeed(len); break;
#endif
    case CTL_LOG:       sendLog(len); break;
    case CTL_SYSINFO:   sendSys(len); break;
    case CTL_FORMAT_SD: formatSd(len); changed = true; break;
    case CTL_FACTORY:
      if (len != 5 || memcmp(gBody, "RESET", 5) != 0) { replyErr(ERR_BAD_REQUEST, "Bad FACTORY_RESET"); break; }
      replyOk(CTL_FACTORY);
      PhysicalSerial.flush();
      defaultEEPROM();
      delay(100);
      ESP.restart();
      break;
    default:
      replyErr(ERR_BAD_REQUEST, "Unknown request");
      break;
  }
  if (enc) RelayProto::secureReset();   // one encrypted command per session
  if (changed && gPendingKey == 0) {
    // Settings or WiFi changed under the screen: back to a fresh main menu.
    msgFlag = true;
    mainMenu(false);
  }
}

// Called with the 0x16 already consumed from io: PhysicalSerial, or the second receiver, where a good frame first moves the menu to its rate (the reply goes out at that rate). True if a good frame came.
bool menuFrame(HardwareSerial &io) {
  bool wasBinary = binaryModeActive;
  setBinaryMode(true);                 // no menu text on the line mid-exchange
  RelayProto::setControlLink(true);
  uint8_t type;
  size_t len;
  bool enc;
  bool ok = readCtlFrame(io, type, len, enc);
  if (ok) {
#if WIRSA_FEATURE_DUAL_BAUD
    if (&io != &PhysicalSerial) heardOnListener();
#endif
    handleMenuFrame(type, len, enc);
  }
  RelayProto::setControlLink(false);
  setBinaryMode(wasBinary);
  return ok;
}

// Burst guard on one port, for the byte c just read: it counts as a key only if the line was quiet for GUARD_MS before it and nothing but CR/LF follows within GUARD_MS. Otherwise the burst is drained
// (stopping at a lead byte, so a control frame right behind the junk still gets through) and false returns. lastRx: that port's last-byte time.
bool guardKey(HardwareSerial &io, unsigned long &lastRx, int c, bool logDrop) {
  unsigned long now = millis();
  bool quietBefore = (now - lastRx) >= GUARD_MS;
  lastRx = now;
  bool burst = false;
  while (millis() - now < GUARD_MS) {
    if (io.available()) {
      int d = io.peek();
      if (d == '\r' || d == '\n') { io.read(); continue; }
#if WIRSA_FEATURE_SLIP
      // A Palm network login script sends "S" and starts SLIP right after, so its first packet (SLIP END, 0xC0) can arrive inside the window: accept the S and leave the packet for SLIP mode, which does not drain at entry.
      if ((c == 'S' || c == 's') && d == 0xC0) break;
#endif
      burst = true;
      break;
    }
    yield();
  }
  if (quietBefore && !burst) return true;
  unsigned long quiet = millis();
  int drained = 1;
  while (millis() - quiet < GUARD_MS) {
    if (io.available()) {
      if (io.peek() == RelayProto::CTL_LEAD) break;
      io.read();
      drained++;
      quiet = millis();
    } else {
      yield();
    }
  }
  lastRx = millis();
  if (logDrop && logOn(LOG_VERBOSE)) logPrintf(LOG_VERBOSE, "PalmCtl: burst guard dropped %d byte(s)\r\n", drained);
  return false;
}

#if WIRSA_FEATURE_DUAL_BAUD
// The second receiver, read only when PhysicalSerial has nothing: a good control frame or a single printable key there moves the menu to its rate. Anything else is the Palm's normal-rate traffic seen
// at the wrong rate, and is dropped quietly.
int listenKey(char &chr) {
  if (!gListenOn || !ListenSerial.available()) return 0;
#if WIRSA_FEATURE_AUTOBAUD
  if (gAw) return 0;                   // a BR line is awaited
#endif
  if (ListenSerial.peek() == RelayProto::CTL_LEAD) {
    ListenSerial.read();
    menuFrame(ListenSerial);
    gLastListenRx = millis();
    return 0;
  }
  int c = ListenSerial.read();
#if WIRSA_FEATURE_AUTOBAUD
  if (phraseCommand(ListenSerial, c)) { gLastListenRx = millis(); return 0; }
#endif
  if (!guardKey(ListenSerial, gLastListenRx, c, false) || c < 0x20 || c > 0x7E) return 0;
  heardOnListener();
  chr = (char) c;
  LOGV("PalmCtl: key 0x%02X from the second receiver\r\n", (unsigned) c);
  return 1;
}

void listenDiscard() {
  if (!gListenOn) return;
  while (ListenSerial.available()) ListenSerial.read();
  gLastListenRx = millis();
}
#endif

}  // namespace

bool palmCtlRxPending() {
  if (PhysicalSerial.available()) return true;
#if WIRSA_FEATURE_DUAL_BAUD
  if (gListenOn && ListenSerial.available()) return true;
#endif
  return false;
}

uint32_t menuBaud() {
  return gSessionBaud ? gSessionBaud : homeBaud();
}

// The maximum (configured baud) changed: the menu keeps its session rate (SPEED is not capped by the maximum); the second receiver may move. The caller restarts PhysicalSerial at menuBaud().
void menuBaudReset() {
#if WIRSA_FEATURE_DUAL_BAUD
  gScript = false;
#endif
  if (gSessionBaud == homeBaud()) gSessionBaud = 0;
#if WIRSA_FEATURE_DUAL_BAUD
  listenSync();
#endif
}

void palmCtlPoll() {
  if (menuMode == MODE_MAIN) {
    // One probe at boot so the first STATUS already knows. After that a missing card is
    // only retried when a STATUS asks (sdProbeAfterReply), never by polling.
    if (gSdState < 0 && SD.cardType() == CARD_NONE && !PhysicalSerial.available())
      sdProbeAfterReply();
    sdUsageStep();   // a few FAT sectors per pass; never inside a mode
  }
#if WIRSA_FEATURE_DUAL_BAUD
  listenSync();
#if WIRSA_FEATURE_AUTOBAUD
  awaitService();
  scriptService();
  autobaudService();
#endif
#endif
}

int menuKey(char &chr) {
  if (gPendingKey && menuMode == MODE_MAIN) {
    chr = gPendingKey;
    gPendingKey = 0;
    return 1;
  }
  if (Serial.available()) {            // USB console: not guarded
    chr = (char) SerialRead();
    return 1;
  }
#if WIRSA_FEATURE_DUAL_BAUD && WIRSA_FEATURE_AUTOBAUD
  if (gAw) return 0;                   // a BR line is awaited: palmCtlPoll reads it
#endif
  if (!PhysicalSerial.available()) {
#if WIRSA_FEATURE_DUAL_BAUD
    return listenKey(chr);
#else
    return 0;
#endif
  }

  if (PhysicalSerial.peek() == RelayProto::CTL_LEAD) {
    PhysicalSerial.read();
    bool ok = menuFrame(PhysicalSerial);
    gLastPhysRx = millis();
#if WIRSA_FEATURE_DUAL_BAUD
    if (ok) listenDiscard();   // the same frame, garbled at the other rate
#else
    (void) ok;
#endif
    return 0;
  }

  // Burst guard: one key, alone on the line (CR/LF after it is fine).
  int c = PhysicalSerial.read();
  hexDumpByte('R', (uint8_t) c);
#if WIRSA_FEATURE_DUAL_BAUD && WIRSA_FEATURE_AUTOBAUD
  if (phraseCommand(PhysicalSerial, c)) { gLastPhysRx = millis(); return 0; }
#endif
  if (!guardKey(PhysicalSerial, gLastPhysRx, c, true)) return 0;
  chr = (char) c;
  LOGV("PalmCtl: key 0x%02X from the Palm line\r\n", (unsigned) c);
  return 1;
}

bool palmCtlModeHook(char mode, bool exitAllowed, uint8_t maxRelayType) {
  // Junk filter (relay modes only): a byte that cannot start one of the mode's frames (type 1..maxRelayType, plain or with 0x80, or a key-exchange frame) is dropped here. Otherwise the mode's frame
  // reader takes it for a header and swallows what follows for up to its 3 s byte timeout (a PING sent at the wrong rate arrives as such junk, and the real PING right after would be lost).
  while (maxRelayType && PhysicalSerial.available()) {
    uint8_t b = (uint8_t) PhysicalSerial.peek();
    uint8_t base = b & 0x7F;
    if (b == RelayProto::CTL_LEAD || (base >= 1 && base <= maxRelayType) || (b >= 0x70 && b <= 0x74))
      break;
    PhysicalSerial.read();
    LOGV("PalmCtl: dropped junk byte 0x%02X in mode %c\r\n", b, mode);
  }
  if (!PhysicalSerial.available() || PhysicalSerial.peek() != RelayProto::CTL_LEAD) return false;
  PhysicalSerial.read();

  // A lead byte at a frame boundary only comes from WiRSaCtl (or a relay app in Screenless Mode before its own session): the relay app is not mid-session, so its session (if any) goes and replies are plaintext.
  RelayProto::secureReset();
  RelayProto::setControlLink(true);
  bool leave = false;
  uint8_t type;
  size_t len;
  bool enc;
  if (readCtlFrame(PhysicalSerial, type, len, enc) && !enc) {
    switch (type) {
      case CTL_PING:   sendPing(gBody, len, mode); break;
      case CTL_STATUS: sendStatus(mode); break;
      case CTL_EXIT:
        if (exitAllowed) {
          replyOk(CTL_EXIT);
          PhysicalSerial.flush();
          leave = true;
        } else {
          replyErr(ERR_BUSY, "A session is still running");
        }
        break;
      case CTL_ENTER:
        // Already in this mode (a relay app in Screenless Mode found it
        // running): nothing to do.
        if (len >= 1 && gBody[0] == (uint8_t) mode) {
          replyOk(CTL_ENTER);
        } else {
          replyErr(ERR_BUSY, "WiRSa is in a mode - choose Main Menu first");
        }
        break;
      default:
        replyErr(ERR_BUSY, "WiRSa is in a mode - choose Main Menu first");
        break;
    }
  }
  RelayProto::setControlLink(false);
  return leave;
}

#endif  // WIRSA_FEATURE_PALMCTL
