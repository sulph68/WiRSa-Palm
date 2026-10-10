#ifndef GLOBALS_H
#define GLOBALS_H

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <ESPmDNS.h>
#include <HardwareSerial.h>
#include <Adafruit_SSD1306.h>
#include "SD.h"
#include "arduino-timer.h"

// Debug flags - uncomment to enable verbose debug output

// Pin definitions
#define SWITCH_PIN 0       // GPIO0 (programming mode pin)
#define LED_PIN 2          // Status LED

// RTS/CTS/DTR aren't wired on this hardware
// DCD/DSR/RI are gone too for the same reason.

#define SW1_PIN 39         //DOWN
#define SW2_PIN 36         //BACK
#define SW3_PIN 37         //ENTER
#define SW4_PIN 38         //UP

// SD Card Pins
#define SD_CS 12
#define SPI_MOSI 41
#define SPI_MISO 42
#define SPI_SCK 40

#define RXD2 16
#define TXD2 17

// Physical UART data/parity/stop-bit format - locked to 8-N-1, not a
// runtime/menu setting. Change here to alter for all builds.
#define WIRSA_SERIAL_FORMAT SERIAL_8N1

// Mode definitions
#define MODE_MAIN 0
// MODE_MODEM (1) MODEM mode removed, replaced by MODE_FTPSERVER (16)
#define MODE_FILE 2   // repurposed: was "File Transfer", now HotSync Install - see hotsync.h/.cpp
#define MODE_PLAYBACK 3  // repurposed - also used by SASD mode, see sasd.cpp
#define MODE_SETTINGS 4
#define MODE_SETBAUD 5
// MODE_LISTFILE (6) retired - was File Transfer's own SD-card file browser
#define MODE_ORIENTATION 7
// MODE_DEFAULTMODE (8) retired - "Default Menu" setting removed along with MODEM mode
// MODE_PROTOCOL (9) retired - was File Transfer's Raw/XMODEM/YMODEM/ZMODEM
// MODE_SERIALCONFIG (10) retired - Serial Config locked to WIRSA_SERIAL_FORMAT, menu removed
#define MODE_SLIP 11
#define MODE_WIFI_SETUP 13
#define MODE_WIFI_PASSWORD 14
#define MODE_DIAGNOSTICS 15
#define MODE_FTPSERVER 16
#define MODE_BITWARDEN 17
#define MODE_CALDAV_RELAY 18

// ============================================================================
// Feature build flags
// ============================================================================
// Set any of these to 0 (e.g. via a build_flags override, or just edit the
// default here) to exclude that feature - both its Main Menu entry AND its
// implementation code - from the firmware entirely at compile time. Default:
// everything on. Config is deliberately NOT one of these - it's always
// built, so there's always a way to reach WiFi Setup/Baud Rate/Factory
// Reset/Reboot/About regardless of what else is disabled.
//
// Feature-specific submenu items that only exist to support ONE of these
// (not independently useful) ride on that SAME flag rather than getting
// their own - currently just Config's own "FTP Setup"/"AP Setup" entries,
// gated by WIRSA_FEATURE_FTPSERVER (AP Setup has no other consumer today -
// see ftp_server.cpp's own AP-mode fallback).
#ifndef WIRSA_FEATURE_FTPSERVER
#define WIRSA_FEATURE_FTPSERVER 1
#endif
#ifndef WIRSA_FEATURE_HOTSYNC
#define WIRSA_FEATURE_HOTSYNC 1
#endif
#ifndef WIRSA_FEATURE_SASD
#define WIRSA_FEATURE_SASD 1
#endif
#ifndef WIRSA_FEATURE_SLIP
#define WIRSA_FEATURE_SLIP 1
#endif
#ifndef WIRSA_FEATURE_BITWARDEN
#define WIRSA_FEATURE_BITWARDEN 1
#endif
#ifndef WIRSA_FEATURE_CALDAV
#define WIRSA_FEATURE_CALDAV 1
#endif
#ifndef WIRSA_FEATURE_UTILITIES
#define WIRSA_FEATURE_UTILITIES 1
#endif

// Menu modes
#define MENU_BOTH 0
#define MENU_NUM  1
#define MENU_DISP 2

// Display settings
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1
#define SCREEN_ADDRESS 0x3C

