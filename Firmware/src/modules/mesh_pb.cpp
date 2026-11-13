// mesh_pb.cpp - see mesh_pb.h.
#include "mesh_pb.h"
#include <string.h>

namespace MeshPb {
namespace {

struct Rd {
  const uint8_t *p; const uint8_t *end; bool ok = true;
  Rd(const uint8_t *b, size_t n) : p(b), end(b + n) {}
  bool more() const { return ok && p < end; }
  uint64_t varint() {
    uint64_t v = 0; int sh = 0;
    while (p < end && sh < 70) {
      uint8_t b = *p++;
      v |= (uint64_t) (b & 0x7F) << sh;
      if (!(b & 0x80)) return v;
      sh += 7;
    }
    ok = false; return 0;
  }
  uint32_t fixed32() {
    if (end - p < 4) { ok = false; return 0; }
    uint32_t v = p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t) p[3] << 24);
    p += 4; return v;
  }
  // The next field's tag; false at the end or on a malformed one.
  bool tag(uint32_t &num, uint8_t &wire) {
    if (!more()) return false;
    uint64_t t = varint();
    if (!ok) return false;
    num = (uint32_t) (t >> 3); wire = (uint8_t) (t & 7);
    return true;
  }
  bool bytes(const uint8_t *&b, size_t &n) {
    uint64_t l = varint();
    if (!ok || l > (uint64_t) (end - p)) { ok = false; return false; }
    b = p; n = (size_t) l; p += l; return true;
  }
  void skip(uint8_t wire) {
    const uint8_t *b; size_t n;
    switch (wire) {
      case 0: varint(); break;
      case 1: if (end - p < 8) ok = false; else p += 8; break;
      case 2: bytes(b, n); break;
      case 5: if (end - p < 4) ok = false; else p += 4; break;
      default: ok = false;
    }
  }
};

float asFloat(uint32_t v) { float f; memcpy(&f, &v, 4); return f; }

void copyStr(char *dst, size_t cap, const uint8_t *b, size_t n) {
  if (n >= cap) n = cap - 1;
  memcpy(dst, b, n); dst[n] = 0;
}

bool parseData(const uint8_t *b, size_t n, Packet &pk) {
  Rd r(b, n); uint32_t f; uint8_t w;
  while (r.tag(f, w)) {
    if (f == 1 && w == 0) pk.port = (uint32_t) r.varint();
    else if (f == 2 && w == 2) r.bytes(pk.payload, pk.payloadLen);
    else if (f == 6 && w == 5) pk.requestId = r.fixed32();
    else r.skip(w);
  }
  return r.ok;
}

bool parsePacket(const uint8_t *b, size_t n, Packet &pk) {
  Rd r(b, n); uint32_t f; uint8_t w;
  while (r.tag(f, w)) {
    const uint8_t *d; size_t dl;
    switch (f) {
      case 1: if (w == 5) pk.from = r.fixed32(); else r.skip(w); break;
      case 2: if (w == 5) pk.to = r.fixed32(); else r.skip(w); break;
      case 3: if (w == 0) pk.channel = (uint8_t) r.varint(); else r.skip(w); break;
      case 4:
        if (w == 2 && r.bytes(d, dl)) { pk.decoded = true; if (!parseData(d, dl, pk)) r.ok = false; }
        else r.skip(w);
        break;
      case 6: if (w == 5) pk.id = r.fixed32(); else r.skip(w); break;
      case 7: if (w == 5) pk.rxTime = r.fixed32(); else r.skip(w); break;
      case 8: if (w == 5) { pk.snr = asFloat(r.fixed32()); pk.hasSnr = true; } else r.skip(w); break;
      case 9: if (w == 0) pk.hopLimit = (uint32_t) r.varint(); else r.skip(w); break;
      case 10: if (w == 0) pk.wantAck = r.varint() != 0; else r.skip(w); break;
      case 12: if (w == 0) pk.rssi = (int32_t) r.varint(); else r.skip(w); break;
      case 15: if (w == 0) pk.hopStart = (uint32_t) r.varint(); else r.skip(w); break;
      default: r.skip(w);
    }
  }
  if (pk.decoded && pk.port == PORT_ROUTING) {   // Routing.error_reason = 3
    Rd q(pk.payload ? pk.payload : b, pk.payload ? pk.payloadLen : 0); uint32_t qf; uint8_t qw;
    bool other = false;                                         // a route request/reply, not an ack or error
    pk.routeError = 0;                                          // an empty Routing is error NONE (an ack)
    while (q.tag(qf, qw)) {
      if (qf == 3 && qw == 0) pk.routeError = (uint8_t) q.varint();
      else { if (qf == 1 || qf == 2) other = true; q.skip(qw); }
    }
    if (other) pk.routeError = 0xFF;
  }
  return r.ok;
}

