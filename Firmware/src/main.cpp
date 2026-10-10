/**************************************************************************
   RetroDisks WiRSa v3 - Wi-Fi RS232 Serial Modem Adapter with File Transfer features
   Copyright (C) 2024 Aron Hoekstra <nullvalue@gmail.com>

   based on:
     Virtual modem for ESP8266
     https://github.com/jsalin/esp8266_modem
     Copyright (C) 2015 Jussi Salin <salinjus@gmail.com>
   
   and the improvements added by:
     WiFi SIXFOUR - A virtual WiFi modem based on the ESP 8266 chipset by Alwyz
     https://1200baud.wordpress.com/2017/03/04/build-your-own-9600-baud-c64-wifi-modem-for-20/
   
  Permission is hereby granted, free of charge, to any person obtaining a copy
  of this software and associated documentation files (the "Software"), to deal
  in the Software without restriction, including without limitation the rights
  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
  copies of the Software, and to permit persons to whom the Software is
  furnished to do so, subject to the following conditions:
 
  The above copyright notice and this permission notice shall be included in all
  copies or substantial portions of the Software.
 
  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
  SOFTWARE.
 **************************************************************************
 This software uses libraries published by Adafruit for LCD screen control
 Adafruit_SSD1306 & Adafruit_GFX.
 **************************************************************************
 This software uses the LinkedList library by Ivan Seidel
 http://github.com/ivanseidel/LinkedList
 **************************************************************************
 This software uses the arduino-timer libary by
 https://github.com/contrem/arduino-timer
 **************************************************************************/

 /*
 changes 3.10
 - modified signal monitor to show on screen while in modem mode
 - fixed telnet call mode to assert DCD
 - fixed some pin polarity issues

 this fork vs. the original nullvalue0/WiRSa firmware (https://github.com/nullvalue0/WiRSa)

 added:
 - HotSync Install mode - pushes .PRC/.PDB files from an SD card to the Palm over a real HotSync session
 - SASD (Serial-Attached SD Card) mode - lets the Palm browse/use the SD card as mass storage over serial
 - BitWarden Relay mode - bridges a Palm password-manager client to a Vaultwarden server over WiFi
 - FTP Server mode - background FTP server for browsing/editing the SD card over WiFi
 - background WiFi auto-connect/reconnect with a live signal-strength status icon on every title bar
 - per-feature compile-time toggles (WIRSA_FEATURE_*) to build a trimmed-down firmware

 removed:
 - MODEM Mode - the original's Hayes AT-command dial-up/telnet/BBS modem emulation is gone entirely
 - PPP Gateway - superseded by SLIP Gateway
 - built-in web server for status monitoring/file management - superseded by FTP Server
 - File Transfer (XMODEM/YMODEM/ZMODEM) as a standalone menu - its mode slot was repurposed for HotSync, no longer reachable
 - Text Playback from SD card - its mode slot was repurposed for SASD, no longer reachable
 - 10 speed-dial entries - only usable for dialing in MODEM Mode; storage/EEPROM plumbing (speedDials[]/DIAL0-9_ADDRESS) had actually been left behind despite this note, confirmed zero real consumer, and was fully removed in a later size-cleanup pass
 - RTS/CTS hardware flow control and DTR signaling - not wired on this hardware
 - Utilities menu items Auto-Detect Baud, Hex Dump Mode toggle, Loopback Test, Signal Monitor, and the whole AT$SIG/AT$LOOP/AT$HEX/AT$BAUD?/AT$SYS/AT$STAT command dispatcher - this note previously claimed they "stay reachable via AT$..." but that was never true post-MODEM-Mode-removal (confirmed zero callers of the dispatcher itself); genuinely removed in the same size-cleanup pass as the speed-dial entries above. hexDumpByte() is the one function of this whole family left in place (still called from serial_io.cpp) since its own enabling flag can now never be set true - functionally inert rather than actually dead code.
 - echo/autoAnswer/petTranslate/consoleMode/IP_TYPE settings and their EEPROM storage - same MODEM-Mode-era pattern: round-tripped through EEPROM every boot, zero real consumer anywhere, removed in the same pass
 - serialConfig/bits[]/bitsDisp[] (24-entry SERIAL_5N1..SERIAL_8O2 config table + display labels) - serialConfig was only ever ASSIGNED via EEPROM.read(), never WRITTEN by any live UI (that menu was already gone), so it was permanently stuck at its default; the 5 real PhysicalSerial.begin() call sites now use WIRSA_SERIAL_FORMAT directly, same as the CalDAV/BitWarden relay modes already did

 other behavior:
 - SLIP Gateway starts immediately from the main menu instead of opening a submenu first; BACK returns to that submenu
 - live baud rate shown on every screen's title bar, including BitWarden Relay and SLIP Gateway
 - Utilities' Information screen is a scrollable status view instead of a fixed popup that could overflow the screen
 - globals.h is the single source of truth for pin/EEPROM #define blocks (main.cpp/network.cpp/settings.cpp no longer keep their own copies)
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <EEPROM.h>
#include <ESPmDNS.h>
#include <LinkedList.h>
#include <SPI.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include "SD.h"
//#include "FS.h"
#include "arduino-timer.h"
#include <HardwareSerial.h>

// Project modules
#include "globals.h"
#include "serial_io.h"
#include "settings.h"
#include "network.h"
#include "display_menu.h"
#include "firmware.h"
#include "ftp_server.h"    // Background FTP server (replaces MODEM mode + web config)
#include "slip_mode.h"     // SLIP gateway mode
#include "sasd.h"          // Serial-attached SD mode
#include "hotsync.h"        // HotSync Install mode
#include "wifi_setup.h"    // WiFi setup wizard
#include "diagnostics.h"   // Diagnostic tools
#include "bitwarden_relay.h" // BitWarden Relay mode (Vaultwarden serial bridge)
#include "caldav_relay.h" // CalDAV Relay mode (calendar sync serial bridge)

// globals.h is the canonical copy for EEPROM-address, pin, and menu-mode
// constants: VERSIONA/B, every *_ADDRESS/*_LEN EEPROM layout constant,
// SWITCH_PIN/LED_PIN, SW1-4_PIN, MENU_BOTH/NUM/DISP,
// SCREEN_WIDTH/HEIGHT/OLED_RESET/SCREEN_ADDRESS, SD_CS/SPI_*, RXD2/TXD2 -
// this file has no local copies of any of those, so there's exactly one
// place to manage each. OLED_SDA/OLED_SCL are not in globals.h (I2C pins,
// only ever needed here for the Wire.begin() call below).
#define OLED_SDA 21
#define OLED_SCL 20
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// CRC-16 CCITT table
const uint16_t crc16_ccitt_table[256] PROGMEM = {
  0x0000, 0x1021, 0x2042, 0x3063, 0x4084, 0x50a5, 0x60c6, 0x70e7,
  0x8108, 0x9129, 0xa14a, 0xb16b, 0xc18c, 0xd1ad, 0xe1ce, 0xf1ef,
  0x1231, 0x0210, 0x3273, 0x2252, 0x52b5, 0x4294, 0x72f7, 0x62d6,
  0x9339, 0x8318, 0xb37b, 0xa35a, 0xd3bd, 0xc39c, 0xf3ff, 0xe3de,
  0x2462, 0x3443, 0x0420, 0x1401, 0x64e6, 0x74c7, 0x44a4, 0x5485,
  0xa56a, 0xb54b, 0x8528, 0x9509, 0xe5ee, 0xf5cf, 0xc5ac, 0xd58d,
  0x3653, 0x2672, 0x1611, 0x0630, 0x76d7, 0x66f6, 0x5695, 0x46b4,
  0xb75b, 0xa77a, 0x9719, 0x8738, 0xf7df, 0xe7fe, 0xd79d, 0xc7bc,
  0x48c4, 0x58e5, 0x6886, 0x78a7, 0x0840, 0x1861, 0x2802, 0x3823,
  0xc9cc, 0xd9ed, 0xe98e, 0xf9af, 0x8948, 0x9969, 0xa90a, 0xb92b,
  0x5af5, 0x4ad4, 0x7ab7, 0x6a96, 0x1a71, 0x0a50, 0x3a33, 0x2a12,
  0xdbfd, 0xcbdc, 0xfbbf, 0xeb9e, 0x9b79, 0x8b58, 0xbb3b, 0xab1a,
  0x6ca6, 0x7c87, 0x4ce4, 0x5cc5, 0x2c22, 0x3c03, 0x0c60, 0x1c41,
  0xedae, 0xfd8f, 0xcdec, 0xddcd, 0xad2a, 0xbd0b, 0x8d68, 0x9d49,
  0x7e97, 0x6eb6, 0x5ed5, 0x4ef4, 0x3e13, 0x2e32, 0x1e51, 0x0e70,
  0xff9f, 0xefbe, 0xdfdd, 0xcffc, 0xbf1b, 0xaf3a, 0x9f59, 0x8f78,
  0x9188, 0x81a9, 0xb1ca, 0xa1eb, 0xd10c, 0xc12d, 0xf14e, 0xe16f,
  0x1080, 0x00a1, 0x30c2, 0x20e3, 0x5004, 0x4025, 0x7046, 0x6067,
  0x83b9, 0x9398, 0xa3fb, 0xb3da, 0xc33d, 0xd31c, 0xe37f, 0xf35e,
  0x02b1, 0x1290, 0x22f3, 0x32d2, 0x4235, 0x5214, 0x6277, 0x7256,
  0xb5ea, 0xa5cb, 0x95a8, 0x8589, 0xf56e, 0xe54f, 0xd52c, 0xc50d,
  0x34e2, 0x24c3, 0x14a0, 0x0481, 0x7466, 0x6447, 0x5424, 0x4405,
  0xa7db, 0xb7fa, 0x8799, 0x97b8, 0xe75f, 0xf77e, 0xc71d, 0xd73c,
  0x26d3, 0x36f2, 0x0691, 0x16b0, 0x6657, 0x7676, 0x4615, 0x5634,
  0xd94c, 0xc96d, 0xf90e, 0xe92f, 0x99c8, 0x89e9, 0xb98a, 0xa9ab,
  0x5844, 0x4865, 0x7806, 0x6827, 0x18c0, 0x08e1, 0x3882, 0x28a3,
  0xcb7d, 0xdb5c, 0xeb3f, 0xfb1e, 0x8bf9, 0x9bd8, 0xabbb, 0xbb9a,
  0x4a75, 0x5a54, 0x6a37, 0x7a16, 0x0af1, 0x1ad0, 0x2ab3, 0x3a92,
  0xfd2e, 0xed0f, 0xdd6c, 0xcd4d, 0xbdaa, 0xad8b, 0x9de8, 0x8dc9,
  0x7c26, 0x6c07, 0x5c64, 0x4c45, 0x3ca2, 0x2c83, 0x1ce0, 0x0cc1,
  0xef1f, 0xff3e, 0xcf5d, 0xdf7c, 0xaf9b, 0xbfba, 0x8fd9, 0x9ff8,
  0x6e17, 0x7e36, 0x4e55, 0x5e74, 0x2e93, 0x3eb2, 0x0ed1, 0x1ef0
};

// Relocated from file_transfer.cpp during its removal (XMODEM/YMODEM/ZMODEM
// file transfer, confirmed unreachable from the main menu) - sasd.cpp and
// hotsync.cpp both still call this for their own shared slk-layer CRC (see
// its declaration in globals.h). The CRC-32 table that used to live here
// too (crc32tab[]) had no other consumer and was removed with the rest.
uint16_t zmCRC16Update(uint16_t crc, uint8_t b) {
  return (crc << 8) ^ pgm_read_word(&crc16_ccitt_table[(crc >> 8) ^ b]);
}

// Global variables
String build = "v3.10";
String cmd = "";              // Gather a new AT command to this string from serial
bool cmdMode = true;          // Are we in AT command mode or connected mode
bool verboseResults = false;
//#define DEBUG 1             // Print additional debug information to serial channel
#undef DEBUG
// TX_BUF_SIZE is defined in globals.h only - the variables below resolve
// their values from there.
unsigned long lastRingMs = 0; // Time of last "RING" message (millis())
//long myBps;                 // What is the current BPS setting
char plusCount = 0;           // Go to AT mode at "+++" sequence, that has to be counted
unsigned long plusTime = 0;   // When did we last receive a "+++" sequence
unsigned long ledTime = 0;
// (TX_BUF_SIZE's own buffer fill direction is very blocking by the ESP TCP
// stack, so we can't do one byte a time.)
uint8_t txBuf[TX_BUF_SIZE];

extern const int bauds[] = { 300, 1200, 2400, 4800, 9600, 19200, 38400, 57600, 115200 };
String baudDisp[] = { "300", "1200", "2400", "4800", "9600", "19.2k", "38.4k", "57.6k", "115k" };
byte serialSpeed;

// Feature build flags (globals.h) turn menu ITEMS (and their whole
// implementation) on/off at compile time - both mainMenuDisp[] and
// settingsMenuDisp[] used to be fixed initializer lists, but a disabled
// item can't just leave a gap (every LATER item's array position, and every
// menuSel== comparison in mainLoop()/settingsLoop() that keys off that
// position, has to shift down to fill it). Fixed-size arrays + a runtime
// COUNT + named runtime INDEX variables (buildMainMenu()/buildSettingsMenu()
// below, called once from setup()) replace the old static initializers so
// each enabled item lands wherever it actually ends up, and the dispatch
// code below compares against the resulting index instead of a literal.
String mainMenuDisp[8];
int mainMenuCount = 0;
int mmIdxFtp = -1, mmIdxHotsync = -1, mmIdxSasd = -1, mmIdxSlip = -1, mmIdxBitwarden = -1, mmIdxCalDav = -1, mmIdxUtilities = -1, mmIdxConfig = -1;

void buildMainMenu() {
  mainMenuCount = 0;
#if WIRSA_FEATURE_FTPSERVER
  mmIdxFtp = mainMenuCount; mainMenuDisp[mainMenuCount++] = "FTP Server";
#endif
#if WIRSA_FEATURE_HOTSYNC
  mmIdxHotsync = mainMenuCount; mainMenuDisp[mainMenuCount++] = "HotSync";
#endif
#if WIRSA_FEATURE_SASD
  mmIdxSasd = mainMenuCount; mainMenuDisp[mainMenuCount++] = "SAS[D] SDCard";
#endif
#if WIRSA_FEATURE_SLIP
  mmIdxSlip = mainMenuCount; mainMenuDisp[mainMenuCount++] = "SLIP Gateway";
#endif
#if WIRSA_FEATURE_BITWARDEN
  mmIdxBitwarden = mainMenuCount; mainMenuDisp[mainMenuCount++] = "BitWarden Relay";
#endif
#if WIRSA_FEATURE_CALDAV
  mmIdxCalDav = mainMenuCount; mainMenuDisp[mainMenuCount++] = "C[A]lDAV Relay";
#endif
#if WIRSA_FEATURE_UTILITIES
  mmIdxUtilities = mainMenuCount; mainMenuDisp[mainMenuCount++] = "Utilities";
#endif
  mmIdxConfig = mainMenuCount; mainMenuDisp[mainMenuCount++] = "Config";  // always built, never gated
}

// "FTP Setup" would auto-bracket to the same "[F]..." serial hint as
// "Factory Reset", and "AP Setup" to the same "[A]..." as "About" - both
// real keys are 'T'/'P' (settingsLoop() below), not the literal first
// letter, so both need an explicit inline "[X]" hint (same convention
// mainMenuDisp's own "SAS[D] SDCard" already uses) rather than relying on
// showMenu()'s auto-bracket-first-letter fallback.
String settingsMenuDisp[10];
int settingsMenuCount = 0;
int smIdxMain = -1, smIdxWifiSetup = -1, smIdxFtpSetup = -1, smIdxApSetup = -1, smIdxBaud = -1, smIdxOrientation = -1, smIdxUsbDebug = -1, smIdxFactoryReset = -1, smIdxReboot = -1, smIdxAbout = -1;

void buildSettingsMenu() {
  settingsMenuCount = 0;
  smIdxMain = settingsMenuCount; settingsMenuDisp[settingsMenuCount++] = "Main Menu";
  smIdxWifiSetup = settingsMenuCount; settingsMenuDisp[settingsMenuCount++] = "WiFi Setup";
#if WIRSA_FEATURE_FTPSERVER
  smIdxFtpSetup = settingsMenuCount; settingsMenuDisp[settingsMenuCount++] = "F[T]P Setup";
  smIdxApSetup = settingsMenuCount; settingsMenuDisp[settingsMenuCount++] = "A[P] Setup";
#endif
  smIdxBaud = settingsMenuCount; settingsMenuDisp[settingsMenuCount++] = "Baud Rate";
  smIdxOrientation = settingsMenuCount; settingsMenuDisp[settingsMenuCount++] = "Orientation";
  smIdxUsbDebug = settingsMenuCount; settingsMenuDisp[settingsMenuCount++] = "USB Debug";
  smIdxFactoryReset = settingsMenuCount; settingsMenuDisp[settingsMenuCount++] = "Factory Reset";
  smIdxReboot = settingsMenuCount; settingsMenuDisp[settingsMenuCount++] = "Reboot";
  smIdxAbout = settingsMenuCount; settingsMenuDisp[settingsMenuCount++] = "About";
}
String orientationMenuDisp[] = { "Normal", "Flipped" };
String defaultModeDisp[] = { "Main Menu", "MODEM Mode" };
// Auto-Detect Baud/Hex Dump Mode/Loopback Test/Signal Monitor were never
// Utilities menu items, and (unlike this comment used to claim) weren't
// actually reachable via AT$BAUD?/AT$HEX/AT$LOOP/AT$SIG either - the
// dispatcher those AT commands went through had zero real callers left
// anywhere once MODEM Mode's own command parser was removed. Confirmed
// and removed for real in a later size-cleanup pass (see globals.h).
String diagnosticsMenuDisp[] = { "Main Menu", "Retry WiFi", "Information" };

bool quietMode = false;        // ATQ1 suppresses result codes
byte escChar = '+';            // ATS2 escape character (255=disabled)
String ssid, password, busyMsg;
String ftpUser, ftpPass;
String apSsid, apPass;
bool ftpServerRunning = false;
byte ringCount = 0;
String resultCodes[] = { "OK", "CONNECT", "RING", "NO CARRIER", "ERROR", "", "NO DIALTONE", "BUSY", "NO ANSWER" };
enum resultCodes_t { R_OK_STAT, R_CONNECT, R_RING, R_NOCARRIER, R_ERROR, R_NONE, R_NODIALTONE, R_BUSY, R_NOANSWER };
unsigned long connectTime = 0;
bool bHex = false;
// This device uses automatic flow control only, no hardware flow control -
// FLOW_CONTROL_ADDRESS (119) and DTR_MODE_ADDRESS (121) EEPROM slots exist
// but are left unused rather than reassigned; see settings.cpp.
enum dispOrientation_t { D_NORMAL, D_FLIPPED }; // Normal or Flipped
byte dispOrientation = D_NORMAL;
enum defaultMode_t { D_MAINMENU, D_MODEMMENU }; // Main or Modem
byte defaultMode = D_MAINMENU;
bool usbDebug = true;  // Enable USB serial debug output (default: enabled)

int menuMode = MODE_MAIN;
int lastMenu = -1;
int menuCnt = 0;
int menuIdx = 0;
int dispCharCnt = 0;
int dispLineCnt = 0;
int dispAnsiCnt = 0;

auto timer = timer_create_default();
bool msgFlag=false;
bool BTNUP=false;
bool BTNDN=false;
bool BTNEN=false;
bool BTNBK=false;

extern const uint8_t PROGMEM wifi_symbol[] = {
  0b00111100,
  0b01000010,
  0b10011001,
  0b00100100,
  0b01000010,
  0b00011000,
  0b00100100
};

MDNSResponder mdns;
HardwareSerial PhysicalSerial(2);

// Serial I/O functions moved to src/modules/serial_io.cpp
// Settings functions moved to src/modules/settings.cpp
// Network functions moved to src/modules/network.cpp
// Menu-loop functions (mainLoop, baudLoop, orientationLoop, settingsLoop,
// ftpServerLoop, getLine, prompt, led_on, checkCancel, clearInputBuffer,
// printSDCardInfo) moved to src/modules/display_menu.cpp

// Forward declarations for functions still in main.cpp
void showMessage(String message);
void showMenu(String menuName, String options[], int sz, int dispMode, int defaultSel);
void menuOption(int idx, String item);
void settingsMenu(bool arrow);
void baudMenu(bool arrow);
void orientationMenu(bool arrow);
String getLine();
void clearInputBuffer();
bool checkCancel();

// ============================================================================
// MODEM mode removed entirely (AT-command dial-up/telnet emulation,
// modem.cpp) - replaced by the background FTP server (ftp_server.cpp/.h).
// Some of what used to live in modem.cpp was genuinely modem-specific and
// was deleted with it (connectTimeString, sendString, checkButton,
// displayCurrentSettings/StoredSettings, waitForSpace/waitForEnter,
// displayHelp, storeSpeedDial, hangUp, led_on's modem-only callers,
// answerCall, handleIncomingConnection, command() the AT parser,
// handleFlowControl, enterModemMode, modemLoop, refreshDisplay). The rest
// wasn't actually modem-specific - it was the general menu-loop
// infrastructure (mainLoop, baudLoop, orientationLoop, settingsLoop,
// getLine, prompt, led_on, printSDCardInfo, sendResult) that other kept
// features (slip_mode.cpp) also depended on - that was relocated to
// src/modules/display_menu.cpp instead, alongside its existing *Menu()
// counterparts.
// ============================================================================

// File Transfer (XModem/YModem/ZModem over fileMenu/fileLoop/protocolMenu/
// protocolLoop/listFilesMenu/listFilesLoop in display_menu.cpp, plus all of
// src/modules/file_transfer.cpp/include/file_transfer.h itself) was
// confirmed unreachable from the main menu - MODE_FILE, its old mode slot,
// had already been repurposed for HotSync Install (see globals.h/hotsync.h)
// - and has since been removed entirely. Only zmCRC16Update() (now defined
// just above the CRC-16 table it reads, declared in globals.h) survived,
// since sasd.cpp/hotsync.cpp both still use it for their own shared
// slk-layer CRC. src/modules/playback.cpp/include/playback.h went with it -
// their own remaining content (displayChar/listFiles/getFileSize) existed
// only to support File Transfer (its own header comment said so), and had
// zero other callers once that was gone.

void setup() {
  // Setup I2C addresses for Wire
  Wire.begin(OLED_SDA, OLED_SCL);

  // GPIO 15 is an ESP32 strapping pin, regardless of what (if anything) is
  // wired to it - an undriven/pulled-low GPIO15 can hang the ESP32 at boot.
  // Configure it as OUTPUT HIGH immediately, before any delays or
  // peripheral init.
  const int GPIO15_BOOT_SAFETY_PIN = 15;
  pinMode(GPIO15_BOOT_SAFETY_PIN, OUTPUT);
  digitalWrite(GPIO15_BOOT_SAFETY_PIN, HIGH);

  // SSD1306_SWITCHCAPVCC = generate display voltage from 3.3V internally
  if(!display.begin(SSD1306_SWITCHCAPVCC, SCREEN_ADDRESS)) {
    Serial.println(F("SSD1306 allocation failed"));
    for(;;); // Don't proceed, loop forever
  }
  display.display();
  display.setTextSize(1);      // Normal 1:1 pixel scale
  display.setTextColor(SSD1306_WHITE); // Draw white text
  display.cp437(true);         // Use full 256 char 'Code Page 437' font
  delay(2000); // Pause for 2 seconds
  display.clearDisplay();
  display.setCursor(0, 0);     // Start at top-left corner

  pinMode(LED_PIN, OUTPUT);
  pinMode(SD_CS, OUTPUT);
  digitalWrite(SD_CS, HIGH);
  SPI.begin(SPI_SCK, SPI_MISO, SPI_MOSI, SD_CS);
  digitalWrite(LED_PIN, HIGH); // off
  pinMode(SWITCH_PIN, INPUT);
  digitalWrite(SWITCH_PIN, HIGH);
  pinMode(SW1_PIN, INPUT);
  pinMode(SW2_PIN, INPUT);
  pinMode(SW3_PIN, INPUT);
  pinMode(SW4_PIN, INPUT);

  EEPROM.begin(LAST_ADDRESS + 1);
  delay(10);

  if (EEPROM.read(VERSION_ADDRESS) != VERSIONA || EEPROM.read(VERSION_ADDRESS + 1) != VERSIONB) {
    defaultEEPROM();
  }

  readSettings();
  // Check if it's out of bounds-- we have to be able to talk.
  // BUG (found while diagnosing a reproducible CalDAV baud-negotiation
  // failure): sizeof(bauds) is the array's BYTE size (36, sizeof(int)*9),
  // not its element count (9) - this let serialSpeed values 9..36 through
  // as "valid", each one an out-of-bounds bauds[] read at both the
  // PhysicalSerial.begin() below and CDDV_HELLO's own maxAllowed =
  // bauds[serialSpeed] (caldav_relay.cpp). Whatever garbage memory that
  // read landed on became the real UART baud rate at boot - every
  // CDDV_HELLO the Palm sends at the real, hardcoded 9600 idle baud gets
  // misread as noise against a WiRSa UART actually running at that bogus
  // rate, deterministically every attempt. (serialSpeed is an unsigned
  // byte - the `< 0` half of the old check was always false, dead code.)
  if (serialSpeed >= sizeof(bauds) / sizeof(bauds[0])) {
    serialSpeed = 4; //9600
  }
  // Data/parity/stop bits are locked to WIRSA_SERIAL_FORMAT (globals.h) -
  // no longer a runtime setting, see settingsMenuDisp[]'s dropped
  // "Serial Config" entry.

  if (dispOrientation==D_NORMAL)
    display.setRotation(0);
  else
    display.setRotation(2);

 
  Serial.setRxBufferSize(4096);
  Serial.begin(115200, SERIAL_8N1); //USB Serial always runs at 115k
  PhysicalSerial.setRxBufferSize(4096);  // Larger buffer for ZModem transfers (reduces CRC errors from WiFi delays)
  PhysicalSerial.begin(bauds[serialSpeed], WIRSA_SERIAL_FORMAT, RXD2, TXD2); //Physical Serial

  SerialPrintLn("");
  SerialPrintLn("-= RetroDisks  WiRSa =-");

  // Populate mainMenuDisp[]/settingsMenuDisp[] (and their matching mmIdx*/
  // smIdx* runtime indices) from the WIRSA_FEATURE_* flags - must run
  // before the first mainMenu(false) below, which reads mainMenuCount.
  buildMainMenu();
  buildSettingsMenu();

  mainMenu(false);
}

