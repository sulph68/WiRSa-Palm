#ifndef GLOBALS_H
#define GLOBALS_H

#include "log.h"
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <ESPmDNS.h>
#include <HardwareSerial.h>
#include <Adafruit_SSD1306.h>
#include "SD.h"
#include "arduino-timer.h"

// Debug flags: uncomment for verbose output

// Pin definitions
#define SWITCH_PIN 0       // GPIO0 (programming mode pin)
#define LED_PIN 2          // Status LED

// RTS/CTS/DTR are not wired on this hardware, and DCD/DSR/RI are gone for the same reason.

#define SW1_PIN 39         //DOWN
#define SW2_PIN 18         //BACK   (was 36: an octal-PSRAM pin)
#define SW3_PIN 8          //ENTER  (was 37: an octal-PSRAM pin)
#define SW4_PIN 38         //UP

// SD Card Pins
#define SD_CS 12
#define SPI_MOSI 41
#define SPI_MISO 42
#define SPI_SCK 40

// SPI clock for the SD card once it is initialised (it always starts at 400 kHz)
#define WIRSA_SD_HZ 20000000

#define RXD2 16
#define TXD2 17

// LED polarity: the generic board's LED is active-low (LOW = lit); boards with an active-high LED override these in their own block.
#define LED_ON  LOW
#define WIRSA_LED_BRIGHTNESS 100   // percent; below 100 the LED is PWM-dimmed (led.h)
#define LED_OFF HIGH
// An RGB status LED (WS2812) instead of the plain one: -DWIRSA_LED_NEOPIXEL=<gpio> in the board's build flags. A WS2812 is very bright, so it is dimmed;
// the level can be set the same way (-DWIRSA_LED_NEO_PCT=5). With -DWIRSA_LED_DISCRETE the colours are three separate LEDs on the board's
// WIRSA_LED_R_PIN / _G_PIN / _B_PIN (the T7-S3 block, below); the colours, levels and states are the same.
#if defined(WIRSA_LED_NEOPIXEL) || defined(WIRSA_LED_DISCRETE)
#ifndef WIRSA_LED_NEO_PCT
#if defined(WIRSA_LED_DISCRETE)
#define WIRSA_LED_NEO_PCT 5   // LEDs behind resistors are much dimmer than a WS2812
#else
#define WIRSA_LED_NEO_PCT 1
#endif
#endif
#endif

// Native-USB (CDC) builds wait this long at boot for a monitor to open the port, only when a USB host is attached. 0 = never (the Palm link is up as soon as the chip has booted;
// the 4 KB TX buffer holds the greeting for a monitor that attaches later). Raise it (e.g. 1500) if the monitor misses the greeting.
#ifndef WIRSA_USB_WAIT_MS
#define WIRSA_USB_WAIT_MS 0
#endif

// The hardware UART of the Palm line (the dual-baud listener always uses UART1, so this must not be 1).
#define WIRSA_PALM_UART 2

// ---- Board: LilyGO T7-S3 (ESP32-S3, 16 MB flash, 8 MB octal PSRAM), built with -DWIRSA_BOARD_T7S3 (the [env:t7s3] section of platformio.ini) ----
// Everything for the board is in this block and nothing outside it changes with the flag, so generic builds are unaffected.
// Screenless: no OLED and no buttons. USB (native CDC) is the debug console, the Palm is on GPIO43/44 (UART2), and UART0 is free for an expansion port.
#if defined(WIRSA_BOARD_T7S3)
#if defined(WIRSA_LED_NEOPIXEL)
#error "The T7-S3 has discrete LEDs (WIRSA_LED_DISCRETE): WIRSA_LED_NEOPIXEL is for other boards"
#endif
#undef SWITCH_PIN
#undef LED_PIN
#undef SD_CS
#undef SPI_MOSI
#undef SPI_MISO
#undef SPI_SCK
#undef RXD2
#undef TXD2
#undef LED_ON
#undef WIRSA_LED_BRIGHTNESS
#undef LED_OFF
#undef WIRSA_PALM_UART
#undef WIRSA_SD_HZ

