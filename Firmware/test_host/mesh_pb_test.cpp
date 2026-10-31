// Host tests for mesh_pb.cpp. From Firmware/:
//   g++ -std=c++17 -Wall -Wextra -Iinclude src/modules/mesh_pb.cpp test_host/mesh_pb_test.cpp -o /tmp/mesh_pb_test && /tmp/mesh_pb_test
#include "mesh_pb.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
using namespace MeshPb;

static int fails = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL: %s\n", msg); fails++; } } while (0)

typedef std::vector<uint8_t> V;
static void varint(V &v, uint64_t x) { while (x >= 0x80) { v.push_back((uint8_t) (x | 0x80)); x >>= 7; } v.push_back((uint8_t) x); }
static void tag(V &v, int f, int w) { varint(v, ((uint64_t) f << 3) | w); }
static void vu(V &v, int f, uint64_t x) { tag(v, f, 0); varint(v, x); }
static void fx(V &v, int f, uint32_t x) { tag(v, f, 5); for (int i = 0; i < 4; i++) v.push_back((uint8_t) (x >> (8 * i))); }
static void ln(V &v, int f, const V &in) { tag(v, f, 2); varint(v, in.size()); v.insert(v.end(), in.begin(), in.end()); }
static V str(const char *s) { return V(s, s + strlen(s)); }
static void fl(V &v, int f, float x) { uint32_t u; memcpy(&u, &x, 4); fx(v, f, u); }

static V textPacket(uint32_t from, uint32_t to, uint32_t id, uint32_t rx, const char *text) {
  V data; vu(data, 1, PORT_TEXT); ln(data, 2, str(text));
  V pk; fx(pk, 1, from); fx(pk, 2, to); vu(pk, 3, 1); ln(pk, 4, data); fx(pk, 6, id); fx(pk, 7, rx);
  fl(pk, 8, -7.5f); vu(pk, 9, 3); vu(pk, 12, (uint64_t) (int64_t) -95); vu(pk, 15, 5);
  V fr; vu(fr, 1, 42); ln(fr, 2, pk); return fr;
}