/**
   Arduino main loop function
*/
void loop()
{
#if WIRSA_FEATURE_FTPSERVER
  // FTP server pump runs unconditionally every iteration, independent of
  // menuMode, so it keeps accepting connections/moving data while the user
  // is in HotSync/SASD/SLIP Gateway/any other mode - mirrors slip_nat.cpp's
  // own non-blocking socket idiom (no FreeRTOS tasking anywhere in this
  // codebase).
  ftpServerPump();
#endif

  // Called unconditionally every loop() iteration (same placement as
  // ftpServerPump() above) so a configured network gets connected and kept
  // connected regardless of which mode/menu is active. Not feature-gated -
  // WiFi Setup (Config) is always built.
  maintainWiFi();

  if (menuMode==MODE_MAIN)
    mainLoop();
#if WIRSA_FEATURE_HOTSYNC
  else if (menuMode==MODE_FILE)
    hotsyncLoop();
#endif
#if WIRSA_FEATURE_SASD
  else if (menuMode==MODE_PLAYBACK)
    sasdLoop();
#endif
  else if (menuMode==MODE_SETTINGS)
    settingsLoop();
  else if (menuMode==MODE_SETBAUD)
    baudLoop();
  else if (menuMode==MODE_ORIENTATION)
    orientationLoop();
#if WIRSA_FEATURE_SLIP
  else if (menuMode==MODE_SLIP)
    slipLoop();
#endif
  else if (menuMode==MODE_WIFI_SETUP)
    wifiSetupLoop();
  else if (menuMode==MODE_WIFI_PASSWORD)
    wifiPasswordLoop();
#if WIRSA_FEATURE_UTILITIES
  else if (menuMode==MODE_DIAGNOSTICS)
    diagnosticsLoop();
#endif
#if WIRSA_FEATURE_FTPSERVER
  else if (menuMode==MODE_FTPSERVER)
    ftpServerLoop();
#endif
#if WIRSA_FEATURE_BITWARDEN
  else if (menuMode==MODE_BITWARDEN)
    bitwardenRelayLoop();
#endif
#if WIRSA_FEATURE_CALDAV
  else if (menuMode==MODE_CALDAV_RELAY)
    caldavRelayLoop();
#endif
}
