// Host test for relay_proto: the frame layer every Palm-facing mode stands on (Protocol-wire.md sections 2 to 5). The Arduino core and mbedtls are stand-ins
// (test_host/fake): a serial port made of two queues, a clock that moves only when the code waits, a real AES-128, and a fake RSA that "decrypts" by taking
// the first 16 bytes. So this tests the framing, the key-exchange state machine, the encrypted-frame layout and the acknowledged-frame rules, not RSA itself.
//   g++ -std=c++17 -Wall -Wextra -Itest_host/fake -Iinclude src/modules/relay_proto.cpp test_host/relay_proto_test.cpp -o /tmp/relay_proto_test && /tmp/relay_proto_test
#include "relay_proto.h"
#include "log.h"
#include <mbedtls/aes.h>
#include <mbedtls/rsa.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>
#include <stdio.h>
#include <string.h>
#include <vector>

using namespace RelayProto;
typedef std::vector<uint8_t> Bytes;

// ---- the stand-ins ---------------------------------------------------------------------------------------------------------------------------------------
unsigned long gFakeNow = 0;
uint8_t gLogLevel = LOG_INFO;
bool usbDebug = true;
static int gWarnings = 0;
void logPrintf(uint8_t level, const char *, ...) { if (level == LOG_WARN) gWarnings++; }

static uint8_t sbox[256];
static void sboxInit() {
  if (sbox[0]) return;
  uint8_t p = 1, q = 1;
  do {
    p = (uint8_t) (p ^ (p << 1) ^ ((p & 0x80) ? 0x1B : 0));
    q ^= (uint8_t) (q << 1); q ^= (uint8_t) (q << 2); q ^= (uint8_t) (q << 4);
    if (q & 0x80) q ^= 0x09;
    uint8_t x = (uint8_t) (q ^ ((q << 1) | (q >> 7)) ^ ((q << 2) | (q >> 6)) ^ ((q << 3) | (q >> 5)) ^ ((q << 4) | (q >> 4)));
    sbox[p] = x ^ 0x63;
  } while (p != 1);
  sbox[0] = 0x63;
}
static uint8_t xt(uint8_t x) { return (uint8_t) ((x << 1) ^ ((x >> 7) * 0x1B)); }
void mbedtls_aes_init(mbedtls_aes_context *c) { memset(c, 0, sizeof *c); }
void mbedtls_aes_free(mbedtls_aes_context *c) { memset(c, 0, sizeof *c); }
int mbedtls_aes_setkey_enc(mbedtls_aes_context *c, const unsigned char *key, unsigned int bits) {
  if (bits != 128) return -1;
  sboxInit();
  memcpy(c->rk, key, 16);
  uint8_t rcon = 1;
  for (int i = 16; i < 176; i += 4) {
    uint8_t t[4]; memcpy(t, c->rk + i - 4, 4);
    if (i % 16 == 0) {
      uint8_t u = t[0]; t[0] = sbox[t[1]] ^ rcon; t[1] = sbox[t[2]]; t[2] = sbox[t[3]]; t[3] = sbox[u];
      rcon = xt(rcon);
    }
    for (int j = 0; j < 4; j++) c->rk[i + j] = c->rk[i - 16 + j] ^ t[j];
  }
  return 0;
}
int mbedtls_aes_crypt_ecb(mbedtls_aes_context *c, int, const unsigned char in[16], unsigned char out[16]) {
  uint8_t s[16];
  for (int i = 0; i < 16; i++) s[i] = in[i] ^ c->rk[i];
  for (int r = 1; r <= 10; r++) {
    uint8_t t[16];
    for (int i = 0; i < 16; i++) t[i] = sbox[s[i]];
    for (int col = 0; col < 4; col++) for (int row = 0; row < 4; row++) s[col * 4 + row] = t[((col + row) % 4) * 4 + row];   // ShiftRows
    if (r < 10) {
      for (int col = 0; col < 4; col++) {
        uint8_t *a = s + col * 4, a0 = a[0], a1 = a[1], a2 = a[2], a3 = a[3];
        a[0] = xt(a0) ^ (xt(a1) ^ a1) ^ a2 ^ a3;
        a[1] = a0 ^ xt(a1) ^ (xt(a2) ^ a2) ^ a3;
        a[2] = a0 ^ a1 ^ xt(a2) ^ (xt(a3) ^ a3);
        a[3] = (xt(a0) ^ a0) ^ a1 ^ a2 ^ xt(a3);
      }
    }
    for (int i = 0; i < 16; i++) s[i] ^= c->rk[r * 16 + i];
  }
  memcpy(out, s, 16);
  return 0;
}
int mbedtls_aes_crypt_ctr(mbedtls_aes_context *c, size_t length, size_t *nc_off, unsigned char ctr[16], unsigned char stream[16],
                          const unsigned char *in, unsigned char *out) {
  size_t n = *nc_off;
  for (size_t i = 0; i < length; i++) {
    if (n == 0) {
      mbedtls_aes_crypt_ecb(c, MBEDTLS_AES_ENCRYPT, ctr, stream);
      for (int k = 15; k >= 0; k--) if (++ctr[k] != 0) break;
    }
    out[i] = in[i] ^ stream[n];
    n = (n + 1) & 15;
  }
  *nc_off = n;
  return 0;
}

