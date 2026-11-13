// led.cpp - see led.h.
#include "globals.h"
#include "led.h"
#include "settings.h"

#if defined(WIRSA_LED_COLOUR)
// The colour LED: the states and flickers of led.h. Activity is only a flag write (ledActivity); a 10 ms tick shows it.
// The pins, and the level (WIRSA_LED_NEO_PCT, percent) belong to the board: globals.h.
#include <esp_timer.h>
#include <WiFi.h>
#include <esp_netif.h>
#include <esp_netif_net_stack.h>
#include <lwip/netif.h>
#include <driver/uart.h>
#if defined(WIRSA_LED_NEOPIXEL)
static void colourWrite(bool r, bool g, bool b, uint32_t pct) {
  neopixelWrite(WIRSA_LED_NEOPIXEL, r ? 255 * pct / 100 : 0, g ? 255 * pct / 100 : 0, b ? 255 * pct / 100 : 0);
}
#else
static const uint8_t RGB_PIN[3] = { WIRSA_LED_R_PIN, WIRSA_LED_G_PIN, WIRSA_LED_B_PIN };
static const uint8_t RGB_CH[3] = { 4, 5, 6 };   // LEDC channels; the plain LED has 7, same 5 kHz / 8 bit
static void colourWrite(bool r, bool g, bool b, uint32_t pct) {
  const bool on[3] = { r, g, b };
  for (int i = 0; i < 3; i++) {
    uint32_t duty = on[i] ? 255 * pct / 100 : 0;
    ledcWrite(RGB_CH[i], WIRSA_LED_RGB_ON == HIGH ? duty : 255 - duty);
  }
}
#endif
#endif
#if !defined(WIRSA_LED_NEOPIXEL) && WIRSA_LED_BRIGHTNESS < 100
static const uint8_t LED_CH = 7;   // LEDC channel of the plain LED
#endif
static volatile bool gOn = false;   // the plain LED's state (ledSet)

#if !defined(WIRSA_LED_NEOPIXEL)
static void plainDrive(bool on) {
#if WIRSA_LED_BRIGHTNESS < 100
  uint32_t duty = on ? (255UL * WIRSA_LED_BRIGHTNESS) / 100 : 0;
  ledcWrite(LED_CH, LED_ON == HIGH ? duty : 255 - duty);
#else
  digitalWrite(LED_PIN, on ? LED_ON : LED_OFF);
#endif
}
#endif

#if defined(WIRSA_LED_DISCRETE)
void ledPlain(bool on) { plainDrive(on); }
#endif

#if defined(WIRSA_LED_COLOUR)
enum { A_RED = 1, A_GREEN = 2, A_BLUE = 4 };   // flicker request bits
static volatile uint8_t gReq = 0;        // requested since the last tick
static volatile bool gDark = false;      // ledDark(): the deep sleep is next
static volatile bool gStaUp = false;     // the station has an address
static volatile bool gApUp = false;      // the access point is up
static volatile bool gBleUp = false;
enum { EV_TRY = 1, EV_UP = 2, EV_DOWN = 4 };   // WiFi / BLE events for the tick: connecting, came up, lost
static volatile uint8_t gEvt = 0;
static volatile bool gHotSync = false;   // the BOOT button's red blink
static esp_timer_handle_t gTick = nullptr;
static const uint32_t FLICKER_TICKS = 3;   // 30 ms lit, then 30 ms without a flicker

void ledActivity(uint8_t kind) {
  uint8_t bit = kind == LED_ACT_SD ? A_RED : kind == LED_ACT_SERIAL ? A_GREEN : A_BLUE;
  __atomic_fetch_or(&gReq, bit, __ATOMIC_RELAXED);
}

void ledBle(bool up) {
  gBleUp = up;
  if (up) __atomic_fetch_or(&gEvt, (uint8_t) EV_UP, __ATOMIC_RELAXED);
}
void ledHotSync(bool on) { gHotSync = on; }

