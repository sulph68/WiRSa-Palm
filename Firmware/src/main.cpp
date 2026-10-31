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

// Changes against the original firmware: docs/CHANGELOG.md.

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
#include "arduino-timer.h"
#include <HardwareSerial.h>

// project modules
#include "globals.h"
#include "led.h"
#include "serial_io.h"
#include "settings.h"
#include "network.h"
#include "display_menu.h"
#include "modes.h"
#include "firmware.h"
#include "ftp_server.h"    // Background FTP server (replaces MODEM mode + web config)
#include "slip_mode.h"     // SLIP gateway mode
#include "sasd.h"          // Serial-attached SD mode
#include "hotsync.h"        // HotSync Install mode
#include "boot_button.h"
#include "palm_ctl.h"       // menuKey(), dual-rate menu
#include "deep_sleep.h"     // deep sleep after idle (T7-S3)
#include "wifi_setup.h"    // WiFi setup wizard
#include "diagnostics.h"   // Diagnostic tools
#include "bitwarden_relay.h" // BitWarden Relay mode (Vaultwarden serial bridge)
#include "caldav_relay.h" // CalDAV Relay mode (calendar sync serial bridge)
#include "mesh_relay.h" // Mesh Relay mode (Meshtastic node over Bluetooth)

// globals.h holds the EEPROM-address, pin and menu-mode constants (VERSIONA/B, the *_ADDRESS/*_LEN layout, SWITCH_PIN/LED_PIN, SW1-4_PIN, MENU_*, SCREEN_*, SD_CS/SPI_*, RXD2/TXD2); this file
// keeps no copies. OLED_SDA/OLED_SCL are not there (I2C pins, needed only for Wire.begin() below).
#if defined(WIRSA_BOARD_T7S3)
#define OLED_SDA I2C_SDA_PIN   // Qwiic bus (no OLED on this board)
#define OLED_SCL I2C_SCL_PIN
#else
#define OLED_SDA 21
#define OLED_SCL 20
#endif
#if WIRSA_HAS_DISPLAY
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
#else
NullDisplay display;   // screenless build - see globals.h/null_display.h
#endif

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

// Relocated from file_transfer.cpp when it was removed; sasd.cpp and hotsync.cpp use it for their slk-layer CRC (declared in globals.h). The CRC-32 table that lived here had no other consumer and went too.
uint16_t zmCRC16Update(uint16_t crc, uint8_t b) {
  return (crc << 8) ^ pgm_read_word(&crc16_ccitt_table[(crc >> 8) ^ b]);
}

// globals
String build = "v1.1";
String cmd = "";              // Gather a new AT command to this string from serial
bool cmdMode = true;          // Are we in AT command mode or connected mode
bool verboseResults = false;
#undef DEBUG
// TX_BUF_SIZE is defined in globals.h only.
unsigned long lastRingMs = 0; // Time of last "RING" message (millis())
char plusCount = 0;           // Go to AT mode at "+++" sequence, that has to be counted
unsigned long plusTime = 0;   // When did we last receive a "+++" sequence
unsigned long ledTime = 0;
// (the ESP TCP stack fills TX_BUF_SIZE's buffer in blocks, so it cannot be one byte at a time)
uint8_t txBuf[TX_BUF_SIZE];

// Nothing below 9600 is used (the Palm line has not run slower since the first WiRSaCtl).
extern const int bauds[] = { 9600, 14400, 19200, 28800, 38400, 57600, 115200 };
String baudDisp[] = { "9600", "14.4k", "19.2k", "28.8k", "38.4k", "57.6k", "115k" };
byte serialSpeed;

// Feature build flags (globals.h) turn menu items and their code on or off at compile time. A disabled item cannot leave a gap (every later item's position, and every menuSel== comparison
// in mainLoop()/settingsLoop() that uses it, must shift down). So fixed-size arrays, a runtime COUNT and named runtime INDEX variables (buildMainMenu()/buildSettingsMenu(), called once from setup())
// replace the static initializers, and the dispatch code compares against the index, not a literal.
String mainMenuDisp[10];
int mainMenuCount = 0;

void buildMainMenu() {
  // The entries are the table's built rows with a label, in row order (modes.h); the last one is Config, always built.
  const char *labels[10];
  mainMenuCount = Modes::menuEntries(labels, 10);
  for (int i = 0; i < mainMenuCount; i++) mainMenuDisp[i] = labels[i];
}

