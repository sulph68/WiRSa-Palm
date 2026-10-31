// log.h - leveled log: USB serial plus a small session log kept in RTC memory.
//
// Levels: off < warn < info < verbose. A line goes to USB serial if its level is at or below
// gLogLevel, and into the session log if it is at or below max(gLogLevel, warn), so warnings
// and errors are kept even with USB output off. The session log lives in RTC no-init memory:
// nothing is written to flash or the SD card; it survives a software restart, a crash or
// watchdog reset, and normally a brownout reset, but not a power loss (it is then discarded).
#pragma once
#include <stddef.h>
#include <stdint.h>

enum LogLevel : uint8_t { LOG_OFF = 0, LOG_WARN = 1, LOG_INFO = 2, LOG_VERBOSE = 3 };

extern uint8_t gLogLevel;   // default LOG_INFO
extern bool usbDebug;       // gLogLevel >= LOG_INFO (legacy gate for code not converted to LOGx)

void logInit();                              // once, first thing in setup(): validates the session log, notes the reset reason
void logSetLevel(uint8_t level);             // also keeps usbDebug in step
void logPrintf(uint8_t level, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

// Session log as text, oldest first. logSize() bytes in all; logRead copies from offset.
size_t logSize();
size_t logRead(size_t offset, uint8_t *out, size_t max);
void logClear();

// Settings byte: bit 4 set = new format (low bits = level); otherwise the old 0/1 on-off flag.
uint8_t logLevelToEeprom(uint8_t level);
uint8_t logLevelFromEeprom(uint8_t b);

static inline bool logOn(uint8_t level) {
  return level <= (gLogLevel < LOG_WARN ? (uint8_t) LOG_WARN : gLogLevel);
}

#define LOGW(...) do { if (logOn(LOG_WARN))    logPrintf(LOG_WARN,    __VA_ARGS__); } while (0)
#define LOGI(...) do { if (logOn(LOG_INFO))    logPrintf(LOG_INFO,    __VA_ARGS__); } while (0)
#define LOGV(...) do { if (logOn(LOG_VERBOSE)) logPrintf(LOG_VERBOSE, __VA_ARGS__); } while (0)