void parseDeviceMetrics(const uint8_t *b, size_t n, NodeInfo &ni) {
  Rd r(b, n); uint32_t f; uint8_t w;
  while (r.tag(f, w)) {
    if (f == 1 && w == 0) ni.battery = (int16_t) r.varint();
    else if (f == 3 && w == 5) ni.chanUtil10 = (uint16_t) (asFloat(r.fixed32()) * 10.0f + 0.5f);
    else if (f == 4 && w == 5) ni.airUtil10 = (uint16_t) (asFloat(r.fixed32()) * 10.0f + 0.5f);
    else if (f == 5 && w == 0) ni.uptime = (uint32_t) r.varint();
    else r.skip(w);
  }
}

void parseUser(const uint8_t *b, size_t n, NodeInfo &ni) {
  if (n <= sizeof ni.userRaw) { memcpy(ni.userRaw, b, n); ni.userRawLen = (uint8_t) n; }
  Rd r(b, n); uint32_t f; uint8_t w; const uint8_t *d; size_t dl;
  while (r.tag(f, w)) {
    if (f == 1 && w == 2 && r.bytes(d, dl)) copyStr(ni.id, sizeof ni.id, d, dl);
    else if (f == 2 && w == 2 && r.bytes(d, dl)) copyStr(ni.longName, sizeof ni.longName, d, dl);
    else if (f == 3 && w == 2 && r.bytes(d, dl)) copyStr(ni.shortName, sizeof ni.shortName, d, dl);
    else if (f == 5 && w == 0) ni.hw = (uint32_t) r.varint();
    else if (f == 7 && w == 0) ni.role = (uint32_t) r.varint();
    else if (f == 8 && w == 2 && r.bytes(d, dl)) ni.hasKey = dl == 32;
    else r.skip(w);
  }
}

void parsePosition(const uint8_t *b, size_t n, NodeInfo &ni) {
  Rd r(b, n); uint32_t f; uint8_t w; bool la = false, lo = false;
  while (r.tag(f, w)) {
    if (f == 1 && w == 5) { ni.latE7 = (int32_t) r.fixed32(); la = true; }
    else if (f == 2 && w == 5) { ni.lonE7 = (int32_t) r.fixed32(); lo = true; }
    else r.skip(w);
  }
  ni.hasPos = la && lo && (ni.latE7 != 0 || ni.lonE7 != 0);
}

bool parseNodeInfo(const uint8_t *b, size_t n, NodeInfo &ni) {
  Rd r(b, n); uint32_t f; uint8_t w; const uint8_t *d; size_t dl;
  ni.favKnown = true;                         // a full record: no is_favorite means not a favourite
  while (r.tag(f, w)) {
    if (f == 1 && w == 0) ni.num = (uint32_t) r.varint();
    else if (f == 2 && w == 2 && r.bytes(d, dl)) parseUser(d, dl, ni);
    else if (f == 3 && w == 2 && r.bytes(d, dl)) parsePosition(d, dl, ni);
    else if (f == 4 && w == 5) { ni.snr = asFloat(r.fixed32()); ni.hasSnr = true; }
    else if (f == 5 && w == 5) ni.lastHeard = r.fixed32();
    else if (f == 6 && w == 2 && r.bytes(d, dl)) parseDeviceMetrics(d, dl, ni);
    else if (f == 9 && w == 0) ni.hops = (int16_t) r.varint();
    else if (f == 10 && w == 0) ni.fav = r.varint() != 0;
    else r.skip(w);
  }
  return r.ok;
}

bool parseChannel(const uint8_t *b, size_t n, Channel &ch) {
  Rd r(b, n); uint32_t f; uint8_t w; const uint8_t *d; size_t dl;
  while (r.tag(f, w)) {
    if (f == 1 && w == 0) ch.index = (uint8_t) r.varint();
    else if (f == 3 && w == 0) ch.role = (uint8_t) r.varint();
    else if (f == 2 && w == 2 && r.bytes(d, dl)) {              // ChannelSettings: psk = 2, name = 3
      Rd s(d, dl); uint32_t sf; uint8_t sw; const uint8_t *e; size_t el;
      while (s.tag(sf, sw)) {
        if (sf == 2 && sw == 2 && s.bytes(e, el)) {
          ch.hasPsk = el > 1 || (el == 1 && e[0] != 0);
          if (el <= sizeof ch.psk) { memcpy(ch.psk, e, el); ch.pskLen = (uint8_t) el; }
        }
        else if (sf == 3 && sw == 2 && s.bytes(e, el)) copyStr(ch.name, sizeof ch.name, e, el);
        else if (sf == 5 && sw == 0) ch.uplink = s.varint() != 0;
        else if (sf == 6 && sw == 0) ch.downlink = s.varint() != 0;
        else s.skip(sw);
      }
      if (!s.ok) r.ok = false;
      if (dl <= sizeof ch.raw) { memcpy(ch.raw, d, dl); ch.rawLen = (uint8_t) dl; }
    } else r.skip(w);
  }
  return r.ok;
}