// Modulus N = 2^1024 - 3, so R^3 mod N = 27 (R = 2^1024 = 3 mod N): the test can check what computeR3 sends.
static bool gRsaDecryptFails = false;
void mbedtls_rsa_init(mbedtls_rsa_context *, int, int) {}
void mbedtls_rsa_free(mbedtls_rsa_context *) {}
int mbedtls_rsa_gen_key(mbedtls_rsa_context *, int (*)(void *, unsigned char *, size_t), void *, unsigned int, int) { return 0; }
size_t mbedtls_rsa_get_len(const mbedtls_rsa_context *) { return 128; }
int mbedtls_rsa_export_raw(const mbedtls_rsa_context *, unsigned char *N, size_t N_len, unsigned char *, size_t, unsigned char *, size_t,
                           unsigned char *, size_t, unsigned char *, size_t) {
  memset(N, 0xFF, N_len); N[N_len - 1] = 0xFD; return 0;
}
int mbedtls_rsa_pkcs1_decrypt(mbedtls_rsa_context *, int (*)(void *, unsigned char *, size_t), void *, int, size_t *olen,
                              const unsigned char *in, unsigned char *out, size_t max) {
  if (gRsaDecryptFails || max < 16) return -1;
  memcpy(out, in, 16); *olen = 16; return 0;     // the "ciphertext" is the secret itself
}
void mbedtls_entropy_init(mbedtls_entropy_context *) {}
int mbedtls_entropy_func(void *, unsigned char *, size_t) { return 0; }
void mbedtls_ctr_drbg_init(mbedtls_ctr_drbg_context *c) { c->state = 0; }
int mbedtls_ctr_drbg_seed(mbedtls_ctr_drbg_context *, int (*)(void *, unsigned char *, size_t), void *, const unsigned char *, size_t) { return 0; }
int mbedtls_ctr_drbg_random(void *, unsigned char *out, size_t n) { for (size_t i = 0; i < n; i++) out[i] = (uint8_t) (0xA0 + i); return 0; }

// ---- helpers -----------------------------------------------------------------------------------------------------------------------------------------------
static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

static HardwareSerial io;

