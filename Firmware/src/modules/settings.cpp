#include "settings.h"
#include "globals.h"
#include "ftp_server.h"
#include "slip_mode.h"  // SLIP_IP_ADDRESS/SLIP_CLIENT_IP_ADDRESS/SLIP_SUBNET_ADDRESS/SLIP_DNS_ADDRESS
#include <EEPROM.h>

// The EEPROM-address constants all come from globals.h, the one source for that layout.

// globals defined in main.cpp
extern String ssid, password, busyMsg;
extern String ftpUser, ftpPass, apSsid, apPass;
extern byte serialSpeed;
extern bool verboseResults, quietMode;
extern byte escChar;
extern byte dispOrientation, defaultMode;
extern bool usbDebug;

uint8_t modeUnusedExitSec = MODE_UNUSED_EXIT_DEFAULT;
uint8_t deepSleepMin = 0;
uint8_t wifiPulse = WIFI_PULSE_DEFAULT;

bool wifiPulseValid(uint8_t m) {
  return m <= WIFI_PULSE_NEVER;
}

bool deepSleepValid(uint8_t m) {
  return m == 0 || m == 1 || m == 3 || m == 5 || m == 15;
}
uint8_t modeAfterExitSec = MODE_AFTER_EXIT_DEFAULT;
uint8_t slipTimeoutMin = SLIP_TIMEOUT_DEFAULT;
uint8_t sasdTimeoutMin = SASD_TIMEOUT_DEFAULT;
uint8_t sasdDiskMode = SASD_DISK_AUTO;

bool slipTimeoutValid(uint8_t m) {
  return m == 1 || m == 2 || m == 3 || m == 5;
}

bool sasdTimeoutValid(uint8_t m) {
  return m == 3 || m == 5 || m == 10 || m == 15;
}

void writeSettings() {
  setEEPROM(ssid, SSID_ADDRESS, SSID_LEN);
  setEEPROM(password, PASS_ADDRESS, PASS_LEN);
  setEEPROM(busyMsg, BUSY_MSG_ADDRESS, BUSY_MSG_LEN);

  EEPROM.write(BAUD_ADDRESS, serialSpeed);
  EEPROM.write(BAUD_FORMAT_ADDRESS, BAUD_FORMAT_MARK);
  EEPROM.write(VERBOSE_ADDRESS, byte(verboseResults));
  EEPROM.write(ORIENTATION_ADDRESS, byte(dispOrientation));
  EEPROM.write(DEFAULTMODE_ADDRESS, byte(defaultMode));
  EEPROM.write(USB_DEBUG_ADDRESS, logLevelToEeprom(gLogLevel));
  EEPROM.write(QUIET_MODE_ADDRESS, byte(quietMode));
  EEPROM.write(ESC_CHAR_ADDRESS, escChar);

  setEEPROM(ftpUser, FTP_USER_ADDRESS, FTP_USER_MAX);
  setEEPROM(ftpPass, FTP_PASS_ADDRESS, FTP_PASS_MAX);
  setEEPROM(apSsid, AP_SSID_ADDRESS, AP_SSID_MAX);
  setEEPROM(apPass, AP_PASS_ADDRESS, AP_PASS_MAX);

  EEPROM.write(EXIT_TIMERS_MARK_ADDRESS, EXIT_TIMERS_MARK);
  EEPROM.write(MODE_UNUSED_EXIT_ADDRESS, modeUnusedExitSec);
  EEPROM.write(DEEP_SLEEP_ADDRESS, deepSleepMin);
  EEPROM.write(WIFI_PULSE_ADDRESS, wifiPulse + 1);
  EEPROM.write(MODE_AFTER_EXIT_ADDRESS, modeAfterExitSec);
  EEPROM.write(SLIP_TIMEOUT_ADDRESS, slipTimeoutMin);
  EEPROM.write(SASD_TIMEOUT_ADDRESS, sasdTimeoutMin);
  EEPROM.write(SASD_DISK_ADDRESS, sasdDiskMode);

  EEPROM.commit();
}