// ---- writer ----------------------------------------------------------------------------------

struct Wr {
  uint8_t *b; size_t cap; size_t n = 0; bool ok = true;
  Wr(uint8_t *buf, size_t c) : b(buf), cap(c) {}
  void put(uint8_t v) { if (n < cap) b[n++] = v; else ok = false; }
  void varint(uint64_t v) { while (v >= 0x80) { put((uint8_t) (v | 0x80)); v >>= 7; } put((uint8_t) v); }
  void tag(uint32_t f, uint8_t w) { varint(((uint64_t) f << 3) | w); }
  void u(uint32_t f, uint32_t v) { tag(f, 0); varint(v); }
  void fx32(uint32_t f, uint32_t v) { tag(f, 5); for (int i = 0; i < 4; i++) put((uint8_t) (v >> (8 * i))); }
  void bytes(uint32_t f, const uint8_t *p, size_t len) { tag(f, 2); varint(len); for (size_t i = 0; i < len; i++) put(p[i]); }
};

size_t varintLen(uint64_t v) { size_t n = 1; while (v >= 0x80) { v >>= 7; n++; } return n; }

}  // namespace

bool parseFromRadio(const uint8_t *buf, size_t len, FromRadio &out) {
  Rd r(buf, len); uint32_t f; uint8_t w; const uint8_t *d; size_t dl;
  while (r.tag(f, w)) {
    if (f == 1 && w == 0) { out.id = (uint32_t) r.varint(); continue; }
    if (w == 0) {
      uint32_t v = (uint32_t) r.varint();
      if (f == 7) { out.kind = K_CONFIG_DONE; out.configId = v; }
      else if (f == 8) out.kind = K_REBOOTED;
      continue;
    }
    if (w != 2 || !r.bytes(d, dl)) { r.skip(w); continue; }
    switch (f) {
      case 2: out.kind = K_PACKET; if (!parsePacket(d, dl, out.packet)) r.ok = false; break;
      case 3: {
        out.kind = K_MY_INFO;
        Rd m(d, dl); uint32_t mf; uint8_t mw;
        while (m.tag(mf, mw)) { if (mf == 1 && mw == 0) out.myNum = (uint32_t) m.varint(); else m.skip(mw); }
        if (!m.ok) r.ok = false;
        break;
      }
      case 5: {                                                 // Config { device = 1, lora = 6, bluetooth = 7 }
        Rd m(d, dl); uint32_t mf; uint8_t mw; const uint8_t *s; size_t sl;
        while (m.tag(mf, mw)) {
          if ((mf == 1 || mf == 6 || mf == 7) && mw == 2 && m.bytes(s, sl)) {
            if (sl <= sizeof out.cfgRaw) { out.kind = K_CONFIG; out.cfgWhich = (uint8_t) mf; memcpy(out.cfgRaw, s, sl); out.cfgLen = (uint8_t) sl; }
          } else m.skip(mw);
        }
        if (!m.ok) r.ok = false;
        break;
      }
      case 4: out.kind = K_NODE_INFO; if (!parseNodeInfo(d, dl, out.node)) r.ok = false; break;
      case 10: out.kind = K_CHANNEL; if (!parseChannel(d, dl, out.channel)) r.ok = false; break;
      case 11: {
        out.kind = K_QUEUE;
        Rd q(d, dl); uint32_t qf; uint8_t qw;
        while (q.tag(qf, qw)) {
          if (qf == 1 && qw == 0) out.queue.res = (int32_t) q.varint();
          else if (qf == 2 && qw == 0) out.queue.free = (uint32_t) q.varint();
          else if (qf == 3 && qw == 0) out.queue.maxlen = (uint32_t) q.varint();
          else if (qf == 4 && qw == 0) out.queue.packetId = (uint32_t) q.varint();
          else q.skip(qw);
        }
        if (!q.ok) r.ok = false;
        break;
      }
      case 13: {
        out.kind = K_METADATA;
        Rd m(d, dl); uint32_t mf; uint8_t mw; const uint8_t *s; size_t sl;
        while (m.tag(mf, mw)) {
          if (mf == 1 && mw == 2 && m.bytes(s, sl)) copyStr(out.firmware, sizeof out.firmware, s, sl);
          else m.skip(mw);
        }
        if (!m.ok) r.ok = false;
        break;
      }
      case 9: {                                                 // ModuleConfig { mqtt = 1 }: the others are not needed
        Rd m(d, dl); uint32_t mf; uint8_t mw; const uint8_t *s; size_t sl;
        while (m.tag(mf, mw)) {
          if (mf == 1 && mw == 2 && m.bytes(s, sl)) {
            if (sl <= sizeof out.mqttRaw) { out.kind = K_MQTT; memcpy(out.mqttRaw, s, sl); out.mqttLen = (uint16_t) sl; }
          } else m.skip(mw);
        }
        if (!m.ok) r.ok = false;
        break;
      }
      default: break;                 // fileInfo, ...: not needed
    }
  }
  return r.ok;
}

