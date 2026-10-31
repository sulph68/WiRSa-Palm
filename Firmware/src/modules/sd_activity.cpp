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

void restore(void *) {
  ledSet(gRest);
  gLit = false;
}
}

void sdActivity() {
  sdUsageActivity();
#if defined(WIRSA_LED_NEOPIXEL)
  ledActivity(LED_ACT_SD);   // an RGB LED: a dark flash over its colour
  return;
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
    ledSet(!gRest);
  } else {
    esp_timer_stop(gTimer);
  }
  esp_timer_start_once(gTimer, FLASH_US);
}
