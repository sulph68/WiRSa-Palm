// sd_format.cpp - see sd_format.h.
#include "globals.h"
#include "sd_format.h"
#include "sd_mount.h"
#include "sd_usage.h"
#include "vdfs.h"
#include "vdisk.h"
#include <SD.h>
#include "sd_diskio.h"
extern "C" {
#include "ff.h"
#include "diskio.h"
}

static const char kSasdDir[] = "/PALM/PROGRAMS/SASD";
static const uint64_t kFat16Max = 2ULL * 1024 * 1024 * 1024;   // cards up to 2 GB are FAT16, bigger ones FAT32

// One go at the format with the card at hz, f_mkfs writing at most workLen bytes at a time. Returns the FatFs result
// (FR_NOT_READY when the card did not start); *bytes is the card's size, *fmt the type written.
static FRESULT formatOnce(int hz, void *work, UINT workLen, uint64_t *bytes, BYTE *fmt) {
  uint8_t pdrv = sdcard_init(SD_CS, &SPI, hz);
  if (pdrv == 0xFF) return FR_INT_ERR;
  FRESULT r = FR_NOT_READY;
  if (!(disk_initialize(pdrv) & STA_NOINIT)) {
    *bytes = (uint64_t) sdcard_num_sectors(pdrv) * sdcard_sector_size(pdrv);
    *fmt = *bytes <= kFat16Max ? FM_FAT : FM_FAT32;
    // One partition over the whole card (as ESP-IDF's own card format: f_fdisk, then f_mkfs into it).
    DWORD plist[] = { 100, 0, 0, 0 };
    char drv[3] = { (char) ('0' + pdrv), ':', 0 };
    r = f_fdisk(pdrv, plist, work);
    if (r == FR_OK) r = f_mkfs(drv, *fmt, 0, work, workLen);
    if (r == FR_MKFS_ABORTED && *fmt == FM_FAT) {         // no FAT16 cluster size fits this card: FAT32
      LOGW("SD format: FAT16 does not fit, using FAT32\r\n");
      *fmt = FM_FAT32;
      r = f_mkfs(drv, *fmt, 0, work, workLen);
    }
    disk_ioctl(pdrv, CTRL_SYNC, NULL);
  }
  sdcard_uninit(pdrv);
  return r;
}

bool sdFormatCard(uint8_t *fatBits, String &err) {
  *fatBits = 0;
  // Release the card from the SD object (and the FTP root's /SDCard), then drive it through FatFs alone.
  SD.end();
  delay(20);

  // A bigger work area makes f_mkfs clear the FAT in fewer, longer writes. Some cards fail every multi-sector write
  // (FR_DISK_ERR, at 20 MHz and 4 MHz alike: seen on the generic board's 1 GB card) while single sectors work, as FTP's
  // short writes do: so step down to one sector at a time, then to a slow clock (as sdMount() falls back to 4 MHz).
  const UINT kWork = 32 * 1024;
  void *work = psramFound() ? ps_malloc(kWork) : malloc(kWork);
  if (!work) work = malloc(4096);
  if (!work) { err = "Out of memory"; sdMount(); return false; }
  struct { int hz; UINT len; } tries[] = { { WIRSA_SD_HZ, kWork }, { WIRSA_SD_HZ, 512 }, { 4000000, 512 } };
  FRESULT r = FR_NOT_READY;
  uint64_t bytes = 0;
  BYTE fmt = FM_FAT;
  for (auto &t : tries) {
    if (t.len == kWork && !psramFound()) continue;          // only a 4 KB area was allocated
    r = formatOnce(t.hz, work, t.len, &bytes, &fmt);
    LOGI("SD format: %lu MB card, FAT%u, %d Hz, %u-byte writes -> %d\r\n", (unsigned long) (bytes >> 20),
         fmt == FM_FAT ? 16u : 32u, t.hz, (unsigned) t.len, (int) r);
    if (r == FR_OK || r == FR_NOT_READY && t.hz == 4000000) break;
    delay(50);
  }
  free(work);
  if (r != FR_OK) {
    err = r == FR_NOT_READY ? "No SD card" : "Format failed (" + String((int) r) + ")";
    LOGW("SD format: %s\r\n", err.c_str());
    sdMount();
    return false;
  }
  sdUsageReset();                                          // the old card's count is meaningless now
  if (!sdMount()) { err = "Formatted, but the card did not mount"; return false; }
  *fatBits = fmt == FM_FAT ? 16 : 32;
  LOGI("SD format: done, FAT%u\r\n", (unsigned) *fatBits);
  return true;
}