// The blue LED's own level, apart from the traffic flicker (wifiPulse in settings.h):
//   connecting   slow blink (0.5 s on / 0.5 s off), for 30 s at most; if nothing is up by then: the drop blinks
//   came up      steady for 30 s, then dark
//   lost         three quick blinks (150 ms on / 150 ms off), then dark
//   up           wifiPulse: always on; or a 100 ms blip every 15 s / 30 s / minute; or never a blip
static bool blueBase(uint32_t now, uint8_t ev) {
  static uint32_t tryAt = 0, tryUntil = 0, steadyUntil = 0, dropAt = 0, nextBeat = 0, beatUntil = 0;
  static bool dropping = false;
  static uint8_t lastMode = 255;
  const bool up = gStaUp || gApUp || gBleUp;
  const uint8_t mode = wifiPulse;
  const uint32_t period = mode == WIFI_PULSE_15S ? 15000 : mode == WIFI_PULSE_30S ? 30000 : 60000;
  if (ev & EV_TRY) { tryAt = now; tryUntil = now + 30000; dropping = false; }
  if (ev & EV_UP) { tryUntil = 0; dropping = false; steadyUntil = now + 30000; nextBeat = now + period; }
  if (ev & EV_DOWN) { tryUntil = 0; steadyUntil = 0; dropping = true; dropAt = now; }
  if (tryUntil && (int32_t) (now - tryUntil) >= 0) { tryUntil = 0; if (!up) { dropping = true; dropAt = now; } }
  if (mode != lastMode) { lastMode = mode; nextBeat = now + period; }
  if (dropping) {
    uint32_t t = now - dropAt;
    if (t < 900) return (t % 300) < 150;
    dropping = false;
  }
  if (tryUntil) return ((now - tryAt) % 1000) < 500;
  if (!up) return false;
  if (mode == WIFI_PULSE_ALWAYS) return true;
  if (mode != WIFI_PULSE_NEVER && (int32_t) (now - nextBeat) >= 0) {
    beatUntil = now + 100;
    do nextBeat += period; while ((int32_t) (now - nextBeat) >= 0);
  }
  return (int32_t) (now - steadyUntil) < 0 || (int32_t) (now - beatUntil) < 0;
}

static void ledTick(void *) {
  static uint8_t phase[3], left[3], pend = 0;   // per flicker: 0 free, 1 lit, 2 gap
  static int last = -1;
  pend |= __atomic_exchange_n(&gReq, (uint8_t) 0, __ATOMIC_RELAXED);
  for (int k = 0; k < 3; k++) {
    if (left[k]) left[k]--;
    if (phase[k] == 1 && !left[k]) { phase[k] = 2; left[k] = FLICKER_TICKS; }
    else if (phase[k] == 2 && !left[k]) phase[k] = 0;
    if (phase[k] == 0 && (pend & (1 << k))) { phase[k] = 1; left[k] = FLICKER_TICKS; pend &= ~(1 << k); }
  }
  uint32_t now = (uint32_t) (esp_timer_get_time() / 1000);
  bool base = blueBase(now, __atomic_exchange_n(&gEvt, (uint8_t) 0, __ATOMIC_RELAXED));
  bool r = phase[0] == 1 || gHotSync;
  bool g = phase[1] == 1;
  bool b = base != (phase[2] == 1);   // a flicker dips the lit blue, or flashes the dark one
  int now_ = r | (g << 1) | (b << 2);
  if (now_ == last) return;
  last = now_;
  colourWrite(r, g, b, WIRSA_LED_NEO_PCT);
}

