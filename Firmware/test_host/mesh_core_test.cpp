// Host tests for mesh_core.cpp. From Firmware/:
//   g++ -std=c++17 -Wall -Wextra -Iinclude src/modules/mesh_pb.cpp src/modules/mesh_core.cpp test_host/mesh_core_test.cpp -o /tmp/mesh_core_test && /tmp/mesh_core_test
#include "mesh_core.h"
#include <cstdio>
#include <cstring>
using namespace MeshCore;
using namespace MeshPb;

static int fails = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL: %s\n", msg); fails++; } } while (0)

static FromRadio textFrom(uint32_t from, uint32_t to, uint32_t id, uint32_t rx, const char *t) {
  FromRadio f; f.kind = K_PACKET; f.packet.decoded = true; f.packet.port = PORT_TEXT; f.packet.from = from; f.packet.to = to;
  f.packet.id = id; f.packet.rxTime = rx; f.packet.payload = (const uint8_t *) t; f.packet.payloadLen = strlen(t); return f;
}
static FromRadio routing(uint32_t req, uint8_t err, uint32_t from = 0) {
  FromRadio f; f.kind = K_PACKET; f.packet.from = from; f.packet.decoded = true; f.packet.port = PORT_ROUTING; f.packet.requestId = req; f.packet.routeError = err; return f;
}