size_t buildWantConfig(uint8_t *buf, size_t cap, uint32_t nonce) {
  Wr w(buf, cap); w.u(3, nonce); return w.ok ? w.n : 0;
}
size_t buildHeartbeat(uint8_t *buf, size_t cap) {
  Wr w(buf, cap); w.bytes(7, nullptr, 0); return w.ok ? w.n : 0;
}
size_t buildDisconnect(uint8_t *buf, size_t cap) {
  Wr w(buf, cap); w.u(4, 1); return w.ok ? w.n : 0;
}

size_t buildText(uint8_t *buf, size_t cap, uint32_t to, uint8_t channel, uint32_t id, bool wantAck,
                 const char *text, size_t textLen) {
  // Data { portnum = 1 (TEXT), payload = 2 }
  size_t dataLen = 1 + 1 + 1 + varintLen(textLen) + textLen;
  // MeshPacket { to = 2 (fixed32), channel = 3, decoded = 4, id = 6 (fixed32), hop_limit = 9, want_ack = 10 }
  size_t pkLen = (1 + 4) + (channel ? 2 : 0) + 1 + varintLen(dataLen) + dataLen + (1 + 4)
               + 2 + (wantAck ? 2 : 0);
  Wr w(buf, cap);
  w.tag(1, 2); w.varint(pkLen);
  w.fx32(2, to);
  if (channel) w.u(3, channel);
  w.tag(4, 2); w.varint(dataLen);
  w.u(1, PORT_TEXT);
  w.bytes(2, (const uint8_t *) text, textLen);
  w.fx32(6, id);
  w.u(9, 3);
  if (wantAck) w.u(10, 1);
  return w.ok ? w.n : 0;
}


// ---- settings -----------------------------------------------------------------------------------

void readLora(const uint8_t *raw, size_t n, LoraVals &v) {
  Rd r(raw, n); uint32_t f; uint8_t w;
  while (r.tag(f, w)) {
    if (w != 0) { r.skip(w); continue; }
    uint64_t x = r.varint();
    if (f == 1) v.usePreset = x != 0;
    else if (f == 2) v.preset = (uint32_t) x;
    else if (f == 7) v.region = (uint32_t) x;
    else if (f == 8) v.hop = (uint32_t) x;
    else if (f == 10) v.txPower = (int32_t) x;
  }
}

uint32_t readDeviceRole(const uint8_t *raw, size_t n) {
  Rd r(raw, n); uint32_t f; uint8_t w; uint32_t role = 0;
  while (r.tag(f, w)) { if (f == 1 && w == 0) role = (uint32_t) r.varint(); else r.skip(w); }
  return role;
}

void readBluetooth(const uint8_t *raw, size_t n, BtVals &v) {
  Rd r(raw, n); uint32_t f; uint8_t w;
  while (r.tag(f, w)) {
    if (w != 0) { r.skip(w); continue; }
    uint64_t x = r.varint();
    if (f == 1) v.enabled = x != 0;
    else if (f == 2) v.mode = (uint32_t) x;
    else if (f == 3) v.fixedPin = (uint32_t) x;
  }
}

