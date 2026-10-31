// Host test for autobaud.cpp (no ESP32 needed). From Firmware/:
//   g++ -std=c++17 -Wall -Wextra -Iinclude src/modules/autobaud.cpp test_host/autobaud_test.cpp -o /tmp/autobaud_test && /tmp/autobaud_test
#include "autobaud.h"
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <vector>

static int fails = 0;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL: %s\n", m); fails++; } } while (0)

static const int kRates[] = { 9600, 14400, 19200, 28800, 38400, 57600, 115200 };
#define NRATES ((uint8_t) (sizeof(kRates) / sizeof(kRates[0])))
static const uint32_t CPS = 240000000UL;

// edges of one 8N1 byte sent at `rate`, starting at cycle t0, with +-jit cycles of noise
static std::vector<uint32_t> edgesOf(uint8_t byte, int rate, uint32_t t0, int jit) {
  std::vector<uint32_t> out;
  double bit = (double) CPS / rate;
  int level = 1;
  double t = 0;
  int bits[10];
  bits[0] = 0;
  for (int i = 0; i < 8; i++) bits[1 + i] = (byte >> i) & 1;
  bits[9] = 1;
  for (int i = 0; i < 10; i++) {
    if (bits[i] != level) {
      int j = jit ? (rand() % (2 * jit + 1)) - jit : 0;
      out.push_back(t0 + (uint32_t) (t + j));
      level = bits[i];
    }
    t += bit;
  }
  return out;
}

static uint32_t detect(uint8_t byte, int rate, int jitUs) {
  std::vector<uint32_t> v = edgesOf(byte, rate, 1000000, jitUs * 240);
  // the next character follows (back to back or after a gap) - the detector uses the first 8 edges only
  std::vector<uint32_t> w = edgesOf('E', rate, 1000000 + (uint32_t) ((double) CPS / rate * 10), jitUs * 240);
  v.insert(v.end(), w.begin(), w.end());
  if (v.size() < AUTOBAUD_EDGES) return 0;
  return autobaudDetect(v.data(), CPS, kRates, sizeof(kRates) / sizeof(kRates[0]));
}

int main() {
  srand(1);
  for (size_t r = 0; r < sizeof(kRates) / sizeof(kRates[0]); r++) {
    int jit = kRates[r] >= 57600 ? 2 : 1;   // +-2 us per edge: a pessimistic interrupt latency
    int bad = 0;
    for (int k = 0; k < 500; k++) {
      if (detect('S', kRates[r], jit) != (uint32_t) kRates[r]) bad++;
      if (detect('S', kRates[r], jit) != (uint32_t) kRates[r]) bad++;
    }
    printf("rate %6d: %d/1000 wrong\n", kRates[r], bad);
    CHECK(bad <= (kRates[r] >= 57600 ? 3 : 0), "S detected at every rate with interrupt jitter");
  }
  // other first bytes (a control frame lead 0x16, 'A', CR, 0x55, ...) must not match
  const uint8_t others[] = { 0x16, 'A', 'a', 's', 'T', '\r', 0x55, 'B', 'H', 0xC0, 0x00, 0xFF };
  for (size_t o = 0; o < sizeof(others); o++)
    for (size_t r = 0; r < sizeof(kRates) / sizeof(kRates[0]); r++)
      CHECK(detect(others[o], kRates[r], 1) == 0, "non-A bytes are not detected");
  // a rate that is not in the table (e.g. 48000) is not mapped to a neighbour
  {
    std::vector<uint32_t> v = edgesOf('S', 48000, 5, 0);
    CHECK(autobaudDetect(v.data(), CPS, kRates, NRATES) == 0, "48000 is not forced onto 38400/57600");
  }
  // a Palm's 'S' at 38400 (measured on hardware, us from the first edge): the sixth edge is half a bit late
  {
    const uint32_t us[AUTOBAUD_EDGES] = { 0, 22, 76, 126, 154, 194, 206, 230 };
    const uint32_t us2[AUTOBAUD_EDGES] = { 0, 23, 77, 129, 154, 195, 207, 231 };
    uint32_t e[AUTOBAUD_EDGES];
    for (int i = 0; i < AUTOBAUD_EDGES; i++) e[i] = us[i] * 240;
    CHECK(autobaudDetect(e, CPS, kRates, NRATES) == 38400, "the measured Palm 38400 S is detected");
    for (int i = 0; i < AUTOBAUD_EDGES; i++) e[i] = us2[i] * 240;
    CHECK(autobaudDetect(e, CPS, kRates, NRATES) == 38400, "the second measured Palm 38400 S is detected");
    // a Palm's 'S' at 28800 (measured): the fourth edge is 24 us late
    const uint32_t us3[AUTOBAUD_EDGES] = { 0, 32, 102, 198, 208, 240, 276, 310 };
    for (int i = 0; i < AUTOBAUD_EDGES; i++) e[i] = us3[i] * 240;
    CHECK(autobaudDetect(e, CPS, kRates, NRATES) == 28800, "the measured Palm 28800 S is detected");
    // SLIP traffic seen at the wrong rate (measured: 199 258 283 311 335 364 439 us) is not an S
    const uint32_t junk[AUTOBAUD_EDGES] = { 0, 199, 258, 283, 311, 335, 364, 439 };
    for (int i = 0; i < AUTOBAUD_EDGES; i++) e[i] = junk[i] * 240;
    CHECK(autobaudDetect(e, CPS, kRates, NRATES) == 0, "measured junk is not detected");
  }
  // one edge of the 'S' up to 45% of a bit off (any of the six interior edges) is read at every rate
  for (size_t r = 0; r < sizeof(kRates) / sizeof(kRates[0]); r++)
    for (int k = 1; k < AUTOBAUD_EDGES - 1; k++)
      for (int dir = -1; dir <= 1; dir += 2) {
        std::vector<uint32_t> v = edgesOf('S', kRates[r], 1000000, 0);
        v[k] += (uint32_t) (int) (dir * 0.45 * (double) CPS / kRates[r]);
        CHECK(autobaudDetect(v.data(), CPS, kRates, NRATES) == (uint32_t) kRates[r], "one off edge still gives the rate");
      }
  // random edge noise
  int hits = 0;
  for (int k = 0; k < 100000; k++) {
    uint32_t e[AUTOBAUD_EDGES];
    uint32_t t = 0;
    for (int i = 0; i < AUTOBAUD_EDGES; i++) { t += 500 + rand() % 60000; e[i] = t; }
    if (autobaudDetect(e, CPS, kRates, NRATES)) hits++;
  }
  printf("noise: %d/100000 false detections\n", hits);
  CHECK(hits < 1000, "random edges are (almost) never detected");   // < 1% (0.13% before the one-off-edge case); a wrong guess is also caught by the BR line
  printf(fails ? "FAILED\n" : "autobaud checks: all passed\n");
  return fails ? 1 : 0;
}