static void reset(bool ctl) {
  gFakeNow = 0; gWarnings = 0; gRsaDecryptFails = false;
  io.rx.clear(); io.tx.clear(); io.onFlush = nullptr;
  secureReset();
  setControlLink(ctl);
}
static Bytes plain(uint8_t type, const Bytes &body) {
  Bytes f; f.push_back(type); f.push_back((uint8_t) (body.size() >> 8)); f.push_back((uint8_t) body.size());
  uint8_t chk = f[0] ^ f[1] ^ f[2];
  for (uint8_t b : body) { f.push_back(b); chk ^= b; }
  f.push_back(chk);
  return f;
}
static void feed(const Bytes &b) { io.rx.insert(io.rx.end(), b.begin(), b.end()); }
static Bytes seq(int n, int from = 0) { Bytes b; for (int i = 0; i < n; i++) b.push_back((uint8_t) (from + i)); return b; }
static void aesEcb(const uint8_t key[16], const uint8_t in[16], uint8_t out[16]) {
  mbedtls_aes_context c; mbedtls_aes_init(&c); mbedtls_aes_setkey_enc(&c, key, 128); mbedtls_aes_crypt_ecb(&c, MBEDTLS_AES_ENCRYPT, in, out);
}
// The keystream of Protocol-wire.md section 4, worked out here from the spec and not by the code under test.
static Bytes keystream(const uint8_t key[16], uint8_t dir, uint32_t sq, size_t len) {
  Bytes ks;
  for (size_t blk = 0; ks.size() < len; blk++) {
    uint8_t ctr[16] = {0}, out[16];
    ctr[0] = dir; ctr[4] = (uint8_t) (sq >> 24); ctr[5] = (uint8_t) (sq >> 16); ctr[6] = (uint8_t) (sq >> 8); ctr[7] = (uint8_t) sq;
    ctr[14] = (uint8_t) (blk >> 8); ctr[15] = (uint8_t) blk;
    aesEcb(key, ctr, out);
    ks.insert(ks.end(), out, out + 16);
  }
  ks.resize(len);
  return ks;
}
static Bytes encFrame(const uint8_t key[16], uint8_t dir, uint32_t sq, uint8_t type, const Bytes &body) {
  Bytes ks = keystream(key, dir, sq, body.size());
  Bytes f; size_t wl = body.size() + 4;
  f.push_back((uint8_t) (type | 0x80)); f.push_back((uint8_t) (wl >> 8)); f.push_back((uint8_t) wl);
  f.push_back((uint8_t) (sq >> 24)); f.push_back((uint8_t) (sq >> 16)); f.push_back((uint8_t) (sq >> 8)); f.push_back((uint8_t) sq);
  for (size_t i = 0; i < body.size(); i++) f.push_back(body[i] ^ ks[i]);
  uint8_t chk = 0; for (uint8_t b : f) chk ^= b;
  f.push_back(chk);
  return f;
}
// One frame out of the bytes WiRSa wrote, from pos on.
static bool nextTx(size_t &pos, bool lead, uint8_t &type, Bytes &body, uint8_t &chkOk) {
  const Bytes &t = io.tx;
  if (lead) { if (pos >= t.size() || t[pos] != 0x16) return false; pos++; }
  if (pos + 3 > t.size()) return false;
  type = t[pos]; size_t n = ((size_t) t[pos + 1] << 8) | t[pos + 2];
  if (pos + 4 + n > t.size()) return false;
  uint8_t chk = 0; for (size_t i = 0; i < 3 + n; i++) chk ^= t[pos + i];
  body.assign(t.begin() + pos + 3, t.begin() + pos + 3 + n);
  chkOk = (chk == t[pos + 3 + n]);
  pos += 4 + n;
  return true;
}
static size_t countFrames(bool lead) {
  size_t pos = 0, n = 0; uint8_t t, ok; Bytes b;
  while (nextTx(pos, lead, t, b, ok)) n++;
  return n;
}

// ---- the tests ---------------------------------------------------------------------------------------------------------------------------------------------
static void testAes() {
  const uint8_t key[16] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
  const uint8_t pt[16] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
  const uint8_t want[16] = {0x69, 0xC4, 0xE0, 0xD8, 0x6A, 0x7B, 0x04, 0x30, 0xD8, 0xCD, 0xB7, 0x80, 0x70, 0xB4, 0xC5, 0x5A};   // FIPS-197 C.1
  uint8_t out[16];
  aesEcb(key, pt, out);
  CHECK(memcmp(out, want, 16) == 0);
}

