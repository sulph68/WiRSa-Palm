// Host test for vdisk.cpp (no ESP32 needed). From Firmware/:
//   g++ -std=c++17 -Wall -Wextra -Iinclude src/modules/vdisk.cpp test_host/vdisk_test.cpp -o /tmp/vdisk_test && /tmp/vdisk_test
#include "vdisk.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

static int fails = 0;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL: %s\n", m); fails++; } } while (0)

static std::vector<uint8_t> flash;
static int erases = 0;
static bool fRead(uint32_t o, void *b, size_t n) { memcpy(b, &flash[o], n); return true; }
static bool fErase(uint32_t o, size_t n) { if (o % 4096 || n % 4096) return false; memset(&flash[o], 0xFF, n); erases++; return true; }
static bool fWrite(uint32_t o, const void *b, size_t n) {
  const uint8_t *p = (const uint8_t *) b;
  for (size_t i = 0; i < n; i++) { if (p[i] & ~flash[o + i]) return false; flash[o + i] &= p[i]; }   // NOR: 1->0 only
  return true;
}

int main() {
  flash.assign(0xC00000, 0xFF);
  VdiskBackend b = { fRead, fErase, fWrite, (uint32_t) flash.size() };
  vdiskSetBackend(&b);
  CHECK(vdiskPresent() && vdiskSectors() == 24576, "geometry");
  CHECK(vdiskOpen(), "open");
  CHECK(!vdiskIsFormatted(), "blank is not formatted");
  CHECK(vdiskFormat(0x12345678), "format");
  CHECK(vdiskIsFormatted(), "formatted after format");
  {
    uint8_t q[512];
    vdiskRead(q, 8, 1);
    CHECK(!memcmp(q + 43, "ESP32      ", 11), "boot sector label");
    vdiskRead(q, 8 + 8 + 48, 1);
    CHECK(!memcmp(q, "ESP32      ", 11) && q[11] == 0x08, "root directory label entry");
  }
  {
    uint64_t u = 1, t = 0;
    CHECK(vdiskUsage(&u, &t) && u == 0 && t == 6120ull * 2048, "empty volume usage");
  }
  uint8_t s[512];
  vdiskRead(s, 8, 1);
  CHECK(s[510] == 0x55 && s[511] == 0xAA && !memcmp(s + 54, "FAT16", 5), "boot sector");
  uint32_t fatSecs = s[22] | (s[23] << 8), res = s[14] | (s[15] << 8);
  CHECK(((8 + res + 2 * fatSecs + 32) % 8) == 0, "data area 4K aligned");
  uint32_t clusters = (24568 - res - 2 * fatSecs - 32) / 4;
  CHECK(clusters >= 4085 && clusters <= 65524, "FAT16 cluster count");
  printf("fatSecs=%u reserved=%u clusters=%u erases=%d\n", fatSecs, res, clusters, erases);
  // unflushed data is invisible on flash but readable through the cache
  uint8_t w[512 * 3], r[512 * 3];
  for (size_t i = 0; i < sizeof(w); i++) w[i] = (uint8_t) (i * 7 + 1);
  CHECK(vdiskWrite(w, 6000, 3), "write 3 (straddles blocks)");
  CHECK(vdiskRead(r, 6000, 3) && !memcmp(r, w, sizeof(w)), "read back through cache");
  CHECK(vdiskDirtyBlocks() >= 1, "dirty");
  CHECK(vdiskFlush() && vdiskDirtyBlocks() == 0, "flush");
  CHECK(!memcmp(&flash[6000 * 512], w, sizeof(w)), "on flash after flush");
  // rewriting identical data causes no erase
  int e0 = erases;
  vdiskWrite(w, 6000, 3); vdiskFlush();
  CHECK(erases == e0, "identical rewrite skips the erase");
  // eviction: touch more blocks than ways, everything still correct
  std::vector<uint8_t> big(512 * 8);
  for (int k = 0; k < 10; k++) {
    for (size_t i = 0; i < big.size(); i++) big[i] = (uint8_t) (k * 31 + i);
    CHECK(vdiskWrite(big.data(), 8000 + k * 64, 8), "write whole block");
  }
  for (int k = 0; k < 10; k++) {
    for (size_t i = 0; i < big.size(); i++) big[i] = (uint8_t) (k * 31 + i);
    uint8_t rr[512 * 8];
    CHECK(vdiskRead(rr, 8000 + k * 64, 8) && !memcmp(rr, big.data(), sizeof(rr)), "evicted block reads back");
  }
  CHECK(!vdiskRead(r, 24576, 1) && !vdiskWrite(w, 24575, 2), "bounds");
  vdiskClose();
  CHECK(vdiskOpen() && vdiskIsFormatted(), "persists across close/open");
  CHECK(vdiskPrepare(1), "prepare on a formatted disk");
  {
    std::vector<uint8_t> save = flash;
    vdiskClose();
    flash.assign(0xC00000, 0xFF);
    CHECK(vdiskPrepare(2) && vdiskIsFormatted(), "prepare formats a blank disk");
    vdiskClose();
    flash = save;
    vdiskOpen();
  }
  vdiskRead(r, 6000, 3);
  CHECK(!memcmp(r, w, sizeof(w)), "data persists");
  if (const char *img = getenv("VDISK_IMG")) { FILE *f = fopen(img, "wb"); fwrite(&flash[0], 1, flash.size(), f); fclose(f); }
  printf(fails ? "%d FAILED\n" : "all ok\n", fails);
  return fails != 0;
}