// WiFi traffic: the STA and AP interfaces get a thin wrapper around their lwIP receive and send functions, which flags the activity and calls the original.
static struct netif *sNetif[2];
static netif_input_fn sIn[2];
static netif_linkoutput_fn sOut[2];
static int netifIdx(struct netif *n) { return n == sNetif[0] ? 0 : n == sNetif[1] ? 1 : -1; }
// Only traffic of ours lights it: a unicast IP frame, not ARP, not a broadcast or multicast (the router's and the other devices' chatter on the LAN), not mDNS (UDP port 5353).
static bool ourTraffic(const struct pbuf *p) {
  if (!p || p->len < 14) return true;
  const uint8_t *f = (const uint8_t *) p->payload;
  if (f[0] & 1) return false;                                     // broadcast or multicast destination
  uint16_t type = (f[12] << 8) | f[13];
  if (type == 0x0800) {                                           // IPv4
    if (p->len >= 14 + 20) {
      uint8_t ihl = (f[14] & 0x0f) * 4;
      if (f[23] == 17 && p->len >= 14 + ihl + 4) {                // UDP
        uint16_t sp = (f[14 + ihl] << 8) | f[15 + ihl], dp = (f[16 + ihl] << 8) | f[17 + ihl];
        if (sp == 5353 || dp == 5353) return false;
      }
    }
    return true;
  }
  return type == 0x86dd;                                          // IPv6 unicast; ARP and anything else: no
}
static err_t wrapIn(struct pbuf *p, struct netif *n) {
  if (ourTraffic(p)) ledActivity(LED_ACT_NET);
  int i = netifIdx(n);
  return i < 0 ? ERR_IF : sIn[i](p, n);
}
static err_t wrapOut(struct netif *n, struct pbuf *p) {
  if (ourTraffic(p)) ledActivity(LED_ACT_NET);
  int i = netifIdx(n);
  return i < 0 ? ERR_IF : sOut[i](n, p);
}
static void hookNetif(const char *key, int i) {
  esp_netif_t *h = esp_netif_get_handle_from_ifkey(key);
  struct netif *n = h ? (struct netif *) esp_netif_get_netif_impl(h) : nullptr;
  if (!n || (sNetif[i] == n && n->input == wrapIn && n->linkoutput == wrapOut)) return;
  if (n->input != wrapIn) sIn[i] = n->input;
  if (n->linkoutput != wrapOut) sOut[i] = n->linkoutput;
  sNetif[i] = n;
  n->input = wrapIn;
  n->linkoutput = wrapOut;
}

static void wifiEvent(arduino_event_id_t ev, arduino_event_info_t) {
  auto lost = [] { if (!gStaUp && !gApUp) __atomic_fetch_or(&gEvt, (uint8_t) EV_DOWN, __ATOMIC_RELAXED); };
  switch (ev) {
    case ARDUINO_EVENT_WIFI_STA_START: if (!gStaUp && !gApUp && !gBleUp) __atomic_fetch_or(&gEvt, (uint8_t) EV_TRY, __ATOMIC_RELAXED); break;
    case ARDUINO_EVENT_WIFI_STA_GOT_IP: case ARDUINO_EVENT_WIFI_STA_GOT_IP6:
      hookNetif("WIFI_STA_DEF", 0);
      if (!gStaUp) { gStaUp = true; __atomic_fetch_or(&gEvt, (uint8_t) EV_UP, __ATOMIC_RELAXED); }
      break;
    case ARDUINO_EVENT_WIFI_AP_START:
      hookNetif("WIFI_AP_DEF", 1);
      if (!gApUp) { gApUp = true; __atomic_fetch_or(&gEvt, (uint8_t) EV_UP, __ATOMIC_RELAXED); }
      break;
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED: case ARDUINO_EVENT_WIFI_STA_LOST_IP: case ARDUINO_EVENT_WIFI_STA_STOP:
      if (gStaUp) { gStaUp = false; lost(); }
      break;
    case ARDUINO_EVENT_WIFI_AP_STOP:
      if (gApUp) { gApUp = false; lost(); }
      break;
    default: break;
  }
}

