// mesh_store.cpp - Mesh Relay mode: texts kept outside the store (PSRAM) until a Palm has them: the SD card (/Meshtastic), the RTC ring that
// survives a deep sleep, and the seq marks that keep message numbers rising across restarts. Main loop thread only.
#include "mesh_internal.h"

#if WIRSA_FEATURE_MESH

#include "serial_io.h"
#include "display_menu.h"
#include "network.h"
#include "palm_ctl.h"
#include "log.h"
#include "settings.h"
#include "relay_timing.h"
#include "palm_text.h"
#include "mesh_spool_sd.h"

namespace MeshRelay {

// ---- texts on the SD card ------------------------------------------------------------------------------------------
// Main loop thread only. The store (PSRAM) is the working copy; the card is what survives a restart or a deep sleep.

uint32_t spoolFloor() {
  uint32_t f = gFlushedSeq;
  if (!gHistKeep && gConfirmedSeq > f) f = gConfirmedSeq;      // history off: what the Palm has needs no copy
  return f;
}

int spoolPending() {
  CoreLock lk;
  uint32_t fl = spoolFloor();
  int n = 0;
  for (int i = 0; i < MeshCore::msgCount(); i++) {
    const MeshCore::Msg *m = MeshCore::msgAt(i);
    if (!(m->flags & MeshCore::F_OUT) && m->seq > fl) n++;
  }
  return n;
}

// Writes the texts the card does not hold yet. True when there was nothing to do or it worked.
bool spoolFlush() {
  uint8_t *buf = nullptr;
  size_t used = 0;
  uint32_t top = 0;
  int n = 0;
  {
    CoreLock lk;
    uint32_t fl = spoolFloor();
    for (int i = 0; i < MeshCore::msgCount(); i++) {
      const MeshCore::Msg *m = MeshCore::msgAt(i);
      if (!(m->flags & MeshCore::F_OUT) && m->seq > fl) n++;
    }
    if (!n) { gPendingSince = 0; return true; }
    buf = (uint8_t *) ps_malloc((size_t) n * MeshSpool::REC_MAX);
    if (!buf) buf = (uint8_t *) malloc((size_t) n * MeshSpool::REC_MAX);
    if (!buf) return false;
    for (int i = 0; i < MeshCore::msgCount(); i++) {
      const MeshCore::Msg *m = MeshCore::msgAt(i);
      if ((m->flags & MeshCore::F_OUT) || m->seq <= fl) continue;
      MeshSpool::Rec r;
      r.from = m->from; r.to = m->to; r.rxTime = m->rxTime; r.packetId = m->packetId;
      r.chan = m->chan; r.flags = m->flags; r.len = m->len;
      memcpy(r.text, m->text, m->len); r.text[m->len] = 0;
      used += MeshSpool::encode(r, buf + used, (size_t) n * MeshSpool::REC_MAX - used);
      if (m->seq > top) top = m->seq;
    }
  }
  bool ok = MeshSpoolSd::append(buf, used);
  free(buf);
  if (ok) {
    gFlushedSeq = top > gFlushedSeq ? top : gFlushedSeq;
    gSdHas = true;
    gPendingSince = 0;
    LOGI("Mesh: %d text(s) written to the SD card", n);
    gSpoolSaid = false;
  } else {
    gSpoolRetryAt = millis() + RelayTiming::SPOOL_RETRY_MS;                      // no card or a write error: not before a minute
    if (!gSpoolSaid) {                                       // once per spell without a card, not every minute
      gSpoolSaid = true;
      LOGW("Mesh: SD card not available - %d text(s) wait in memory, trying again every minute", n);
    }
  }
  return ok;
}

// ---- the RTC ring ---------------------------------------------------------------------------------------------------

void ringPackCb(const MeshSpool::Rec &r, void *c) {
  struct Pack { uint8_t *b; size_t n; } *p = (Pack *) c;
  p->n += MeshSpool::encode(r, p->b + p->n, RING_BYTES - p->n);
}

// The ring's records to the card, then it is empty. False when there is no card (the records stay).
bool ringFlushToSd() {
  if (!gRing.count()) return true;
  static uint8_t buf[RING_BYTES];
  struct Pack { uint8_t *b; size_t n; } p = { buf, 0 };
  gRing.forEach(ringPackCb, &p);
  if (gHeadless && !gSpiUp) {                    // a timer wake never ran the rest of setup()
    gSpiUp = true;
    pinMode(SD_CS, OUTPUT); digitalWrite(SD_CS, HIGH);
    SPI.begin(SPI_SCK, SPI_MISO, SPI_MOSI, SD_CS);
  }
  if (!MeshSpoolSd::append(buf, p.n)) return false;
  gRing.clear();
  gRingChecks = 0;
  LOGI("Mesh: the ring (%u bytes) written to the SD card", (unsigned) p.n);
  return true;
}

// A text into the ring. Full: to the card if there is one, otherwise the oldest records go (and are counted).
void ringAdd(const MeshSpool::Rec &r) {
  uint8_t tmp[MeshSpool::REC_MAX];
  size_t n = MeshSpool::encode(r, tmp, sizeof tmp);
  if (n && gRing.freeBytes() < n) ringFlushToSd();
  gRing.push(r);
}

void ringImportCb(const MeshSpool::Rec &r, void *c) {
  if (MeshCore::importText(r.from, r.to, r.rxTime, r.packetId, r.chan, r.flags, r.text, r.len)) (*(size_t *) c)++;
}

// A full wake: what the sleep checks left in the ring goes back into the store.
void ringRestore() {
  gRing.attach();
  size_t n = 0;
  if (gRing.count()) {
    gRing.forEach(ringImportCb, &n);
    gRing.clear();
    gRingChecks = 0;
    LOGI("Mesh: %u text(s) from the sleep checks", (unsigned) n);
  }
  if (gRing.dropped()) {
    LOGW("Mesh: %u text(s) were lost while asleep (the ring was full and there was no SD card)", (unsigned) gRing.dropped());
    gRing.clearDropped();
  }
  gBgSleepMagic = 0;
}

void restoreCb(const MeshSpool::Rec &r, void *) {
  MeshCore::importText(r.from, r.to, r.rxTime, r.packetId, r.chan, r.flags, r.text, r.len);
}

// The Palm keeps the highest seq it has. Numbers must therefore rise from one boot to the next, or a restart (restored
// texts get small numbers again) hides messages from a Palm whose cursor is inside that range. NVS holds a mark no seq
// has reached: a boot starts at the mark and moves it SEQ_STEP up, and the main loop moves it again before a long
// awake period catches up with it. The first ever mark is 1000000, above anything an older firmware handed out.
const uint32_t SEQ_STEP = 64, SEQ_HEADROOM = 16;
uint32_t gSeqMark = 0;

void seqSaveMark() {
  gPrefs.begin("wmesh", false); gPrefs.putUInt("seq", gSeqMark); gPrefs.end();
}

// The mark must stay ahead of every seq handed out (a restore can take 200 at once).
void seqEnsure() {
  if (!gSeqMark || MeshCore::lastSeq() + SEQ_HEADROOM <= gSeqMark) return;
  while (MeshCore::lastSeq() + SEQ_HEADROOM > gSeqMark) gSeqMark += SEQ_STEP;
  seqSaveMark();
}

void seqBootOnce() {
  static bool done = false;
  if (done) return;
  done = true;
  gPrefs.begin("wmesh", true);
  uint32_t base = gPrefs.getUInt("seq", 1000000UL);
  gPrefs.end();
  gSeqMark = base + SEQ_STEP;
  seqSaveMark();
  MeshCore::setSeqBase(base);
}

// Once per boot, with an empty store: what the card holds goes back in (oldest first, so the seq order is the time order).
void spoolRestore() {
  if (gRestored) return;
  gRestored = true;
  size_t n = 0;
  if (!MeshCore::msgCount()) {
    CoreLock lk; n = MeshSpoolSd::loadNewest(restoreCb, nullptr);
  }
  gFlushedSeq = MeshCore::lastSeq();                          // all of it came from the card
  gConfirmedSeq = 0;
  if (n) { gSdHas = true; LOGI("Mesh: %u text(s) restored from the SD card", (unsigned) n); }
  { CoreLock lk; ringRestore(); }                             // then what the sleep checks found (not on the card yet)
  if (spoolPending()) spoolFlush();                           // a full wake: to the card
}

}  // namespace MeshRelay

#endif  // WIRSA_FEATURE_MESH
