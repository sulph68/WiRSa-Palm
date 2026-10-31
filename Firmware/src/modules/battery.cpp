#include "battery.h"
#include "globals.h"

#if defined(BATTERY_PIN)

#ifndef BATTERY_DIVIDER
#define BATTERY_DIVIDER 2        // the board's divider ratio (T7-S3: 1/2)
#endif
#ifndef BATTERY_CAL_PERMILLE
#define BATTERY_CAL_PERMILLE 1000   // no gain correction
#endif
#define BATTERY_SAMPLES 16
#define BATTERY_NONE_MV 2500     // below this nothing sensible is on the input

bool batteryAvailable() { return true; }

uint32_t batteryMilliVolts() {
  analogSetPinAttenuation(BATTERY_PIN, ADC_11db);   // pin sees at most ~2.1V
  uint32_t sum = 0;
  for (int i = 0; i < BATTERY_SAMPLES; i++) {
    sum += analogReadMilliVolts(BATTERY_PIN);        // calibrated by the core
    delay(2);
  }
  uint32_t mv = (uint32_t) (((uint64_t) (sum / BATTERY_SAMPLES) * BATTERY_DIVIDER * BATTERY_CAL_PERMILLE + 500) / 1000);
  return mv < BATTERY_NONE_MV ? 0 : mv;
}

// Resting LiPo curve, millivolts -> percent (descending).
static const uint16_t kCurveMv[]  = {4200, 4150, 4110, 4080, 4020, 3980, 3950, 3910, 3870, 3850,
                                     3840, 3820, 3800, 3790, 3770, 3750, 3730, 3710, 3690, 3610, 3270};
static const uint8_t  kCurvePct[] = { 100,   95,   90,   85,   80,   75,   70,   65,   60,   55,
                                       50,   45,   40,   35,   30,   25,   20,   15,   10,    5,    0};

int batteryPercent(uint32_t mv) {
  const int n = sizeof(kCurveMv) / sizeof(kCurveMv[0]);
  if (mv >= kCurveMv[0]) return 100;
  if (mv <= kCurveMv[n - 1]) return 0;
  for (int i = 1; i < n; i++) {
    if (mv >= kCurveMv[i]) {
      uint32_t span = kCurveMv[i - 1] - kCurveMv[i];
      uint32_t pos = mv - kCurveMv[i];
      return kCurvePct[i] + (int) ((pos * (kCurvePct[i - 1] - kCurvePct[i]) + span / 2) / span);
    }
  }
  return 0;
}

String batteryInfoLine() {
  uint32_t mv = batteryMilliVolts();
  if (mv == 0) return "Batt: none";
  char buf[24];
  snprintf(buf, sizeof(buf), "Batt: %u.%02uV %d%%", (unsigned) (mv / 1000), (unsigned) ((mv % 1000) / 10), batteryPercent(mv));
  return String(buf);
}

#else   // no battery input on this board

bool batteryAvailable() { return false; }
uint32_t batteryMilliVolts() { return 0; }
int batteryPercent(uint32_t) { return 0; }
String batteryInfoLine() { return String(); }

#endif