// "FTP Setup" would auto-bracket to the same "[F]..." hint as "Factory Reset", and "AP Setup" to the same "[A]..." as "About", but the real keys are 'T' and 'P' (settingsLoop()), so both carry
// an explicit inline "[X]" (as mainMenuDisp's "SAS[D] SDCard") instead of showMenu()'s auto-bracket-first-letter fallback.
String settingsMenuDisp[12];
int settingsMenuCount = 0;
int smIdxMain = -1, smIdxWifiSetup = -1, smIdxFtpSetup = -1, smIdxApSetup = -1, smIdxSlipSetup = -1, smIdxBaud = -1, smIdxOrientation = -1, smIdxUsbDebug = -1, smIdxExitTimers = -1, smIdxFactoryReset = -1, smIdxReboot = -1, smIdxAbout = -1;

void buildSettingsMenu() {
  settingsMenuCount = 0;
  smIdxMain = settingsMenuCount; settingsMenuDisp[settingsMenuCount++] = "Main Menu";
  smIdxWifiSetup = settingsMenuCount; settingsMenuDisp[settingsMenuCount++] = "WiFi Setup";
#if WIRSA_FEATURE_FTPSERVER
  smIdxFtpSetup = settingsMenuCount; settingsMenuDisp[settingsMenuCount++] = "F[T]P Setup";
  smIdxApSetup = settingsMenuCount; settingsMenuDisp[settingsMenuCount++] = "A[P] Setup";
#endif
#if WIRSA_FEATURE_SLIP
  smIdxSlipSetup = settingsMenuCount; settingsMenuDisp[settingsMenuCount++] = "SLIP Setup";
#endif
#if WIRSA_FEATURE_PALMCTL && WIRSA_FEATURE_DUAL_BAUD
  smIdxBaud = settingsMenuCount; settingsMenuDisp[settingsMenuCount++] = "Baud Rate (Max)";
#else
  smIdxBaud = settingsMenuCount; settingsMenuDisp[settingsMenuCount++] = "Baud Rate";
#endif
  smIdxOrientation = settingsMenuCount; settingsMenuDisp[settingsMenuCount++] = "Orientation";
  smIdxUsbDebug = settingsMenuCount; settingsMenuDisp[settingsMenuCount++] = "USB Debug";
  smIdxExitTimers = settingsMenuCount; settingsMenuDisp[settingsMenuCount++] = "Exit Timers";
  smIdxFactoryReset = settingsMenuCount; settingsMenuDisp[settingsMenuCount++] = "Factory Reset";
  smIdxReboot = settingsMenuCount; settingsMenuDisp[settingsMenuCount++] = "Reboot";
  smIdxAbout = settingsMenuCount; settingsMenuDisp[settingsMenuCount++] = "About";
}
String orientationMenuDisp[] = { "Normal", "Flipped" };
String defaultModeDisp[] = { "Main Menu", "MODEM Mode" };
// Auto-Detect Baud, Hex Dump Mode, Loopback Test and Signal Monitor were never Utilities items, and were not reachable via AT$BAUD?/AT$HEX/AT$LOOP/AT$SIG either: that dispatcher had no callers once
// MODEM mode's parser went. Removed in a later size cleanup (see globals.h).
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
// No hardware flow control: FLOW_CONTROL_ADDRESS (119) and DTR_MODE_ADDRESS (121) exist but stay unused rather than reassigned (see settings.cpp).
enum dispOrientation_t { D_NORMAL, D_FLIPPED }; // Normal or Flipped
byte dispOrientation = D_NORMAL;
enum defaultMode_t { D_MAINMENU, D_MODEMMENU }; // Main or Modem
byte defaultMode = D_MAINMENU;

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
HardwareSerial PhysicalSerial(WIRSA_PALM_UART);

// Serial I/O is in src/modules/serial_io.cpp, settings in settings.cpp, network in network.cpp, and the menu loops (mainLoop, baudLoop, orientationLoop, settingsLoop, ftpServerLoop, led_on,
// printSDCardInfo) in display_menu.cpp.

// forward declarations for functions still in main.cpp
void showMessage(String message);
void showMenu(String menuName, String options[], int sz, int dispMode, int defaultSel);
void menuOption(int idx, String item);
void settingsMenu(bool arrow);
void baudMenu(bool arrow);
void orientationMenu(bool arrow);

