#include "settings.h"
#include "globals.h"
#include "ftp_server.h"
#include "slip_mode.h"  // SLIP_IP_ADDRESS/SLIP_CLIENT_IP_ADDRESS/SLIP_SUBNET_ADDRESS/SLIP_DNS_ADDRESS
#include <EEPROM.h>

// The EEPROM-address constants this file needs all come from globals.h
// (included above), the single source of truth for that layout.

// Global variables (defined in main.cpp)
extern String ssid, password, busyMsg;
extern String ftpUser, ftpPass, apSsid, apPass;
extern byte serialSpeed;
extern bool verboseResults, quietMode;
extern byte escChar;
extern byte dispOrientation, defaultMode;
extern bool usbDebug;

void writeSettings() {
  setEEPROM(ssid, SSID_ADDRESS, SSID_LEN);
  setEEPROM(password, PASS_ADDRESS, PASS_LEN);
  setEEPROM(busyMsg, BUSY_MSG_ADDRESS, BUSY_MSG_LEN);

  EEPROM.write(BAUD_ADDRESS, serialSpeed);
  EEPROM.write(VERBOSE_ADDRESS, byte(verboseResults));
  EEPROM.write(ORIENTATION_ADDRESS, byte(dispOrientation));
  EEPROM.write(DEFAULTMODE_ADDRESS, byte(defaultMode));
  EEPROM.write(USB_DEBUG_ADDRESS, byte(usbDebug));
  EEPROM.write(QUIET_MODE_ADDRESS, byte(quietMode));
  EEPROM.write(ESC_CHAR_ADDRESS, escChar);

  setEEPROM(ftpUser, FTP_USER_ADDRESS, FTP_USER_MAX);
  setEEPROM(ftpPass, FTP_PASS_ADDRESS, FTP_PASS_MAX);
  setEEPROM(apSsid, AP_SSID_ADDRESS, AP_SSID_MAX);
  setEEPROM(apPass, AP_PASS_ADDRESS, AP_PASS_MAX);

  EEPROM.commit();
}

void readSettings() {
  serialSpeed = EEPROM.read(BAUD_ADDRESS);

  ssid = getEEPROM(SSID_ADDRESS, SSID_LEN);
  password = getEEPROM(PASS_ADDRESS, PASS_LEN);
  busyMsg = getEEPROM(BUSY_MSG_ADDRESS, BUSY_MSG_LEN);
  verboseResults = EEPROM.read(VERBOSE_ADDRESS);
  dispOrientation = EEPROM.read(ORIENTATION_ADDRESS);
  defaultMode = EEPROM.read(DEFAULTMODE_ADDRESS);
  usbDebug = EEPROM.read(USB_DEBUG_ADDRESS);
  quietMode = EEPROM.read(QUIET_MODE_ADDRESS);
  escChar = EEPROM.read(ESC_CHAR_ADDRESS);

  // These addresses are new as of this firmware version - on an EEPROM that
  // predates them (upgrade from an older build, version bytes unchanged so
  // defaultEEPROM() doesn't run), never-written flash reads back as 0xFF,
  // not 0x00, so getEEPROM()'s null-terminator scan wouldn't stop early and
  // would return a garbage string. Treat an erased first byte as "empty"
  // explicitly rather than trusting it.
  ftpUser = (EEPROM.read(FTP_USER_ADDRESS) == 0xFF) ? "" : getEEPROM(FTP_USER_ADDRESS, FTP_USER_MAX);
  ftpPass = (EEPROM.read(FTP_PASS_ADDRESS) == 0xFF) ? "" : getEEPROM(FTP_PASS_ADDRESS, FTP_PASS_MAX);
  apSsid = (EEPROM.read(AP_SSID_ADDRESS) == 0xFF) ? "" : getEEPROM(AP_SSID_ADDRESS, AP_SSID_MAX);
  apPass = (EEPROM.read(AP_PASS_ADDRESS) == 0xFF) ? "" : getEEPROM(AP_PASS_ADDRESS, AP_PASS_MAX);
}

void defaultEEPROM() {
  EEPROM.write(VERSION_ADDRESS, VERSIONA);
  EEPROM.write(VERSION_ADDRESS + 1, VERSIONB);

  setEEPROM("", SSID_ADDRESS, SSID_LEN);
  setEEPROM("", PASS_ADDRESS, PASS_LEN);

  EEPROM.write(BAUD_ADDRESS, 0x04);
  EEPROM.write(VERBOSE_ADDRESS, 0x01);

  setEEPROM("SORRY, SYSTEM IS CURRENTLY BUSY. PLEASE TRY AGAIN LATER.", BUSY_MSG_ADDRESS, BUSY_MSG_LEN);

  EEPROM.write(ORIENTATION_ADDRESS, 0x00);
  EEPROM.write(DEFAULTMODE_ADDRESS, 0x00);
  EEPROM.write(USB_DEBUG_ADDRESS, 0x01);  // USB debug enabled by default
  EEPROM.write(QUIET_MODE_ADDRESS, 0x00); // Quiet mode off by default
  EEPROM.write(ESC_CHAR_ADDRESS, '+');    // Default escape char is '+'

  setEEPROM("", FTP_USER_ADDRESS, FTP_USER_MAX);   // blank = anonymous FTP login
  setEEPROM("", FTP_PASS_ADDRESS, FTP_PASS_MAX);
  setEEPROM("", AP_SSID_ADDRESS, AP_SSID_MAX);
  setEEPROM("", AP_PASS_ADDRESS, AP_PASS_MAX);

  // SLIP Gateway defaults - via the named SLIP_*_ADDRESS constants
  // (slip_mode.h), not hardcoded literal offsets - a prior version of this
  // function hardcoded 800-815 directly, which silently broke (wrote to
  // the wrong, now-unused location) the moment those constants were
  // renumbered elsewhere without this function being updated to match.
  // Gateway IP: 192.168.7.1
  EEPROM.write(SLIP_IP_ADDRESS,     192);
  EEPROM.write(SLIP_IP_ADDRESS + 1, 168);
  EEPROM.write(SLIP_IP_ADDRESS + 2, 7);
  EEPROM.write(SLIP_IP_ADDRESS + 3, 1);
  // Client IP: 192.168.7.2
  EEPROM.write(SLIP_CLIENT_IP_ADDRESS,     192);
  EEPROM.write(SLIP_CLIENT_IP_ADDRESS + 1, 168);
  EEPROM.write(SLIP_CLIENT_IP_ADDRESS + 2, 7);
  EEPROM.write(SLIP_CLIENT_IP_ADDRESS + 3, 2);
  // Subnet: 255.255.255.0
  EEPROM.write(SLIP_SUBNET_ADDRESS,     255);
  EEPROM.write(SLIP_SUBNET_ADDRESS + 1, 255);
  EEPROM.write(SLIP_SUBNET_ADDRESS + 2, 255);
  EEPROM.write(SLIP_SUBNET_ADDRESS + 3, 0);
  // DNS: 8.8.8.8
  EEPROM.write(SLIP_DNS_ADDRESS,     8);
  EEPROM.write(SLIP_DNS_ADDRESS + 1, 8);
  EEPROM.write(SLIP_DNS_ADDRESS + 2, 8);
  EEPROM.write(SLIP_DNS_ADDRESS + 3, 8);
  // Clear port forward entries (used by SLIP)
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
  // null pad the remainder of the memory space
  for (unsigned int i = inString.length() + startAddress; i < maxLen + startAddress; i++) {
    EEPROM.write(i, 0x00);
  }
}
