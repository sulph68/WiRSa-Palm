// mesh_core.cpp - see mesh_core.h.
#include "mesh_core.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#ifdef ARDUINO
#include <Arduino.h>
#include <esp32-hal-psram.h>
#else
static unsigned long millis() { return 0; }      // host tests
#endif

namespace MeshCore {
namespace {

using MeshPb::NodeInfo;

NodeInfo *gNodes = nullptr;
int gMaxNodes = 0;
int gNNodes = 0;
MeshPb::Channel gChans[MAX_CHANS];
bool gHaveChan[MAX_CHANS];
bool gConfigDone = false;
uint32_t gMyNum = 0, gNodeUpdates = 0;
char gFw[16] = "";
Msg *gMsgs = nullptr;                 // ring: oldest at gHead
int gHead = 0, gCount = 0, gCap = 0;
uint32_t gSeq = 0, gVer = 0;   // gVer moves when a stored message changes state
uint32_t gIdState = 0x2545F491u;
struct Seen { uint32_t from, id; } gSeen[64];
int gSeenPos = 0;

void *bigAlloc(size_t n) {
#ifdef ARDUINO
  void *p = ps_malloc(n);
  if (p) return p;
#endif
  return malloc(n);
}

uint32_t nextId() {
  for (;;) {                                           // xorshift32; never 0
    gIdState ^= gIdState << 13; gIdState ^= gIdState >> 17; gIdState ^= gIdState << 5;
    if (gIdState) return gIdState;
  }
}

bool seenBefore(uint32_t from, uint32_t id) {
  if (!id) return false;
  for (int i = 0; i < 64; i++) if (gSeen[i].from == from && gSeen[i].id == id) return true;
  gSeen[gSeenPos].from = from; gSeen[gSeenPos].id = id; gSeenPos = (gSeenPos + 1) % 64;
  return false;
}

Msg &push() {                                           // oldest drops when full
  Msg *m;
  if (gCount < gCap) { m = &gMsgs[(gHead + gCount) % gCap]; gCount++; }
  else { m = &gMsgs[gHead]; gHead = (gHead + 1) % gCap; }
  *m = Msg();
  m->seq = ++gSeq;
  return *m;
}

Msg *at(int i) { return &gMsgs[(gHead + i) % gCap]; }

uint32_t gOwnHeard = 0; unsigned long gOwnHeardAt = 0;      // the node's own clock, from its own node record
struct CfgSlot { uint8_t raw[96]; uint8_t len = 0; bool have = false; } gCfgSlot[3];     // device, lora, bluetooth
uint8_t gMqttRaw[272]; uint16_t gMqttLen = 0; bool gMqttHave = false;                  // the MQTT module
int cfgSlot(uint8_t which) { return which == MeshPb::CFG_DEVICE ? 0 : which == MeshPb::CFG_LORA ? 1 : which == MeshPb::CFG_BLUETOOTH ? 2 : -1; }

void upsertNode(const NodeInfo &in) {
  int i = findNode(in.num);
  if (i < 0) {
    if (gNNodes >= gMaxNodes) return;
    i = gNNodes++;
    gNodes[i] = in;
  } else {
    NodeInfo &d = gNodes[i];
    NodeInfo keep = d;
    d = in;
    if (!d.longName[0]) strcpy(d.longName, keep.longName);       // a position-only update keeps the name
    if (!d.shortName[0]) strcpy(d.shortName, keep.shortName);
    if (!d.id[0]) strcpy(d.id, keep.id);
    if (d.battery < 0) d.battery = keep.battery;
    if (!d.hasKey) d.hasKey = keep.hasKey;                       // an update without the key does not take it away
    if (!d.hasPos && keep.hasPos) { d.hasPos = true; d.latE7 = keep.latE7; d.lonE7 = keep.lonE7; }
    if (!d.uptime) d.uptime = keep.uptime;
    if (d.hops < 0) d.hops = keep.hops;
    if (!in.favKnown) d.fav = keep.fav;
    if (!d.hw) d.hw = keep.hw;
    if (!d.role) d.role = keep.role;
    if (!d.userRawLen && keep.userRawLen) { memcpy(d.userRaw, keep.userRaw, keep.userRawLen); d.userRawLen = keep.userRawLen; }
  }
  gNodeUpdates++;
  // This node always first, so the Palm can show it as "this node".
  if (in.num == gMyNum && gMyNum) {
    int me = findNode(gMyNum);
    if (me > 0) { NodeInfo t = gNodes[0]; gNodes[0] = gNodes[me]; gNodes[me] = t; }
  }
}


// "!1a2b3c4d" or the short name, for a route.
void nodeLabel(uint32_t num, char *out, size_t cap) {
  int i = findNode(num);
  if (i >= 0 && gNodes[i].shortName[0]) snprintf(out, cap, "%s", gNodes[i].shortName);
  else snprintf(out, cap, "!%08lx", (unsigned long) num);
}

void routeText(const uint32_t *hop, int n, char *out, size_t cap, size_t &o) {
  char nm[16];
  for (int i = 0; i < n; i++) {
    nodeLabel(hop[i], nm, sizeof nm);
    o += snprintf(out + o, cap - o, " > %s", nm);
  }
}

// The words for a reply to one of our requests (or for a waypoint/alert somebody sent). Empty when it says nothing.
size_t describe(uint32_t port, uint32_t from, const uint8_t *pl, size_t n, char *out, size_t cap) {
  size_t o = 0;
  char a[40], b[8];
  out[0] = 0;
  if (port == MeshPb::PORT_NODEINFO) {
    if (!MeshPb::parseUserNames(pl, n, a, sizeof a, b, sizeof b)) return 0;
    o = snprintf(out, cap, "Node info: %s (%s)", a, b);
  } else if (port == MeshPb::PORT_POSITION) {
    MeshPb::Pos p; MeshPb::parsePosition(pl, n, p);
    if (!p.hasPos) { o = snprintf(out, cap, "Position: not known"); }
    else {
      o = snprintf(out, cap, "Position: %.5f, %.5f", (double) p.latE7 / 1e7, (double) p.lonE7 / 1e7);
      if (p.hasAlt) o += snprintf(out + o, cap - o, ", %d m", (int) p.alt);
    }
  } else if (port == MeshPb::PORT_TELEMETRY) {
    MeshPb::Tel t; MeshPb::parseTelemetry(pl, n, t);
    o = snprintf(out, cap, "Telemetry:");
    if (t.hasDev) {
      if (t.battery >= 0) o += snprintf(out + o, cap - o, " battery %d%%,", t.battery > 100 ? 100 : t.battery);
      if (t.volt > 0) o += snprintf(out + o, cap - o, " %.2f V,", (double) t.volt);
      o += snprintf(out + o, cap - o, " channel %.1f%%, air %.1f%%,", (double) t.chanUtil, (double) t.airUtil);
      if (t.uptime) o += snprintf(out + o, cap - o, " up %luh %lum,", (unsigned long) (t.uptime / 3600), (unsigned long) (t.uptime / 60 % 60));
    }
    if (t.hasEnv) o += snprintf(out + o, cap - o, " %.1f C, %.0f%% humidity, %.0f hPa,", (double) t.temp, (double) t.hum, (double) t.press);
    if (out[o - 1] == ',' || out[o - 1] == ':') { if (out[o - 1] == ',') out[--o] = 0; else o += snprintf(out + o, cap - o, " nothing"); }
  } else if (port == MeshPb::PORT_TRACEROUTE) {
    MeshPb::Route r; MeshPb::parseRoute(pl, n, r);
    o = snprintf(out, cap, "Route: Me");
    routeText(r.towards, r.nTowards, out, cap, o);
    nodeLabel(from, a, sizeof a);
    o += snprintf(out + o, cap - o, " > %s", a);
    if (r.nBack) {
      o += snprintf(out + o, cap - o, "; back: %s", a);
      routeText(r.back, r.nBack, out, cap, o);
      o += snprintf(out + o, cap - o, " > Me");
    }
  } else if (port == MeshPb::PORT_WAYPOINT) {
    MeshPb::Way w; MeshPb::parseWaypoint(pl, n, w);
    o = snprintf(out, cap, "Waypoint %s: %.5f, %.5f", w.name[0] ? w.name : "-", (double) w.latE7 / 1e7, (double) w.lonE7 / 1e7);
  }
  if (o >= cap) o = cap - 1;
  return o;
}

// The state of the one key exchange at a time.
bool gKeyOn = false, gKeyPosted = false, gKeyReplied = false;
uint32_t gKeyTo = 0, gKeyPacket = 0;
uint8_t gKeyChan = 0;

Msg *ourMsg(uint32_t packetId);

// A message fails. A direct message that lacked a key goes to ST_KEYS instead, once, and posts the exchange.
void failMsg(Msg *m, uint8_t err) {
  gVer++;
  if ((m->flags & F_DM) && !m->tries && !gKeyOn && (err == 6 || err == 35 || err == 39)) {
    m->tries = 1; m->status = ST_KEYS; m->err = err;
    gKeyOn = gKeyPosted = true; gKeyReplied = false;
    gKeyTo = m->to; gKeyChan = m->chan; gKeyPacket = m->packetId;
    return;
  }
  m->status = ST_FAILED; m->err = err;
}

Msg *ourMsg(uint32_t packetId) {
  if (!packetId) return nullptr;
  for (int i = gCount - 1; i >= 0; i--) {
    Msg *m = at(i);
    if ((m->flags & F_OUT) && m->packetId == packetId) return m;
  }
  return nullptr;
}

void addIncoming(const MeshPb::Packet &p, const char *text, size_t len, uint32_t now, bool dm) {
  Msg &m = push();
  m.from = p.from; m.to = p.to; m.chan = p.channel; m.packetId = p.id;
  m.rxTime = p.rxTime ? p.rxTime : now;
  m.flags = dm ? F_DM : 0;
  if (len > (size_t) TEXT_MAX) len = TEXT_MAX;
  memcpy(m.text, text, len); m.text[len] = 0; m.len = (uint8_t) len;
}

}  // namespace

void setFavorite(uint32_t num, bool on) {
  int i = findNode(num);
  if (i >= 0) { gNodes[i].fav = on; gNodes[i].favKnown = true; gNodeUpdates++; }
}

bool init() {
  if (!gNodes) {
    gMaxNodes = MAX_NODES;
    gNodes = (NodeInfo *) bigAlloc(sizeof(NodeInfo) * gMaxNodes);
    if (!gNodes) { gMaxNodes = 64; gNodes = (NodeInfo *) bigAlloc(sizeof(NodeInfo) * gMaxNodes); }     // no room for the full list
  }
  if (!gNodes) return false;
  if (!gMsgs) {
    gCap = MSG_CAP;
    gMsgs = (Msg *) bigAlloc(sizeof(Msg) * gCap);
    if (!gMsgs) { gCap = 24; gMsgs = (Msg *) bigAlloc(sizeof(Msg) * gCap); }
    if (!gMsgs) { gCap = 0; return false; }
  }
  return true;
}

void resetSession() {
  keyGiveUp();
  gNNodes = 0; gConfigDone = false; gMyNum = 0; gFw[0] = 0;
  memset(gHaveChan, 0, sizeof gHaveChan);
  for (auto &c : gCfgSlot) c.have = false;
  gMqttHave = false;
  gNodeUpdates++;
}

void seedIds(uint32_t seed) { gIdState ^= seed ? seed : 0x9E3779B9u; if (!gIdState) gIdState = 1; }

uint32_t nodeClock() {
  if (!gOwnHeard) return 0;
  return gOwnHeard + (uint32_t) ((millis() - gOwnHeardAt) / 1000UL);
}

void forgetSettings() {
  for (auto &c : gCfgSlot) c.have = false;
  gMqttHave = false;
  memset(gHaveChan, 0, sizeof gHaveChan);
  gOwnHeard = 0;
}

bool mqttConfig(const uint8_t *&raw, size_t &len) {
  if (!gMqttHave) return false;
  raw = gMqttRaw; len = gMqttLen;
  return true;
}

bool config(uint8_t which, const uint8_t *&raw, size_t &len) {
  int c = cfgSlot(which);
  if (c < 0 || !gCfgSlot[c].have) return false;
  raw = gCfgSlot[c].raw; len = gCfgSlot[c].len;
  return true;
}

bool configDone() { return gConfigDone; }
uint32_t myNum() { return gMyNum; }
const char *firmware() { return gFw; }
int nodeCount() { return gNNodes; }
int namedNodes() { int n = 0; for (int i = 0; i < gNNodes; i++) if (gNodes[i].longName[0]) n++; return n; }
const NodeInfo *node(int i) { return (i >= 0 && i < gNNodes) ? &gNodes[i] : nullptr; }
uint32_t nodeUpdates() { return gNodeUpdates; }
int findNode(uint32_t num) { for (int i = 0; i < gNNodes; i++) if (gNodes[i].num == num) return i; return -1; }
int myBattery() { int i = findNode(gMyNum); return (gMyNum && i >= 0) ? gNodes[i].battery : -1; }
const MeshPb::Channel *channel(int slot) { return (slot >= 0 && slot < MAX_CHANS && gHaveChan[slot]) ? &gChans[slot] : nullptr; }

int msgCount() { return gCount; }
const Msg *msgAt(int i) { return (i >= 0 && i < gCount) ? at(i) : nullptr; }
uint32_t lastSeq() { return gSeq; }
void setSeqBase(uint32_t base) { gSeq = base; }
uint32_t version() { return gVer; }

int firstAfter(uint32_t cursor) {
  if (cursor > gSeq) cursor = 0;
  for (int i = 0; i < gCount; i++) if (at(i)->seq > cursor) return i;
  return -1;
}
uint32_t incomingSince(uint32_t cursor) {
  uint32_t n = 0;
  int i = firstAfter(cursor);
  if (i >= 0) for (; i < gCount; i++) n++;
  return n;
}
const Msg *findByLocalId(uint32_t localId) {
  if (!localId) return nullptr;
  for (int i = gCount - 1; i >= 0; i--) if ((at(i)->flags & F_OUT) && at(i)->localId == localId) return at(i);
  return nullptr;
}

void onFromRadio(const MeshPb::FromRadio &fr, uint32_t now) {
  switch (fr.kind) {
    case MeshPb::K_MY_INFO:
      gMyNum = fr.myNum;
      { NodeInfo me; me.num = fr.myNum; upsertNode(me); }
      break;
    case MeshPb::K_NODE_INFO:
      if (fr.node.num) upsertNode(fr.node);
      if (fr.node.num && fr.node.num == gMyNum && fr.node.lastHeard > 1700000000u) { gOwnHeard = fr.node.lastHeard; gOwnHeardAt = millis(); }
      break;
    case MeshPb::K_CHANNEL:
      if (fr.channel.index < MAX_CHANS) { gChans[fr.channel.index] = fr.channel; gHaveChan[fr.channel.index] = true; }
      break;
    case MeshPb::K_CONFIG: {
      int c = cfgSlot(fr.cfgWhich);
      if (c >= 0) { memcpy(gCfgSlot[c].raw, fr.cfgRaw, fr.cfgLen); gCfgSlot[c].len = fr.cfgLen; gCfgSlot[c].have = true; }
      break;
    }
    case MeshPb::K_METADATA:
      memcpy(gFw, fr.firmware, sizeof gFw);
      break;
    case MeshPb::K_MQTT:
      memcpy(gMqttRaw, fr.mqttRaw, fr.mqttLen); gMqttLen = fr.mqttLen; gMqttHave = true;
      break;
    case MeshPb::K_CONFIG_DONE:
      gConfigDone = true;
      break;
    case MeshPb::K_PACKET: {
      const MeshPb::Packet &p = fr.packet;
      if (!p.decoded) break;
      if (p.port == MeshPb::PORT_NODEINFO && gKeyOn && p.from == gKeyTo) gKeyReplied = true;
      if ((p.port == MeshPb::PORT_TEXT || p.port == MeshPb::PORT_ALERT) && p.payload && p.payloadLen) {
        if (!gMsgs || seenBefore(p.from, p.id)) break;
        addIncoming(p, (const char *) p.payload, p.payloadLen, now, p.to != MeshPb::BROADCAST);
      } else if (p.port == MeshPb::PORT_ROUTING && p.requestId && gMsgs) {
        Msg *m = ourMsg(p.requestId);                         // an ack or an error for one of ours
        if (m) {
          if (p.routeError == 0) {
            // A direct message is delivered when the recipient says so; an answer from anybody else is a relay's.
            if (!(m->flags & F_DM) || p.from == m->to) { m->status = ST_ACKED; gVer++; }
            else if (m->status != ST_ACKED) { m->status = ST_SENT_NOACK; gVer++; }
          }
          else if (p.routeError != 0xFF) failMsg(m, p.routeError);
        }
      } else if (gMsgs && p.payload && !seenBefore(p.from, p.id) &&
                 (p.port == MeshPb::PORT_WAYPOINT ||
                  (p.requestId && (p.port == MeshPb::PORT_NODEINFO || p.port == MeshPb::PORT_POSITION ||
                                   p.port == MeshPb::PORT_TELEMETRY || p.port == MeshPb::PORT_TRACEROUTE)))) {
        // The answer to one of our requests (or a waypoint somebody shared): stored as a message from that node.
        // The node folds node info, position and telemetry into its own list and sends the node again.
        Msg *m = ourMsg(p.requestId);
        if (m && m->status != ST_FAILED) { m->status = ST_ACKED; gVer++; }
        if (p.port != MeshPb::PORT_WAYPOINT && !m) break;
        char txt[TEXT_MAX + 1];
        size_t n = describe(p.port, p.from, p.payload, p.payloadLen, txt, sizeof txt);
        if (n) addIncoming(p, txt, n, now, p.port == MeshPb::PORT_WAYPOINT ? p.to != MeshPb::BROADCAST : true);
      }
      break;
    }
    case MeshPb::K_QUEUE:
      if (fr.queue.res != 0 && gMsgs) {                       // the node refused to send it at all
        Msg *m = ourMsg(fr.queue.packetId);
        if (m && m->status != ST_ACKED) failMsg(m, (uint8_t) fr.queue.res);
      }
      break;
    default: break;
  }
}

uint32_t queueText(uint32_t to, uint8_t chan, const char *text, size_t len, uint32_t localId,
                   uint32_t now, uint8_t *out, size_t cap, size_t &outLen) {
  outLen = 0;
  if (!gMsgs || len == 0 || len > (size_t) TEXT_MAX) return 0;
  uint32_t id = nextId();
  size_t n = MeshPb::buildText(out, cap, to, chan, id, true, text, len);
  if (!n) return 0;
  Msg &m = push();
  m.from = gMyNum; m.to = to; m.chan = chan; m.packetId = id; m.localId = localId; m.rxTime = now;
  m.flags = F_OUT | ((to != MeshPb::BROADCAST) ? F_DM : 0);
  m.status = ST_SENDING;
  memcpy(m.text, text, len); m.text[len] = 0; m.len = (uint8_t) len;
  outLen = n;
  return id;
}

bool keyJob(uint32_t &to, uint8_t &chan) {
  if (!gKeyPosted) return false;
  gKeyPosted = false; to = gKeyTo; chan = gKeyChan;
  return true;
}

bool keyReplied() { return gKeyReplied; }
int keyState() { Msg *m = gKeyOn ? ourMsg(gKeyPacket) : nullptr; return m ? (int) m->status : -1; }

uint32_t keyRequest(uint32_t to, uint8_t chan, uint8_t *out, size_t cap, size_t &outLen) {
  outLen = 0;
  if (!gMsgs) return 0;
  uint32_t id = nextId();
  bool own = gNNodes > 0 && gNodes[0].userRawLen;
  size_t n = MeshPb::buildData(out, cap, to, chan, id, MeshPb::PORT_NODEINFO, own ? gNodes[0].userRaw : nullptr, own ? gNodes[0].userRawLen : 0, true, true);
  if (!n) return 0;
  outLen = n;
  return id;
}

struct Asked { uint32_t num, at; } gAsked[16];
int gAskedPos = 0;

bool nameRequest(uint32_t nowMs, uint32_t againMs, uint32_t &to, uint8_t *out, size_t cap, size_t &outLen) {
  outLen = 0;
  if (!gMsgs) return false;
  for (int i = gCount - 1; i >= 0; i--) {
    const Msg *m = at(i);
    uint32_t from = m->from;
    if ((m->flags & F_OUT) || !from || from == gMyNum || from == MeshPb::BROADCAST) continue;
    int ni = findNode(from);
    if (ni >= 0 && gNodes[ni].longName[0]) continue;                 // it has a name
    bool again = false;
    for (int k = 0; k < 16; k++)
      if (gAsked[k].num == from && gAsked[k].at && (uint32_t) (nowMs - gAsked[k].at + 1) < againMs) again = true;
    if (again) continue;
    bool own = gNNodes > 0 && gNodes[0].userRawLen;
    size_t n = MeshPb::buildData(out, cap, from, m->chan, nextId(), MeshPb::PORT_NODEINFO, own ? gNodes[0].userRaw : nullptr, own ? gNodes[0].userRawLen : 0, false, true);
    if (!n) return false;
    gAsked[gAskedPos].num = from; gAsked[gAskedPos].at = nowMs ? nowMs : 1; gAskedPos = (gAskedPos + 1) % 16;
    outLen = n; to = from;
    return true;
  }
  return false;
}

uint32_t keyResend(uint8_t *out, size_t cap, size_t &outLen, uint32_t &packetId) {
  outLen = 0;
  Msg *m = gKeyOn ? ourMsg(gKeyPacket) : nullptr;
  gKeyOn = false;
  if (!m || m->status != ST_KEYS) return 0;
  uint32_t id = nextId();
  size_t n = MeshPb::buildText(out, cap, m->to, m->chan, id, true, m->text, m->len);
  if (!n) { m->status = ST_FAILED; gVer++; return 0; }
  m->packetId = id; m->status = ST_SENDING; gVer++;
  outLen = n; packetId = id;
  return id;
}

void keyGiveUp() {
  Msg *m = gKeyOn ? ourMsg(gKeyPacket) : nullptr;
  gKeyOn = false;
  if (m && m->status == ST_KEYS) { m->status = ST_FAILED; gVer++; }
}

uint32_t queueRequest(uint8_t kind, uint32_t to, uint8_t chan, int32_t latE7, int32_t lonE7, const char *name,
                      uint32_t localId, uint32_t now, uint8_t *out, size_t cap, size_t &outLen) {
  outLen = 0;
  if (!gMsgs) return 0;
  uint8_t pl[96]; size_t pn = 0;
  uint32_t port = 0; bool resp = false;
  char label[TEXT_MAX + 1];
  bool dm = to != MeshPb::BROADCAST;
  uint32_t id = nextId();
  switch (kind) {
    case REQ_INFO: {
      port = MeshPb::PORT_NODEINFO; resp = true;
      if (gNNodes > 0 && gNodes[0].userRawLen) { memcpy(pl, gNodes[0].userRaw, gNodes[0].userRawLen); pn = gNodes[0].userRawLen; }
      snprintf(label, sizeof label, dm ? "[Asked for node info]" : "[Shared my node info]");
      break;
    }
    case REQ_POSITION: port = MeshPb::PORT_POSITION; resp = true; snprintf(label, sizeof label, "[Asked for position]"); break;
    case REQ_TELEMETRY:
      port = MeshPb::PORT_TELEMETRY; resp = true;
      pn = MeshPb::telemetryRequestPayload(pl, sizeof pl);
      snprintf(label, sizeof label, "[Asked for telemetry]");
      break;
    case REQ_TRACE:
      if (!dm) return 0;
      port = MeshPb::PORT_TRACEROUTE; resp = true; snprintf(label, sizeof label, "[Traceroute]");
      break;
    case REQ_SHARE_POS:
      port = MeshPb::PORT_POSITION;
      pn = MeshPb::positionPayload(pl, sizeof pl, latE7, lonE7);
      snprintf(label, sizeof label, "[Shared position %.5f, %.5f]", (double) latE7 / 1e7, (double) lonE7 / 1e7);
      break;
    case REQ_WAYPOINT:
      port = MeshPb::PORT_WAYPOINT;
      pn = MeshPb::waypointPayload(pl, sizeof pl, id, latE7, lonE7, name);
      snprintf(label, sizeof label, "[Waypoint %s %.5f, %.5f]", name && name[0] ? name : "-", (double) latE7 / 1e7, (double) lonE7 / 1e7);
      break;
    default: return 0;
  }
  if (kind != REQ_INFO && kind != REQ_POSITION && kind != REQ_TRACE && !pn) return 0;
  size_t n = MeshPb::buildData(out, cap, to, chan, id, port, pl, pn, true, resp);
  if (!n) return 0;
  Msg &m = push();
  m.from = gMyNum; m.to = to; m.chan = chan; m.packetId = id; m.localId = localId; m.rxTime = now;
  m.flags = F_OUT | (dm ? F_DM : 0);
  m.status = ST_SENDING;
  m.len = (uint8_t) strlen(label); memcpy(m.text, label, m.len + 1);
  outLen = n;
  return id;
}

void textWritten(uint32_t packetId, bool ok) {
  for (int i = gCount - 1; i >= 0; i--) {
    Msg *m = at(i);
    if ((m->flags & F_OUT) && m->packetId == packetId) {
      gVer++;
      if (!ok) { m->status = ST_FAILED; m->err = ERR_WRITE; }
      else if (m->status == ST_SENDING) m->status = ST_SENT;
      return;
    }
  }
}

void expire(uint32_t now, uint32_t ackWaitSec) {
  (void) now; (void) ackWaitSec;     // SENT stays SENT: the Palm shows [>.]; an ack turns it into [>>]
}

void clearMessages() { gHead = gCount = 0; }

// The node shown changed: the other node hears the same packets again, and they are new to it.
void resetSeen() { memset(gSeen, 0, sizeof gSeen); gSeenPos = 0; memset(gAsked, 0, sizeof gAsked); gAskedPos = 0; }

bool importText(uint32_t from, uint32_t to, uint32_t rxTime, uint32_t packetId, uint8_t chan, uint8_t flags,
                const char *text, size_t len) {
  if (!gMsgs || !gCap) return false;
  if (len > (size_t) TEXT_MAX) len = TEXT_MAX;
  for (int i = 0; i < gCount; i++) {
    const Msg *m = at(i);
    if (m->from == from && m->rxTime == rxTime && m->len == len && !memcmp(m->text, text, len)) return false;
  }
  if (packetId && seenBefore(from, packetId)) return false;
  Msg &m = push();
  m.from = from; m.to = to; m.chan = chan; m.packetId = packetId; m.rxTime = rxTime; m.flags = flags;
  memcpy(m.text, text, len); m.text[len] = 0; m.len = (uint8_t) len;
  return true;
}

uint32_t clearOlderThan(uint32_t before) {
  // Keep the newer ones in order, drop the rest. Outgoing messages without a time (0) are kept.
  uint32_t gone = 0; int keep = 0;
  for (int i = 0; i < gCount; i++) {
    Msg *m = at(i);
    if (m->rxTime && m->rxTime < before) { gone++; continue; }
    if (keep != i) *at(keep) = *m;
    keep++;
  }
  gCount = keep;
  return gone;
}

}  // namespace MeshCore
