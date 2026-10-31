// autobaud.cpp - see autobaud.h.
#include "autobaud.h"

uint32_t autobaudDetect(const uint32_t e[AUTOBAUD_EDGES], uint32_t cps, const int *rates, uint8_t nRates) {
  // Where the edges of 'S' (0x53, bits 1,1,0,0,1,0,1,0 LSB first) fall, in bit times from the start bit.
  static const uint8_t kPos[1][AUTOBAUD_EDGES] = { {0, 1, 3, 5, 6, 7, 8, 9} };
  for (uint8_t p = 0; p < 1; p++) {
    // Least-squares fit x = c + t*k through the 6 (position, time) points: one bit time t
    // from all the edges, so one late interrupt doesn't move it far (adjacent-edge
    // intervals double the jitter, which fails at 115200).
    int64_t x[AUTOBAUD_EDGES];
    for (uint8_t i = 0; i < AUTOBAUD_EDGES; i++) x[i] = (int64_t) (uint32_t) (e[i] - e[0]);
    // skip = -1: all edges. Else that interior edge is left out of the fit: a Palm's UART can put one edge of
    // its 'S' half a bit off, every time (the sixth, 12 us late, at 38400), and the speed must still be read for
    // any rate (the edge may be at most 90% of a bit away).
    int64_t t = 0;
    bool ok = false;
    for (int skip = -1; skip < AUTOBAUD_EDGES - 1 && !ok; skip++) {   // all edges, then without one
      if (skip == 0) continue;                                         // the start edge is the reference
      int64_t sk = 0, sx = 0, skk = 0, skx = 0, n = 0;
      for (uint8_t i = 0; i < AUTOBAUD_EDGES; i++) {
        if (i == skip) continue;
        sk += kPos[p][i]; sx += x[i]; n++;
        skk += (int64_t) kPos[p][i] * kPos[p][i];
        skx += (int64_t) kPos[p][i] * x[i];
      }
      int64_t den = n * skk - sk * sk;
      t = (n * skx - sk * sx) / den;               // bit time, cycles
      if (t <= 0) continue;
      int64_t c = (sx - t * sk) / n;
      ok = true;
      for (uint8_t i = 0; i < AUTOBAUD_EDGES && ok; i++) {
        int64_t res = x[i] - (c + t * kPos[p][i]);
        if (res < 0) res = -res;
        ok = i == skip ? res * 100 <= t * 90 : res * 100 <= t * 35;    // interrupt latency jitters each edge
      }
    }
    if (!ok) continue;
    for (uint8_t r = 0; r < nRates; r++) {
      int64_t bit = (int64_t) (cps / (uint32_t) rates[r]);
      int64_t d = t > bit ? t - bit : bit - t;
      if (d * 100 <= bit * 8) return (uint32_t) rates[r];
    }
    return 0;
  }
  return 0;
}

#ifdef ARDUINO
#include <Arduino.h>

namespace {
volatile uint32_t sEdge[AUTOBAUD_EDGES];
volatile uint8_t sN = 0;
volatile uint32_t sLast = 0;
volatile bool sQuietBefore = false;
uint32_t sQuietCycles = 0;
bool sArmed = false;
int sPin = -1;

inline uint32_t cycles() {
  uint32_t c;
  __asm__ __volatile__("rsr %0, ccount" : "=a"(c));
  return c;
}

void IRAM_ATTR edgeIsr() {
  uint32_t t = cycles();
  uint8_t n = sN;
  if (n == 0) {
    sQuietBefore = (t - sLast) >= sQuietCycles;
    sEdge[0] = t;
    sN = 1;
  } else if (n < AUTOBAUD_EDGES) {
    sEdge[n] = t;
    sN = n + 1;
  }
  sLast = t;
}
}  // namespace

void autobaudArm(int pin) {
  if (sArmed) return;
  sQuietCycles = (uint32_t) getCpuFrequencyMhz() * 2000UL;   // 2 ms
  sPin = pin;
  sN = 0;
  sLast = 0;
  attachInterrupt(digitalPinToInterrupt(pin), edgeIsr, CHANGE);
  sArmed = true;
}

void autobaudDisarm() {
  if (!sArmed) return;
  detachInterrupt(digitalPinToInterrupt(sPin));
  sArmed = false;
  sN = 0;
}

bool autobaudTake(uint32_t e[AUTOBAUD_EDGES]) {
  if (!sArmed) return false;
  noInterrupts();
  uint8_t n = sN;
  bool quiet = sQuietBefore;
  uint32_t last = sLast;
  uint32_t idle = cycles() - last;
  bool take = (n >= AUTOBAUD_EDGES && idle >= sQuietCycles + sQuietCycles / 2);   // 3 ms: phrase over
  if (take) {
    for (uint8_t i = 0; i < AUTOBAUD_EDGES; i++) e[i] = sEdge[i];
    sN = 0;
  } else if (n > 0 && n < AUTOBAUD_EDGES && idle > sQuietCycles * 10) {
    sN = 0;                                    // 20 ms of nothing: a partial burst
  }
  interrupts();
  return take && quiet;
}
#endif