static void testPlainFrames() {
  reset(true);
  uint8_t body[64]; size_t n = 0; uint8_t type = 0;
  // a frame as WiRSa writes it: lead byte, type, u16 length, body, XOR checksum
  const uint8_t b3[3] = {1, 2, 3};
  writeFrame(io, 0x22, b3, 3);
  const uint8_t want[] = {0x16, 0x22, 0x00, 0x03, 1, 2, 3, (uint8_t) (0x22 ^ 0x03 ^ 1 ^ 2 ^ 3)};
  CHECK(io.tx.size() == sizeof want && memcmp(io.tx.data(), want, sizeof want) == 0);
  io.tx.clear();
  writeFrame(io, 0x05, nullptr, 0);
  CHECK(io.tx.size() == 5 && io.tx[1] == 0x05 && io.tx[2] == 0 && io.tx[3] == 0 && io.tx[4] == 0x05);

  // reading: a good frame
  feed(plain(0x30, seq(5, 10)));
  CHECK(readFrame(io, type, body, sizeof body, n, 50, 20) && type == 0x30 && n == 5 && body[0] == 10 && body[4] == 14);
  // a wrong checksum is dropped
  Bytes bad = plain(0x30, seq(5)); bad.back() ^= 0xFF;
  feed(bad);
  int w = gWarnings;
  CHECK(!readFrame(io, type, body, sizeof body, n, 50, 20) && gWarnings == w + 1);
  // too big for the buffer: read through and thrown away, so the next frame is in step
  feed(plain(0x31, seq(40)));
  feed(plain(0x32, seq(3)));
  CHECK(!readFrame(io, type, body, 8, n, 50, 20));
  CHECK(readFrame(io, type, body, 8, n, 50, 20) && type == 0x32 && n == 3);
  // nothing arrives
  gFakeNow = 0;
  CHECK(!readFrame(io, type, body, sizeof body, n, 50, 20) && gFakeNow >= 50);
  // stops in the middle of the body
  Bytes cut = plain(0x33, seq(10)); cut.resize(6);
  feed(cut);
  CHECK(!readFrame(io, type, body, sizeof body, n, 50, 20));
  io.rx.clear();
  // a body of 0 and of the largest the buffer takes
  feed(plain(0x34, Bytes()));
  CHECK(readFrame(io, type, body, sizeof body, n, 50, 20) && type == 0x34 && n == 0);
  feed(plain(0x35, seq(64)));
  CHECK(readFrame(io, type, body, sizeof body, n, 50, 20) && n == 64 && body[63] == 63);
}

static void testPlaintextRefused() {
  reset(false);
  uint8_t body[16]; size_t n; uint8_t type;
  // off the control link a plaintext frame is only accepted if it is a key-exchange frame
  feed(plain(0x22, seq(2)));
  int w = gWarnings;
  CHECK(!readFrame(io, type, body, sizeof body, n, 50, 20) && gWarnings == w + 1);
  feed(plain(KEX_REQ, seq(4)));
  CHECK(readFrame(io, type, body, sizeof body, n, 50, 20) && type == KEX_REQ);
  // an encrypted frame without a session is dropped
  feed(encFrame((const uint8_t *) "0123456789abcdef", DIR_PALM, 0, 0x22, seq(3)));
  CHECK(!readFrame(io, type, body, sizeof body, n, 50, 20));
  // and nothing but a key-exchange frame is written without a session
  io.tx.clear();
  writeFrame(io, 0x22, body, 3);
  CHECK(io.tx.empty());
}

