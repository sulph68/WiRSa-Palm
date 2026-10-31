// mesh_spool_sd.cpp - see mesh_spool_sd.h.
#include "globals.h"
#include "mesh_spool_sd.h"
#include "sd_mount.h"
#include "log.h"
#include "mesh_slots.h"
#include <SD.h>

namespace MeshSpoolSd {
namespace {

// The files of the active node: /Meshtastic/<node id>/mesh.dat and mesh.old. Empty until the node has said who it is: nothing is written or read then, and the texts wait in RAM.
char gDir[24] = "", gDat[40] = "", gOld[40] = "";

// Only the main menu and Mesh mode may use the card (SASD hands it to the Palm; the file modes mount it themselves).
bool usable() {
  if (menuMode != MODE_MAIN && menuMode != MODE_MESH) return false;
  if (SD.cardType() != CARD_NONE && sdAlive()) return true;     // a pulled card is noticed here
  return sdMount();
}

void *bigAlloc(size_t n) {
  void *p = ps_malloc(n);
  return p ? p : malloc(n);
}

// The newest `want` bytes of a file, into buf; returns the byte count.
size_t tail(const char *path, uint8_t *buf, size_t want) {
  File f = SD.open(path, FILE_READ);
  if (!f) return 0;
  size_t sz = f.size(), n = sz < want ? sz : want;
  if (sz > n) f.seek(sz - n);
  size_t got = f.read(buf, n);
  f.close();
  return got;
}

bool ensureDir() {
  if (!SD.exists("/Meshtastic") && !SD.mkdir("/Meshtastic")) return false;
  return SD.exists(gDir) || SD.mkdir(gDir);
}

}  // namespace

void setNode(uint32_t id) {
  if (!id) { gDir[0] = gDat[0] = gOld[0] = '\0'; return; }
  MeshSlots::dirName(gDir, sizeof gDir, id);
  snprintf(gDat, sizeof gDat, "%s/mesh.dat", gDir);
  snprintf(gOld, sizeof gOld, "%s/mesh.old", gDir);
}

bool append(const uint8_t *enc, size_t n) {
  if (!n || !gDat[0] || !usable()) return false;
  if (!ensureDir()) { LOGW("Mesh spool: cannot create %s", gDir); return false; }
  {
    File f = SD.open(gDat, FILE_READ);
    if (f) {
      size_t sz = f.size();
      f.close();
      if (sz >= FILE_MAX) {                         // roll: the old file goes, the full one becomes the old one
        SD.remove(gOld);
        SD.rename(gDat, gOld);
      }
    }
  }
  File f = SD.open(gDat, FILE_APPEND);
  if (!f) { LOGW("Mesh spool: cannot open %s", gDat); SD.end(); return false; }
  size_t w = f.write(enc, n);
  f.close();
  if (w != n) { LOGW("Mesh spool: short write (%u of %u)", (unsigned) w, (unsigned) n); SD.end(); return false; }
  return true;
}

size_t loadNewest(MeshSpool::Fn fn, void *ctx) {
  if (!gDat[0] || !usable()) return 0;
  if (!SD.exists(gDat) && !SD.exists(gOld)) return 0;
  uint8_t *buf = (uint8_t *) bigAlloc(READ_MAX);
  if (!buf) return 0;
  size_t found = 0;
  // The old file's tail first (older texts), then the current file, so the callback sees them oldest first.
  size_t nDat = 0;
  { File f = SD.open(gDat, FILE_READ); if (f) { nDat = f.size(); f.close(); } }
  size_t room = READ_MAX > nDat ? READ_MAX - nDat : 0;
  if (room && SD.exists(gOld)) {
    size_t n = tail(gOld, buf, room);
    found += MeshSpool::scan(buf, n, fn, ctx);
  }
  size_t n = tail(gDat, buf, READ_MAX);
  found += MeshSpool::scan(buf, n, fn, ctx);
  free(buf);
  return found;
}

bool zero() {
  if (!gDat[0] || !usable()) return false;
  bool ok = true;
  if (SD.exists(gDat)) ok = SD.remove(gDat) && ok;
  if (SD.exists(gOld)) ok = SD.remove(gOld) && ok;
  return ok;
}

// A node's folder, files and all.
bool removeNode(uint32_t id) {
  if (!id || !usable()) return false;
  char dir[24], d[40], o[40];
  MeshSlots::dirName(dir, sizeof dir, id);
  snprintf(d, sizeof d, "%s/mesh.dat", dir);
  snprintf(o, sizeof o, "%s/mesh.old", dir);
  if (SD.exists(d)) SD.remove(d);
  if (SD.exists(o)) SD.remove(o);
  return !SD.exists(dir) || SD.rmdir(dir);
}

}  // namespace MeshSpoolSd