void readMqtt(const uint8_t *raw, size_t n, MqttVals &v) {
  Rd r(raw, n); uint32_t f; uint8_t w; const uint8_t *d; size_t dl;
  while (r.tag(f, w)) {
    if (w == 0) {
      uint64_t x = r.varint();
      if (f == 1) v.enabled = x != 0;
      else if (f == 5) v.encrypt = x != 0;
      else if (f == 6) v.json = x != 0;
      else if (f == 7) v.tls = x != 0;
      else if (f == 9) v.proxy = x != 0;
      else if (f == 10) v.mapRep = x != 0;
    } else if (w == 2 && r.bytes(d, dl)) {
      if (f == 2) copyStr(v.address, sizeof v.address, d, dl);
      else if (f == 3) copyStr(v.username, sizeof v.username, d, dl);
      else if (f == 4) copyStr(v.password, sizeof v.password, d, dl);
      else if (f == 8) copyStr(v.root, sizeof v.root, d, dl);
    } else r.skip(w);
  }
}

void readOwner(const uint8_t *raw, size_t n, char *longName, size_t lcap, char *shortName, size_t scap) {
  Rd r(raw, n); uint32_t f; uint8_t w; const uint8_t *d; size_t dl;
  while (r.tag(f, w)) {
    if (f == 2 && w == 2 && r.bytes(d, dl)) copyStr(longName, lcap, d, dl);
    else if (f == 3 && w == 2 && r.bytes(d, dl)) copyStr(shortName, scap, d, dl);
    else r.skip(w);
  }
}

namespace {

struct Patch { uint32_t field; bool isStr; uint64_t v; const char *s; bool done; size_t len; bool useLen; };   // useLen: s is binary, len bytes

// Copies raw field by field, swapping in the patched ones; patched fields the source lacked go on the end.
size_t patchMsg(const uint8_t *raw, size_t n, Patch *p, int np, uint8_t *out, size_t cap, bool skipDefaults = false) {
  Wr w(out, cap);
  Rd r(raw, n); uint32_t f; uint8_t wt;
  while (true) {
    const uint8_t *start = r.p;
    if (!r.tag(f, wt)) break;
    r.skip(wt);
    if (!r.ok) return 0;
    Patch *hit = nullptr;
    for (int i = 0; i < np; i++) if (p[i].field == f && !p[i].done) { hit = &p[i]; break; }
    if (hit) continue;                                      // replaced below
    for (const uint8_t *q = start; q < r.p; q++) w.put(*q);
  }
  for (int i = 0; i < np; i++) {
    // proto3 leaves a default (false, 0, empty) out, which is what the Meshtastic clients do: skipDefaults does the same
    if (skipDefaults && (p[i].isStr ? (p[i].useLen ? p[i].len : strlen(p[i].s)) == 0 : p[i].v == 0)) continue;
    if (p[i].isStr) w.bytes(p[i].field, (const uint8_t *) p[i].s, p[i].useLen ? p[i].len : strlen(p[i].s));
    else { w.tag(p[i].field, 0); w.varint(p[i].v); }
  }
  return w.ok ? w.n : 0;
}

}  // namespace

size_t patchLora(const uint8_t *raw, size_t n, uint32_t preset, uint32_t region, uint32_t hop, int32_t txPower,
                 uint8_t *out, size_t cap) {
  Patch p[5] = {{1, false, 1, nullptr, false}, {2, false, preset, nullptr, false}, {7, false, region, nullptr, false},
                {8, false, hop, nullptr, false}, {10, false, (uint64_t) (int64_t) txPower, nullptr, false}};
  return patchMsg(raw, n, p, 5, out, cap);
}
size_t patchDevice(const uint8_t *raw, size_t n, uint32_t role, uint8_t *out, size_t cap) {
  Patch p[1] = {{1, false, role, nullptr, false}};
  return patchMsg(raw, n, p, 1, out, cap);
}
size_t patchBluetooth(const uint8_t *raw, size_t n, uint32_t mode, uint32_t fixedPin, uint8_t *out, size_t cap) {
  Patch p[3] = {{1, false, 1, nullptr, false}, {2, false, mode, nullptr, false}, {3, false, fixedPin, nullptr, false}};
  return patchMsg(raw, n, p, 3, out, cap);
}
size_t patchMqtt(const uint8_t *raw, size_t n, const MqttVals &v, uint8_t *out, size_t cap) {
  // Written the way the Meshtastic clients serialise it: fields 1 to 10 in order, a default (false, empty) left out, then
  // whatever else the node sent (map_report_settings and anything newer) exactly as it was.
  Wr w(out, cap);
  auto flag = [&w](uint32_t f, bool on) { if (on) { w.tag(f, 0); w.varint(1); } };
  auto text = [&w](uint32_t f, const char *t) { if (t[0]) w.bytes(f, (const uint8_t *) t, strlen(t)); };
  flag(1, v.enabled); text(2, v.address); text(3, v.username); text(4, v.password); flag(5, v.encrypt);
  flag(6, v.json); flag(7, v.tls); text(8, v.root); flag(9, v.proxy); flag(10, v.mapRep);
  Rd r(raw, n); uint32_t f; uint8_t wt;
  while (true) {
    const uint8_t *start = r.p;
    if (!r.tag(f, wt)) break;
    r.skip(wt);
    if (!r.ok) return 0;
    if (f >= 1 && f <= 10) continue;
    for (const uint8_t *q = start; q < r.p; q++) w.put(*q);
  }
  return w.ok ? w.n : 0;
}
namespace { void putKey(Wr &w, const uint8_t *key, size_t keyLen); }