int main() {
  CHECK(init(), "init");
  seedIds(12345);
  { FromRadio f; f.kind = K_MY_INFO; f.myNum = 100; onFromRadio(f, 0);
    FromRadio n; n.kind = K_NODE_INFO; n.node.num = 200; strcpy(n.node.longName, "Anna"); n.node.battery = 91; onFromRadio(n, 0);
    FromRadio me; me.kind = K_NODE_INFO; me.node.num = 100; strcpy(me.node.longName, "Mine"); me.node.battery = 80; onFromRadio(me, 0);
    CHECK(nodeCount() == 2 && node(0)->num == 100 && !strcmp(node(0)->longName, "Mine"), "own node first");
    CHECK(myBattery() == 80, "my battery");
    FromRadio pos; pos.kind = K_NODE_INFO; pos.node.num = 200; pos.node.hasPos = true; pos.node.latE7 = 5; pos.node.lonE7 = 6; onFromRadio(pos, 0);
    CHECK(!strcmp(node(findNode(200))->longName, "Anna") && node(findNode(200))->hasPos && node(findNode(200))->battery == 91, "update keeps the name and battery");
    FromRadio ch; ch.kind = K_CHANNEL; ch.channel.index = 1; strcpy(ch.channel.name, "Hikers"); onFromRadio(ch, 0);
    CHECK(channel(1) && !strcmp(channel(1)->name, "Hikers") && !channel(0), "channel");
    FromRadio d; d.kind = K_CONFIG_DONE; onFromRadio(d, 0); CHECK(configDone(), "config done");
  }
  { onFromRadio(textFrom(200, BROADCAST, 7, 1000, "hi all"), 0);
    onFromRadio(textFrom(200, BROADCAST, 7, 1000, "hi all"), 0);          // same packet again
    onFromRadio(textFrom(200, 100, 8, 0, "direct"), 5000);
    CHECK(msgCount() == 2, "duplicates dropped");
    CHECK(msgAt(0)->rxTime == 1000 && !(msgAt(0)->flags & F_DM), "broadcast msg");
    CHECK(msgAt(1)->rxTime == 5000 && (msgAt(1)->flags & F_DM), "dm msg, rx time from now");
    CHECK(lastSeq() == 2 && firstAfter(0) == 0 && firstAfter(1) == 1 && firstAfter(2) == -1, "cursor");
    CHECK(firstAfter(999) == 0, "cursor from before a restart counts as everything");
    CHECK(incomingSince(1) == 1, "incomingSince");
  }
  { uint8_t out[300]; size_t n; uint32_t id = queueText(200, 0, "yo", 2, 77, 6000, out, sizeof out, n);
    CHECK(id && n > 4, "queued");
    CHECK(findByLocalId(77) && findByLocalId(77)->status == ST_SENDING, "sending");
    textWritten(id, true); CHECK(findByLocalId(77)->status == ST_SENT, "sent");
    onFromRadio(routing(id, 0, 300), 0); CHECK(findByLocalId(77)->status == ST_SENT_NOACK, "a relay's ack is not delivery");
    onFromRadio(routing(id, 0, 200), 0); CHECK(findByLocalId(77)->status == ST_ACKED, "acked by the recipient");
    onFromRadio(routing(id, 0, 300), 0); CHECK(findByLocalId(77)->status == ST_ACKED, "a later relay ack does not undo it");
    uint32_t id2 = queueText(BROADCAST, 1, "bye", 3, 78, 6001, out, sizeof out, n);
    textWritten(id2, true); onFromRadio(routing(id2, ROUTE_MAX_RETRANSMIT), 0);
    CHECK(findByLocalId(78)->status == ST_FAILED, "failed");
    uint32_t id3 = queueText(200, 0, "x", 1, 79, 6002, out, sizeof out, n); textWritten(id3, false);
    CHECK(findByLocalId(79)->status == ST_FAILED, "write failed");
    CHECK(id != id2 && id2 != id3, "ids differ");
    char big[300]; memset(big, 'a', sizeof big);
    CHECK(queueText(200, 0, big, 234, 80, 1, out, sizeof out, n) == 0, "too long refused");
    CHECK(queueText(200, 0, "", 0, 81, 1, out, sizeof out, n) == 0, "empty refused");
    onFromRadio(routing(0xDEAD, 0), 0);                                   // an ack for nothing we know
  }
  { resetSession(); CHECK(nodeCount() == 0 && !configDone() && msgCount() == 5, "reset keeps messages"); }
  { uint32_t gone = clearOlderThan(5500); CHECK(gone == 2 && msgCount() == 3, "clear older");
    CHECK(msgAt(0)->rxTime == 5000 || msgAt(0)->rxTime == 6000, "order kept");
    clearMessages(); CHECK(msgCount() == 0 && firstAfter(0) == -1, "clear all"); }
  { for (int i = 0; i < 250; i++) { char t[8]; snprintf(t, sizeof t, "m%d", i); onFromRadio(textFrom(1, BROADCAST, 1000 + i, 1, t), 0); }
    CHECK(msgCount() == MSG_CAP && !strcmp(msgAt(msgCount() - 1)->text, "m249") && !strcmp(msgAt(0)->text, "m50"), "ring drops the oldest"); }
  { clearMessages();
    FromRadio mi; mi.kind = K_MY_INFO; mi.myNum = 100; onFromRadio(mi, 0);
    FromRadio n2; n2.kind = K_NODE_INFO; n2.node.num = 200; strcpy(n2.node.shortName, "ANNA"); onFromRadio(n2, 0);
    uint8_t out[400]; size_t n;
    // an error code reaches the stored message: from the routing answer and from the node's queue status
    uint32_t a = queueText(200, 0, "hi", 2, 90, 7000, out, sizeof out, n); textWritten(a, true);
    onFromRadio(routing(a, 33), 0);
    CHECK(findByLocalId(90)->status == ST_FAILED && findByLocalId(90)->err == 33, "routing error kept");
    // a missing key: the first time the message waits for a key exchange and is sent again
    uint32_t b = queueText(200, 0, "hi", 2, 91, 7001, out, sizeof out, n);
    { FromRadio q; q.kind = K_QUEUE; q.queue.res = 39; q.queue.packetId = b; onFromRadio(q, 0); }
    CHECK(findByLocalId(91)->status == ST_KEYS && findByLocalId(91)->err == 39, "missing key: waits for the exchange");
    { uint32_t to; uint8_t ch;
      CHECK(keyJob(to, ch) && to == 200 && !keyJob(to, ch), "one key job");
      uint32_t kid = keyRequest(200, 0, out, sizeof out, n); CHECK(kid && n > 8, "node info request built");
      CHECK(!keyReplied(), "no answer yet");
      { FromRadio f; f.kind = K_PACKET; f.packet.decoded = true; f.packet.port = PORT_NODEINFO; f.packet.from = 200; onFromRadio(f, 0); }
      CHECK(keyReplied(), "answer seen");
      uint32_t pid = 0; uint32_t nid = keyResend(out, sizeof out, n, pid);
      CHECK(nid && nid == pid && n > 8 && findByLocalId(91)->status == ST_SENDING && findByLocalId(91)->packetId == nid, "sent again with a new id");
      onFromRadio(routing(nid, 39), 0);
      CHECK(findByLocalId(91)->status == ST_FAILED && findByLocalId(91)->err == 39, "fails the second time");
      CHECK(!keyJob(to, ch), "no second job"); }
    uint32_t b2 = queueText(200, 0, "hi", 2, 95, 7005, out, sizeof out, n);
    onFromRadio(routing(b2, 35), 0);
    { uint32_t to; uint8_t ch; CHECK(keyJob(to, ch), "they lack our key: a job too"); keyGiveUp();
      CHECK(findByLocalId(95)->status == ST_FAILED, "given up: it fails"); }
    uint32_t b3 = queueText(BROADCAST, 0, "hi", 2, 96, 7006, out, sizeof out, n);
    onFromRadio(routing(b3, 39), 0);
    CHECK(findByLocalId(96)->status == ST_FAILED, "a channel message has no key exchange");
    uint32_t c = queueText(200, 0, "hi", 2, 92, 7002, out, sizeof out, n); textWritten(c, false);
    CHECK(findByLocalId(92)->err == ERR_WRITE, "write error code");
    // requests
    CHECK(queueRequest(REQ_TRACE, BROADCAST, 0, 0, 0, "", 93, 7003, out, sizeof out, n) == 0, "no traceroute to everyone");
    uint32_t t = queueRequest(REQ_TRACE, 200, 0, 0, 0, "", 94, 7004, out, sizeof out, n);
    CHECK(t && n > 8 && !strcmp(findByLocalId(94)->text, "[Traceroute]"), "traceroute queued");
    { uint8_t rd[24]; size_t rn = 0;                               // the reply: route 100 -> 300 -> 200, back 400
      uint32_t hop = 300; rd[rn++] = 0x0A; rd[rn++] = 4; memcpy(rd + rn, &hop, 4); rn += 4;
      hop = 400; rd[rn++] = 0x1A; rd[rn++] = 4; memcpy(rd + rn, &hop, 4); rn += 4;
      FromRadio f; f.kind = K_PACKET; f.packet.decoded = true; f.packet.port = PORT_TRACEROUTE; f.packet.from = 200; f.packet.to = 100;
      f.packet.id = 55; f.packet.requestId = t; f.packet.payload = rd; f.packet.payloadLen = rn; onFromRadio(f, 7100);
      CHECK(findByLocalId(94)->status == ST_ACKED, "a reply acks the request");
      const Msg *m = msgAt(msgCount() - 1);
      CHECK((m->flags & F_DM) && m->from == 200 && !strncmp(m->text, "Route: Me > !0000012c > ANNA; back: ANNA > !00000190 > Me", 60), m->text); }
    uint32_t p = queueRequest(REQ_SHARE_POS, BROADCAST, 1, 13521000, 103819000, "", 95, 7005, out, sizeof out, n);
    CHECK(p && !strncmp(findByLocalId(95)->text, "[Shared position 1.35210, 10.38190", 33) , findByLocalId(95)->text);
    CHECK(queueRequest(REQ_WAYPOINT, BROADCAST, 0, 1, 2, "Camp", 96, 7006, out, sizeof out, n) && strstr(findByLocalId(96)->text, "Camp"), "waypoint");
    CHECK(queueRequest(99, 200, 0, 0, 0, "", 97, 7007, out, sizeof out, n) == 0, "unknown kind refused");
    // telemetry reply
    uint32_t q = queueRequest(REQ_TELEMETRY, 200, 0, 0, 0, "", 98, 7008, out, sizeof out, n);
    { uint8_t rd[] = {0x12, 0x04, 0x08, 87, 0x28, 0x3C};            // device_metrics { battery 87, uptime 60 }
      FromRadio f; f.kind = K_PACKET; f.packet.decoded = true; f.packet.port = PORT_TELEMETRY; f.packet.from = 200; f.packet.to = 100;
      f.packet.id = 56; f.packet.requestId = q; f.packet.payload = rd; f.packet.payloadLen = sizeof rd; onFromRadio(f, 7200);
      const Msg *m = msgAt(msgCount() - 1);
      CHECK(strstr(m->text, "battery 87%") && strstr(m->text, "up 0h 1m"), m->text); }
    // an unasked position broadcast says nothing
    { uint8_t rd[] = {0x0D, 1, 0, 0, 0, 0x15, 2, 0, 0, 0}; int before = msgCount();
      FromRadio f; f.kind = K_PACKET; f.packet.decoded = true; f.packet.port = PORT_POSITION; f.packet.from = 200; f.packet.id = 57;
      f.packet.payload = rd; f.packet.payloadLen = sizeof rd; onFromRadio(f, 7300); CHECK(msgCount() == before, "unasked position ignored"); }
  }
  { // importText: a text kept elsewhere comes back once
    clearMessages();
    CHECK(importText(900, 0xFFFFFFFFu, 5000, 77, 1, 0, "back again", 10), "import adds");
    CHECK(!importText(900, 0xFFFFFFFFu, 5000, 77, 1, 0, "back again", 10), "same id is not doubled");
    CHECK(!importText(900, 0xFFFFFFFFu, 5000, 0, 1, 0, "back again", 10), "same time and text is not doubled");
    CHECK(importText(900, 0xFFFFFFFFu, 5001, 0, 1, 0, "back again", 10), "another time is a new text");
    CHECK(msgCount() == 2 && msgAt(0)->seq < msgAt(1)->seq && !strcmp(msgAt(0)->text, "back again"), "import order and seq");
    setSeqBase(1000000); clearMessages();
    importText(1, 0, 9, 1, 0, 0, "x", 1);
    CHECK(msgAt(0)->seq == 1000001 && lastSeq() == 1000001, "seq base");
  }
  { // two nodes hear the same packet: after the node shown changes, the second one's copy is new
    clearMessages(); resetSeen();
    CHECK(importText(300, 0xFFFFFFFFu, 6000, 88, 1, 0, "heard twice", 11), "first node takes it");
    CHECK(!importText(300, 0xFFFFFFFFu, 6000, 88, 1, 0, "heard twice", 11), "the same node does not again");
    clearMessages(); resetSeen();
    CHECK(importText(300, 0xFFFFFFFFu, 6000, 88, 1, 0, "heard twice", 11), "the other node takes it too");
  }
  { // names: the sender of a text without a name is asked once
    resetSeen(); clearMessages();
    uint8_t pk[256]; size_t pn = 0; uint32_t to = 0;
    CHECK(!nameRequest(1000, 21600000UL, to, pk, sizeof pk, pn), "no texts, nobody to ask");
    importText(5001, 0xFFFFFFFFu, 7000, 11, 1, 0, "hi", 2);
    CHECK(nameRequest(1000, 21600000UL, to, pk, sizeof pk, pn) && to == 5001 && pn > 0, "an unnamed sender is asked");
    CHECK(!nameRequest(2000, 21600000UL, to, pk, sizeof pk, pn), "not asked again within the time");
    CHECK(nameRequest(1000 + 21600000UL, 21600000UL, to, pk, sizeof pk, pn) && to == 5001, "asked again after the time");
    importText(5002, 0xFFFFFFFFu, 7001, 12, 1, 0, "yo", 2);
    CHECK(nameRequest(1000 + 21600000UL + 5, 21600000UL, to, pk, sizeof pk, pn) && to == 5002, "the newest sender not asked lately comes first");
    resetSeen();
    CHECK(nameRequest(5, 21600000UL, to, pk, sizeof pk, pn), "another node shown: asked afresh");
  }
  printf(fails ? "%d FAILED\n" : "all passed\n", fails);
  return fails ? 1 : 0;
}