// EEPROM addresses - compacted layout (recomputed, no gaps between real
// fields, and every genuinely dead field removed outright). Reclaimed
// vs. the original layout:
//  - STATIC_IP/STATIC_GW/STATIC_DNS/STATIC_MASK (16B): defined, never
//    read or written anywhere - a static-IP feature never implemented.
//  - DIAL0-DIAL9/speedDials (500B), speedDialAddresses[], speedDialShown:
//    round-tripped through EEPROM every boot, never consulted by any
//    dial/connect code path - grep-confirmed zero real consumer.
//  - IP_TYPE_ADDRESS (1B): WRITE-ONLY - defaultEEPROM() sets it, nothing
//    anywhere ever reads it back, not even into a discarded local.
//  - ECHO_ADDRESS/echo, AUTO_ANSWER_ADDRESS/autoAnswer,
//    PET_TRANSLATE_ADDRESS/petTranslate, CONSOLE_MODE_ADDRESS/consoleMode
//    (4B): each round-tripped through EEPROM, zero other consumer -
//    leftovers from the old AT-command modem/telnet-BBS logic that
//    network.cpp's own comment confirms was already "removed as dead
//    weight" elsewhere, just not these settings' own storage.
//  - SERIALCONFIG_ADDRESS/serialConfig/bits[]/bitsDisp[] (1B + 2 arrays):
//    serialConfig is only ever ASSIGNED via EEPROM.read() - nothing
//    anywhere ever WRITES a different value to it (the menu that once
//    let a user change it was already removed - see WIRSA_SERIAL_FORMAT's
//    own "menu removed" comment below), so it's permanently stuck at its
//    EEPROM default and bits[serialConfig] always resolves to SERIAL_8N1.
//    The 5 real call sites now use WIRSA_SERIAL_FORMAT directly.
//  - Gaps: DIAL0-DIAL9/BUSY_MSG/PORTFWD/FTP/AP each used to start well
//    past where the previous block actually ended, plus a couple of
//    single-byte gaps left "for hardware flow control" never implemented.
//  - SERVER_PORT_ADDRESS/tcpServerPort, TELNET_ADDRESS/telnet (3B): checked
//    again in a later pass and actually removed (this comment used to say
//    they were deliberately left alone - they weren't as harmless as that
//    note assumed). The entire "telnet console" they configured
//    (consoleClient/consoleConnected/tcpServer/tcpClient, serial_io.cpp's
//    own per-print consoleReady() checks, ConsoleAvailable/ConsoleRead/
//    ConsolePrint/ConsolePrintLn, IAC/DO/WONT/WILL/DONT) turned out to be
//    100% structurally dead - consoleConnected was only ever set false at
//    its own declaration, so nothing could ever make tcpServer's accepted-
//    client machinery actually populate consoleClient with a real
//    connection. quietMode/verboseResults are still genuinely live (real
//    gating checks in display_menu.cpp) and untouched.
//  - PIN_POLARITY_ADDRESS/pinPolarity (1B, removed in a later pass still):
//    same "old AT-command modem/telnet-BBS" class of dead weight as above -
//    its only 3 consumers were setCarrier/setDSR/setRI (DCD/DSR/RI pin
//    signaling, see this file's own pin-definitions comment), each called
//    exactly once at boot with a hardcoded `false` and never again by any
//    current mode. callConnected/phon_symbol (the on-screen "call" icon
//    setCarrier also drove) were dead alongside it - callConnected was only
//    ever set false at its own declaration, same shape as consoleConnected
//    above.
// Total reclaimed: 1190 -> 440 -> 439 bytes (LAST_ADDRESS).
//
// IMPORTANT: this is a byte-for-byte incompatible renumbering. Any device
// already running a firmware with the OLD layout will read every setting
// from the WRONG offset after this update - VERSIONA/VERSIONB (below) are
// bumped specifically so setup()'s own version-mismatch check (main.cpp)
// runs defaultEEPROM() once on first boot after flashing this, instead of
// silently misinterpreting old bytes as new settings. That's a full
// factory reset (WiFi SSID/password, FTP/AP credentials, all wiped back
// to defaults) - a deliberate, one-time tradeoff, not a bug.
#define VERSION_ADDRESS 0
#define VERSION_LEN     2
#define SSID_ADDRESS    2
#define SSID_LEN        32
#define PASS_ADDRESS    34
#define PASS_LEN        63
#define BAUD_ADDRESS    97
#define VERBOSE_ADDRESS 98
#define ORIENTATION_ADDRESS 99
#define DEFAULTMODE_ADDRESS 100
#define USB_DEBUG_ADDRESS 101
#define QUIET_MODE_ADDRESS 102
#define ESC_CHAR_ADDRESS 103
// SLIP_IP_ADDRESS/SLIP_CLIENT_IP_ADDRESS/SLIP_SUBNET_ADDRESS/
// SLIP_DNS_ADDRESS (slip_mode.h) occupy 104-119 (4 bytes each) -
// renumbered there to sit immediately after ESC_CHAR_ADDRESS, no gap.
#define BUSY_MSG_ADDRESS 120
#define BUSY_MSG_LEN    80