size_t patchChannelSettings(const uint8_t *raw, size_t n, const uint8_t *psk, size_t pskLen, const char *name, bool uplink, bool downlink,
                            uint8_t *out, size_t cap) {
  Patch p[4] = {{2, true, 0, (const char *) psk, false, pskLen, true}, {3, true, 0, name, false, 0, false},
                {5, false, uplink ? 1u : 0u, nullptr, false, 0, false}, {6, false, downlink ? 1u : 0u, nullptr, false, 0, false}};
  return patchMsg(raw, n, p, 4, out, cap);
}
size_t adminSetChannelRaw(uint8_t *out, size_t cap, uint8_t index, uint8_t role, const uint8_t *settings, size_t sl,
                          const uint8_t *key, size_t keyLen) {
  uint8_t ch[160]; Wr c(ch, sizeof ch);                      // Channel { index = 1, settings = 2, role = 3 }
  c.u(1, index); c.bytes(2, settings, sl); c.u(3, role);
  if (!c.ok) return 0;
  Wr w(out, cap); w.bytes(33, ch, c.n); putKey(w, key, keyLen);
  return w.ok ? w.n : 0;
}

size_t patchOwner(const uint8_t *raw, size_t n, const char *longName, const char *shortName, uint8_t *out, size_t cap) {
  Patch p[2] = {{2, true, 0, longName, false}, {3, true, 0, shortName, false}};
  return patchMsg(raw, n, p, 2, out, cap);
}

namespace {
void putKey(Wr &w, const uint8_t *key, size_t keyLen) { if (key && keyLen) w.bytes(101, key, keyLen); }
}

size_t adminGetConfig(uint8_t *out, size_t cap, uint32_t type) { Wr w(out, cap); w.u(5, type); return w.ok ? w.n : 0; }

size_t adminSetOwner(uint8_t *out, size_t cap, const uint8_t *user, size_t n, const uint8_t *key, size_t keyLen) {
  Wr w(out, cap); w.bytes(32, user, n); putKey(w, key, keyLen); return w.ok ? w.n : 0;
}

size_t adminSetConfig(uint8_t *out, size_t cap, uint8_t which, const uint8_t *cfg, size_t n, const uint8_t *key, size_t keyLen) {
  Wr w(out, cap);
  size_t inner = 1 + varintLen(n) + n;                       // Config { which = bytes }
  w.tag(34, 2); w.varint(inner);
  w.bytes(which, cfg, n);
  putKey(w, key, keyLen);
  return w.ok ? w.n : 0;
}

size_t adminSetModuleConfig(uint8_t *out, size_t cap, uint8_t which, const uint8_t *cfg, size_t n, const uint8_t *key, size_t keyLen) {
  Wr w(out, cap);
  size_t inner = 1 + varintLen(n) + n;                       // ModuleConfig { which = bytes }
  w.tag(35, 2); w.varint(inner);
  w.bytes(which, cfg, n);
  putKey(w, key, keyLen);
  return w.ok ? w.n : 0;
}

size_t adminSetChannel(uint8_t *out, size_t cap, uint8_t index, uint8_t role, const char *name, const uint8_t *psk,
                       size_t pskLen, const uint8_t *key, size_t keyLen) {
  uint8_t set[96]; Wr s(set, sizeof set);                    // ChannelSettings { psk = 2, name = 3 }
  s.bytes(2, psk, pskLen); s.bytes(3, (const uint8_t *) name, strlen(name));
  if (!s.ok) return 0;
  uint8_t ch[128]; Wr c(ch, sizeof ch);                      // Channel { index = 1, settings = 2, role = 3 }
  c.u(1, index); c.bytes(2, set, s.n); c.u(3, role);
  if (!c.ok) return 0;
  Wr w(out, cap); w.bytes(33, ch, c.n); putKey(w, key, keyLen);
  return w.ok ? w.n : 0;
}