void readSettings() {
  serialSpeed = EEPROM.read(BAUD_ADDRESS);
  if (EEPROM.read(BAUD_FORMAT_ADDRESS) != BAUD_FORMAT_MARK) {
    // Older numberings. Before the 14400/28800 entries the table was 9600 19200 38400 57600 115200 (mark BAUD_FORMAT_MARK_5); before 2026-10-24 it began at 300 (300, 1200, 2400, 4800, 9600 ...): 9600 was 4.
    static const uint8_t kFromFive[5] = { 0, 2, 4, 5, 6 };
    if (EEPROM.read(BAUD_FORMAT_ADDRESS) != BAUD_FORMAT_MARK_5)
      serialSpeed = serialSpeed >= 4 ? serialSpeed - 4 : 0;
    serialSpeed = serialSpeed < 5 ? kFromFive[serialSpeed] : 0;
    EEPROM.write(BAUD_ADDRESS, serialSpeed);
    EEPROM.write(BAUD_FORMAT_ADDRESS, BAUD_FORMAT_MARK);
    EEPROM.commit();
  }

  ssid = getEEPROM(SSID_ADDRESS, SSID_LEN);
  password = getEEPROM(PASS_ADDRESS, PASS_LEN);
  busyMsg = getEEPROM(BUSY_MSG_ADDRESS, BUSY_MSG_LEN);
  verboseResults = EEPROM.read(VERBOSE_ADDRESS);
  dispOrientation = EEPROM.read(ORIENTATION_ADDRESS);
  defaultMode = EEPROM.read(DEFAULTMODE_ADDRESS);
  logSetLevel(logLevelFromEeprom(EEPROM.read(USB_DEBUG_ADDRESS)));
  quietMode = EEPROM.read(QUIET_MODE_ADDRESS);
  escChar = EEPROM.read(ESC_CHAR_ADDRESS);

  // These addresses are new in this firmware: on an EEPROM that predates them (an upgrade, version bytes unchanged so defaultEEPROM() does not run), never-written flash reads 0xFF, not 0x00, so
  // getEEPROM()'s null-terminator scan would not stop early and would return garbage. Treat an erased first byte as "empty" explicitly.
  ftpUser = (EEPROM.read(FTP_USER_ADDRESS) == 0xFF) ? "" : getEEPROM(FTP_USER_ADDRESS, FTP_USER_MAX);
  ftpPass = (EEPROM.read(FTP_PASS_ADDRESS) == 0xFF) ? "" : getEEPROM(FTP_PASS_ADDRESS, FTP_PASS_MAX);
  apSsid = (EEPROM.read(AP_SSID_ADDRESS) == 0xFF) ? "" : getEEPROM(AP_SSID_ADDRESS, AP_SSID_MAX);
  apPass = (EEPROM.read(AP_PASS_ADDRESS) == 0xFF) ? "" : getEEPROM(AP_PASS_ADDRESS, AP_PASS_MAX);

  if (EEPROM.read(EXIT_TIMERS_MARK_ADDRESS) == EXIT_TIMERS_MARK) {
    modeUnusedExitSec = EEPROM.read(MODE_UNUSED_EXIT_ADDRESS);
    modeAfterExitSec = EEPROM.read(MODE_AFTER_EXIT_ADDRESS);
  } else {
    modeUnusedExitSec = MODE_UNUSED_EXIT_DEFAULT;
    modeAfterExitSec = MODE_AFTER_EXIT_DEFAULT;
  }
  slipTimeoutMin = EEPROM.read(SLIP_TIMEOUT_ADDRESS);
  if (!slipTimeoutValid(slipTimeoutMin)) slipTimeoutMin = SLIP_TIMEOUT_DEFAULT;
  sasdTimeoutMin = EEPROM.read(SASD_TIMEOUT_ADDRESS);
  if (!sasdTimeoutValid(sasdTimeoutMin)) sasdTimeoutMin = SASD_TIMEOUT_DEFAULT;
  deepSleepMin = EEPROM.read(DEEP_SLEEP_ADDRESS);
  if (!deepSleepValid(deepSleepMin)) deepSleepMin = 0;
  { uint8_t v = EEPROM.read(WIFI_PULSE_ADDRESS); wifiPulse = (v >= 1 && wifiPulseValid(v - 1)) ? v - 1 : WIFI_PULSE_DEFAULT; }
  sasdDiskMode = EEPROM.read(SASD_DISK_ADDRESS);
  if (sasdDiskMode > SASD_DISK_VIRTUAL) sasdDiskMode = SASD_DISK_AUTO;
}