// Port Forwarding (generic, used by SLIP)
#define PORTFWD_BASE    200   // Port forwards start here
#define PORTFWD_SIZE    10    // 10 bytes per entry (active, proto, extPort, intPort, intIP)
#define PORTFWD_COUNT   8     // Max 8 port forwards
// Total: 200 + (8 * 10) = 280

// FTP server + AP-mode credentials (see ftp_server.h for the FTP_USER_MAX/
// FTP_PASS_MAX/AP_SSID_MAX/AP_PASS_MAX length constants these must match)
#define FTP_USER_ADDRESS 280  // 32 bytes
#define FTP_PASS_ADDRESS 312  // 32 bytes
#define AP_SSID_ADDRESS  344  // 32 bytes
#define AP_PASS_ADDRESS  376  // 63 bytes
// Total: 376 + 63 = 439

#define LAST_ADDRESS    439

// Version - bumped (VERSIONB 1 -> 2) to force one-time defaultEEPROM() on
// first boot after this layout change - see the big comment above. Still
// correct after the later SERVER_PORT_ADDRESS/TELNET_ADDRESS removal below
// (that shrank the layout further but didn't need its own extra bump - any
// device with the old, larger layout still gets the same one-time reset).
// Bumped again (VERSIONB 2 -> 3) for the PIN_POLARITY_ADDRESS removal - that
// one shifts every address after it by -1 byte, unlike the SERVER_PORT/
// TELNET removal above, so it genuinely needs its own fresh one-time reset.
#define VERSIONA 0
#define VERSIONB 3

// Modem constants
// LISTEN_PORT (23, the telnet control port) / RING_INTERVAL / MAX_CMD_LENGTH
// / LED_TIME were the last traces of the removed telnet console/AT-command
// modem subsystem still sitting here unused - confirmed zero real consumers
// anywhere (LISTEN_PORT's own last user, tcpServer(LISTEN_PORT) in main.cpp,
// was removed along with the rest of that subsystem) and removed outright.
#define TX_BUF_SIZE 256

// Global objects
extern Adafruit_SSD1306 display;
extern MDNSResponder mdns;
extern HardwareSerial PhysicalSerial;

// Global variables
extern String build;
extern String cmd;
extern bool cmdMode;
extern bool verboseResults;
extern unsigned long lastRingMs;
extern char plusCount;
extern unsigned long plusTime;
extern unsigned long ledTime;
extern uint8_t txBuf[TX_BUF_SIZE];
extern byte serialSpeed;
extern String ssid, password, busyMsg;
extern unsigned long connectTime;
extern int menuMode;
extern int lastMenu;
extern int menuIdx;
extern int menuCnt;
extern int menuStart;
extern bool msgFlag;
extern byte dispOrientation;
extern byte defaultMode;

// Constant arrays
extern const int bauds[];
extern const uint8_t PROGMEM wifi_symbol[];
extern String baudDisp[];
extern String mainMenuDisp[];
extern int mainMenuCount;
extern int mmIdxFtp, mmIdxHotsync, mmIdxSasd, mmIdxSlip, mmIdxBitwarden, mmIdxCalDav, mmIdxUtilities, mmIdxConfig;
void buildMainMenu();
extern String settingsMenuDisp[];
extern int settingsMenuCount;
extern int smIdxMain, smIdxWifiSetup, smIdxFtpSetup, smIdxApSetup, smIdxBaud, smIdxOrientation, smIdxUsbDebug, smIdxFactoryReset, smIdxReboot, smIdxAbout;
void buildSettingsMenu();
extern String orientationMenuDisp[];
extern String defaultModeDisp[];
extern String resultCodes[];

// CRC-16/XMODEM table + its update step, shared by SASD (sasd.cpp) and
// HotSync Install (hotsync.cpp) for their slk-layer frame checksum -
// zmCRC16Update() used to live in file_transfer.cpp (ZMODEM); relocated to
// main.cpp, alongside the table it reads, when file_transfer.cpp/.h was
// removed as unreachable dead code (XMODEM/YMODEM/ZMODEM's own menu entry
// points had already been repurposed for HotSync Install - see MODE_FILE
// above - leaving fileLoop/protocolLoop/listFilesLoop with zero callers).
extern const uint16_t crc16_ccitt_table[256];
uint16_t zmCRC16Update(uint16_t crc, uint8_t b);

// Button state variables (not macros) for proper debouncing
extern bool BTNUP;
extern bool BTNDN;
extern bool BTNEN;
extern bool BTNBK;

#endif // GLOBALS_H