// Palm serial traffic: the core's UART driver calls are wrapped at link time (platformio.ini: -Wl,--wrap=uart_write_bytes / uart_read_bytes), so every byte in or out of the Palm UART flickers
// the green LED without any call site knowing. The USB console is not a UART, and not counted.
extern "C" {
int __real_uart_write_bytes(uart_port_t n, const void *src, size_t size);
int __real_uart_read_bytes(uart_port_t n, void *buf, uint32_t length, TickType_t wait);
int __wrap_uart_write_bytes(uart_port_t n, const void *src, size_t size) {
  if (n == WIRSA_PALM_UART) ledActivity(LED_ACT_SERIAL);
  return __real_uart_write_bytes(n, src, size);
}
int __wrap_uart_read_bytes(uart_port_t n, void *buf, uint32_t length, TickType_t wait) {
  int got = __real_uart_read_bytes(n, buf, length, wait);
  if (got > 0 && n == WIRSA_PALM_UART) ledActivity(LED_ACT_SERIAL);
  return got;
}
}
#endif

void ledInit() {
#if defined(WIRSA_LED_DISCRETE)
  gpio_deep_sleep_hold_dis();                       // the pins were held dark through a deep sleep
  for (int i = 0; i < 3; i++) {
    gpio_hold_dis((gpio_num_t) RGB_PIN[i]);
    ledcSetup(RGB_CH[i], 5000, 8);
    ledcAttachPin(RGB_PIN[i], RGB_CH[i]);
  }
#endif
#if !defined(WIRSA_LED_NEOPIXEL)
#if WIRSA_LED_BRIGHTNESS < 100
  ledcSetup(LED_CH, 5000, 8);
  ledcAttachPin(LED_PIN, LED_CH);
#else
  pinMode(LED_PIN, OUTPUT);
#endif
#endif
  ledSet(false);
#if defined(WIRSA_LED_TEST) && defined(WIRSA_LED_DISCRETE)
  // Hardware test build (env t7s3_ledtest): red, green, blue in turn, one at a time, forever. The firmware does nothing else.
  Serial.begin(115200);
  for (int n = 0;; n++) {
    int i = n % 3;
    static const char *const name[3] = { "red GPIO%d\n", "green GPIO%d\n", "blue GPIO%d\n" };
    for (int k = 0; k < 3; k++) ledcWrite(RGB_CH[k], k == i ? 80 : 0);
    Serial.printf(name[i], RGB_PIN[i]);
    delay(500);
  }
#endif
#if defined(WIRSA_LED_COLOUR)
  colourWrite(false, false, false, WIRSA_LED_NEO_PCT);
  WiFi.onEvent(wifiEvent);
  esp_timer_create_args_t a = {};
  a.callback = ledTick;
  a.name = "ledtick";
  if (esp_timer_create(&a, &gTick) == ESP_OK) esp_timer_start_periodic(gTick, 10000);
#endif
}

#if defined(WIRSA_LED_COLOUR)
void ledRefresh() {}   // the tick shows the state within 10 ms

void ledDark() {
  gDark = true;
  if (gTick) esp_timer_stop(gTick);
  colourWrite(false, false, false, WIRSA_LED_NEO_PCT);
#if defined(WIRSA_LED_DISCRETE)
  // The pins would float in the sleep: drive each to its off level and hold it (released again by ledInit()).
  for (int i = 0; i < 3; i++) {
    ledcDetachPin(RGB_PIN[i]);
    pinMode(RGB_PIN[i], OUTPUT);
    digitalWrite(RGB_PIN[i], WIRSA_LED_RGB_ON == HIGH ? LOW : HIGH);
    gpio_hold_en((gpio_num_t) RGB_PIN[i]);
  }
  gpio_deep_sleep_hold_en();
#endif
}
#endif

void ledSet(bool on) {
  gOn = on;
#if !defined(WIRSA_LED_NEOPIXEL)
  plainDrive(on);
#endif
}

bool ledIsOn() { return gOn; }