static bool sdMakeDirs(const char *path) {
  String p;
  const char *s = path;
  while (*s) {
    p += *s++;
    if (*s == '/' || *s == 0) {
      if (!SD.exists(p) && !SD.mkdir(p)) return false;
    }
  }
  return true;
}

// Copies src (on from) to dst on the SD card and reads both back to compare. Returns "" or the step that failed.
static const char *copyOne(fs::FS &from, const String &src, const String &dst, uint8_t *buf) {
  File in = from.open(src, FILE_READ);
  if (!in) return "open source";
  if (SD.exists(dst)) SD.remove(dst);
  File out = SD.open(dst, FILE_WRITE);
  if (!out) { in.close(); return "create"; }
  out.setBufferSize(512);   // the file's stdio buffer would otherwise hand FatFs 4 KB, a multi-sector write
  size_t size = in.size(), done = 0;
  const char *bad = NULL;
  // One sector per write and per read: FatFs then sends single-block commands, which every card takes (see sdFormatCard).
  while (!bad && done < size) {
    size_t n = in.read(buf, 512);
    if (n == 0) bad = "read source";
    else if (out.write(buf, n) != n) bad = "write";
    done += n;
  }
  in.close();
  out.close();
  if (bad) return bad;
  in = from.open(src, FILE_READ);
  out = SD.open(dst, FILE_READ);
  if (!in || !out) { if (in) in.close(); if (out) out.close(); return "reopen"; }
  if (out.size() != size) { in.close(); out.close(); return "size"; }
  while (!bad && done > 0) {
    size_t a = in.read(buf, 512), b = out.read(buf + 512, 512);
    if (a != b || a == 0 || memcmp(buf, buf + 512, a) != 0) bad = "compare";
    done -= a < done ? a : done;
  }
  in.close();
  out.close();
  return bad ? bad : "";
}

bool sdCopySasdFromVirtual(uint8_t *copied, uint8_t *total, String &err) {
  *copied = *total = 0;
  if (SD.cardType() == CARD_NONE && !sdMount()) { err = "No SD card"; return false; }
  if (!vdfsMount()) { err = "The virtual card could not be opened"; return false; }
  File dir = vdfs().open(kSasdDir);
  if (!dir || !dir.isDirectory()) {
    if (dir) dir.close();
    vdfsUnmount();
    return true;                                           // no SASD set on the virtual card: nothing to copy
  }
  if (!sdMakeDirs(kSasdDir)) { dir.close(); vdfsUnmount(); err = "Could not make " + String(kSasdDir); return false; }
  uint8_t *buf = (uint8_t *) malloc(1024);
  if (!buf) { dir.close(); vdfsUnmount(); err = "Out of memory"; return false; }
  for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
    bool isDir = f.isDirectory();
    String name = f.name();
    f.close();
    if (isDir) continue;
    int slash = name.lastIndexOf('/');
    if (slash >= 0) name = name.substring(slash + 1);
    if (name.startsWith("._")) continue;                   // macOS resource forks
    if (*total < 255) (*total)++;
    String src = String(kSasdDir) + "/" + name;
    const char *bad = "";
    for (int tryNo = 1; tryNo <= 3; tryNo++) {
      bad = copyOne(vdfs(), src, src, buf);
      if (!*bad) break;
      LOGW("SD format: %s: %s failed (try %d of 3)\r\n", src.c_str(), bad, tryNo);
      delay(50);
    }
    if (!*bad) (*copied)++;
  }
  free(buf);
  dir.close();
  vdfsUnmount();
  sdUsageActivity();
  LOGI("SD format: copied %u of %u SASD files from the virtual card\r\n", (unsigned) *copied, (unsigned) *total);
  return true;
}
