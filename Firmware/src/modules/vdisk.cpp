#include "vdisk.h"
#include <string.h>
#include <stdlib.h>
#ifdef ARDUINO
#include <Arduino.h>
#include "esp_partition.h"
#endif

static const uint32_t kSec = 512, kBlk = 4096, kSecPerBlk = kBlk / kSec, kWays = 4;
static const uint32_t kPartStart = 8;            // first sector of the volume: 4 KB aligned
static const uint32_t kReserved = 8, kRootEntries = 512, kSecPerClus = 4;

static const char kLabel[12] = "ESP32      ";   // volume name the Palm shows

static VdiskBackend be = {0, 0, 0, 0};
static bool beSet = false;

struct Slot { uint32_t blk; uint32_t stamp; bool valid, dirty; };
static Slot slot[kWays];
static uint8_t *data = 0;
static uint32_t tick = 0;

#ifdef ARDUINO
static const esp_partition_t *part = 0;
static bool pRead(uint32_t o, void *b, size_t n) { return esp_partition_read(part, o, b, n) == ESP_OK; }
static bool pErase(uint32_t o, size_t n) { return esp_partition_erase_range(part, o, n) == ESP_OK; }
static bool pWrite(uint32_t o, const void *b, size_t n) { return esp_partition_write(part, o, b, n) == ESP_OK; }
static const VdiskBackend devBe = { pRead, pErase, pWrite, 0 };
static bool findPart() {
  if (part) return true;
  part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, (esp_partition_subtype_t) 0x40, "vdisk");
  return part != 0;
}
#endif

void vdiskSetBackend(const VdiskBackend *b) { be = *b; beSet = true; }

bool vdiskPresent() {
  if (beSet) return be.size != 0;
#ifdef ARDUINO
  return findPart();
#else
  return false;
#endif
}

uint32_t vdiskSectors() {
  if (!vdiskPresent()) return 0;
#ifdef ARDUINO
  if (!beSet) return part->size / kSec;
#endif
  return be.size / kSec;
}

bool vdiskOpen() {
  if (data) return true;
  if (!vdiskPresent()) return false;
#ifdef ARDUINO
  if (!beSet) { be = devBe; be.size = part->size; beSet = true; }
  data = (uint8_t *) heap_caps_malloc(kWays * kBlk, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!data) data = (uint8_t *) malloc(kWays * kBlk);
#else
  data = (uint8_t *) malloc(kWays * kBlk);
#endif
  if (!data) return false;
  memset(slot, 0, sizeof(slot));
  tick = 0;
  return true;
}

static bool writeBack(uint32_t i) {
  if (!slot[i].dirty) return true;
  uint32_t off = slot[i].blk * kBlk;
  uint8_t *p = data + i * kBlk;
  static uint8_t cur[kBlk];                      // unchanged block: skip the erase and write
  bool same = be.read(off, cur, kBlk) && memcmp(cur, p, kBlk) == 0;
  if (!same && !(be.erase(off, kBlk) && be.write(off, p, kBlk))) return false;
  slot[i].dirty = false;
  return true;
}

bool vdiskFlush() {
  if (!data) return true;
  bool ok = true;
  for (uint32_t i = 0; i < kWays; i++) if (slot[i].valid && !writeBack(i)) ok = false;
  return ok;
}

uint32_t vdiskDirtyBlocks() {
  uint32_t n = 0;
  if (data) for (uint32_t i = 0; i < kWays; i++) if (slot[i].valid && slot[i].dirty) n++;
  return n;
}

void vdiskClose() {
  if (!data) return;
  vdiskFlush();
  free(data);
  data = 0;
  memset(slot, 0, sizeof(slot));
}

// Find (or load) the block holding `blk`; `load` false when the caller will overwrite all of it.
static int getBlock(uint32_t blk, bool load) {
  for (uint32_t i = 0; i < kWays; i++)
    if (slot[i].valid && slot[i].blk == blk) { slot[i].stamp = ++tick; return (int) i; }
  uint32_t v = 0;                                // victim: an empty way, else the least recently used
  for (uint32_t i = 0; i < kWays; i++) {
    if (!slot[i].valid) { v = i; break; }
    if (slot[i].stamp < slot[v].stamp) v = i;
  }
  if (slot[v].valid && !writeBack(v)) return -1;
  if (load && !be.read(blk * kBlk, data + v * kBlk, kBlk)) { slot[v].valid = false; return -1; }
  slot[v].blk = blk; slot[v].valid = true; slot[v].dirty = false; slot[v].stamp = ++tick;
  return (int) v;
}

