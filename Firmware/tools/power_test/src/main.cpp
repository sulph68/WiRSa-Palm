// Power test: one cycle of eight 25 s phases, repeated. Each phase prints "PHASE n/8 <state>" first, so the log (and
// the meter) can be lined up. Nothing but the named thing runs: no WiFi, no SD, no display.
#include <Arduino.h>
#include <NimBLEDevice.h>
#include <esp_sleep.h>

static const uint32_t PHASE_MS = 25000;

static void say(int n, const char *state) {
  Serial.printf("PHASE %d/8 %s  (cpu %u MHz, %u s)\r\n", n, state, (unsigned) getCpuFrequencyMhz(), (unsigned) (PHASE_MS / 1000));
  Serial.flush();
}

static void cpu(int mhz) { setCpuFrequencyMhz(mhz); }

static void scanFor(uint32_t ms) {
  NimBLEDevice::init("floor");
  NimBLEScan *s = NimBLEDevice::getScan();
  s->setActiveScan(false);
  s->setInterval(100);
  s->setWindow(100);                   // 100 % duty: the radio listens all the time, like a connecting check
  s->start(ms / 1000, false);          // blocks for the whole time and ends by itself: no stop call
  NimBLEDevice::deinit(true);
}

void setup() {
  Serial.begin(115200);
  delay(1500);
  Serial.println("\r\n=== power test: cycle of 8 phases ===");
  cpu(240); say(1, "awake, idle, no radio");           delay(PHASE_MS);
  cpu(160); say(2, "awake, idle, no radio");           delay(PHASE_MS);
  cpu(80);  say(3, "awake, idle, no radio");           delay(PHASE_MS);
  cpu(40);  say(4, "awake, idle, no radio");           delay(PHASE_MS);
  cpu(240); say(5, "awake, Bluetooth listening");      scanFor(PHASE_MS);
  cpu(80);  say(6, "awake, Bluetooth listening");      scanFor(PHASE_MS);
  cpu(80);  say(7, "LIGHT SLEEP (USB may drop)");
  esp_sleep_enable_timer_wakeup(PHASE_MS * 1000ULL);
  esp_light_sleep_start();
  Serial.println("woke from light sleep");
  say(8, "DEEP SLEEP (USB drops), then the cycle restarts");
  esp_sleep_enable_timer_wakeup(PHASE_MS * 1000ULL);
  esp_deep_sleep_start();
}

void loop() {}
