// log.cpp - see log.h.
#include <Arduino.h>
#include <esp_attr.h>
#include <esp_system.h>
#include <stdarg.h>
#include <string.h>
#include "log.h"

uint8_t gLogLevel = LOG_INFO;
bool usbDebug = true;

namespace {

const uint32_t kMagic = 0x57694C47;   // "WiLG"
const size_t kRing = 4096;

struct RtcLog {
  uint32_t magic;
  uint32_t head;     // next write position
  uint32_t count;    // valid bytes, <= kRing
  char data[kRing];
};
RTC_NOINIT_ATTR RtcLog gRtc;

SemaphoreHandle_t gLock = nullptr;

void put(char c) {
  gRtc.data[gRtc.head] = c;
  gRtc.head = (gRtc.head + 1) % kRing;
  if (gRtc.count < kRing) gRtc.count++;
}

void putLine(const char *s, size_t n) {
  for (size_t i = 0; i < n; i++) put(s[i]);
  put('\n');
}

const char *resetName(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_POWERON:   return "power on";
    case ESP_RST_SW:        return "software restart";
    case ESP_RST_PANIC:     return "CRASH (panic)";
    case ESP_RST_INT_WDT:   return "CRASH (interrupt watchdog)";
    case ESP_RST_TASK_WDT:  return "CRASH (task watchdog)";
    case ESP_RST_WDT:       return "CRASH (watchdog)";
    case ESP_RST_BROWNOUT:  return "BROWNOUT";
    case ESP_RST_DEEPSLEEP: return "deep sleep wake";
    case ESP_RST_EXT:       return "reset button";
    default:                return "unknown";
  }
}

}  // namespace

void logInit() {
  if (!gLock) gLock = xSemaphoreCreateMutex();
  bool valid = gRtc.magic == kMagic && gRtc.head < kRing && gRtc.count <= kRing;
  if (!valid) {      // power was lost (or first boot): contents are random
    memset(&gRtc, 0, sizeof(gRtc));
    gRtc.magic = kMagic;
  }
  esp_reset_reason_t r = esp_reset_reason();
  char line[80];
  int n = snprintf(line, sizeof(line), "--- boot: %s%s ---", resetName(r), valid ? "" : ", earlier log lost");
  putLine(line, n > 0 ? (size_t) n : 0);
}

void logSetLevel(uint8_t level) {
  if (level > LOG_VERBOSE) level = LOG_INFO;
  gLogLevel = level;
  usbDebug = level >= LOG_INFO;
}

void logPrintf(uint8_t level, const char *fmt, ...) {
  if (!logOn(level)) return;
  char body[256];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(body, sizeof(body), fmt, ap);
  va_end(ap);
  if (n < 0) return;
  size_t len = (size_t) n < sizeof(body) ? (size_t) n : sizeof(body) - 1;
  while (len && (body[len - 1] == '\n' || body[len - 1] == '\r')) len--;
  body[len] = 0;
  for (size_t i = 0; i < len; i++)          // one line each; a stray control character would break the log text
    if ((uint8_t) body[i] < 0x20 && body[i] != '\t') body[i] = ' ';

  char line[300];
  uint32_t ms = millis();
  int p = snprintf(line, sizeof(line), "[%lu.%03lu] %c %s", (unsigned long) (ms / 1000), (unsigned long) (ms % 1000),
                   level == LOG_WARN ? 'W' : level == LOG_INFO ? 'I' : 'V', body);
  if (p <= 0) return;
  size_t ll = (size_t) p < sizeof(line) ? (size_t) p : sizeof(line) - 1;

  if (gLock) xSemaphoreTake(gLock, portMAX_DELAY);
  putLine(line, ll);
  if (gLock) xSemaphoreGive(gLock);

  if (level <= gLogLevel) {
    Serial.write((const uint8_t *) line, ll);
    Serial.write((const uint8_t *) "\r\n", 2);
  }
}

size_t logSize() { return gRtc.magic == kMagic ? gRtc.count : 0; }

size_t logRead(size_t offset, uint8_t *out, size_t max) {
  if (gRtc.magic != kMagic) return 0;
  if (gLock) xSemaphoreTake(gLock, portMAX_DELAY);
  size_t total = gRtc.count, got = 0;
  size_t start = (gRtc.head + kRing - total) % kRing;
  while (got < max && offset + got < total) {
    out[got] = (uint8_t) gRtc.data[(start + offset + got) % kRing];
    got++;
  }
  if (gLock) xSemaphoreGive(gLock);
  return got;
}

void logClear() {
  if (gLock) xSemaphoreTake(gLock, portMAX_DELAY);
  memset(gRtc.data, 0, sizeof(gRtc.data));
  gRtc.head = 0;
  gRtc.count = 0;
  if (gLock) xSemaphoreGive(gLock);
}

uint8_t logLevelToEeprom(uint8_t level) { return (uint8_t) (0x10 | level); }

uint8_t logLevelFromEeprom(uint8_t b) {
  if (b & 0x10) return (b & 3) <= LOG_VERBOSE ? (uint8_t) (b & 3) : (uint8_t) LOG_INFO;
  return b ? LOG_INFO : LOG_OFF;   // old on/off flag
}