size_t adminFlag(uint8_t *out, size_t cap, uint32_t field, const uint8_t *key, size_t keyLen) {
  Wr w(out, cap); w.u(field, 1); putKey(w, key, keyLen); return w.ok ? w.n : 0;
}
size_t adminInt(uint8_t *out, size_t cap, uint32_t field, uint32_t v, const uint8_t *key, size_t keyLen) {
  // unsigned: a node number with the top bit set (e1a2b3c4) stays 5 bytes instead of sign-extending to 10
  Wr w(out, cap); w.tag(field, 0); w.varint(v); putKey(w, key, keyLen); return w.ok ? w.n : 0;
}
size_t adminTime(uint8_t *out, size_t cap, uint32_t utc, const uint8_t *key, size_t keyLen) {
  Wr w(out, cap); w.fx32(43, utc); putKey(w, key, keyLen); return w.ok ? w.n : 0;
}

size_t buildAdmin(uint8_t *buf, size_t cap, uint32_t myNum, uint32_t id, const uint8_t *admin, size_t alen) {
  // Data { portnum = 1 (ADMIN), payload = 2, want_response = 3 }
  size_t dataLen = 2 + 1 + varintLen(alen) + alen + 2;
  // MeshPacket { to = 2, decoded = 4, id = 6, want_ack = 10, priority = 11, pki_encrypted = 17 }: the same header the Meshtastic
  // clients send for an admin message (priority RELIABLE = 70, PKI flag set, no hop limit); the node's admin handling expects it.
  size_t pkLen = (1 + 4) + 1 + varintLen(dataLen) + dataLen + (1 + 4) + 2 + 2 + 3;
  Wr w(buf, cap);
  w.tag(1, 2); w.varint(pkLen);
  w.fx32(2, myNum);
  w.tag(4, 2); w.varint(dataLen);
  w.u(1, PORT_ADMIN);
  w.bytes(2, admin, alen);
  w.u(3, 1);
  w.fx32(6, id);
  w.u(10, 1);
  w.u(11, 70);
  w.u(17, 1);
  return w.ok ? w.n : 0;
}

bool adminPasskey(const uint8_t *payload, size_t n, uint8_t *key, size_t &keyLen) {
  Rd r(payload, n); uint32_t f; uint8_t wt; const uint8_t *d; size_t dl;
  while (r.tag(f, wt)) {
    if (f == 101 && wt == 2 && r.bytes(d, dl)) {
      if (dl > 16) return false;
      memcpy(key, d, dl); keyLen = dl; return true;
    }
    r.skip(wt);
  }
  return false;
}

size_t buildData(uint8_t *buf, size_t cap, uint32_t to, uint8_t channel, uint32_t id, uint32_t port,
                 const uint8_t *payload, size_t plen, bool wantAck, bool wantResponse) {
  // Data { portnum = 1, payload = 2, want_response = 3 }
  size_t dataLen = 1 + varintLen(port) + (plen ? 1 + varintLen(plen) + plen : 0) + (wantResponse ? 2 : 0);
  size_t pkLen = (1 + 4) + (channel ? 2 : 0) + 1 + varintLen(dataLen) + dataLen + (1 + 4) + 2 + (wantAck ? 2 : 0);
  Wr w(buf, cap);
  w.tag(1, 2); w.varint(pkLen);
  w.fx32(2, to);
  if (channel) w.u(3, channel);
  w.tag(4, 2); w.varint(dataLen);
  w.u(1, port);
  if (plen) w.bytes(2, payload, plen);
  if (wantResponse) w.u(3, 1);
  w.fx32(6, id);
  w.u(9, 3);
  if (wantAck) w.u(10, 1);
  return w.ok ? w.n : 0;
}

size_t positionPayload(uint8_t *out, size_t cap, int32_t latE7, int32_t lonE7) {
  Wr w(out, cap); w.fx32(1, (uint32_t) latE7); w.fx32(2, (uint32_t) lonE7); return w.ok ? w.n : 0;
}

size_t waypointPayload(uint8_t *out, size_t cap, uint32_t id, int32_t latE7, int32_t lonE7, const char *name) {
  Wr w(out, cap);
  w.u(1, id); w.fx32(2, (uint32_t) latE7); w.fx32(3, (uint32_t) lonE7);
  if (name && name[0]) w.bytes(6, (const uint8_t *) name, strlen(name) > 29 ? 29 : strlen(name));
  return w.ok ? w.n : 0;
}

