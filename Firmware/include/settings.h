#ifndef SETTINGS_H
#define SETTINGS_H

#include <Arduino.h>

// EEPROM and settings management functions
void writeSettings();
void readSettings();
void defaultEEPROM();
String getEEPROM(int startAddress, int len);
void setEEPROM(String inString, int startAddress, int maxLen);

// Exit Timers (mode_timer.h), seconds, 0 = never: HotSync / CalDAV Relay /
// Bitwarden Relay go back to the main menu if no session starts within
// modeUnusedExitSec, or modeAfterExitSec after a session ended.
#define MODE_UNUSED_EXIT_DEFAULT  45
#define MODE_AFTER_EXIT_DEFAULT   15
extern uint8_t modeUnusedExitSec;
extern uint8_t modeAfterExitSec;

// SLIP Gateway goes back to the main menu after this many minutes with
// nothing heard from the Palm (Config > Exit Timers / WiRSaCtl Setup).
// One of 1, 2, 3, 5.
#define SLIP_TIMEOUT_DEFAULT  5
extern uint8_t slipTimeoutMin;
bool slipTimeoutValid(uint8_t minutes);

// SASD goes back to the main menu after this many minutes with nothing heard from the Palm (Config > Exit Timers / WiRSaCtl Setup).
// One of 3, 5, 10, 15. The Palm's SASD driver holds the serial port while it works, so WiRSaCtl cannot end SASD from the Palm.
#define SASD_TIMEOUT_DEFAULT  3
extern uint8_t sasdTimeoutMin;
bool sasdTimeoutValid(uint8_t minutes);

// Which disk SASD serves to the Palm (WiRSaCtl Setup): the SD card if one answers else the
// internal virtual card (vdisk.h), the SD card only, or the virtual card only.
enum { SASD_DISK_AUTO = 0, SASD_DISK_SD = 1, SASD_DISK_VIRTUAL = 2 };
extern uint8_t sasdDiskMode;

// Deep sleep (deep_sleep.h): after this many minutes with nothing from the Palm in the main
// menu the board sleeps; the Palm line wakes it. 0 = off, else one of 1, 3, 5, 15.
extern uint8_t deepSleepMin;
bool deepSleepValid(uint8_t minutes);

#endif // SETTINGS_H
