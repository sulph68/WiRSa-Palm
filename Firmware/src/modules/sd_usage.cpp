// sd_usage.cpp - see sd_usage.h. Counts free clusters of a FAT16 or FAT32 card
// (<= 2 GB cards are FAT16, SDHC up to 32 GB FAT32); FAT12 and exFAT show the total only.
#include "globals.h"
#include "sd_usage.h"
#include <SD.h>

String sdHumanBytes(uint64_t bytes, int decimals) {
  static const char *units[] = {"B", "KB", "MB", "GB"};
  float val = (float) bytes;
  int unitIdx = 0;
  while (val >= 1024.0f && unitIdx < 3) {
    val /= 1024.0f;
    unitIdx++;
  }
  String s = String(val, unitIdx == 0 ? 0 : decimals);   // whole bytes stay integers
  s += units[unitIdx];
  return s;
}

namespace {

enum State : uint8_t { S_IDLE, S_SCAN, S_DONE, S_NOFAT32 };   // S_NOFAT32: not FAT16/FAT32, total only

State gState = S_IDLE;
volatile bool gDirty = false;
volatile uint32_t gLastActivity = 0;
uint32_t gFatStart = 0, gFatSectors = 0, gBytesPerCluster = 0, gTotalClusters = 0, gEntryBytes = 4;
uint32_t gNextSector = 0, gFree = 0;
uint64_t gTotal = 0, gUsed = 0;
bool gHaveTotal = false;

const uint32_t kQuietMs = 3000;       // rescan only once the card has been quiet this long
const uint32_t kSectorsPerStep = 32;  // 16 KB of FAT per loop pass

bool parseBoot(uint32_t baseLBA) {
  static uint8_t sec[512];
  if (!SD.readRAW(sec, baseLBA)) return false;
  if (sec[510] != 0x55 || sec[511] != 0xAA) return false;
  uint16_t bytesPerSec = sec[11] | ((uint16_t) sec[12] << 8);
  uint8_t secPerClus = sec[13];
  uint16_t rsvd = sec[14] | ((uint16_t) sec[15] << 8);
  uint8_t numFATs = sec[16];
  uint16_t rootEnt = sec[17] | ((uint16_t) sec[18] << 8);
  uint32_t totSec16 = sec[19] | ((uint32_t) sec[20] << 8);
  uint32_t fatSz16 = sec[22] | ((uint32_t) sec[23] << 8);
  uint32_t totSec32 = sec[32] | ((uint32_t) sec[33] << 8) | ((uint32_t) sec[34] << 16) | ((uint32_t) sec[35] << 24);
  uint32_t fatSz32 = sec[36] | ((uint32_t) sec[37] << 8) | ((uint32_t) sec[38] << 16) | ((uint32_t) sec[39] << 24);
  // A real boot sector, not an MBR whose code happens to sit where a BPB would be.
  bool fat32Name = memcmp(sec + 82, "FAT32   ", 8) == 0;
  bool fat16Name = memcmp(sec + 54, "FAT", 3) == 0;
  if (!fat32Name && !fat16Name) return false;
  if (bytesPerSec != 512 || !secPerClus || (secPerClus & (secPerClus - 1)) || !rsvd || numFATs < 1 || numFATs > 2) return false;
  uint32_t fatSz = fatSz16 ? fatSz16 : fatSz32;
  uint32_t totSec = totSec16 ? totSec16 : totSec32;
  if (!fatSz || !totSec) return false;
  uint32_t rootSectors = ((uint32_t) rootEnt * 32 + bytesPerSec - 1) / bytesPerSec;
  uint32_t meta = rsvd + (uint32_t) numFATs * fatSz + rootSectors;
  if (totSec <= meta) return false;
  uint32_t clusters = (totSec - meta) / secPerClus;
  if (clusters < 4085) return false;                 // FAT12: not counted
  gEntryBytes = clusters < 65525 ? 2 : 4;            // FAT16 or FAT32 by cluster count
  gFatStart = baseLBA + rsvd;
  gFatSectors = fatSz;
  gBytesPerCluster = (uint32_t) bytesPerSec * secPerClus;
  gTotalClusters = clusters;
  return true;
}

bool parseLayout() {
  if (parseBoot(0)) return true;                  // no partition table
  static uint8_t mbr[512];
  if (!SD.readRAW(mbr, 0)) return false;
  if (mbr[510] != 0x55 || mbr[511] != 0xAA) return false;
  uint32_t lba = mbr[454] | ((uint32_t) mbr[455] << 8) | ((uint32_t) mbr[456] << 16) | ((uint32_t) mbr[457] << 24);
  return lba && parseBoot(lba);
}

}  // namespace

void sdUsageActivity() {
  gLastActivity = millis();
  gDirty = true;
}

void sdUsageStep() {
  if (SD.cardType() == CARD_NONE) { gState = S_IDLE; gHaveTotal = false; gDirty = false; return; }

  switch (gState) {
    case S_IDLE:
      if (!parseLayout()) {
        gTotal = SD.cardSize();                   // not FAT16/FAT32: the card size only
        gHaveTotal = gTotal > 0;
        gState = S_NOFAT32;
        return;
      }
      gTotal = (uint64_t) gTotalClusters * gBytesPerCluster;
      gHaveTotal = true;
      gNextSector = 0; gFree = 0;
      gDirty = false;
      gState = S_SCAN;
      return;
    case S_SCAN: {
      static uint8_t sec[512];
      const uint32_t perSector = 512 / gEntryBytes;
      uint32_t totalEntries = gTotalClusters + 2;
      for (uint32_t k = 0; k < kSectorsPerStep && gNextSector < gFatSectors; k++, gNextSector++) {
        uint32_t first = gNextSector * perSector;
        if (first >= totalEntries) { gNextSector = gFatSectors; break; }
        if (!SD.readRAW(sec, gFatStart + gNextSector)) { gState = S_NOFAT32; return; }
        uint32_t from = (gNextSector == 0) ? 2 : 0;
        uint32_t to = perSector;
        if (first + to > totalEntries) to = totalEntries - first;
        for (uint32_t i = from; i < to; i++) {
          uint32_t off = i * gEntryBytes;
          uint32_t v = sec[off] | ((uint32_t) sec[off + 1] << 8);
          if (gEntryBytes == 4) v |= ((uint32_t) sec[off + 2] << 16) | (((uint32_t) sec[off + 3] & 0x0F) << 24);
          if (v == 0) gFree++;
        }
      }
      if (gNextSector >= gFatSectors || gNextSector * perSector >= totalEntries) {
        uint64_t freeBytes = (uint64_t) gFree * gBytesPerCluster;
        gUsed = freeBytes < gTotal ? gTotal - freeBytes : 0;
        gState = S_DONE;
      }
      return;
    }
    case S_DONE:
      if (gDirty && millis() - gLastActivity > kQuietMs) gState = S_IDLE;   // changed: count again
      return;
    case S_NOFAT32:
      return;
  }
}

String sdUsageText() {
  if (SD.cardType() == CARD_NONE || !gHaveTotal) return String();
  if (gState == S_DONE) return sdHumanBytes(gUsed, 3) + "/" + sdHumanBytes(gTotal, 1);
  if (gState == S_NOFAT32) return sdHumanBytes(gTotal, 1);
  return String("?/") + sdHumanBytes(gTotal, 1);
}