static void testKeyExchange() {
  reset(false);
  secureBegin();
  uint8_t body[300]; size_t n; uint8_t type;
  const uint32_t reqId = 0x01020304;
  Bytes req = seq(0); for (int s = 24; s >= 0; s -= 8) req.push_back((uint8_t) (reqId >> s));

  // KEX_REQ -> KEX_PUB: reqId, modLen 128, modulus, R^3 mod N, 16 random bytes
  feed(plain(KEX_REQ, req));
  CHECK(readFrame(io, type, body, sizeof body, n, 50, 20) && handleKex(io, type, body, n));
  size_t pos = 0; Bytes pub; uint8_t chkOk = 0;
  CHECK(nextTx(pos, false, type, pub, chkOk) && type == KEX_PUB && chkOk == 1 && pub.size() == 4 + 2 + 128 + 128 + 16);
  CHECK(pub[0] == 1 && pub[1] == 2 && pub[2] == 3 && pub[3] == 4 && pub[4] == 0 && pub[5] == 128);
  CHECK(pub[6] == 0xFF && pub[6 + 127] == 0xFD);                                   // the modulus as the stand-in made it
  bool r3ok = pub[6 + 128 + 127] == 27;
  for (int i = 0; i < 127; i++) r3ok = r3ok && pub[6 + 128 + i] == 0;
  CHECK(r3ok);                                                                      // R^3 mod (2^1024 - 3) = 27
  uint8_t wRand[16]; memcpy(wRand, pub.data() + 6 + 256, 16);
  CHECK(!isSecure());

  // KEX_KEY -> a session: KEX_OK comes back encrypted, and "RPOK" under the key AES-ECB(secret, wRand)
  const uint8_t secret[16] = {9, 8, 7, 6, 5, 4, 3, 2, 1, 0, 1, 2, 3, 4, 5, 6};
  Bytes kk = req; for (int i = 0; i < 128; i++) kk.push_back(i < 16 ? secret[i] : 0x55);
  feed(plain(KEX_KEY, kk));
  CHECK(readFrame(io, type, body, sizeof body, n, 50, 20) && handleKex(io, type, body, n));
  CHECK(isSecure());
  uint8_t key[16]; aesEcb(secret, wRand, key);
  Bytes ok = encFrame(key, DIR_WIRSA, 0, KEX_OK, Bytes{'R', 'P', 'O', 'K'});
  Bytes got(io.tx.begin() + pos, io.tx.end());
  CHECK(got == ok);

  // WiRSa's later frames: the sequence number counts up, and a long body continues the keystream across its 64-byte chunks
  size_t before = io.tx.size();
  writeFrame(io, 0x40, seq(150, 7).data(), 150);
  Bytes got2(io.tx.begin() + before, io.tx.end());
  CHECK(got2 == encFrame(key, DIR_WIRSA, 1, 0x40, seq(150, 7)));
  before = io.tx.size();
  writeFrame(io, 0x41, nullptr, 0);
  CHECK(Bytes(io.tx.begin() + before, io.tx.end()) == encFrame(key, DIR_WIRSA, 2, 0x41, Bytes()));

  // the Palm's frames: decrypted, with its own direction byte
  feed(encFrame(key, DIR_PALM, 5, 0x43, seq(100, 3)));
  CHECK(readFrame(io, type, body, sizeof body, n, 50, 20) && type == 0x43 && n == 100 && body[0] == 3 && body[99] == 102);
  Bytes wrongDir = encFrame(key, DIR_WIRSA, 6, 0x43, seq(10));       // WiRSa's own direction does not decrypt as the Palm's
  feed(wrongDir);
  CHECK(readFrame(io, type, body, sizeof body, n, 50, 20) && Bytes(body, body + n) != seq(10));
  Bytes tooShort = {0x80 | 0x43, 0x00, 0x02, 0x00, 0x00, 0x00};     // an encrypted frame with a body shorter than the 4-byte sequence number
  feed(tooShort);
  CHECK(!readFrame(io, type, body, sizeof body, n, 50, 20));
  io.rx.clear();

  // a new KEX_REQ drops the session
  feed(plain(KEX_REQ, req));
  CHECK(readFrame(io, type, body, sizeof body, n, 50, 20) && handleKex(io, type, body, n));
  CHECK(!isSecure());
}

