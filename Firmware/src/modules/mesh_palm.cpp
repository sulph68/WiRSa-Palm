// mesh_palm.cpp - Mesh Relay mode: the frames the Palm sends, and what answers each (docs/protocol/Protocol-wire.md, section 9). Everything that writes to the Palm lives here
// or is called from here, on the loop task.
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
#include <mbedtls/base64.h>
#include <vector>
#include <algorithm>
#include <strings.h>
#include "mesh_spool_sd.h"

namespace MeshRelay {

// Tell the Palm something changed (a new message, a delivery, the link) - once per change, repeated
// every few seconds until the Palm has fetched it. The Palm never has to ask.
void pushNews() {
  if (!gPalmSession || millis() - gLastNews < RelayTiming::NEWS_REPEAT_MS) return;
  if (MeshCore::lastSeq() == gSeenSeq && MeshCore::version() == gSeenVer && gLink == gSeenLink && MeshCore::myBattery() == gSeenBatt) return;
  gLastNews = millis();
  Writer w(gOut, sizeof gOut);
  w.u8(gLink); w.u32(MeshCore::lastSeq());
  writeFrame(M_NEWS, gOut, w.len);
}

void markSeen() { gSeenSeq = MeshCore::lastSeq(); gSeenVer = MeshCore::version(); gSeenLink = gLink; gSeenBatt = MeshCore::myBattery(); }

// ---- Palm frames ------------------------------------------------------------------------------

void writeFrame(uint8_t type, const uint8_t *b, size_t n) { RelayProto::writeFrame(PhysicalSerial, type, b, n); }

void sendErr(uint8_t code, const char *text) {
  Writer w(gOut, sizeof gOut);
  w.u8(code); w.str(String(text));
  writeFrame(M_ERR, gOut, w.len);
}

// A frame of the Palm's own (a text it was just told to send) that arrives while a fetch waits for the Palm's acknowledgements: kept here, one
// at a time, and handled when the fetch is over (runStashed). Before, it was read as a missing ack and lost.
static struct { bool have = false; uint8_t type = 0; size_t len = 0; uint8_t body[BODY_MAX]; } gStash;

static bool stashFrame(uint8_t type, const uint8_t *body, size_t len) {
  if (gStash.have || len > sizeof gStash.body) {
    LOGW("Mesh: a Palm frame (type 0x%02X) arrived during a fetch and could not be kept", (unsigned) type);
    return true;                                  // consumed (lost): the wait for the ack goes on
  }
  gStash.type = type; gStash.len = len; memcpy(gStash.body, body, len);
  gStash.have = true;
  return true;
}

void runStashed() {
  if (!gStash.have) return;
  gStash.have = false;
  LOGI("Mesh: handling a Palm frame (type 0x%02X) that arrived during a fetch", (unsigned) gStash.type);
  dispatch(gStash.type, gStash.body, gStash.len);
}

// One streamed frame, acked by the Palm (5 tries, like the other relays).
bool sendAcked(uint8_t type, size_t n) {
  return RelayProto::writeFrameAcked(PhysicalSerial, type, gOut, n, M_ACK, M_BYE, gAckBody, sizeof gAckBody,
                                     RelayTiming::MESH_ACK_ATTEMPTS, RelayTiming::ACK_RETRY_DELAY_MS, RelayTiming::MESH_ACK_TIMEOUT_MS, RelayTiming::MESH_BYTE_TIMEOUT_MS, nullptr, stashFrame);
}

void handleHello(const uint8_t *body, size_t len) {
  if (!RelayMode::handleHello(meshSpec(), body, len)) return;   // agrees the speed and starts the session (gPalmSession, the Exit Timer)
  markSeen();
}

void putState(Writer &w) {
  CoreLock lk;
  int bat = MeshCore::myBattery();
  w.u8(gLink);
  w.u8((uint8_t) (int8_t) bat);
  w.u32(MeshCore::myNum());
  w.str(PalmText::fromUtf8(MeshCore::firmware()));
  w.str(PalmText::fromUtf8(gPaired.name));
  w.u16((uint16_t) MeshCore::nodeCount());
  w.u16((uint16_t) MeshCore::msgCount());
  w.u32(MeshCore::lastSeq());
  w.u8(modeOfActive());
  w.u8(activeEvery());
  w.u32(utcNow());
  w.u32(gLastPollUtc);
  w.str(String(gLinkText));
  w.u8(gActive);
}

void handleState() {
  Writer w(gOut, sizeof gOut);
  putState(w);
  writeFrame(M_STATE_R, gOut, w.len);
}

void handleScan(const uint8_t *body, size_t len) {
  Reader r(body, len);
  uint8_t secs = r.u8();
  if (!r.ok || secs < 1) secs = 5;
  if (secs > 15) secs = 15;
  if (gBusyOp) { sendErr(E_BUSY, "WiRSa is busy with the node"); return; }
  gBusyOp = 1;
  Cmd c{}; c.op = W_SCAN; c.arg = secs;
  post(c);
}

// Held while a pairing is under way: if it fails, the slot and the node shown go back.
static MeshSlots::Slot gPairBackup;
static uint8_t gPairSlot = MeshSlots::NONE, gPairPrev = 0;

void handlePair(const uint8_t *body, size_t len) {
  Reader r(body, len);
  uint8_t addr[6]; r.bytesRaw(addr, 6);
  uint8_t type = r.u8();
  uint32_t pin = r.u32();
  String name = r.str();
  if (!r.ok) { sendErr(E_BAD, "Bad pair request"); return; }
  if (gBusyOp) { sendErr(E_BUSY, "WiRSa is busy with the node"); return; }
  int s = MeshSlots::findAddr(gSlots, addr);                     // a node known before (paired, or archived with its texts) keeps its slot
  if (s < 0) s = MeshSlots::freeSlot(gSlots);
  if (s < 0) { sendErr(E_NOROOM, "Four nodes are kept - remove one first"); return; }
  gPairSlot = (uint8_t) s; gPairPrev = gActive; gPairBackup = gSlots[s];
  MeshSlots::Slot &sl = gSlots[s];
  if (!MeshSlots::used(sl)) { sl = MeshSlots::Slot(); sl.every = 2; sl.flush = 5; }       // a new node: Session mode; checks every 5 min if Background is set later; flush when full
  memcpy(sl.addr, addr, 6); sl.type = type; sl.state = MeshSlots::PAIRED;
  strlcpy(sl.name, name.c_str(), sizeof sl.name);
  if (s != gActive) activateSlot((uint8_t) s); else syncPaired();
  gPaired.pin = pin;
  Cmd c{}; c.op = W_PAIR;
  gBusyOp = 2;
  post(c);
}

// keep 1: the node stays archived with its texts; 0: everything of it goes. An empty body is the form of version 2: the node shown, all of it.
void handleUnpair(const uint8_t *body, size_t len) {
  Reader r(body, len);
  uint8_t s = len >= 1 ? r.u8() : gActive;
  uint8_t keep = len >= 2 ? r.u8() : 0;
  if (!r.ok || !unpairSlot(s, keep != 0)) { sendErr(E_BAD, "No such node"); return; }
  Writer w(gOut, sizeof gOut); w.u16(0);
  writeFrame(M_DONE, gOut, w.len);
}

void handleSlotsGet() {
  Writer w(gOut, sizeof gOut);
  uint8_t n = 0;
  for (int i = 0; i < MeshSlots::MAX; i++) if (MeshSlots::used(gSlots[i])) n++;
  w.u8(n);
  for (int i = 0; i < MeshSlots::MAX; i++) {
    const MeshSlots::Slot &s = gSlots[i];
    if (!MeshSlots::used(s)) continue;
    w.u8((uint8_t) i); w.u8(s.state); w.u32(s.nodeId);
    w.u8((uint8_t) ((i == gActive ? 1 : 0) | (gRelayMode == 1 && i == gBgSlot ? 2 : 0)));
    w.str(PalmText::fromUtf8(s.name));
  }
  writeFrame(M_SLOTS_R, gOut, w.len);
}

// The node shown changes: its texts are saved, the store and the link are the new node's.
void handleSlotSelect(const uint8_t *body, size_t len) {
  Reader r(body, len);
  uint8_t s = r.u8();
  if (!r.ok || s >= MeshSlots::MAX || !MeshSlots::used(gSlots[s])) { sendErr(E_BAD, "No such node"); return; }
  if (gBusyOp) { sendErr(E_BUSY, "WiRSa is busy with the node"); return; }
  if (s != gActive) activateSlot(s);
  Writer w(gOut, sizeof gOut); w.u16(0);
  writeFrame(M_DONE, gOut, w.len);
}

// One node in full (the caller holds the lock): index 0 is this node. False if there is none.
bool buildNodeFull(Writer &w, int i) {
  const MeshPb::NodeInfo *n = MeshCore::node(i);
  if (!n) return false;
  w.u32(n->num);
  w.str(PalmText::fromUtf8(n->id)); w.str(latinName(n->longName)); w.str(latinName(n->shortName));
  w.u8((uint8_t) n->hw); w.u8((uint8_t) n->role);
  w.u8((uint8_t) (n->hasSnr ? (int8_t) n->snr : 0)); w.u8(n->hasSnr ? 1 : 0);
  w.u32(n->lastHeard);
  w.u8((uint8_t) (int8_t) n->hops);
  w.u8((uint8_t) (int8_t) n->battery);
  w.u32(n->uptime);
  w.u16(n->chanUtil10); w.u16(n->airUtil10);
  w.u8(n->hasPos ? 1 : 0); w.u32((uint32_t) n->latE7); w.u32((uint32_t) n->lonE7);
  w.u8(i == 0 ? 1 : 0);
  w.u8(n->fav ? 1 : 0);
  w.u8(n->hasKey ? 1 : 0);
  return true;
}

// The Palm opened a node's card: that node in full, as a NODE frame (nothing to acknowledge).
void handleNodeGet(const uint8_t *body, size_t len) {
  Reader r(body, len);
  uint32_t num = r.u32();
  if (!r.ok) { sendErr(E_BAD, "Bad request"); return; }
  Writer w(gOut, sizeof gOut);
  bool ok;
  { CoreLock lk; int i = MeshCore::findNode(num); ok = i >= 0 && buildNodeFull(w, i); }
  if (!ok) { sendErr(E_BAD, "Unknown node"); return; }
  writeFrame(M_NODE, gOut, w.len);
}

// Stream the node list, the channels and the messages newer than the Palm's cursor, then the state of
// the unfinished sends the Palm lists. Each item waits for the Palm's ack.
void handleSync(const uint8_t *body, size_t len) {
  Reader r(body, len);
  uint32_t cursor = r.u32();
  uint8_t nPend = r.u8();
  uint32_t pend[24];
  if (nPend > 24) nPend = 24;
  for (uint8_t i = 0; i < nPend; i++) pend[i] = r.u32();
  uint8_t what = r.u8();                                     // bit 0: the node list, bit 1: the channels
  if (!r.ok) { sendErr(E_BAD, "Bad sync request"); return; }
  markSeen();

  // The worker keeps the stores up to date meanwhile. The node list goes in two parts: this node in full, then all the others in
  // short, sorted (favourites, then by name), several to a frame: a mesh can have hundreds, and the Palm keeps only the short form.
  if (what & 1) {
    {
      Writer w(gOut, sizeof gOut);
      bool have;
      { CoreLock lk; have = buildNodeFull(w, 0); }
      if (have && !sendAcked(M_NODE, w.len)) return;
    }
    struct Short { uint32_t num, heard; char lng[24], sht[6]; uint8_t flags; };
    std::vector<Short> list;
    {
      CoreLock lk;
      int n = MeshCore::nodeCount();
      list.reserve(n > 0 ? n : 0);
      for (int i = 1; i < n; i++) {
        const MeshPb::NodeInfo *ni = MeshCore::node(i);
        if (!ni) break;
        Short s{};
        s.num = ni->num; s.heard = ni->lastHeard;
        String l = latinName(ni->longName), h = latinName(ni->shortName);
        strlcpy(s.lng, l.c_str(), sizeof s.lng); strlcpy(s.sht, h.c_str(), sizeof s.sht);
        s.flags = (ni->fav ? 1 : 0) | (ni->hasKey ? 2 : 0);
        list.push_back(s);
      }
    }
    std::sort(list.begin(), list.end(), [](const Short &a, const Short &b) {
      if ((a.flags & 1) != (b.flags & 1)) return (a.flags & 1) > (b.flags & 1);
      return strcasecmp(a.lng, b.lng) < 0;
    });
    for (size_t at = 0; at < list.size();) {
      Writer w(gOut, sizeof gOut);
      size_t cnt = list.size() - at < 6 ? list.size() - at : 6;
      w.u8((uint8_t) cnt);
      for (size_t k = 0; k < cnt; k++) {
        const Short &s = list[at + k];
        w.u32(s.num); w.str(String(s.lng)); w.str(String(s.sht)); w.u32(s.heard); w.u8(s.flags);
      }
      if (!sendAcked(M_NODES, w.len)) return;
      at += cnt;
    }
  }
  for (int s = 0; (what & 2) && s < MeshCore::MAX_CHANS; s++) {
    Writer w(gOut, sizeof gOut);
    {
      CoreLock lk;
      const MeshPb::Channel *c = MeshCore::channel(s);
      if (!c || c->role == 0) continue;                      // slot unused
      w.u8(c->index); w.u8(c->role); w.str(latinName(c->name));
    }
    if (!sendAcked(M_CHAN, w.len)) return;
  }
  uint16_t sent = 0;
  uint32_t after = cursor;                                   // the newest seq sent so far
  for (;;) {
    Writer w(gOut, sizeof gOut);
    {
      CoreLock lk;
      int i = MeshCore::firstAfter(after);
      if (i < 0) break;
      const MeshCore::Msg *m = MeshCore::msgAt(i);
      if (m->seq <= after && after != 0) break;              // nothing newer
      after = m->seq;
      w.u32(m->seq); w.u8(m->chan); w.u8(m->flags); w.u8(m->status);
      w.u32(m->from); w.u32(m->to); w.u32(m->rxTime); w.u32((m->flags & MeshCore::F_OUT) ? m->localId : m->packetId);   // a received text carries its packet id, so the Palm can tell a resend from a second, identical text
      w.str(PalmText::fromUtf8(m->text));
      w.u8(m->err);
    }
    if (!sendAcked(M_MSG, w.len)) return;
    sent++;
  }
  for (uint8_t i = 0; i < nPend; i++) {
    Writer w(gOut, sizeof gOut);
    {
      CoreLock lk;
      const MeshCore::Msg *m = MeshCore::findByLocalId(pend[i]);
      if (!m) continue;
      w.u32(pend[i]); w.u8(m->status); w.u8(m->err);
    }
    if (!sendAcked(M_DELIV, w.len)) return;
  }
  Writer w(gOut, sizeof gOut);
  { CoreLock lk; w.u32(MeshCore::lastSeq()); w.u16(sent); w.u16((uint16_t) MeshCore::nodeCount()); }
  writeFrame(M_SYNC_DONE, gOut, w.len);
}

void handleSend(const uint8_t *body, size_t len) {
  Reader r(body, len);
  uint32_t to = r.u32();
  uint8_t chan = r.u8();
  uint32_t localId = r.u32();
  String text = r.str();
  if (!r.ok) { sendErr(E_BAD, "Bad send request"); return; }
  auto fail = [&](const char *why, uint8_t code) {
    Writer w(gOut, sizeof gOut);
    w.u32(localId); w.u8(0); w.u32(0); w.str(String(why)); w.u8(code);
    writeFrame(M_SEND_R, gOut, w.len);
    LOGW("Mesh: send refused: %s", why);
  };
  LOGI("Mesh: send to %08lx channel %u, %u chars, link %u", (unsigned long) to, (unsigned) chan, (unsigned) text.length(), (unsigned) gLink);
  if (gLink != L_READY) { fail("Not linked to the node", 241); return; }
  char utf[MeshCore::TEXT_MAX + 4];
  size_t n = PalmText::toUtf8(text.c_str(), text.length(), utf, sizeof utf - 1);
  Cmd c{}; c.op = W_SEND; c.lid = localId;
  size_t trLen = 0;
  uint32_t id;
  { CoreLock lk; id = MeshCore::queueText(to, chan, utf, n, localId, utcNow(), c.data, sizeof c.data, trLen); }
  if (!id) { fail(n > (size_t) MeshCore::TEXT_MAX ? "Too long: 233 bytes at most" : "No room for the message", 243); return; }
  c.id = id; c.len = (uint16_t) trLen;
  post(c);                                  // the worker writes it to the node; the answer follows (serviceWorker)
}

// A request or a share other than text: node info, position, telemetry, traceroute, a position, a waypoint.
// Same answer frames as a text (SEND_R now, DELIV and MSG later), the other node's reply arrives as a message.
void handleReq(const uint8_t *body, size_t len) {
  Reader r(body, len);
  uint32_t to = r.u32();
  uint8_t chan = r.u8();
  uint32_t localId = r.u32();
  uint8_t kind = r.u8();
  int32_t lat = (int32_t) r.u32(), lon = (int32_t) r.u32();
  String name = r.str();
  if (!r.ok) { sendErr(E_BAD, "Bad request"); return; }
  auto fail = [&](const char *why, uint8_t code) {
    Writer w(gOut, sizeof gOut);
    w.u32(localId); w.u8(0); w.u32(0); w.str(String(why)); w.u8(code);
    writeFrame(M_SEND_R, gOut, w.len);
    LOGW("Mesh: request refused: %s", why);
  };
  LOGI("Mesh: request %u to %08lx channel %u, link %u", (unsigned) kind, (unsigned long) to, (unsigned) chan, (unsigned) gLink);
  if (gLink != L_READY) { fail("Not linked to the node", 241); return; }
  char nm[32];
  size_t nn = PalmText::toUtf8(name.c_str(), name.length() > 29 ? 29 : name.length(), nm, sizeof nm - 1);
  nm[nn] = 0;
  Cmd c{}; c.op = W_SEND; c.lid = localId;
  size_t trLen = 0;
  uint32_t id;
  if (kind == MeshCore::REQ_INFO && localId == 0 && to != MeshPb::BROADCAST) {   // the Palm opened a chat with a node whose key is not known: ask quietly, nothing is stored
    { CoreLock lk; id = MeshCore::keyRequest(to, chan, c.data, sizeof c.data, trLen); }
    if (!id) return;
    c.id = id; c.len = (uint16_t) trLen;
    post(c);
    return;
  }
  { CoreLock lk; id = MeshCore::queueRequest(kind, to, chan, lat, lon, nm, localId, utcNow(), c.data, sizeof c.data, trLen); }
  if (!id) { fail("That cannot be sent here", 243); return; }
  c.id = id; c.len = (uint16_t) trLen;
  post(c);
}

void handleCfgGet();

// What the worker finished since the last look: hand it to the Palm. Only this task writes to the Palm.
void serviceWorker() {
  uint8_t l = 0; char t[40]; bool have = false;
  { CoreLock lk; if (gLinkPending) { l = gLinkNew; snprintf(t, sizeof t, "%s", gLinkNewText); gLinkPending = false; have = true; } }
  if (have) setLink(l, t);

  if (gPinNeed && !gPinSent) { writeFrame(M_PIN_NEED, nullptr, 0); gPinSent = true; }
  if (!gPinNeed) gPinSent = false;

  if (gScan.done) {
    gScan.done = false;
    Writer w(gOut, sizeof gOut);
    w.u8((uint8_t) gScan.n);
    for (int i = 0; i < gScan.n; i++) {
      w.bytesRaw(gScan.hits[i].addr, 6);
      w.u8(gScan.hits[i].type);
      w.u8((uint8_t) gScan.hits[i].rssi);
      w.str(PalmText::fromUtf8(gScan.hits[i].name));
    }
    writeFrame(M_SCAN_R, gOut, w.len);
    gBusyOp = 0;
  }
  if (gPairRes.done) {
    gPairRes.done = false;
    if (gPairRes.ok) { saveSlots(); learnNodeId(); }
    else if (gPairSlot < MeshSlots::MAX) {                        // back to how it was
      gSlots[gPairSlot] = gPairBackup;
      if (gPairPrev != gActive) activateSlot(gPairPrev); else syncPaired();
      saveSlots();
    }
    Writer w(gOut, sizeof gOut);
    w.u8(gPairRes.ok ? 1 : 0);
    w.str(String(gPairRes.text));
    w.u8(gActive); w.u32(gSlots[gActive].nodeId);
    writeFrame(M_PAIR_R, gOut, w.len);
    gPairSlot = MeshSlots::NONE;
    gPaired.pin = 0;
    gBusyOp = 0;
  }
  if (gCfgReadRes.done) {
    gCfgReadRes.done = false;
    if (gCfgReadRes.ok) handleCfgGet(); else sendErr(E_NOLINK, gCfgReadRes.text);
    gBusyOp = 0;
  }
  if (gCfgWriteRes.done) {
    gCfgWriteRes.done = false;
    if (gCfgWriteRes.ok) { Writer w(gOut, sizeof gOut); w.u16(0); writeFrame(M_DONE, gOut, w.len); }
    else sendErr(E_NOLINK, gCfgWriteRes.text);
    gBusyOp = 0;
  }
  if (gConnRes.done) {
    gConnRes.done = false;
    handleState();
    gBusyOp = 0;
  }
  while (gSendTail != gSendHead) {
    SendRes &s = gSendRes[gSendTail % 6];
    Writer w(gOut, sizeof gOut);
    w.u32(s.localId); w.u8(s.ok ? 1 : 0); w.u32(s.id); w.str(String(s.text)); w.u8(s.code);
    writeFrame(M_SEND_R, gOut, w.len);
    gSendTail = gSendTail + 1;
  }
}

void pumpNode() {
  serviceWorker();
  learnNodeId();
  pushNews();
}

// The node's settings as the Palm shows them, from what the node sent when the link came up.
void handleCfgGet() {
  Writer w(gOut, sizeof gOut);
  CoreLock lk;
  const uint8_t *rl, *rd, *rb; size_t ll = 0, dl = 0, bl = 0;
  const MeshPb::NodeInfo *n0 = MeshCore::node(0);
  if (gLink != L_READY || !n0 || !n0->userRawLen || !MeshCore::config(MeshPb::CFG_LORA, rl, ll) ||
      !MeshCore::config(MeshPb::CFG_DEVICE, rd, dl) || !MeshCore::config(MeshPb::CFG_BLUETOOTH, rb, bl)) {
    w.u8(0); w.str(String(gLink == L_READY ? "The node's settings have not arrived yet" : "Not linked to the node"));
    writeFrame(M_CFG_R, gOut, w.len);
    return;
  }
  char ln[40] = "", sn[8] = "";
  MeshPb::readOwner(n0->userRaw, n0->userRawLen, ln, sizeof ln, sn, sizeof sn);
  MeshPb::LoraVals lv; MeshPb::readLora(rl, ll, lv);
  MeshPb::BtVals bv; MeshPb::readBluetooth(rb, bl, bv);
  uint32_t role = MeshPb::readDeviceRole(rd, dl);
  w.u8(1);
  w.str(PalmText::fromUtf8(ln)); w.str(PalmText::fromUtf8(sn));
  w.u8(indexOf(REGION_CODE, 14, lv.region)); w.u8(indexOf(PRESET_CODE, 8, lv.preset));
  w.u8((uint8_t) (lv.hop < 1 ? 1 : lv.hop > 7 ? 7 : lv.hop));
  w.u8((uint8_t) (lv.txPower < 0 ? 0 : lv.txPower > 30 ? 30 : lv.txPower));
  w.u8(indexOf(ROLE_CODE, 9, role));
  w.u8((uint8_t) bv.mode); w.u32(bv.fixedPin);
  uint8_t n = 0; size_t nPos = w.len;
  w.u8(0);
  for (int i = 0; i < MeshCore::MAX_CHANS; i++) {
    const MeshPb::Channel *c = MeshCore::channel(i);
    if (!c) continue;
    char key[48] = ""; size_t kl = 0;
    if (c->pskLen) mbedtls_base64_encode((unsigned char *) key, sizeof key - 1, &kl, c->psk, c->pskLen);
    key[kl] = 0;
    w.u8(c->index); w.u8(c->role); w.str(PalmText::fromUtf8(c->name)); w.str(String(key));
    n++;
  }
  gOut[nPos] = n;
  w.u32(MeshCore::nodeClock());
  {                                                            // the MQTT module (the Palms before it stopped reading above)
    const uint8_t *mraw; size_t mlen;
    if (MeshCore::mqttConfig(mraw, mlen)) {
      MeshPb::MqttVals mv;
      MeshPb::readMqtt(mraw, mlen, mv);
      w.u8(1);
      w.u8((uint8_t) ((mv.enabled ? 1 : 0) | (mv.encrypt ? 2 : 0) | (mv.json ? 4 : 0) | (mv.tls ? 8 : 0) | (mv.proxy ? 16 : 0) | (mv.mapRep ? 32 : 0)));
      w.str(PalmText::fromUtf8(mv.address)); w.str(PalmText::fromUtf8(mv.username)); w.str(PalmText::fromUtf8(mv.password)); w.str(PalmText::fromUtf8(mv.root));
    } else {
      w.u8(0);
    }
  }
  {                                                            // then, per channel, its MQTT up- and downlink (1 uplink, 2 downlink)
    size_t cPos = w.len;
    w.u8(0);
    uint8_t cn = 0;
    for (int i = 0; i < MeshCore::MAX_CHANS; i++) {
      const MeshPb::Channel *c = MeshCore::channel(i);
      if (!c) continue;
      w.u8(c->index); w.u8((uint8_t) ((c->uplink ? 1 : 0) | (c->downlink ? 2 : 0)));
      cn++;
    }
    gOut[cPos] = cn;
  }
  writeFrame(M_CFG_R, gOut, w.len);
}

// The Palm asked for the settings: read them from the node afresh first (the worker does), then answer.
void handleCfgGetReq() {
  if (gBusyOp) { sendErr(E_BUSY, "WiRSa is busy with the node"); return; }
  if (gLink != L_READY) { handleCfgGet(); return; }               // says why it cannot
  gBusyOp = 5;
  Cmd c{}; c.op = W_CFG_READ;
  post(c);
}

void handleCfgSet(const uint8_t *body, size_t len) {
  if (gBusyOp) { sendErr(E_BUSY, "WiRSa is busy with the node"); return; }
  if (len > sizeof(Cmd::data)) { sendErr(E_BAD, "The settings are too large"); return; }
  gBusyOp = 4;
  Cmd c{}; c.op = W_CFG_SET; c.len = (uint16_t) len; memcpy(c.data, body, len);
  post(c);
}

void handleCfgAct(const uint8_t *body, size_t len) {
  if (gBusyOp) { sendErr(E_BUSY, "WiRSa is busy with the node"); return; }
  Reader r(body, len);
  uint8_t action = r.u8(); uint32_t utc = r.u32();
  if (!r.ok) { sendErr(E_BAD, "Bad request"); return; }
  gBusyOp = 4;
  Cmd c{}; c.op = W_ACT; c.arg = action; c.len = 4;
  c.data[0] = (uint8_t) (utc >> 24); c.data[1] = (uint8_t) (utc >> 16); c.data[2] = (uint8_t) (utc >> 8); c.data[3] = (uint8_t) utc;
  post(c);
}

void handleConnect(const uint8_t *body, size_t len) {
  if (gBusyOp) { sendErr(E_BUSY, "WiRSa is busy with the node"); return; }
  gBusyOp = 3;
  Cmd c{}; c.op = W_CONNECT; c.arg = (len >= 1 && (body[0] == 1 || body[0] == 2)) ? body[0] : 0;
  post(c);
}

void handleRelayGet() {
  Writer w(gOut, sizeof gOut);
  w.u8(modeOfActive()); w.u8(activeEvery()); w.u8(gLink);
  w.str(PalmText::fromUtf8(gPaired.name));
  w.u16(0);                                                  // waiting on the WiRSa side: nothing is held back
  w.u16((uint16_t) MeshCore::msgCount());
  w.u32(gLastPollUtc);
  w.u8((uint8_t) (int8_t) MeshCore::myBattery());
  w.u8(activeFlush());
  writeFrame(M_RELAY_R, gOut, w.len);
}

void handleRelaySet(const uint8_t *body, size_t len) {
  Reader r(body, len);
  uint8_t mode = r.u8(), every = r.u8();
  uint8_t flush = len > 2 ? r.u8() : activeFlush();                // an older Palm sends no third byte: the setting stays
  if (!r.ok || mode > 1 || every > 4 || flush > 4) { sendErr(E_BAD, "Bad relay setting"); return; }
  if (mode == 1 && !gPaired.have) { sendErr(E_BAD, "Pair this node first"); return; }
  if (mode == 1) { gRelayMode = 1; gBgSlot = gActive; }          // one node has the checks: this one
  else if (gBgSlot == gActive) { gRelayMode = 0; gBgSlot = MeshSlots::NONE; }
  gSlots[gActive].every = (uint8_t) (every + 1); gSlots[gActive].flush = (uint8_t) (flush + 1);   // the settings of the node shown
  if (gRelayMode == 1 && gBgSlot == gActive) { gRelayEvery = every; gSdFlush = flush; }                // Background mode runs on the background node's
  saveRelay();
  saveSlots();
  Writer w(gOut, sizeof gOut); w.u16(0);
  writeFrame(M_DONE, gOut, w.len);
}

void handleClear(const uint8_t *body, size_t len) {
  Reader r(body, len);
  uint32_t before = r.u32();
  if (!r.ok) { sendErr(E_BAD, "Bad clear request"); return; }
  uint32_t n;
  CoreLock lk;
  if (before == 0) {
    n = (uint32_t) MeshCore::msgCount(); MeshCore::clearMessages();
    if (MeshSpoolSd::zero()) gSdHas = false;               // WiRSa's own store means the card's copy too
    gFlushedSeq = gConfirmedSeq = MeshCore::lastSeq();
  }
  else n = MeshCore::clearOlderThan(before);
  drawStatus();
  Writer w(gOut, sizeof gOut); w.u16((uint16_t) n);
  writeFrame(M_DONE, gOut, w.len);
}

// Frames the Palm sends, and what answers each (RelayMode::dispatch logs the name at verbose level and warns about a type not in the table).
void onPin(const uint8_t *body, size_t len) {
  if (len >= 4) { gPinValue = ((uint32_t) body[0] << 24) | ((uint32_t) body[1] << 16) | ((uint32_t) body[2] << 8) | body[3]; gPinGiven = true; }
}

void onHold(const uint8_t *body, size_t len) {                // a dialog is open on the Palm: it cannot answer a probe for that long
  if (len >= 1) gHoldUntil = millis() + (unsigned long) body[0] * 1000UL;
}

void onBye(const uint8_t *, size_t) {
  gPalmSession = false;
  gExitAfterBye = true;                                       // the mode ends with the app; in Background mode the checks go on at the main menu
  if (PhysicalSerial.baudRate() != RelayTiming::BAUD_IDLE) {
    gNegotiatedAt = 0;
    PhysicalSerial.begin(RelayTiming::BAUD_IDLE, WIRSA_SERIAL_FORMAT, RXD2, TXD2);
    drawHeader();
  }
}

const RelayMode::Frame kFrames[] = {
  { M_HELLO,     "HELLO",     handleHello },
  { M_STATE,     "STATE",     [](const uint8_t *, size_t) { handleState(); } },
  { M_SCAN,      "SCAN",      handleScan },
  { M_PAIR,      "PAIR",      handlePair },
  { M_UNPAIR,    "UNPAIR",    handleUnpair },
  { M_SLOTS_GET, "SLOTS_GET", [](const uint8_t *, size_t) { handleSlotsGet(); } },
  { M_SLOT_SELECT, "SLOT_SELECT", handleSlotSelect },
  { M_SYNC,      "SYNC",      handleSync },
  { M_SEND,      "SEND",      handleSend },
  { M_REQ,       "REQ",       handleReq },
  { M_RELAY_GET, "RELAY_GET", [](const uint8_t *, size_t) { handleRelayGet(); } },
  { M_RELAY_SET, "RELAY_SET", handleRelaySet },
  { M_CLEAR,     "CLEAR",     handleClear },
  { M_HIST,      "HIST",      handleHist },
  { M_CONNECT,   "CONNECT",   handleConnect },
  { M_CFG_GET,   "CFG_GET",   [](const uint8_t *, size_t) { handleCfgGetReq(); } },
  { M_CFG_SET,   "CFG_SET",   handleCfgSet },
  { M_CFG_ACT,   "CFG_ACT",   handleCfgAct },
  { M_PIN,       "PIN",       onPin },
  { M_PING,      "PING",      [](const uint8_t *, size_t) {} },   // the Palm answering a probe (or its own keep-alive): the line is alive
  { M_NODE_GET,  "NODE_GET",  handleNodeGet },
  { M_HOLD,      "HOLD",      onHold },
  { M_BYE,       "BYE",       onBye },
};

void dispatch(uint8_t type, const uint8_t *body, size_t len) {
  RelayMode::dispatch(meshSpec(), kFrames, sizeof kFrames / sizeof kFrames[0], type, body, len);
}

void handleHist(const uint8_t *body, size_t len) {
  Reader r(body, len);
  uint8_t keep = r.u8();
  uint32_t delivered = r.u32();
  if (!r.ok) { sendErr(E_BAD, "Bad history request"); return; }
  if ((keep != 0) != gHistKeep) {
    gHistKeep = keep != 0;
    gPrefs.begin("wmesh", false); gPrefs.putUChar("hist", gHistKeep ? 1 : 0); gPrefs.end();
  }
  if (delivered > gConfirmedSeq) gConfirmedSeq = delivered;
  uint8_t zeroed = 0;
  if (!gHistKeep && gSdHas && gConfirmedSeq >= gFlushedSeq && MeshSpoolSd::zero()) {
    gSdHas = false;
    gFlushedSeq = gConfirmedSeq;
    zeroed = 1;
    LOGI("Mesh: the Palm has everything - SD card emptied");
  }
  Writer w(gOut, sizeof gOut);
  w.u8(zeroed);
  writeFrame(M_DONE, gOut, w.len);
}

}  // namespace MeshRelay

#endif  // WIRSA_FEATURE_MESH
