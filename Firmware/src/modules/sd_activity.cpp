// sd_activity.cpp - see sd_activity.h.
#include "globals.h"
#include "sd_activity.h"
#include "led.h"
#include "sd_usage.h"
#include <esp_timer.h>

namespace {
const uint64_t FLASH_US = 30000;
esp_timer_handle_t gTimer = nullptr;
volatile bool gLit = false;       // LED currently showing the flash
bool gRest = false;               // LED state to return to

#if defined(WIRSA_LED_DISCRETE)
#define SD_LED(on) ledPlain(on)   // the colour LEDs show the flash through ledActivity()
#else
#define SD_LED(on) ledSet(on)
#endif

void restore(void *) {
  SD_LED(gRest);
  gLit = false;
}
}

void sdActivity() {
  sdUsageActivity();
#if defined(WIRSA_LED_COLOUR)
  ledActivity(LED_ACT_SD);   // a colour LED: a red flicker
#if defined(WIRSA_LED_NEOPIXEL)
  return;                    // no plain LED beside it
#endif
#endif
  if (!gTimer) {
    esp_timer_create_args_t a = {};
    a.callback = restore;
    a.name = "sdled";
    if (esp_timer_create(&a, &gTimer) != ESP_OK) return;
  }
  if (!gLit) {
    gRest = ledIsOn();
    gLit = true;
    SD_LED(!gRest);
  } else {
    esp_timer_stop(gTimer);
  }
  esp_timer_start_once(gTimer, FLASH_US);
}
