// led.cpp - see led.h.
#include "globals.h"
#include "led.h"

#if defined(WIRSA_LED_NEOPIXEL)
// A WS2812 on GPIO WIRSA_LED_NEOPIXEL instead of the plain LED. The pin, colour (WIRSA_LED_RGB) and level (WIRSA_LED_NEO_PCT, percent)
// belong to the board: globals.h.
#include <esp_timer.h>
static SemaphoreHandle_t gLedLock;   // ledSet() is called from the loop, WiFi events and SD activity; one RMT write at a time
static esp_timer_handle_t gPulseTimer;
static volatile uint8_t gPulse = 0;   // ledActivity() kind being shown; ledSet() only records the state while it is
static void neoWrite(const uint8_t rgb[3], uint32_t pct) {
  if (gLedLock) xSemaphoreTake(gLedLock, portMAX_DELAY);
  neopixelWrite(WIRSA_LED_NEOPIXEL, rgb[0] * pct / 100, rgb[1] * pct / 100, rgb[2] * pct / 100);
  if (gLedLock) xSemaphoreGive(gLedLock);
}
#elif WIRSA_LED_BRIGHTNESS < 100
static const uint8_t LED_CH = 7;   // LEDC channel; nothing else here uses LEDC
#endif
static volatile bool gOn = false;

#if defined(WIRSA_LED_NEOPIXEL)
static volatile bool gDark = false;   // ledDark(): the deep sleep is next
#endif

static void ledDrive(bool on) {
#if defined(WIRSA_LED_NEOPIXEL)
  static const uint8_t rgb[3] = { WIRSA_LED_RGB };
  static const uint8_t nowifi[3] = { WIRSA_LED_NOWIFI_RGB };
  if (gPulse) return;
  if (gDark || menuMode == MODE_MESH) neoWrite(rgb, 0);               // asleep, or a Mesh session: only the activity pulses show
  else neoWrite(on ? rgb : nowifi, WIRSA_LED_NEO_PCT);                // blue = WiFi on, red = WiFi off
#elif WIRSA_LED_BRIGHTNESS < 100
  uint32_t duty = on ? (255UL * WIRSA_LED_BRIGHTNESS) / 100 : 0;
  ledcWrite(LED_CH, LED_ON == HIGH ? duty : 255 - duty);
#else
  digitalWrite(LED_PIN, on ? LED_ON : LED_OFF);
#endif
}

void ledInit() {
#if defined(WIRSA_LED_NEOPIXEL)
  gLedLock = xSemaphoreCreateMutex();
#elif WIRSA_LED_BRIGHTNESS < 100
  ledcSetup(LED_CH, 5000, 8);
  ledcAttachPin(LED_PIN, LED_CH);
#else
  pinMode(LED_PIN, OUTPUT);
#endif
  ledSet(false);
}

#if defined(WIRSA_LED_NEOPIXEL)
static void pulseEnd(void *) {
  gPulse = 0;
  ledDrive(gOn);
}

void ledActivity(uint8_t kind) {
  static const uint8_t green[3] = { WIRSA_LED_ACT_RGB };
  if (gDark) return;
  if (!gPulseTimer) {
    esp_timer_create_args_t a = {};
    a.callback = pulseEnd;
    a.name = "ledact";
    if (esp_timer_create(&a, &gPulseTimer) != ESP_OK) return;
  }
  if (gPulse > kind) return;                            // a longer or stronger pulse is showing: let it finish
  gPulse = kind;
  esp_timer_stop(gPulseTimer);
  neoWrite(green, kind == LED_ACT_SD ? 0 : WIRSA_LED_NEO_PCT);        // SD: dark
  esp_timer_start_once(gPulseTimer, kind == LED_ACT_MODE ? 400000 : kind == LED_ACT_TEXT ? 150000 : kind == LED_ACT_FRAME ? 40000 : 30000);
}

void ledRefresh() { ledDrive(gOn); }

void ledDark() {
  gDark = true;
  gPulse = 0;
  ledDrive(false);
}
#endif

void ledSet(bool on) {
  gOn = on;
  ledDrive(on);
}

bool ledIsOn() { return gOn; }
