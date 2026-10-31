// mesh_spool.cpp - see mesh_spool.h.
#include "mesh_spool.h"
#include <string.h>

namespace MeshSpool {
namespace {

const uint8_t M0 = 0xA5, M1 = 0x5A;
const uint32_t RING_MAGIC = 0x4D535031u;      // "MSP1"

uint8_t crc8(const uint8_t *p, size_t n) {
  uint8_t c = 0;
  while (n--) {
    c ^= *p++;
    for (int i = 0; i < 8; i++) c = (c & 0x80) ? (uint8_t) ((c << 1) ^ 0x07) : (uint8_t) (c << 1);
  }
  return c;
}

void put32(uint8_t *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }
uint32_t get32(const uint8_t *p) { return ((uint32_t) p[0] << 24) | ((uint32_t) p[1] << 16) | ((uint32_t) p[2] << 8) | p[3]; }

}  // namespace

size_t encode(const Rec &r, uint8_t *out, size_t cap) {
  size_t len = r.len;
  if (len > TEXT_MAX) return 0;
  size_t plen = PAYLOAD_HDR + len, total = 2 + 1 + plen + 1;
  if (cap < total) return 0;
  out[0] = M0; out[1] = M1; out[2] = (uint8_t) plen;
  uint8_t *p = out + 3;
  put32(p, r.from); put32(p + 4, r.to); put32(p + 8, r.rxTime); put32(p + 12, r.packetId);
  p[16] = r.chan; p[17] = r.flags; p[18] = (uint8_t) len;
  memcpy(p + PAYLOAD_HDR, r.text, len);
  out[3 + plen] = crc8(p, plen);
  return total;
}

size_t decode(const uint8_t *p, size_t n, Rec &out) {
  if (n < 4 || p[0] != M0 || p[1] != M1) return 0;
  size_t plen = p[2];
  if (plen < PAYLOAD_HDR || n < 3 + plen + 1) return 0;
  const uint8_t *q = p + 3;
  if (crc8(q, plen) != q[plen]) return 0;
  size_t len = q[18];
  if (len != plen - PAYLOAD_HDR || len > TEXT_MAX) return 0;
  out.from = get32(q); out.to = get32(q + 4); out.rxTime = get32(q + 8); out.packetId = get32(q + 12);
  out.chan = q[16]; out.flags = q[17]; out.len = (uint8_t) len;
  memcpy(out.text, q + PAYLOAD_HDR, len);
  out.text[len] = 0;
  return 3 + plen + 1;
}

size_t scan(const uint8_t *p, size_t n, Fn fn, void *ctx) {
  size_t i = 0, found = 0;
  Rec r;
  while (i + 4 <= n) {
    size_t used = (p[i] == M0 && p[i + 1] == M1) ? decode(p + i, n - i, r) : 0;
    if (used) { fn(r, ctx); found++; i += used; }
    else i++;
  }
  return found;
}

// ---- ring --------------------------------------------------------------------------------------------------------

Ring::Ring(uint8_t *mem, size_t bytes) {
  h_ = (Hdr *) mem;
  data_ = mem + sizeof(Hdr);
  cap_ = bytes > sizeof(Hdr) ? bytes - sizeof(Hdr) : 0;
  if (cap_ > 0xFFFF) cap_ = 0xFFFF;
}

void Ring::readAt(size_t off, uint8_t *dst, size_t n) const {
  for (size_t i = 0; i < n; i++) dst[i] = data_[(off + i) % cap_];
}

void Ring::writeAt(size_t off, const uint8_t *src, size_t n) {
  for (size_t i = 0; i < n; i++) data_[(off + i) % cap_] = src[i];
}

// Walks the stored records; false if the chain does not add up to exactly `used` bytes.
bool Ring::walk(size_t &recs) const {
  size_t off = h_->head, left = h_->used;
  uint8_t tmp[REC_MAX];
  recs = 0;
  while (left) {
    uint8_t hd[3];
    if (left < 4) return false;
    readAt(off, hd, 3);
    if (hd[0] != M0 || hd[1] != M1) return false;
    size_t total = 3 + (size_t) hd[2] + 1;
    if (total > left || total > sizeof tmp) return false;
    readAt(off, tmp, total);
    Rec r;
    if (decode(tmp, total, r) != total) return false;
    off = (off + total) % cap_; left -= total; recs++;
  }
  return true;
}

void Ring::clear() {
  h_->magic = RING_MAGIC; h_->head = 0; h_->used = 0; h_->cap = (uint16_t) cap_;
}

void Ring::attach() {
  size_t n;
  if (!cap_) return;
  if (h_->magic != RING_MAGIC || h_->cap != cap_ || h_->head >= cap_ || h_->used > cap_ || !walk(n)) {
    clear();
    h_->dropped = 0;
  }
}

bool Ring::push(const Rec &r) {
  uint8_t tmp[REC_MAX];
  size_t n = encode(r, tmp, sizeof tmp);
  if (!n || n > cap_) return false;
  while (cap_ - h_->used < n) {                        // make room: the oldest record goes
    uint8_t hd[3];
    readAt(h_->head, hd, 3);
    size_t total = 3 + (size_t) hd[2] + 1;
    h_->head = (uint16_t) ((h_->head + total) % cap_);
    h_->used = (uint16_t) (h_->used - total);
    if (h_->dropped != 0xFFFF) h_->dropped++;
  }
  writeAt((h_->head + h_->used) % cap_, tmp, n);
  h_->used = (uint16_t) (h_->used + n);
  return true;
}

size_t Ring::forEach(Fn fn, void *ctx) const {
  size_t off = h_->head, left = h_->used, k = 0;
  uint8_t tmp[REC_MAX];
  while (left >= 4) {
    uint8_t hd[3];
    readAt(off, hd, 3);
    size_t total = 3 + (size_t) hd[2] + 1;
    if (total > left || total > sizeof tmp) break;
    readAt(off, tmp, total);
    Rec r;
    if (decode(tmp, total, r) != total) break;
    fn(r, ctx); k++;
    off = (off + total) % cap_; left -= total;
  }
  return k;
}

size_t Ring::count() const { size_t n = 0; walk(n); return n; }
uint16_t Ring::dropped() const { return h_->dropped; }
void Ring::clearDropped() { h_->dropped = 0; }
size_t Ring::capacity() const { return cap_; }
size_t Ring::freeBytes() const { return cap_ - h_->used; }

}  // namespace MeshSpool