#define SWITCH_PIN 0       // BOOT button
#define LED_PIN 17         // user LED, active-high
#define LED_ON  HIGH
#define WIRSA_LED_BRIGHTNESS 5     // PWM duty percent; the eye sees a dim LED brighter than its duty
#define LED_OFF LOW
// Three status LEDs (led.h), each behind a resistor to ground, so active-high: D1 red, D2 green, D3 blue.
#define WIRSA_LED_R_PIN 4
#define WIRSA_LED_G_PIN 7
#define WIRSA_LED_B_PIN 8
#define WIRSA_LED_RGB_ON HIGH
#define BATTERY_PIN 2      // ADC1, battery through a 1/2 divider
#define BATTERY_DIVIDER 2
// Gain correction in parts per thousand, from one multimeter comparison on this board (read 4.25 V, meter 4.202 V); re-measure per board.
#define BATTERY_CAL_PERMILLE 989

// SD card (SPI)
#define SD_CS    10
#define SPI_MOSI 9
#define SPI_MISO 21
#define SPI_SCK  11
// Proven on the hand-wired pins at the full 20 MHz; if reads turn flaky on long wires, drop to 10000000 or 4000000.
#define WIRSA_SD_HZ 20000000

// Palm line on GPIO43/44 (the ESP's RX is GPIO44), on UART2, NOT UART0: 43/44 are UART0's native (IOMUX) pins, and the dual-baud listener (UART1) also reads the RX pin and re-routes
// the pad through the GPIO matrix each time it starts or stops, which cuts UART0 off from it for good. UART2 is matrix-routed like UART1, so the two share the pin. UART0 stays free for the expansion port.
#define WIRSA_PALM_UART 2
#define RXD2 44
#define TXD2 43

// Expansion UART (UART0 when used; pins only, nothing uses it yet) and spare IO
#define EXP_TX_PIN 15
#define EXP_RX_PIN 16
#define EXP_CS2_PIN 5
// Spare GPIOs on the expansion board. 45/46 are strapping pins (nothing may pull them at reset). 47/48 are 1.8 V on octal-PSRAM modules: check the expansion board before connecting 3.3 V devices.
#define EXP_GPIO_LIST 18, 38, 45, 46, 47, 48   // 4, 7 and 8 drive the status LEDs (WIRSA_LED_R_PIN, _G_PIN, _B_PIN)

// I2C (Qwiic)
#define I2C_SDA_PIN 13
#define I2C_SCL_PIN 14

// No display, no buttons (SW1-4 do not exist: 36/37 are PSRAM pins)
#ifndef WIRSA_SCREENLESS
#define WIRSA_SCREENLESS 1
#endif

// Pins that must never be configured on this module: USB 19/20, flash 26-32, octal PSRAM 33-37. A pin macro above that lands on one fails the build, not the board.
#define WIRSA_PIN_RESERVED(p) (((p) >= 19 && (p) <= 20) || ((p) >= 26 && (p) <= 37))
#if WIRSA_PIN_RESERVED(LED_PIN) || WIRSA_PIN_RESERVED(SD_CS) || WIRSA_PIN_RESERVED(SPI_MOSI) \
 || WIRSA_PIN_RESERVED(SPI_MISO) || WIRSA_PIN_RESERVED(SPI_SCK) || WIRSA_PIN_RESERVED(RXD2) \
 || WIRSA_PIN_RESERVED(TXD2) || WIRSA_PIN_RESERVED(EXP_TX_PIN) || WIRSA_PIN_RESERVED(EXP_RX_PIN) \
 || WIRSA_PIN_RESERVED(EXP_CS2_PIN) || WIRSA_PIN_RESERVED(I2C_SDA_PIN) || WIRSA_PIN_RESERVED(I2C_SCL_PIN) \
 || WIRSA_PIN_RESERVED(WIRSA_LED_R_PIN) || WIRSA_PIN_RESERVED(WIRSA_LED_G_PIN) || WIRSA_PIN_RESERVED(WIRSA_LED_B_PIN)
#error "T7-S3 pin block uses a reserved pin (USB 19/20, flash 26-32, PSRAM 33-37)"
#endif
#define WIRSA_DEEP_SLEEP 1
#define WIRSA_WAKE_PIN 3   // wired to GPIO44, the Palm RX
#endif  // WIRSA_BOARD_T7S3
#if defined(WIRSA_LED_DISCRETE) && defined(WIRSA_LED_NEOPIXEL)
#error "WIRSA_LED_DISCRETE and WIRSA_LED_NEOPIXEL are two different colour LEDs: use one"
#endif
#if defined(WIRSA_LED_DISCRETE) && !defined(WIRSA_LED_R_PIN)
#error "WIRSA_LED_DISCRETE needs the board's WIRSA_LED_R_PIN, _G_PIN, _B_PIN and WIRSA_LED_RGB_ON"
#endif