bool vdiskRead(uint8_t *buf, uint32_t sector, uint32_t count) {
  if (!data || sector > vdiskSectors() || count > vdiskSectors() - sector) return false;
  while (count) {
    uint32_t blk = sector / kSecPerBlk, in = sector % kSecPerBlk;
    uint32_t n = kSecPerBlk - in < count ? kSecPerBlk - in : count;
    int i = getBlock(blk, true);
    if (i < 0) return false;
    memcpy(buf, data + i * kBlk + in * kSec, n * kSec);
    buf += n * kSec; sector += n; count -= n;
  }
  return true;
}

bool vdiskWrite(const uint8_t *buf, uint32_t sector, uint32_t count) {
  if (!data || sector > vdiskSectors() || count > vdiskSectors() - sector) return false;
  while (count) {
    uint32_t blk = sector / kSecPerBlk, in = sector % kSecPerBlk;
    uint32_t n = kSecPerBlk - in < count ? kSecPerBlk - in : count;
    int i = getBlock(blk, n != kSecPerBlk);
    if (i < 0) return false;
    memcpy(data + i * kBlk + in * kSec, buf, n * kSec);
    slot[i].dirty = true;
    buf += n * kSec; sector += n; count -= n;
  }
  return true;
}

bool vdiskIsFormatted() {
  uint8_t s[kSec];
  if (!vdiskOpen() || !vdiskRead(s, 0, 1)) return false;
  if (s[510] != 0x55 || s[511] != 0xAA || s[450] != 0x06) return false;
  uint32_t start = s[454] | (s[455] << 8) | (s[456] << 16) | ((uint32_t) s[457] << 24);
  if (start == 0 || start >= vdiskSectors() || !vdiskRead(s, start, 1)) return false;
  return s[510] == 0x55 && s[511] == 0xAA && memcmp(s + 54, "FAT16", 5) == 0;
}

static void put16(uint8_t *p, uint32_t v) { p[0] = (uint8_t) v; p[1] = (uint8_t) (v >> 8); }
static void put32(uint8_t *p, uint32_t v) { put16(p, v & 0xFFFF); put16(p + 2, v >> 16); }

bool vdiskFormat(uint32_t serial) {
  if (!vdiskOpen()) return false;
  uint32_t total = vdiskSectors() - kPartStart;
  uint32_t rootSecs = kRootEntries * 32 / kSec;
  uint32_t fatSecs = 1, clusters = 0;
  for (int k = 0; k < 8; k++) {                  // fatSecs depends on clusters and back
    clusters = (total - kReserved - 2 * fatSecs - rootSecs) / kSecPerClus;
    uint32_t need = ((clusters + 2) * 2 + kSec - 1) / kSec;
    if (need == fatSecs) break;
    fatSecs = need;
  }
  if (clusters < 4085 || clusters > 65524) return false;   // would not be FAT16
  // MBR: one partition, type 0x06
  uint8_t s[kSec];
  memset(s, 0, kSec);
  s[446] = 0x00; s[447] = 0xFE; s[448] = 0xFF; s[449] = 0xFF; s[450] = 0x06;
  s[451] = 0xFE; s[452] = 0xFF; s[453] = 0xFF;
  put32(s + 454, kPartStart); put32(s + 458, total);
  s[510] = 0x55; s[511] = 0xAA;
  if (!vdiskWrite(s, 0, 1)) return false;
  memset(s, 0, kSec);
  for (uint32_t i = 1; i < kPartStart; i++) if (!vdiskWrite(s, i, 1)) return false;
  // boot sector
  s[0] = 0xEB; s[1] = 0x3C; s[2] = 0x90;
  memcpy(s + 3, "WIRSA   ", 8);
  put16(s + 11, kSec); s[13] = kSecPerClus; put16(s + 14, kReserved);
  s[16] = 2; put16(s + 17, kRootEntries);
  put16(s + 19, total < 65536 ? total : 0); s[21] = 0xF8; put16(s + 22, fatSecs);
  put16(s + 24, 63); put16(s + 26, 255); put32(s + 28, kPartStart);
  put32(s + 32, total < 65536 ? 0 : total);
  s[36] = 0x80; s[38] = 0x29; put32(s + 39, serial);
  memcpy(s + 43, kLabel, 11); memcpy(s + 54, "FAT16   ", 8);
  s[510] = 0x55; s[511] = 0xAA;
  if (!vdiskWrite(s, kPartStart, 1)) return false;
  memset(s, 0, kSec);
  for (uint32_t i = 1; i < kReserved; i++) if (!vdiskWrite(s, kPartStart + i, 1)) return false;
  // two FATs (entry 0 = media, entry 1 = end of chain) and an empty root directory
  uint32_t fat0 = kPartStart + kReserved;
  for (uint32_t f = 0; f < 2; f++) {
    uint32_t base = fat0 + f * fatSecs;
    for (uint32_t i = 0; i < fatSecs; i++) {
      memset(s, 0, kSec);
      if (i == 0) { s[0] = 0xF8; s[1] = 0xFF; s[2] = 0xFF; s[3] = 0xFF; }
      if (!vdiskWrite(s, base + i, 1)) return false;
    }
  }
  memset(s, 0, kSec);
  for (uint32_t i = 0; i < rootSecs; i++) if (!vdiskWrite(s, fat0 + 2 * fatSecs + i, 1)) return false;
  return vdiskEnsureLabel() && vdiskFlush();
}

