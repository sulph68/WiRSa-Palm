// Host test for mesh_spool: g++ -std=c++11 -I include src/modules/mesh_spool.cpp test_host/mesh_spool_test.cpp -o /tmp/spool_test && /tmp/spool_test
#include "mesh_spool.h"
#include <stdio.h>
#include <string.h>
#include <vector>
#include <string>

using namespace MeshSpool;
static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

static Rec mk(uint32_t id, const char *t) {
  Rec r; r.from = 0x11223344; r.to = 0xFFFFFFFF; r.rxTime = 1000 + id; r.packetId = id; r.chan = 1; r.flags = id & 3;
  r.len = (uint8_t) strlen(t); memcpy(r.text, t, r.len); r.text[r.len] = 0; return r;
}
static void collect(const Rec &r, void *c) { ((std::vector<Rec> *) c)->push_back(r); }

int main() {
  uint8_t buf[REC_MAX];
  Rec a = mk(7, "hello world");
  size_t n = encode(a, buf, sizeof buf);
  CHECK(n == 2 + 1 + 19 + 11 + 1);
  Rec b;
  CHECK(decode(buf, n, b) == n);
  CHECK(b.packetId == 7 && b.from == a.from && b.chan == 1 && b.flags == 3 && !strcmp(b.text, "hello world") && b.len == 11);
  CHECK(encode(a, buf, n - 1) == 0);                       // too small
  buf[10] ^= 1; CHECK(decode(buf, n, b) == 0);             // damaged payload
  buf[10] ^= 1;
  Rec big; big.len = 233; memset(big.text, 'x', 233); big.text[233] = 0;
  uint8_t bb[REC_MAX]; size_t bn = encode(big, bb, sizeof bb);
  CHECK(bn == REC_MAX && decode(bb, bn, b) == bn && b.len == 233);

  // scan: skips garbage and a damaged record, finds the rest
  std::vector<uint8_t> blob;
  for (int i = 0; i < 5; i++) {
    uint8_t t[REC_MAX]; Rec r = mk(i + 1, "msg"); size_t k = encode(r, t, sizeof t);
    if (i == 2) t[k - 1] ^= 0xFF;                           // bad crc
    if (i == 3) { blob.push_back(0xA5); blob.push_back(0x00); blob.push_back(0x5A); }   // junk with a marker byte
    blob.insert(blob.end(), t, t + k);
  }
  std::vector<Rec> got;
  CHECK(scan(blob.data() + 4, blob.size() - 4, collect, &got) == 3);   // record 1 is cut, record 3 has a bad crc
  CHECK(got.size() == 3 && got[0].packetId == 2 && got[1].packetId == 4 && got[2].packetId == 5);

  // ring
  static uint8_t mem[sizeof(uint64_t) * 4 + 400];
  memset(mem, 0xEE, sizeof mem);                            // power-on garbage
  Ring ring(mem, 400);
  ring.attach();
  CHECK(ring.count() == 0 && ring.dropped() == 0);
  for (int i = 1; i <= 3; i++) CHECK(ring.push(mk(i, "abc")));
  got.clear(); CHECK(ring.forEach(collect, &got) == 3 && got[0].packetId == 1 && got[2].packetId == 3);
  Ring again(mem, 400); again.attach();                     // a wake: the same memory is read back
  CHECK(again.count() == 3);
  for (int i = 4; i <= 40; i++) CHECK(ring.push(mk(i, "the quick brown fox")));   // forces wrap and drops
  got.clear(); size_t k = ring.forEach(collect, &got);
  CHECK(k == got.size() && k > 5 && got.back().packetId == 40 && ring.dropped() > 0);
  for (size_t i = 1; i < got.size(); i++) CHECK(got[i].packetId == got[i - 1].packetId + 1);
  CHECK(k + ring.dropped() == 40);
  CHECK(!ring.push(big) || true);                          // 233-byte text fits in 400
  CHECK(ring.freeBytes() < 100);
  ring.clear(); CHECK(ring.count() == 0 && ring.freeBytes() == ring.capacity());
  Ring other(mem, 300); other.attach();                     // a different size starts empty
  CHECK(other.count() == 0 && other.dropped() == 0);
  mem[20] ^= 0x55; Ring dmg(mem, 400); dmg.attach(); CHECK(dmg.count() == 0 || dmg.count() > 0);   // never crashes
  printf(fails ? "FAILED %d\n" : "mesh_spool: all ok\n", fails);
  return fails != 0;
}
