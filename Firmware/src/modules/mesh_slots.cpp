// mesh_slots.cpp - Mesh Relay mode: the paired nodes (include/mesh_slots.h). One is active: the store, the SD folder and the link are its.
// Loop task only, except where noted.
#include "mesh_internal.h"

#if WIRSA_FEATURE_MESH

#include "log.h"
#include "mesh_spool_sd.h"

namespace MeshRelay {

using MeshSlots::Slot;

void syncPaired() {
  if (gActive >= MeshSlots::MAX) gActive = 0;
  const Slot &s = gSlots[gActive];
  gPaired.have = s.state == MeshSlots::PAIRED;
  memcpy(gPaired.addr, s.addr, 6);
  gPaired.type = s.type;
  gPaired.pin = 0;
  strlcpy(gPaired.name, s.name, sizeof gPaired.name);
  MeshSpoolSd::setNode(s.nodeId);
}

bool activateSlot(uint8_t s) {
  if (s >= MeshSlots::MAX) return false;
  if (s == gActive && gRestored) { syncPaired(); return true; }
  if (gRestored && !spoolFlush()) LOGW("Mesh: the texts of %s could not be written to the SD card - they are lost with the switch", gPaired.name);
  if (gWorkerOn && MeshBle::connected()) {
    Cmd c{}; c.op = W_DISCONNECT; post(c);
    for (int i = 0; i < 150 && MeshBle::connected(); i++) delay(20);
  }
  { CoreLock lk; MeshCore::resetSession(); MeshCore::clearMessages(); MeshCore::resetSeen(); }
  gActive = s;
  syncPaired();
  saveSlots();
  gFlushedSeq = gConfirmedSeq = MeshCore::lastSeq();
  gSdHas = false; gPendingSince = 0; gSpoolSaid = false; gSpoolRetryAt = 0;
  gRestored = false;
  spoolRestore();
  seqEnsure();
  setLink(linkIdle(), "");
  LOGI("Mesh: node %s is now the one shown (slot %u)", gPaired.name, (unsigned) s);
  return true;
}

// The node says who it is once its configuration has arrived; the slot keeps the id, and the SD folder is named by it.
void learnNodeId() {
  static unsigned long last = 0;
  if (millis() - last < 2000) return;
  last = millis();
  Slot &s = gSlots[gActive];
  uint32_t id = MeshCore::myNum();
  if (s.state != MeshSlots::PAIRED || !id || id == s.nodeId) return;
  if (MeshSlots::findId(gSlots, id, gActive) >= 0) { LOGW("Mesh: node %08x is paired in another slot", (unsigned) id); return; }
  s.nodeId = id;
  MeshSpoolSd::setNode(id);
  saveSlots();
  LOGI("Mesh: %s is node %08x", s.name, (unsigned) id);
}

bool unpairSlot(uint8_t n, bool keep) {
  if (n >= MeshSlots::MAX || !MeshSlots::used(gSlots[n])) return false;
  Slot &s = gSlots[n];
  bool active = n == gActive;
  if (s.state == MeshSlots::PAIRED) {                           // the bond goes either way; the active node's link ends with it
    Cmd c{}; c.op = W_FORGET; c.arg = active ? 0 : 1; memcpy(c.data, s.addr, 6); c.data[6] = s.type;
    post(c);
  }
  if (gBgSlot == n) { gBgSlot = MeshSlots::NONE; gRelayMode = 0; saveRelay(); LOGI("Mesh: background checks stopped with the node"); }
  if (keep) {
    s.state = MeshSlots::ARCHIVED;
    if (active) { syncPaired(); setLink(linkIdle(), ""); }
    saveSlots();
    LOGI("Mesh: unpaired %08x (slot %u), kept as an archive: its texts stay on the SD card", (unsigned) s.nodeId, (unsigned) n);
    return true;
  }
  uint32_t gone = s.nodeId;
  if (active) {                                                  // its texts go too: nothing is written for them
    { CoreLock lk; MeshCore::resetSession(); MeshCore::clearMessages(); MeshCore::resetSeen(); }
    gRestored = false;
  }
  bool sdOk = true;
  if (s.nodeId) sdOk = MeshSpoolSd::removeNode(s.nodeId);
  else if (active) sdOk = MeshSpoolSd::zero();
  LOGI("Mesh: unpaired %08x (slot %u), deleted%s", (unsigned) gone, (unsigned) n, sdOk ? ": its folder on the SD card is gone" : ", but the SD card folder could not be removed");
  s = Slot();
  uint8_t next = MeshSlots::nextActive(gSlots, gBgSlot);
  if (active) {
    if (next != MeshSlots::NONE) activateSlot(next);
    else { syncPaired(); saveSlots(); gFlushedSeq = gConfirmedSeq = MeshCore::lastSeq(); gSdHas = false; gRestored = true; setLink(linkIdle(), ""); }
  } else {
    saveSlots();
  }
  return true;
}

}  // namespace MeshRelay

#endif  // WIRSA_FEATURE_MESH