// Deep sleep (deep_sleep.h) is built for the T7-S3, which sets WIRSA_DEEP_SLEEP above, or on any other board with -DWIRSA_DEEP_SLEEP.
// The Palm line wakes it: with no wake pin set it is the Palm RX itself, which must be an RTC pin (GPIO 0-21).
#if defined(WIRSA_DEEP_SLEEP) && !defined(WIRSA_WAKE_PIN)
#define WIRSA_WAKE_PIN RXD2
#endif
#if defined(WIRSA_DEEP_SLEEP) && ((WIRSA_WAKE_PIN) < 0 || (WIRSA_WAKE_PIN) > 21)
#error "WIRSA_WAKE_PIN must be an RTC pin (GPIO 0-21)"
#endif

// Physical UART format, locked to 8-N-1 (not a menu setting); change it here for all builds.
#define WIRSA_SERIAL_FORMAT SERIAL_8N1

// Mode definitions
#define MODE_MAIN 0
// MODE_MODEM (1) is gone, replaced by MODE_FTPSERVER (16)
#define MODE_FILE 2   // repurposed: was "File Transfer", now HotSync Install - see hotsync.h/.cpp
#define MODE_PLAYBACK 3  // repurposed - also used by SASD mode, see sasd.cpp
#define MODE_SETTINGS 4
#define MODE_SETBAUD 5
// MODE_LISTFILE (6) retired: it was File Transfer's SD file browser
#define MODE_ORIENTATION 7
// Retired: MODE_DEFAULTMODE (8, "Default Menu" went with MODEM mode); MODE_PROTOCOL (9, File Transfer's Raw/XMODEM/YMODEM/ZMODEM); MODE_SERIALCONFIG (10, locked to WIRSA_SERIAL_FORMAT)
#define MODE_SLIP 11
#define MODE_WIFI_SETUP 13
#define MODE_WIFI_PASSWORD 14
#define MODE_DIAGNOSTICS 15
#define MODE_FTPSERVER 16
#define MODE_BITWARDEN 17
#define MODE_CALDAV_RELAY 18
#define MODE_MESH 19

// ---- Feature build flags ----
// Set any to 0 (a build_flags override, or edit the default here) to leave out the feature, both its Main Menu entry and its code, at compile time. Default: everything on. Config is deliberately
// not one of them: it is always built, so WiFi Setup, Baud Rate, Factory Reset, Reboot and About stay reachable. Submenu items that exist for only ONE feature ride on its flag (Config's "FTP Setup"
// and "AP Setup" are gated by WIRSA_FEATURE_FTPSERVER; AP Setup has no other consumer, see ftp_server.cpp's AP-mode fallback).
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
#ifndef WIRSA_FEATURE_MESH
#define WIRSA_FEATURE_MESH 1   // Mesh Relay: Bluetooth link to a Meshtastic node (needs NimBLE-Arduino; 0 = no BLE code at all)
#endif
#ifndef WIRSA_FEATURE_UTILITIES
#define WIRSA_FEATURE_UTILITIES 1
#endif

// Palm control link (palm_ctl.cpp, spec docs/protocol/Protocol-wire.md): framed commands from WiRSaCtl on the Palm line, the menu burst guard and the exit hooks in CalDAV, BitWarden and HotSync.
// Off = the menu reads the Palm line as before.
#ifndef WIRSA_FEATURE_PALMCTL
#define WIRSA_FEATURE_PALMCTL 1
#endif

// Dual-rate menu (palm_ctl.cpp, needs WIRSA_FEATURE_PALMCTL): the menu starts at 9600 after every boot. WiRSaCtl's SPEED moves it to the Palm's Preferred Baud (any rate; the configured baud is
// only the maximum for the modes' own negotiation) for this power-on, nothing saved. While in the menus a second, receive-only UART (UART1) reads the same Palm RX pin at 9600, so any Palm can
// get back in: a control frame or single key decoded there moves the menu down to 9600. Only SPEED moves it up; modes still negotiate up to the maximum. Off = the menu runs at the configured baud only.
#ifndef WIRSA_FEATURE_DUAL_BAUD
#define WIRSA_FEATURE_DUAL_BAUD 1
#endif