size_t telemetryRequestPayload(uint8_t *out, size_t cap) {
  Wr w(out, cap); w.bytes(2, nullptr, 0); return w.ok ? w.n : 0;      // Telemetry { device_metrics = {} }
}

bool parseRoute(const uint8_t *p, size_t n, Route &out) {
  Rd r(p, n); uint32_t f; uint8_t w; const uint8_t *d; size_t dl;
  out.nTowards = out.nBack = 0;
  while (r.tag(f, w)) {
    if ((f == 1 || f == 3) && w == 2 && r.bytes(d, dl)) {         // packed fixed32
      uint32_t *dst = f == 1 ? out.towards : out.back;
      uint8_t &cnt = f == 1 ? out.nTowards : out.nBack;
      for (size_t i = 0; i + 4 <= dl && cnt < 8; i += 4)
        dst[cnt++] = d[i] | (d[i + 1] << 8) | (d[i + 2] << 16) | ((uint32_t) d[i + 3] << 24);
    } else if ((f == 1 || f == 3) && w == 5) {
      uint32_t v = r.fixed32();
      if (f == 1 && out.nTowards < 8) out.towards[out.nTowards++] = v;
      else if (f == 3 && out.nBack < 8) out.back[out.nBack++] = v;
    } else r.skip(w);
  }
  return r.ok;
}

bool parsePosition(const uint8_t *p, size_t n, Pos &out) {
  Rd r(p, n); uint32_t f; uint8_t w; bool la = false, lo = false;
  while (r.tag(f, w)) {
    if (f == 1 && w == 5) { out.latE7 = (int32_t) r.fixed32(); la = true; }
    else if (f == 2 && w == 5) { out.lonE7 = (int32_t) r.fixed32(); lo = true; }
    else if (f == 3 && w == 0) { out.alt = (int32_t) r.varint(); out.hasAlt = true; }
    else r.skip(w);
  }
  out.hasPos = la && lo && (out.latE7 || out.lonE7);
  return r.ok;
}

bool parseTelemetry(const uint8_t *p, size_t n, Tel &out) {
  Rd r(p, n); uint32_t f; uint8_t w; const uint8_t *d; size_t dl;
  while (r.tag(f, w)) {
    if (f == 2 && w == 2 && r.bytes(d, dl)) {
      Rd q(d, dl); uint32_t qf; uint8_t qw;
      out.hasDev = true;
      while (q.tag(qf, qw)) {
        if (qf == 1 && qw == 0) out.battery = (int) q.varint();
        else if (qf == 2 && qw == 5) out.volt = asFloat(q.fixed32());
        else if (qf == 3 && qw == 5) out.chanUtil = asFloat(q.fixed32());
        else if (qf == 4 && qw == 5) out.airUtil = asFloat(q.fixed32());
        else if (qf == 5 && qw == 0) out.uptime = (uint32_t) q.varint();
        else q.skip(qw);
      }
    } else if (f == 3 && w == 2 && r.bytes(d, dl)) {
      Rd q(d, dl); uint32_t qf; uint8_t qw;
      out.hasEnv = true;
      while (q.tag(qf, qw)) {
        if (qf == 1 && qw == 5) out.temp = asFloat(q.fixed32());
        else if (qf == 2 && qw == 5) out.hum = asFloat(q.fixed32());
        else if (qf == 3 && qw == 5) out.press = asFloat(q.fixed32());
        else q.skip(qw);
      }
    } else r.skip(w);
  }
  return r.ok;
}

bool parseWaypoint(const uint8_t *p, size_t n, Way &out) {
  Rd r(p, n); uint32_t f; uint8_t w; const uint8_t *d; size_t dl;
  while (r.tag(f, w)) {
    if (f == 1 && w == 0) out.id = (uint32_t) r.varint();
    else if (f == 2 && w == 5) out.latE7 = (int32_t) r.fixed32();
    else if (f == 3 && w == 5) out.lonE7 = (int32_t) r.fixed32();
    else if (f == 6 && w == 2 && r.bytes(d, dl)) copyStr(out.name, sizeof out.name, d, dl);
    else r.skip(w);
  }
  return r.ok;
}

bool parseUserNames(const uint8_t *p, size_t n, char *longName, size_t lcap, char *shortName, size_t scap) {
  longName[0] = shortName[0] = 0;
  readOwner(p, n, longName, lcap, shortName, scap);
  return longName[0] || shortName[0];
}

}  // namespace MeshPb