static void testKeyExchangeFailures() {
  reset(false);
  secureBegin();
  uint8_t body[300]; size_t n; uint8_t type;
  Bytes req = {1, 2, 3, 4};
  // a KEX_REQ whose body is not 4 bytes gets no answer
  feed(plain(KEX_REQ, Bytes{1, 2, 3}));
  CHECK(readFrame(io, type, body, sizeof body, n, 50, 20) && handleKex(io, type, body, n));
  CHECK(io.tx.empty());
  // KEX_KEY with no exchange pending -> KEX_FAIL with the reqId
  Bytes kk = req; for (int i = 0; i < 128; i++) kk.push_back(1);
  feed(plain(KEX_KEY, kk));
  CHECK(readFrame(io, type, body, sizeof body, n, 50, 20) && handleKex(io, type, body, n));
  size_t pos = 0; Bytes b; uint8_t ok = 0;
  CHECK(nextTx(pos, false, type, b, ok) && type == KEX_FAIL && b == Bytes({1, 2, 3, 4}) && !isSecure());
  // a wrong reqId
  feed(plain(KEX_REQ, req));
  CHECK(readFrame(io, type, body, sizeof body, n, 50, 20) && handleKex(io, type, body, n));
  nextTx(pos, false, type, b, ok);                                                  // the KEX_PUB
  Bytes other = {9, 9, 9, 9}; for (int i = 0; i < 128; i++) other.push_back(1);
  feed(plain(KEX_KEY, other));
  CHECK(readFrame(io, type, body, sizeof body, n, 50, 20) && handleKex(io, type, body, n));
  CHECK(nextTx(pos, false, type, b, ok) && type == KEX_FAIL && b == Bytes({9, 9, 9, 9}) && !isSecure());
  // a wrong length
  feed(plain(KEX_REQ, req));
  CHECK(readFrame(io, type, body, sizeof body, n, 50, 20) && handleKex(io, type, body, n));
  nextTx(pos, false, type, b, ok);
  feed(plain(KEX_KEY, Bytes{1, 2, 3, 4, 5}));
  CHECK(readFrame(io, type, body, sizeof body, n, 50, 20) && handleKex(io, type, body, n));
  CHECK(nextTx(pos, false, type, b, ok) && type == KEX_FAIL && !isSecure());
  // the decryption fails
  feed(plain(KEX_REQ, req));
  CHECK(readFrame(io, type, body, sizeof body, n, 50, 20) && handleKex(io, type, body, n));
  nextTx(pos, false, type, b, ok);
  gRsaDecryptFails = true;
  feed(plain(KEX_KEY, kk));
  CHECK(readFrame(io, type, body, sizeof body, n, 50, 20) && handleKex(io, type, body, n));
  CHECK(nextTx(pos, false, type, b, ok) && type == KEX_FAIL && !isSecure());
  // a frame that is not a key-exchange frame is not handled here
  CHECK(!handleKex(io, 0x22, body, 0));
}

// ---- acknowledged frames ----------------------------------------------------------------------------------------------------------------------------------
static const uint8_t ACK = 0x04, BYE = 0x03;
static int gOtherCalls = 0; static uint8_t gOtherType = 0; static Bytes gOtherBody;
static bool onOther(uint8_t t, const uint8_t *b, size_t n) { gOtherCalls++; gOtherType = t; gOtherBody.assign(b, b + n); return true; }

static bool sendAcked(bool *bye = nullptr, OtherFrameFn other = nullptr, int tries = 3) {
  static uint8_t scratch[64];
  const uint8_t body[3] = {1, 2, 3};
  return writeFrameAcked(io, 0x22, body, 3, ACK, BYE, scratch, sizeof scratch, tries, 500, 1000, 100, bye, other);
}