void defaultEEPROM() {
  EEPROM.write(VERSION_ADDRESS, VERSIONA);
  EEPROM.write(VERSION_ADDRESS + 1, VERSIONB);

  setEEPROM("", SSID_ADDRESS, SSID_LEN);
  setEEPROM("", PASS_ADDRESS, PASS_LEN);

  EEPROM.write(BAUD_ADDRESS, 0x00);              // 9600
  EEPROM.write(BAUD_FORMAT_ADDRESS, BAUD_FORMAT_MARK);
  EEPROM.write(VERBOSE_ADDRESS, 0x01);

  setEEPROM("SORRY, SYSTEM IS CURRENTLY BUSY. PLEASE TRY AGAIN LATER.", BUSY_MSG_ADDRESS, BUSY_MSG_LEN);

  EEPROM.write(ORIENTATION_ADDRESS, 0x00);
  EEPROM.write(DEFAULTMODE_ADDRESS, 0x00);
  EEPROM.write(USB_DEBUG_ADDRESS, logLevelToEeprom(LOG_INFO));  // log level info by default
  EEPROM.write(QUIET_MODE_ADDRESS, 0x00); // Quiet mode off by default
  EEPROM.write(ESC_CHAR_ADDRESS, '+');    // Default escape char is '+'

  setEEPROM("", FTP_USER_ADDRESS, FTP_USER_MAX);   // blank = anonymous FTP login
  setEEPROM("", FTP_PASS_ADDRESS, FTP_PASS_MAX);
  setEEPROM("", AP_SSID_ADDRESS, AP_SSID_MAX);
  setEEPROM("", AP_PASS_ADDRESS, AP_PASS_MAX);

  EEPROM.write(EXIT_TIMERS_MARK_ADDRESS, EXIT_TIMERS_MARK);
  EEPROM.write(MODE_UNUSED_EXIT_ADDRESS, MODE_UNUSED_EXIT_DEFAULT);
  EEPROM.write(MODE_AFTER_EXIT_ADDRESS, MODE_AFTER_EXIT_DEFAULT);
  EEPROM.write(SLIP_TIMEOUT_ADDRESS, SLIP_TIMEOUT_DEFAULT);
  EEPROM.write(SASD_TIMEOUT_ADDRESS, SASD_TIMEOUT_DEFAULT);
  EEPROM.write(SASD_DISK_ADDRESS, SASD_DISK_AUTO);
  EEPROM.write(DEEP_SLEEP_ADDRESS, 0);

  // SLIP Gateway defaults, through the named SLIP_*_ADDRESS constants (slip_mode.h), not literal offsets: an earlier version hardcoded 800-815 and silently wrote to the wrong, now unused place when
  // those constants were renumbered. Gateway IP: 192.168.7.1
  EEPROM.write(SLIP_IP_ADDRESS,     192);
  EEPROM.write(SLIP_IP_ADDRESS + 1, 168);
  EEPROM.write(SLIP_IP_ADDRESS + 2, 7);
  EEPROM.write(SLIP_IP_ADDRESS + 3, 1);
  EEPROM.write(SLIP_CLIENT_IP_ADDRESS,     192);
  EEPROM.write(SLIP_CLIENT_IP_ADDRESS + 1, 168);
  EEPROM.write(SLIP_CLIENT_IP_ADDRESS + 2, 7);
  EEPROM.write(SLIP_CLIENT_IP_ADDRESS + 3, 2);
  EEPROM.write(SLIP_SUBNET_ADDRESS,     255);
  EEPROM.write(SLIP_SUBNET_ADDRESS + 1, 255);
  EEPROM.write(SLIP_SUBNET_ADDRESS + 2, 255);
  EEPROM.write(SLIP_SUBNET_ADDRESS + 3, 0);
  EEPROM.write(SLIP_DNS_ADDRESS,     8);
  EEPROM.write(SLIP_DNS_ADDRESS + 1, 8);
  EEPROM.write(SLIP_DNS_ADDRESS + 2, 8);
  EEPROM.write(SLIP_DNS_ADDRESS + 3, 8);
  // clear the port forward entries (used by SLIP)
  for (int i = PORTFWD_BASE; i < PORTFWD_BASE + (PORTFWD_COUNT * PORTFWD_SIZE); i++) {
    EEPROM.write(i, 0);
  }

  EEPROM.commit();
}

String getEEPROM(int startAddress, int len) {
  String myString;

  for (int i = startAddress; i < startAddress + len; i++) {
    if (EEPROM.read(i) == 0x00) {
      break;
    }
    myString += char(EEPROM.read(i));
  }
  return myString;
}

void setEEPROM(String inString, int startAddress, int maxLen) {
  for (unsigned int i = startAddress; i < inString.length() + startAddress; i++) {
    EEPROM.write(i, inString[i - startAddress]);
  }
  // null-pad the rest of the memory space
  for (unsigned int i = inString.length() + startAddress; i < maxLen + startAddress; i++) {
    EEPROM.write(i, 0x00);
  }
}