// Speed handshake in the menus (palm_ctl.cpp + autobaud.cpp, needs WIRSA_FEATURE_DUAL_BAUD): a Palm network login script or terminal sends the line SETBAUD, WiRSa answers OK, then the Palm sends
// BR<rate> (e.g. BR115200) and WiRSa answers OK once the speed is proven.
//  - At the speed the menu listens at (9600, or its current speed): both lines are read as text; BR<rate> moves the menu to that rate (9600..115200).
//  - At any other speed (a script fixed at 115200 right after a boot, menu at 9600): WiRSa measures the speed from the first character's edges (autobaud.h), moves the menu there and answers OK
//    at the new speed. The UART then decodes the BR line at that speed; it must name the same rate, else ERROR and the menu goes back to 9600. No BR line within 5 s: back to 9600.
// Autobaud watches the Palm RX pin with a pin interrupt, only in the menus while the menu is at 9600. Off = the menu ignores the handshake.
#ifndef WIRSA_FEATURE_AUTOBAUD
#define WIRSA_FEATURE_AUTOBAUD 1
#endif

// BOOT button (GPIO0 = SWITCH_PIN) as a HotSync button (boot_button.cpp): a short press in any menu starts HotSync, another press while it still waits cancels it; the LED blinks to confirm.
// Press it only after WiRSa has started: held during reset, GPIO0 selects download mode.
#ifndef WIRSA_FEATURE_BOOT_HOTSYNC
#define WIRSA_FEATURE_BOOT_HOTSYNC 1
#endif

// Screenless build: no OLED and no buttons, everything driven from the Palm over the control link. The display is a do-nothing NullDisplay (null_display.h) and readSwitches() reports no
// button (SW1-SW4 are plain INPUTs relying on the button board's pull-ups; without it they float and read as random presses).
#ifndef WIRSA_SCREENLESS
#define WIRSA_SCREENLESS 0
#endif
#if WIRSA_SCREENLESS
#define WIRSA_HAS_DISPLAY 0
#define WIRSA_HAS_BUTTONS 0
#else
#define WIRSA_HAS_DISPLAY 1
#define WIRSA_HAS_BUTTONS 1
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

// EEPROM addresses: a compact layout with no gaps between real fields. Dead storage was removed outright (static IP fields, speed dials, IP_TYPE, echo/autoAnswer/petTranslate/consoleMode,
// serialConfig and its tables, the server-port/telnet bytes, pinPolarity), leftovers of the removed AT-command modem and telnet console; LAST_ADDRESS went 1190 -> 439. The old gaps (each block
// started well past where the previous ended, plus single "hardware flow control" bytes) went too.
// IMPORTANT: that renumbering is byte-for-byte incompatible. A device on the old layout would read every setting from the wrong offset, so VERSIONA/VERSIONB (below) are bumped to make setup()'s
// version check (main.cpp) run defaultEEPROM() once on the first boot after flashing. That is a full factory reset (WiFi SSID/password and FTP/AP credentials wiped to defaults), deliberate and one-time.
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
// SLIP_IP_ADDRESS, SLIP_CLIENT_IP_ADDRESS, SLIP_SUBNET_ADDRESS and SLIP_DNS_ADDRESS (slip_mode.h) occupy 104-119 (4 bytes each), right after ESC_CHAR_ADDRESS.
#define BUSY_MSG_ADDRESS 120
#define BUSY_MSG_LEN    80

// Port forwarding (generic, used by SLIP)
#define PORTFWD_BASE    200   // Port forwards start here
#define PORTFWD_SIZE    10    // 10 bytes per entry (active, proto, extPort, intPort, intIP)
#define PORTFWD_COUNT   8     // Max 8 port forwards
// Total: 200 + (8 * 10) = 280