static void testAckedFrames() {
  Bytes ack = plain(ACK, Bytes());
  bool bye = true;

  // the ack comes at once
  reset(true);
  io.onFlush = [&](HardwareSerial &) { feed(ack); io.onFlush = nullptr; };
  CHECK(sendAcked(&bye) && !bye && countFrames(true) == 1);

  // no ack the first time: the same frame is sent again after the retry delay
  reset(true);
  int flushes = 0;
  io.onFlush = [&](HardwareSerial &) { if (++flushes == 2) feed(ack); };
  CHECK(sendAcked() && flushes == 2 && countFrames(true) == 2);
  size_t half = io.tx.size() / 2;
  CHECK(Bytes(io.tx.begin(), io.tx.begin() + half) == Bytes(io.tx.begin() + half, io.tx.end()));    // identical frames
  CHECK(gFakeNow >= 1000 + 500);                                                                    // the ack wait, then the retry delay

  // never an ack: every try is made, then false
  reset(true);
  // (the ack window of a try starts before its retry delay, so a retry waits ackTimeout minus the delay: 3 tries take 3 x ackTimeout in all)
  CHECK(!sendAcked() && countFrames(true) == 3 && gFakeNow >= 3 * 1000 && gFakeNow < 3 * 1000 + 50);

  // an ack left over from an abandoned try is thrown away before each send, so it does not count for this one
  reset(true);
  feed(ack);
  CHECK(!sendAcked(nullptr, nullptr, 2) && countFrames(true) == 2);

  // the peer sends BYE instead of the ack: stop at once
  reset(true);
  io.onFlush = [&](HardwareSerial &) { feed(plain(BYE, Bytes())); };
  bye = false;
  CHECK(!sendAcked(&bye) && bye && countFrames(true) == 1);

  // a frame of the peer's own while the ack is awaited: kept when there is a handler, and the wait goes on
  reset(true);
  gOtherCalls = 0;
  io.onFlush = [&](HardwareSerial &) { feed(plain(0x55, Bytes{9})); feed(ack); io.onFlush = nullptr; };
  CHECK(sendAcked(nullptr, onOther) && gOtherCalls == 1 && gOtherType == 0x55 && gOtherBody == Bytes({9}) && countFrames(true) == 1);

  // without a handler it counts as a failed try, and the frame is lost
  reset(true);
  flushes = 0;
  io.onFlush = [&](HardwareSerial &) { if (++flushes == 1) feed(plain(0x55, Bytes{9})); else feed(ack); };
  CHECK(sendAcked() && flushes == 2);
}

static void testWriterReader() {
  uint8_t buf[12];
  Writer w(buf, sizeof buf);
  w.u8(1); w.u16(0x0203); w.u32(0x04050607);
  CHECK(w.len == 7 && buf[0] == 1 && buf[1] == 2 && buf[2] == 3 && buf[3] == 4 && buf[6] == 7);
  // a string that does not fit is cut, and its length says what was written
  w.str(String("hello world"));
  CHECK(w.len == 12 && buf[7] == 0 && buf[8] == 3 && memcmp(buf + 9, "hel", 3) == 0);
  Reader r(buf, w.len);
  CHECK(r.u8() == 1 && r.u16() == 0x0203 && r.u32() == 0x04050607 && r.ok);
  String s = r.str();
  CHECK(r.ok && s.length() == 3 && !strcmp(s.c_str(), "hel"));
  r.u8();                                                            // past the end
  CHECK(!r.ok);
  Reader r2(buf, 3);
  r2.u32();
  CHECK(!r2.ok);
  Reader r3(buf, 9);                                                  // a string whose length runs past the body
  r3.u8(); r3.u16(); r3.u32();
  String t = r3.str();
  CHECK(!r3.ok && t.length() == 0);
}

int main() {
  testAes();
  testPlainFrames();
  testPlaintextRefused();
  testKeyExchange();
  testKeyExchangeFailures();
  testAckedFrames();
  testWriterReader();
  if (fails) { printf("%d check(s) FAILED\n", fails); return 1; }
  printf("relay_proto: all checks passed\n");
  return 0;
}
