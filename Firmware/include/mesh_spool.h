// mesh_spool.h - texts kept outside the PSRAM store: the record format shared by the SD files and the RTC ring.
// Plain C++, no Arduino (host-tested, test_host/mesh_spool_test.cpp).
//
// A record is  A5 5A | plen | payload | crc8(payload)  with the payload  from4 to4 rxTime4 packetId4 chan1 flags1 len1 text.
// The two marker bytes let a reader that lands in the middle of a file (or behind a damaged record) find the next one.
#ifndef MESH_SPOOL_H
#define MESH_SPOOL_H

#include <stddef.h>
#include <stdint.h>

namespace MeshSpool {

const size_t TEXT_MAX = 233, PAYLOAD_HDR = 19, REC_MAX = 2 + 1 + PAYLOAD_HDR + TEXT_MAX + 1;

struct Rec {
  uint32_t from = 0, to = 0, rxTime = 0, packetId = 0;
  uint8_t chan = 0, flags = 0, len = 0;
  char text[TEXT_MAX + 1] = "";
};

// Bytes written to out (0 if cap is too small or the text is too long).
size_t encode(const Rec &r, uint8_t *out, size_t cap);
// One record at p: its byte count, 0 when there is no valid record there.
size_t decode(const uint8_t *p, size_t n, Rec &out);

typedef void (*Fn)(const Rec &, void *);
// Every valid record in a block, skipping damage. Returns how many.
size_t scan(const uint8_t *p, size_t n, Fn fn, void *ctx);

// A ring of records in memory the caller owns (fast RTC memory, which a deep sleep leaves alone). When it is full the
// oldest records go, and they are counted. attach() makes sense of what is there after a wake and starts empty when it
// does not (first power-on, or a reset that cleared the memory).
class Ring {
 public:
  Ring(uint8_t *mem, size_t bytes);
  void attach();
  bool push(const Rec &r);
  size_t count() const;
  size_t forEach(Fn fn, void *ctx) const;     // oldest first
  void clear();
  uint16_t dropped() const;
  void clearDropped();
  size_t capacity() const;                     // bytes for records
  size_t freeBytes() const;                    // room left before the oldest record has to go

 private:
  struct Hdr { uint32_t magic; uint16_t head, used, cap, dropped; };
  Hdr *h_;
  uint8_t *data_;
  size_t cap_;
  void readAt(size_t off, uint8_t *dst, size_t n) const;
  void writeAt(size_t off, const uint8_t *src, size_t n);
  bool walk(size_t &recs) const;
};

}  // namespace MeshSpool

#endif