// FTP server and AP-mode credentials (the lengths must match FTP_USER_MAX/FTP_PASS_MAX/AP_SSID_MAX/AP_PASS_MAX in ftp_server.h)
#define FTP_USER_ADDRESS 280  // 32 bytes
#define FTP_PASS_ADDRESS 312  // 32 bytes
#define AP_SSID_ADDRESS  344  // 32 bytes
#define AP_PASS_ADDRESS  376  // 63 bytes
// Total: 376 + 63 = 439

// Exit Timers (mode_timer.h, settings.h): a marker byte, then the two values. They sit past the old end, so no VERSIONB bump: growing the EEPROM blob fills new bytes with 0, which is a real
// value ("never"), hence the marker; without it readSettings() uses the defaults.
#define EXIT_TIMERS_MARK_ADDRESS   440
#define EXIT_TIMERS_MARK           0xA5
#define MODE_UNUSED_EXIT_ADDRESS   441
#define MODE_AFTER_EXIT_ADDRESS    442
// SLIP timeout, minutes (1/2/3/5); anything else (e.g. 0 from a blob that grew past 442 after the marker was written) reads as the default.
#define SLIP_TIMEOUT_ADDRESS       443
// SASD disk (settings.h): 0 auto, 1 SD card, 2 virtual card. Past the old end, and 0 (a freshly grown byte) is the default, so no marker.
#define SASD_DISK_ADDRESS          444
// Deep sleep delay (settings.h), minutes: 0 off, 1/3/5/15. Past the end too; 0 is the default.
#define DEEP_SLEEP_ADDRESS         445
// Baud table format. main.cpp bauds[] began at 300 until 2026-10-24, then was cut to 9600 and up (mark 0xB9: 9600 19200 38400 57600 115200), then got 14400 and 28800 (mark 0xBA); each renumbers
// serialSpeed. Any other mark here means BAUD_ADDRESS is in an older numbering.
#define BAUD_FORMAT_ADDRESS        446
#define BAUD_FORMAT_MARK           0xBA
#define BAUD_FORMAT_MARK_5         0xB9

// SASD timeout, minutes (3/5/10/15): SASD returns to the main menu after this long with nothing from the Palm. Past the end too; 0 (a freshly grown byte) or anything else reads as the default.
#define SASD_TIMEOUT_ADDRESS       447

// WiFi pulse (settings.h): the blue LED's idle behaviour, stored as mode + 1 so a freshly grown byte (0) reads as the default.
#define WIFI_PULSE_ADDRESS         448

#define LAST_ADDRESS    448

// Version. VERSIONB was bumped 1 -> 2 to force the one-time defaultEEPROM() after the layout change (the later server-port/telnet removal needed no bump of its own: an older, larger layout
// gets the same reset). Bumped 2 -> 3 for the pinPolarity removal, which shifts every later address by one byte and so needs its own reset.
#define VERSIONA 0
#define VERSIONB 3

// Modem constants. LISTEN_PORT, RING_INTERVAL, MAX_CMD_LENGTH and LED_TIME (the last traces of the removed telnet console and AT-command modem) were removed as unused.
#define TX_BUF_SIZE 256

// Global objects
#if WIRSA_HAS_DISPLAY
typedef Adafruit_SSD1306 WirsaDisplay;
#else
#include "null_display.h"
typedef NullDisplay WirsaDisplay;
#endif
extern WirsaDisplay display;
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
void buildMainMenu();
extern String settingsMenuDisp[];
extern int settingsMenuCount;
extern int smIdxMain, smIdxWifiSetup, smIdxFtpSetup, smIdxApSetup, smIdxSlipSetup, smIdxBaud, smIdxOrientation, smIdxUsbDebug, smIdxExitTimers, smIdxFactoryReset, smIdxReboot, smIdxAbout;
void buildSettingsMenu();
extern String orientationMenuDisp[];
extern String defaultModeDisp[];
extern String resultCodes[];

// CRC-16/XMODEM table and its update step, shared by SASD (sasd.cpp) and HotSync Install (hotsync.cpp) for the slk frame checksum. zmCRC16Update() moved here from file_transfer.cpp
// (ZMODEM) when that was removed as unreachable.
extern const uint16_t crc16_ccitt_table[256];
uint16_t zmCRC16Update(uint16_t crc, uint8_t b);

// Button state variables (not macros) for debouncing
extern bool BTNUP;
extern bool BTNDN;
extern bool BTNEN;
extern bool BTNBK;

#endif // GLOBALS_H
