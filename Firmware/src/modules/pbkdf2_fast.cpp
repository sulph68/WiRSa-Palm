// pbkdf2_fast.cpp - see pbkdf2_fast.h. Plain SHA-256 compression: each iteration is two
// compressions (inner and outer hash, both over a pad state computed once) instead of
// mbedtls re-keying the HMAC every time (four compressions plus the call overhead).
#include "pbkdf2_fast.h"
#include <string.h>

#ifdef ARDUINO
#include <esp_attr.h>
#include <hal/sha_hal.h>
#include <sha/sha_dma.h>
#define PBKDF2_HW 1
#define K_ATTR DRAM_ATTR
#define FAST_ATTR IRAM_ATTR
#else
#define K_ATTR
#define FAST_ATTR
#endif

namespace {

K_ATTR static const uint32_t K[64] = {
  0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
  0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
  0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
  0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
  0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
  0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
  0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
  0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

K_ATTR static const uint32_t IV[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                      0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};

static inline uint32_t ror(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

// state += compress(block); block = 16 big-endian words.
static void FAST_ATTR compress(uint32_t st[8], const uint32_t blk[16]) {
  uint32_t w[64];
  for (int i = 0; i < 16; i++) w[i] = blk[i];
  for (int i = 16; i < 64; i++) {
    uint32_t s0 = ror(w[i - 15], 7) ^ ror(w[i - 15], 18) ^ (w[i - 15] >> 3);
    uint32_t s1 = ror(w[i - 2], 17) ^ ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }
  uint32_t a = st[0], b = st[1], c = st[2], d = st[3], e = st[4], f = st[5], g = st[6], h = st[7];
  for (int i = 0; i < 64; i++) {
    uint32_t S1 = ror(e, 6) ^ ror(e, 11) ^ ror(e, 25);
    uint32_t t1 = h + S1 + ((e & f) ^ (~e & g)) + K[i] + w[i];
    uint32_t S0 = ror(a, 2) ^ ror(a, 13) ^ ror(a, 22);
    uint32_t t2 = S0 + ((a & b) ^ (a & c) ^ (b & c));
    h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
  }
  st[0] += a; st[1] += b; st[2] += c; st[3] += d; st[4] += e; st[5] += f; st[6] += g; st[7] += h;
}

static uint32_t be32(const uint8_t *p) {
  return ((uint32_t) p[0] << 24) | ((uint32_t) p[1] << 16) | ((uint32_t) p[2] << 8) | p[3];
}

// SHA-256 of a message of at most 119 bytes (one or two blocks) - enough for hashing a long key.
static void shaShort(const uint8_t *msg, size_t len, uint8_t out[32]) {
  uint32_t st[8], blk[16];
  uint8_t buf[128];
  memcpy(st, IV, sizeof(st));
  memset(buf, 0, sizeof(buf));
  memcpy(buf, msg, len);
  buf[len] = 0x80;
  size_t total = len < 56 ? 64 : 128;
  uint64_t bits = (uint64_t) len * 8;
  for (int i = 0; i < 8; i++) buf[total - 1 - i] = (uint8_t) (bits >> (8 * i));
  for (size_t off = 0; off < total; off += 64) {
    for (int i = 0; i < 16; i++) blk[i] = be32(buf + off + 4 * i);
    compress(st, blk);
  }
  for (int i = 0; i < 8; i++) {
    out[4 * i] = (uint8_t) (st[i] >> 24); out[4 * i + 1] = (uint8_t) (st[i] >> 16);
    out[4 * i + 2] = (uint8_t) (st[i] >> 8); out[4 * i + 3] = (uint8_t) st[i];
  }
}

static void FAST_ATTR iterSoft(const uint32_t ist[8], const uint32_t ost[8], uint32_t u[8], uint32_t t[8],
                               uint32_t count) {
  uint32_t st[8], b[16];
  b[8] = 0x80000000u;                       // message = 64-byte pad block + 32 bytes = 96 bytes
  for (int i = 9; i < 15; i++) b[i] = 0;
  b[15] = (64 + 32) * 8;
  for (uint32_t it = 0; it < count; it++) {
    memcpy(b, u, 32);
    memcpy(st, ist, sizeof(st));
    compress(st, b);                        // inner hash
    memcpy(b, st, 32);
    memcpy(st, ost, sizeof(st));
    compress(st, b);                        // outer hash = next U
    for (int i = 0; i < 8; i++) { u[i] = st[i]; t[i] ^= st[i]; }
  }
}

#ifdef PBKDF2_HW
// The engine takes a message block and a state as raw big-endian bytes read as words (byte-swapped
// relative to the SHA-256 words above); the engine lock is held by the caller.
static inline uint32_t sw(uint32_t x) { return __builtin_bswap32(x); }

static void iterHw(const uint32_t ist[8], const uint32_t ost[8], uint32_t u[8], uint32_t t[8],
                   uint32_t count) {
  uint32_t hi[8], ho[8], b[16], d[8];
  for (int i = 0; i < 8; i++) { hi[i] = sw(ist[i]); ho[i] = sw(ost[i]); }
  b[8] = sw(0x80000000u);
  for (int i = 9; i < 15; i++) b[i] = 0;
  b[15] = sw((64 + 32) * 8);
  for (int i = 0; i < 8; i++) b[i] = sw(u[i]);
  for (uint32_t it = 0; it < count; it++) {
    sha_hal_write_digest(SHA2_256, hi);
    sha_hal_hash_block(SHA2_256, b, 16, false);
    sha_hal_wait_idle();
    sha_hal_read_digest(SHA2_256, d);
    for (int i = 0; i < 8; i++) b[i] = d[i];
    sha_hal_write_digest(SHA2_256, ho);
    sha_hal_hash_block(SHA2_256, b, 16, false);
    sha_hal_wait_idle();
    sha_hal_read_digest(SHA2_256, d);
    for (int i = 0; i < 8; i++) { b[i] = d[i]; t[i] ^= sw(d[i]); }
  }
  for (int i = 0; i < 8; i++) u[i] = sw(b[i]);
}
#endif

}  // namespace

static bool gUsedHw = false;

bool pbkdf2UsedHardware() { return gUsedHw; }

void pbkdf2Sha256Iterate(const uint8_t *pw, size_t pwLen, const uint8_t u1[32],
                         uint32_t iterations, uint8_t out[32]) {
  gUsedHw = false;
  uint8_t key[64];
  memset(key, 0, sizeof(key));
  if (pwLen > 64) {
    // Only ever the master key (32 bytes) or an account password; a key over 64 bytes is
    // hashed first, per HMAC. Passwords over 119 bytes are not supported here.
    if (pwLen > 119) { memset(out, 0, 32); return; }
    shaShort(pw, pwLen, key);
  } else {
    memcpy(key, pw, pwLen);
  }

  uint32_t ist[8], ost[8], blk[16];
  memcpy(ist, IV, sizeof(ist));
  memcpy(ost, IV, sizeof(ost));
  uint8_t pad[64];
  for (int i = 0; i < 64; i++) pad[i] = key[i] ^ 0x36;
  for (int i = 0; i < 16; i++) blk[i] = be32(pad + 4 * i);
  compress(ist, blk);
  for (int i = 0; i < 64; i++) pad[i] = key[i] ^ 0x5c;
  for (int i = 0; i < 16; i++) blk[i] = be32(pad + 4 * i);
  compress(ost, blk);
  memset(key, 0, sizeof(key));
  memset(pad, 0, sizeof(pad));

  uint32_t u[8], t[8];
  for (int i = 0; i < 8; i++) t[i] = u[i] = be32(u1 + 4 * i);
  uint32_t left = iterations - 1;
#ifdef PBKDF2_HW
  // The SHA engine does one block in a few microseconds. It is checked against the software
  // path on the first iterations of this very job, and used only if both agree (the engine
  // is held for the whole run); otherwise software does all of it.
  if (left > 256 && (esp_sha_acquire_hardware(), true)) {
    uint32_t su[8], stt[8];
    memcpy(su, u, sizeof(su)); memcpy(stt, t, sizeof(stt));
    iterHw(ist, ost, su, stt, 64);
    iterSoft(ist, ost, u, t, 64);
    if (memcmp(su, u, sizeof(su)) == 0 && memcmp(stt, t, sizeof(stt)) == 0) {
      left -= 64;
      iterHw(ist, ost, u, t, left);
      left = 0;
      gUsedHw = true;
    } else {
      left -= 64;     // u, t are the software result
    }
    esp_sha_release_hardware();
  }
#endif
  iterSoft(ist, ost, u, t, left);
  for (int i = 0; i < 8; i++) {
    out[4 * i] = (uint8_t) (t[i] >> 24); out[4 * i + 1] = (uint8_t) (t[i] >> 16);
    out[4 * i + 2] = (uint8_t) (t[i] >> 8); out[4 * i + 3] = (uint8_t) t[i];
  }
  memset(u, 0, sizeof(u)); memset(t, 0, sizeof(t));
}