// Size and free space of the FAT16 volume vdiskFormat() writes (volume at sector 8): reads the boot
// sector and counts the zero FAT entries (24 sectors).
bool vdiskUsage(uint64_t *used, uint64_t *total) {
  if (!vdiskOpen()) return false;
  uint8_t s[kSec];
  if (!vdiskRead(s, kPartStart, 1) || memcmp(s + 54, "FAT16", 5) != 0) return false;
  uint32_t reserved = s[14] | (s[15] << 8), fatSecs = s[22] | (s[23] << 8);
  uint32_t perClus = s[13], rootSecs = (s[17] | (s[18] << 8)) * 32 / kSec;
  uint32_t secs = s[19] | (s[20] << 8);
  if (!secs) secs = s[32] | (s[33] << 8) | ((uint32_t) s[34] << 16) | ((uint32_t) s[35] << 24);
  if (!perClus || !fatSecs || secs < reserved + 2 * fatSecs + rootSecs) return false;
  uint32_t clusters = (secs - reserved - 2 * fatSecs - rootSecs) / perClus, freeC = 0;
  for (uint32_t f = 0; f < fatSecs; f++) {
    if (!vdiskRead(s, kPartStart + reserved + f, 1)) return false;
    for (uint32_t i = (f == 0 ? 2 : 0); i < 256 && f * 256 + i < clusters + 2; i++)
      if (!(s[i * 2] | s[i * 2 + 1])) freeC++;
  }
  *total = (uint64_t) clusters * perClus * kSec;
  *used = (uint64_t) (clusters - freeC) * perClus * kSec;
  return true;
}

bool vdiskPrepare(uint32_t serial) {
  if (!vdiskOpen()) return false;
  if (vdiskIsFormatted()) return vdiskEnsureLabel();
  return vdiskFormat(serial);
}

// Volume label in the boot sector and as the root directory's label entry (attribute 0x08). Safe on a
// disk that already holds files: it only reuses the label entry or the first free root entry.
bool vdiskEnsureLabel() {
  if (!vdiskOpen()) return false;
  uint8_t s[kSec];
  if (!vdiskRead(s, kPartStart, 1) || memcmp(s + 54, "FAT16", 5) != 0) return false;
  uint32_t reserved = s[14] | (s[15] << 8), fatSecs = s[22] | (s[23] << 8);
  uint32_t rootSecs = (s[17] | (s[18] << 8)) * 32 / kSec;
  if (memcmp(s + 43, kLabel, 11) != 0) {
    memcpy(s + 43, kLabel, 11);
    if (!vdiskWrite(s, kPartStart, 1)) return false;
  }
  uint32_t root = kPartStart + reserved + 2 * fatSecs;
  int freeSec = -1, freeIdx = -1;
  for (uint32_t i = 0; i < rootSecs; i++) {
    if (!vdiskRead(s, root + i, 1)) return false;
    for (uint32_t e = 0; e < kSec; e += 32) {
      if (s[e] != 0x00 && s[e] != 0xE5 && s[e + 11] == 0x08) {        // existing label entry
        if (memcmp(s + e, kLabel, 11) == 0) return vdiskFlush();
        memcpy(s + e, kLabel, 11);
        return vdiskWrite(s, root + i, 1) && vdiskFlush();
      }
      if (freeSec < 0 && (s[e] == 0x00 || s[e] == 0xE5)) { freeSec = (int) i; freeIdx = (int) e; }
    }
  }
  if (freeSec < 0) return true;                                      // root full: the boot sector label stays
  if (!vdiskRead(s, root + freeSec, 1)) return false;
  memset(s + freeIdx, 0, 32);
  memcpy(s + freeIdx, kLabel, 11);
  s[freeIdx + 11] = 0x08;
  return vdiskWrite(s, root + freeSec, 1) && vdiskFlush();
}