int main() {
  { // a text packet
    V fr = textPacket(0x11223344, BROADCAST, 0xAABBCCDD, 1800000000u, "hello mesh");
    FromRadio o; CHECK(parseFromRadio(fr.data(), fr.size(), o), "text parse");
    CHECK(o.kind == K_PACKET && o.id == 42, "text kind");
    CHECK(o.packet.from == 0x11223344 && o.packet.to == BROADCAST, "text from/to");
    CHECK(o.packet.id == 0xAABBCCDD && o.packet.rxTime == 1800000000u, "text id/rx");
    CHECK(o.packet.channel == 1 && o.packet.port == PORT_TEXT, "text channel/port");
    CHECK(o.packet.hasSnr && o.packet.snr == -7.5f && o.packet.rssi == -95, "text snr/rssi");
    CHECK(o.packet.hopLimit == 3 && o.packet.hopStart == 5, "text hops");
    CHECK(o.packet.payloadLen == 10 && memcmp(o.packet.payload, "hello mesh", 10) == 0, "text payload");
  }
  { // a rx_time beyond 2038 as an unsigned 32-bit value stays unsigned
    V fr = textPacket(1, 2, 3, 4000000000u, "x");
    FromRadio o; parseFromRadio(fr.data(), fr.size(), o);
    CHECK(o.packet.rxTime == 4000000000u, "rx_time unsigned");
  }
  { // routing ack / error
    V rt; vu(rt, 3, ROUTE_NONE);
    V data; vu(data, 1, PORT_ROUTING); ln(data, 2, rt); fx(data, 6, 0xCAFE0001);
    V pk; fx(pk, 1, 7); fx(pk, 2, 8); ln(pk, 4, data);
    V fr; ln(fr, 2, pk);
    FromRadio o; CHECK(parseFromRadio(fr.data(), fr.size(), o), "routing parse");
    CHECK(o.packet.port == PORT_ROUTING && o.packet.requestId == 0xCAFE0001 && o.packet.routeError == 0, "routing ack");
    V rt2; vu(rt2, 3, ROUTE_MAX_RETRANSMIT);
    V d2; vu(d2, 1, PORT_ROUTING); ln(d2, 2, rt2); fx(d2, 6, 5);
    V p2; ln(p2, 4, d2); V f2; ln(f2, 2, p2);
    FromRadio o2; parseFromRadio(f2.data(), f2.size(), o2);
    CHECK(o2.packet.routeError == ROUTE_MAX_RETRANSMIT, "routing error");
    V d3; vu(d3, 1, PORT_ROUTING); fx(d3, 6, 5);          // empty Routing = NONE (proto3 default)
    V p3; ln(p3, 4, d3); V f3; ln(f3, 2, p3);
    FromRadio o3; parseFromRadio(f3.data(), f3.size(), o3);
    CHECK(o3.packet.routeError == 0, "routing empty = none");
  }
  { // node info with user, position, metrics, hops
    V user; ln(user, 1, str("!1b66d0e2")); ln(user, 2, str("Base Camp 1")); ln(user, 3, str("BC01")); vu(user, 5, 43); vu(user, 7, 2);
    V pos; fx(pos, 1, (uint32_t) 12345678); fx(pos, 2, (uint32_t) (int32_t) -98765432);
    V dm; vu(dm, 1, 82); fl(dm, 3, 12.34f); fl(dm, 4, 0.5f); vu(dm, 5, 90000);
    V ni; vu(ni, 1, 0x1b66d0e2); ln(ni, 2, user); ln(ni, 3, pos); fl(ni, 4, -9.0f); fx(ni, 5, 1700000000u); ln(ni, 6, dm); vu(ni, 9, 2);
    V fr; ln(fr, 4, ni);
    FromRadio o; CHECK(parseFromRadio(fr.data(), fr.size(), o), "node parse");
    CHECK(o.kind == K_NODE_INFO && o.node.num == 0x1b66d0e2, "node num");
    CHECK(!strcmp(o.node.longName, "Base Camp 1") && !strcmp(o.node.shortName, "BC01") && !strcmp(o.node.id, "!1b66d0e2"), "node names");
    CHECK(o.node.hw == 43 && o.node.role == 2, "node hw/role");
    CHECK(o.node.hasPos && o.node.latE7 == 12345678 && o.node.lonE7 == -98765432, "node pos");
    CHECK(o.node.battery == 82 && o.node.uptime == 90000, "node metrics");
    CHECK(o.node.chanUtil10 == 123 && o.node.airUtil10 == 5, "node util");
    CHECK(o.node.hasSnr && o.node.snr == -9.0f && o.node.lastHeard == 1700000000u && o.node.hops == 2, "node snr/heard/hops");
  }
  { // the favourite flag: set in a full record, absent means no
    V ni; vu(ni, 1, 7); vu(ni, 10, 1); V fr; ln(fr, 4, ni);
    FromRadio o; CHECK(parseFromRadio(fr.data(), fr.size(), o), "fav parse");
    CHECK(o.node.fav && o.node.favKnown, "favourite set");
    V nj; vu(nj, 1, 8); V fq; ln(fq, 4, nj);
    FromRadio p; parseFromRadio(fq.data(), fq.size(), p);
    CHECK(!p.node.fav && p.node.favKnown, "favourite absent = no");
    uint8_t adm[24]; size_t n = adminInt(adm, sizeof adm, 39, 0x1b66d0e2, nullptr, 0);
    CHECK(n > 2 && adm[0] == 0xB8 && adm[1] == 0x02, "set_favorite_node tag");
    n = adminInt(adm, sizeof adm, 39, 0xe2e53670u, nullptr, 0);
    CHECK(n == 2 + 5, "a node number with the top bit set is a 5-byte varint, not 10");
  }
  { // a long name is cut, not overrun
    std::string big(200, 'N'); V user; ln(user, 2, str(big.c_str()));
    V ni; vu(ni, 1, 5); ln(ni, 2, user); V fr; ln(fr, 4, ni);
    FromRadio o; CHECK(parseFromRadio(fr.data(), fr.size(), o), "long parse");
    CHECK(strlen(o.node.longName) == sizeof o.node.longName - 1, "long name cut");
  }
  { // my_info, config complete, channel, queue, metadata, rebooted
    V mi; vu(mi, 1, 0xDEADBEEF); V f1; ln(f1, 3, mi);
    FromRadio o; parseFromRadio(f1.data(), f1.size(), o); CHECK(o.kind == K_MY_INFO && o.myNum == 0xDEADBEEF, "my_info");
    V f2; vu(f2, 7, NONCE_ALL); FromRadio o2; parseFromRadio(f2.data(), f2.size(), o2);
    CHECK(o2.kind == K_CONFIG_DONE && o2.configId == NONCE_ALL, "config complete");
    V set; ln(set, 2, V{1}); ln(set, 3, str("LongFast")); V ch; vu(ch, 1, 0); ln(ch, 2, set); vu(ch, 3, 1);
    V f3; ln(f3, 10, ch); FromRadio o3; parseFromRadio(f3.data(), f3.size(), o3);
    CHECK(o3.kind == K_CHANNEL && !strcmp(o3.channel.name, "LongFast") && o3.channel.role == 1 && o3.channel.hasPsk, "channel");
    V qs; vu(qs, 1, 0); vu(qs, 2, 14); vu(qs, 3, 16); vu(qs, 4, 777); V f4; ln(f4, 11, qs);
    FromRadio o4; parseFromRadio(f4.data(), f4.size(), o4);
    CHECK(o4.kind == K_QUEUE && o4.queue.free == 14 && o4.queue.maxlen == 16 && o4.queue.packetId == 777, "queue");
    V md; ln(md, 1, str("2.7.15.abc")); V f5; ln(f5, 13, md);
    FromRadio o5; parseFromRadio(f5.data(), f5.size(), o5); CHECK(o5.kind == K_METADATA && !strcmp(o5.firmware, "2.7.15.abc"), "metadata");
    V f6; vu(f6, 8, 1); FromRadio o6; parseFromRadio(f6.data(), f6.size(), o6); CHECK(o6.kind == K_REBOOTED, "rebooted");
  }
  { // unknown fields are skipped
    V junk; vu(junk, 99, 5); fx(junk, 98, 1); ln(junk, 97, str("zz")); tag(junk, 96, 1); for (int i = 0; i < 8; i++) junk.push_back(0);
    V f; f.insert(f.end(), junk.begin(), junk.end()); vu(f, 7, 9);
    FromRadio o; CHECK(parseFromRadio(f.data(), f.size(), o) && o.kind == K_CONFIG_DONE, "skip unknown");
  }
  { // malformed input fails cleanly
    uint8_t bad1[] = { 0x12, 0x40, 0x01 };              // length runs past the end
    FromRadio o; CHECK(!parseFromRadio(bad1, sizeof bad1, o), "truncated length");
    uint8_t bad2[] = { 0x08, 0x80 };                    // varint never ends
    FromRadio o2; CHECK(!parseFromRadio(bad2, sizeof bad2, o2), "truncated varint");
    uint8_t bad3[] = { 0x0F };                          // wire type 7
    FromRadio o3; CHECK(!parseFromRadio(bad3, sizeof bad3, o3), "bad wire type");
    FromRadio o4; CHECK(parseFromRadio(nullptr, 0, o4) && o4.kind == K_NONE, "empty");
  }
  { // builders, read back through the parser's own rules
    uint8_t b[16]; size_t n = buildWantConfig(b, sizeof b, NONCE_ALL);
    CHECK(n == 2 && b[0] == 0x18 && b[1] == 5, "want_config bytes");
    n = buildWantConfig(b, sizeof b, NONCE_CONFIG_ONLY);
    CHECK(n == 4 && b[0] == 0x18, "want_config 69420");
    n = buildHeartbeat(b, sizeof b); CHECK(n == 2 && b[0] == 0x3A && b[1] == 0, "heartbeat bytes");
    n = buildDisconnect(b, sizeof b); CHECK(n == 2 && b[0] == 0x20 && b[1] == 1, "disconnect bytes");
    uint8_t t[300]; const char *msg = "ping from the Palm";
    n = buildText(t, sizeof t, 0x11223344, 0, 0x01020304, true, msg, strlen(msg));
    CHECK(n > 0 && t[0] == 0x0A, "text frame starts with packet");
    // decode it as a MeshPacket (ToRadio.packet is the same message type)
    size_t i = 1, inner = t[i++]; CHECK(inner == n - 2, "text inner length");
    Packet pk; (void) pk;
    V fr; fr.push_back(0x12); fr.insert(fr.end(), t + 1, t + n);    // re-wrap as FromRadio.packet
    FromRadio o; CHECK(parseFromRadio(fr.data(), fr.size(), o), "text round trip parse");
    CHECK(o.packet.to == 0x11223344 && o.packet.id == 0x01020304 && o.packet.wantAck && o.packet.port == PORT_TEXT, "text round trip");
    CHECK(o.packet.payloadLen == strlen(msg) && !memcmp(o.packet.payload, msg, strlen(msg)), "text round trip payload");
    CHECK(o.packet.hopLimit == 3 && o.packet.channel == 0, "text hop/channel");
    n = buildText(t, sizeof t, BROADCAST, 2, 9, false, msg, strlen(msg));
    V fr2; fr2.push_back(0x12); fr2.insert(fr2.end(), t + 1, t + n);
    FromRadio o2; parseFromRadio(fr2.data(), fr2.size(), o2);
    CHECK(o2.packet.to == BROADCAST && o2.packet.channel == 2 && !o2.packet.wantAck, "broadcast text");
    std::string full(233, 'a'); n = buildText(t, sizeof t, 1, 0, 1, true, full.c_str(), full.size());
    CHECK(n > 233 && n < 270, "233-byte text fits");
    CHECK(buildText(b, sizeof b, 1, 0, 1, true, full.c_str(), full.size()) == 0, "small buffer refused");
  }
  { // settings: a patched copy keeps the fields it was not asked to change
    const uint8_t lora[] = {0x08, 0x01, 0x38, 0x01, 0x40, 0x03, 0x78, 0x07};      // use_preset, region 1, hop 3, field 15 = 7
    uint8_t o[64];
    size_t n = patchLora(lora, sizeof lora, 4, 3, 5, -2, o, sizeof o);
    CHECK(n > 0, "patchLora built");
    LoraVals v; readLora(o, n, v);
    CHECK(v.region == 3 && v.hop == 5 && v.preset == 4 && v.txPower == -2 && v.usePreset, "patchLora values");
    bool kept = false; for (size_t i = 0; i + 1 < n; i++) if (o[i] == 0x78 && o[i + 1] == 0x07) kept = true;
    CHECK(kept, "patchLora keeps an unknown field");
    const uint8_t user[] = {0x0A, 0x02, '!', 'a', 0x12, 0x03, 'O', 'l', 'd', 0x1A, 0x01, 'O', 0x28, 0x2B};   // id, long, short, hw 43
    n = patchOwner(user, sizeof user, "New name", "NN", o, sizeof o);
    char ln[40], sn[8]; readOwner(o, n, ln, sizeof ln, sn, sizeof sn);
    CHECK(!strcmp(ln, "New name") && !strcmp(sn, "NN"), "patchOwner names");
    bool hw = false; for (size_t i = 0; i + 1 < n; i++) if (o[i] == 0x28 && o[i + 1] == 0x2B) hw = true;
    CHECK(hw, "patchOwner keeps hw_model");
    uint8_t a[32]; size_t al = adminFlag(a, sizeof a, 64, nullptr, 0);
    CHECK(al == 3 && a[0] == 0x80 && a[1] == 0x04 && a[2] == 1, "begin_edit_settings bytes");
    uint8_t pk[8]; size_t kl = 0; const uint8_t keyed[] = {0xAA, 0x06, 0x03, 1, 2, 3};
    CHECK(adminPasskey(keyed, sizeof keyed, pk, kl) && kl == 3 && pk[2] == 3, "session passkey read");
    uint8_t tr[200]; size_t tl = buildAdmin(tr, sizeof tr, 0x11223344, 7, a, al);
    V fr; fr.push_back(0x12); fr.insert(fr.end(), tr + 1, tr + tl);
    FromRadio ofr; CHECK(parseFromRadio(fr.data(), fr.size(), ofr) && ofr.packet.port == PORT_ADMIN && ofr.packet.to == 0x11223344, "admin packet parses");
  }
  { // MQTT module config: FromRadio.moduleConfig { mqtt = 1 }
    V m;
    vu(m, 1, 1); ln(m, 2, str("mqtt.example.org")); ln(m, 3, str("user1")); ln(m, 4, str("pw!"));
    vu(m, 5, 1); vu(m, 7, 1); ln(m, 8, str("msh/XX")); vu(m, 10, 1);
    V ms; vu(ms, 1, 3600); vu(ms, 2, 14); ln(m, 11, ms);                      // map_report_settings: must survive a patch
    V mc; ln(mc, 1, m);
    V fr; ln(fr, 9, mc);
    FromRadio o;
    CHECK(parseFromRadio(fr.data(), fr.size(), o) && o.kind == K_MQTT && o.mqttLen == m.size(), "moduleConfig mqtt parses");
    MqttVals v; readMqtt(o.mqttRaw, o.mqttLen, v);
    CHECK(v.enabled && v.encrypt && !v.json && v.tls && !v.proxy && v.mapRep, "mqtt flags");
    CHECK(!strcmp(v.address, "mqtt.example.org") && !strcmp(v.username, "user1") && !strcmp(v.password, "pw!") && !strcmp(v.root, "msh/XX"), "mqtt strings");
    v.enabled = false; v.proxy = true; strcpy(v.root, ""); strcpy(v.address, "other.example.org");
    uint8_t out[300]; size_t n = patchMqtt(o.mqttRaw, o.mqttLen, v, out, sizeof out);
    CHECK(n > 0, "patchMqtt writes");
    MqttVals w; readMqtt(out, n, w);
    CHECK(!w.enabled && w.proxy && w.encrypt && w.tls && !strcmp(w.address, "other.example.org") && w.root[0] == 0 && !strcmp(w.username, "user1"), "patched mqtt reads back");
    bool kept = false; { V pv(out, out + n); for (size_t i = 0; i + 1 < pv.size(); i++) if (pv[i] == 0x5A) kept = true; }   // field 11, length-delimited
    CHECK(kept, "map_report_settings kept");
    uint8_t adm[400]; uint8_t key[3] = {9, 8, 7};
    size_t al = adminSetModuleConfig(adm, sizeof adm, 1, out, n, key, 3);
    CHECK(al > n && adm[0] == 0x9A && adm[1] == 0x02, "set_module_config is field 35, length-delimited");
    CHECK(adminSetModuleConfig(adm, 20, 1, out, n, key, 3) == 0, "a too small buffer is refused");
  }
  printf(fails ? "%d FAILED\n" : "all passed\n", fails);
  return fails ? 1 : 0;
}