// MODEM mode (AT-command dial-up and telnet emulation, modem.cpp) is removed, replaced by the background FTP server (ftp_server.cpp/.h). What was modem-specific went with it (connectTimeString,
// sendString, checkButton, displayCurrentSettings/StoredSettings, waitForSpace/waitForEnter, displayHelp, storeSpeedDial, hangUp, answerCall, handleIncomingConnection, the AT parser command(),
// handleFlowControl, enterModemMode, modemLoop, refreshDisplay). The general menu-loop code that other features need (mainLoop, baudLoop, orientationLoop, settingsLoop, led_on, printSDCardInfo)
// moved to display_menu.cpp.

// File Transfer (XMODEM/YMODEM/ZMODEM: fileMenu/fileLoop/protocolMenu/protocolLoop/listFilesMenu/listFilesLoop in display_menu.cpp, and src/modules/file_transfer.cpp/.h) was unreachable
// (MODE_FILE had become HotSync Install) and is removed. Only zmCRC16Update() survives (above the CRC-16 table it reads, declared in globals.h), for sasd.cpp and hotsync.cpp. playback.cpp/.h went
// too: displayChar/listFiles/getFileSize existed only to support File Transfer.

void setup() {
  logInit();   // session log in RTC memory: first, so the reset reason of this boot is in it
  deepSleepBoot();
#if WIRSA_FEATURE_MESH
  meshHeadlessWake();   // a timer wake armed for a background check does it here and sleeps again: no return
#endif
  // I2C for Wire
  Wire.begin(OLED_SDA, OLED_SCL);

  // GPIO 15 is an ESP32 strapping pin: an undriven or pulled-low GPIO15 can hang the chip at boot. Drive it OUTPUT HIGH at once, before any delay or peripheral init.
#if !defined(WIRSA_BOARD_T7S3)   // 15 is the expansion UART TX there
  const int GPIO15_BOOT_SAFETY_PIN = 15;
  pinMode(GPIO15_BOOT_SAFETY_PIN, OUTPUT);
  digitalWrite(GPIO15_BOOT_SAFETY_PIN, HIGH);
#endif

#if WIRSA_HAS_DISPLAY
  // SSD1306_SWITCHCAPVCC: generate the display voltage from 3.3 V
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
#endif

  ledInit();
  pinMode(SD_CS, OUTPUT);
  digitalWrite(SD_CS, HIGH);
  SPI.begin(SPI_SCK, SPI_MISO, SPI_MOSI, SD_CS);
  pinMode(SWITCH_PIN, INPUT_PULLUP);   // BOOT button (boot_button.cpp)
#if !defined(WIRSA_BOARD_T7S3)   // no SW1-4 there (36/37 are PSRAM pins)
  pinMode(SW1_PIN, INPUT);
  pinMode(SW2_PIN, INPUT);
  pinMode(SW3_PIN, INPUT);
  pinMode(SW4_PIN, INPUT);
#endif

  EEPROM.begin(LAST_ADDRESS + 1);
  delay(10);

  if (EEPROM.read(VERSION_ADDRESS) != VERSIONA || EEPROM.read(VERSION_ADDRESS + 1) != VERSIONB) {
    defaultEEPROM();
  }

  readSettings();
  // Out-of-bounds check: we must be able to talk. This fixed a bug found diagnosing a reproducible CalDAV baud-negotiation failure: sizeof(bauds) is the BYTE size (36), not the element count (9),
  // so serialSpeed 9..36 passed as valid, each an out-of-bounds bauds[] read at PhysicalSerial.begin() below and at CDDV_HELLO's maxAllowed = bauds[serialSpeed] (caldav_relay.cpp). Whatever
  // memory it hit became the real UART baud at boot, and every CDDV_HELLO the Palm sent at its fixed 9600 idle baud was read as noise, on every attempt. (serialSpeed is unsigned, so the old `< 0` half was dead.)
  if (serialSpeed >= sizeof(bauds) / sizeof(bauds[0])) {
    serialSpeed = 0; //9600
  }
  // Data/parity/stop bits are locked to WIRSA_SERIAL_FORMAT (globals.h), no longer a setting (the "Serial Config" menu entry is gone).

  if (dispOrientation==D_NORMAL)
    display.setRotation(0);
  else
    display.setRotation(2);

 
  Serial.setRxBufferSize(4096);
#if defined(ARDUINO_USB_CDC_ON_BOOT) && ARDUINO_USB_CDC_ON_BOOT
  // 4 KB TX buffer: with no monitor attached the ring keeps the most recent output and sends it when one connects.
  Serial.setTxBufferSize(4096);
  Serial.begin(115200);  // USB CDC (HWCDC): baud is ignored, no frame format
  Serial.setTxTimeoutMs(50);
  // Native USB re-enumerates on every reset and the monitor reopens the port a moment later. Wait for it, but only if a host is there: the cable's SOF frames tell a live host from a charger or a
  // battery-only board, which get no delay.
  {
    bool host = true;
    for (uint32_t t0 = millis(); millis() - t0 < 60; delay(5))
      if (!HWCDC::isPlugged()) { host = false; break; }     // settles in a few ms
    if (host) {
      for (uint32_t t0 = millis(); !Serial && HWCDC::isPlugged() && millis() - t0 < WIRSA_USB_WAIT_MS; ) delay(10);
      if (Serial) delay(150);   // let the monitor finish opening
    }
  }
#else
  Serial.begin(115200, SERIAL_8N1); //USB Serial always runs at 115k
#endif
  PhysicalSerial.setRxBufferSize(4096);  // larger buffer for ZModem transfers (fewer CRC errors from WiFi delays)
  // The menu's rate: 9600 after every boot with the dual-rate menu (the configured baud is then the maximum), else the configured baud.
  PhysicalSerial.begin(menuBaud(), WIRSA_SERIAL_FORMAT, RXD2, TXD2); //Physical Serial

  // Settle the Palm line: after a reset or a deep-sleep wake (which also runs setup()) it can carry noise, and one stray 'D' would start SASD. Discard everything that arrives for 1.5 s before any key or frame is read.
  for (uint32_t t0 = millis(); millis() - t0 < 1500; ) {
    while (PhysicalSerial.available()) PhysicalSerial.read();
    delay(5);
  }

  SerialPrintLn("");
  SerialPrintLn("-= RetroDisks  WiRSa =-");
#if defined(ARDUINO_USB_CDC_ON_BOOT) && ARDUINO_USB_CDC_ON_BOOT
  Serial.printf("Log level: %s  free heap: %u  PSRAM: %u of %u\r\n",
                gLogLevel == LOG_OFF ? "off" : gLogLevel == LOG_WARN ? "warn" : gLogLevel == LOG_INFO ? "info" : "verbose", (unsigned) ESP.getFreeHeap(),
                (unsigned) ESP.getFreePsram(), (unsigned) ESP.getPsramSize());
#endif

  // Fill mainMenuDisp[]/settingsMenuDisp[] (and the mmIdx*/smIdx* indices) from the WIRSA_FEATURE_* flags; must run before the first mainMenu(false) below, which reads mainMenuCount.
  buildMainMenu();
  buildSettingsMenu();

  mainMenu(false);
#if WIRSA_FEATURE_MESH
  meshBgBoot();
#endif
}

// Arduino main loop
void loop()
{
#if WIRSA_FEATURE_FTPSERVER
  // The FTP server pump runs every pass whatever menuMode is, so it keeps accepting connections and moving data in HotSync, SASD, SLIP Gateway or any mode (the non-blocking socket idiom of slip_nat.cpp; no FreeRTOS tasks here).
  ftpServerPump();
#endif

  // Every pass (as ftpServerPump()) so a configured network is connected and kept connected in any mode. Not feature-gated: WiFi Setup (Config) is always built.
  maintainWiFi();

#if WIRSA_FEATURE_BOOT_HOTSYNC && WIRSA_FEATURE_HOTSYNC
  // BOOT button: HotSync on demand (boot_button.cpp) - one pin read a pass.
  bootButtonPoll();
#endif

  // Dual-rate menu: the second receiver on the Palm RX pin runs in the menus only (palm_ctl.cpp); one state check a pass.
  deepSleepPoll();
#if WIRSA_FEATURE_MESH
  meshBgPoll();
  meshSpoolPoll();
#endif
  palmCtlPoll();

  Modes::runLoop(menuMode);   // the loop of the running mode (modes.h)
}
