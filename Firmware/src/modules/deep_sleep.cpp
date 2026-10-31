// deep_sleep.cpp - see deep_sleep.h.

#include "deep_sleep.h"

#if defined(WIRSA_DEEP_SLEEP)

#include "globals.h"
#include "settings.h"
#include "log.h"
#include "led.h"
#include "palm_ctl.h"
#include "vdisk.h"
#include "vdfs.h"
#include "ftp_server.h"
#include "mesh_relay.h"
#include <WiFi.h>
#include <esp_sleep.h>
#include <esp_system.h>
#include <driver/rtc_io.h>

namespace {

const gpio_num_t kWakePin = (gpio_num_t) WIRSA_WAKE_PIN;   // the Palm RX, or a pin wired to it
const uint32_t kFastMagic = 0xF457D5EE;

// RTC fast memory: loaded at power-on and every reset, but left alone by a deep sleep wake. It holds
// the marker below, so a wake can tell whether the memory kept its contents while asleep.
RTC_FAST_ATTR uint32_t gFastMagic = 0;
RTC_FAST_ATTR uint32_t gWakes = 0;

unsigned long gLastActive = 0;
unsigned long gBusySince = 0;

const char *causeName(esp_sleep_wakeup_cause_t c) {
  switch (c) {
    case ESP_SLEEP_WAKEUP_EXT0:  return "ext0";
    case ESP_SLEEP_WAKEUP_EXT1:  return "Palm line";
    case ESP_SLEEP_WAKEUP_TIMER: return "timer";
    case ESP_SLEEP_WAKEUP_GPIO:  return "gpio";
    default:                     return "other";
  }
}

bool buttonDown() {
#if WIRSA_HAS_DISPLAY
  return digitalRead(SW1_PIN) == LOW || digitalRead(SW2_PIN) == LOW || digitalRead(SW3_PIN) == LOW || digitalRead(SW4_PIN) == LOW;
#else
  return false;
#endif
}

bool busy() {
  if (menuMode != MODE_MAIN) return true;
#if WIRSA_FEATURE_FTPSERVER
  if (ftpServerRunning) return true;
#endif
  if (vdfsMounted()) return true;
  return false;
}

// A background check is running: not activity (the idle time must run on across the checks), but the board
// must not go to sleep in the middle of one.
bool checkRunning() {
#if WIRSA_FEATURE_MESH
  return meshBgActive() && !meshBgIdle();
#else
  return false;
#endif
}

void sleepNow() {
  LOGI("Deep sleep: nothing from the Palm for %u min, waking on the Palm line (wake %lu so far)\r\n",
       (unsigned) deepSleepMin, (unsigned long) gWakes);
  uint64_t timerUs = 0;
#if WIRSA_FEATURE_MESH
  if (meshBgActive()) timerUs = meshBeforeSleep();   // texts to the SD card (or the ring), and the next check's timer
#endif
  if (!vdfsMounted()) vdiskClose();
#if defined(WIRSA_LED_NEOPIXEL)
  ledDark();   // an RGB LED would keep its red in the sleep
#else
  ledSet(false);
#endif
  PhysicalSerial.flush();
  Serial.flush();
  WiFi.disconnect(true, false);
  WiFi.mode(WIFI_OFF);
#if WIRSA_HAS_DISPLAY
  display.ssd1306_command(SSD1306_DISPLAYOFF);   // the panel keeps its last picture and its current while the board sleeps
#endif
  deepSleepEnter(timerUs);
}

}  // namespace

void deepSleepEnter(uint64_t timerUs) {
  gFastMagic = kFastMagic;
  esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_FAST_MEM, ESP_PD_OPTION_ON);
  esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_SLOW_MEM, ESP_PD_OPTION_ON);
  rtc_gpio_init(kWakePin);
  rtc_gpio_set_direction(kWakePin, RTC_GPIO_MODE_INPUT_ONLY);
  rtc_gpio_pulldown_dis(kWakePin);
  rtc_gpio_pullup_en(kWakePin);                  // the line idles high; a start bit pulls it low
  esp_sleep_enable_ext1_wakeup(1ULL << kWakePin, ESP_EXT1_WAKEUP_ANY_LOW);
  if (timerUs) esp_sleep_enable_timer_wakeup(timerUs);
  esp_deep_sleep_start();
}

void deepSleepBoot() {
  gLastActive = millis();
  if (esp_reset_reason() != ESP_RST_DEEPSLEEP) {
    gFastMagic = 0;
    gWakes = 0;
    return;
  }
  rtc_gpio_deinit(kWakePin);
  gWakes++;
  LOGI("Deep sleep: woke by %s (wake %lu); fast RTC memory %s\r\n", causeName(esp_sleep_get_wakeup_cause()),
       (unsigned long) gWakes, gFastMagic == kFastMagic ? "kept" : "LOST");
  gFastMagic = 0;
}

// Bytes waiting on the Palm line: palm_ctl.cpp watches both receivers; without it there is just the one.
static bool palmRxPending() {
#if WIRSA_FEATURE_PALMCTL
  return palmCtlRxPending();
#else
  return PhysicalSerial.available() > 0;
#endif
}

void deepSleepPoll() {
  if (!deepSleepMin) { gLastActive = millis(); return; }
  if (busy() || buttonDown() || palmRxPending() || Serial.available() || digitalRead(kWakePin) == LOW) {
    gLastActive = millis();
    return;
  }
  if (millis() - gLastActive >= (unsigned long) deepSleepMin * 60000UL && !checkRunning()) sleepNow();
}

#endif
