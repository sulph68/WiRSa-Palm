// relay_proto.cpp - see include/relay_proto.h for the design rationale,
// including the mandatory serial-link encryption.
// Originally extracted from bitwarden_relay.cpp's frame I/O layer; both
// bitwarden_relay.cpp and caldav_relay.cpp now build on this module.

#include "relay_proto.h"
#include <mbedtls/aes.h>
#include <mbedtls/rsa.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>

extern bool usbDebug;

namespace RelayProto {

// ---- serial-link encryption state (one relay mode active at a time) ----
static mbedtls_entropy_context gEntropy;
static mbedtls_ctr_drbg_context gDrbg;
static bool gDrbgReady = false;
static mbedtls_rsa_context gRsa;
static bool gRsaReady = false;
static mbedtls_aes_context gAes;
static bool gAesInit = false;
static bool gSecure = false;
static uint32_t gTxSeq = 0;
static bool gKexPending = false;
static uint32_t gKexId = 0;
static uint8_t gKexRand[16];

static const size_t RSA_BYTES = 128;  // RSA-1024
// e = 3 (with R^3 mod N supplied, below): the Palm's public-key step is
// three Montgomery multiplications instead of eighteen plus an R^2
// computation. Safe for key transport under PKCS#1 v1.5 padding (100+
// random bytes per encryption).
static const int RSA_EXPONENT = 3;
static uint8_t gKexN[RSA_BYTES];   // modulus, big-endian
static uint8_t gKexR3[RSA_BYTES];  // R^3 mod N, R = 2^(8*RSA_BYTES) - lets the Palm finish in 3 multiplications

static uint32_t be32(const uint8_t *p) {
  return ((uint32_t) p[0] << 24) | ((uint32_t) p[1] << 16) | ((uint32_t) p[2] << 8) | p[3];
}
static void putBe32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t) (v >> 24); p[1] = (uint8_t) (v >> 16); p[2] = (uint8_t) (v >> 8); p[3] = (uint8_t) v;
}

static bool drbgReady() {
  if (gDrbgReady) return true;
  mbedtls_entropy_init(&gEntropy);
  mbedtls_ctr_drbg_init(&gDrbg);
  static const char pers[] = "WiRSa-RelayProto";
  gDrbgReady = mbedtls_ctr_drbg_seed(&gDrbg, mbedtls_entropy_func, &gEntropy,
                                     (const unsigned char *) pers, sizeof(pers) - 1) == 0;
  return gDrbgReady;
}

// AES-128-CTR in place with the session key, starting at 16-byte block blk
// of frame (dir, seq). Encrypt and decrypt are the same call.
static void xcrypt(uint8_t *buf, size_t len, uint8_t dir, uint32_t seq, uint16_t blk) {
  uint8_t ctr[16] = {0};
  uint8_t stream[16];
  size_t off = 0;
  ctr[0] = dir;
  putBe32(ctr + 4, seq);
  ctr[14] = (uint8_t) (blk >> 8);
  ctr[15] = (uint8_t) blk;
  mbedtls_aes_crypt_ctr(&gAes, len, &off, ctr, stream, buf, buf);
}

// R^3 mod N by 3*bits modular doublings of 1 (32-bit limbs, little-endian).
static void computeR3(const uint8_t *nBe, uint8_t *outBe) {
  const size_t L = RSA_BYTES / 4;
  uint32_t n[L], x[L];
  for (size_t i = 0; i < L; i++) {
    const uint8_t *p = nBe + RSA_BYTES - 4 * (i + 1);
    n[i] = be32(p);
    x[i] = 0;
  }
  x[0] = 1;
  for (size_t it = 0; it < 3 * 8 * RSA_BYTES; it++) {
    uint32_t carry = 0;
    for (size_t i = 0; i < L; i++) {
      uint32_t v = x[i];
      x[i] = (v << 1) | carry;
      carry = v >> 31;
    }
    bool ge = carry != 0;
    if (!ge) {
      ge = true;  // equal counts as >=
      for (size_t i = L; i-- > 0;) {
        if (x[i] != n[i]) { ge = x[i] > n[i]; break; }
      }
    }
    if (ge) {
      uint64_t br = 0;
      for (size_t i = 0; i < L; i++) {
        uint64_t d = (uint64_t) x[i] - n[i] - br;
        x[i] = (uint32_t) d;
        br = (d >> 63) & 1;
      }
    }
  }
  for (size_t i = 0; i < L; i++) putBe32(outBe + RSA_BYTES - 4 * (i + 1), x[i]);
}

void secureReset() {
  gSecure = false;
  gKexPending = false;
  gTxSeq = 0;
  if (gAesInit) mbedtls_aes_free(&gAes);
  mbedtls_aes_init(&gAes);
  gAesInit = true;
  memset(gKexRand, 0, sizeof(gKexRand));
}

void secureBegin() {
  secureReset();
  if (gRsaReady || !drbgReady()) return;
  unsigned long t0 = millis();
  mbedtls_rsa_init(&gRsa, MBEDTLS_RSA_PKCS_V15, 0);
  gRsaReady = mbedtls_rsa_gen_key(&gRsa, mbedtls_ctr_drbg_random, &gDrbg, RSA_BYTES * 8, RSA_EXPONENT) == 0 &&
              mbedtls_rsa_get_len(&gRsa) == RSA_BYTES &&
              mbedtls_rsa_export_raw(&gRsa, gKexN, RSA_BYTES, NULL, 0, NULL, 0, NULL, 0, NULL, 0) == 0 &&
              (gKexN[0] & 0x80) != 0;
  if (gRsaReady) computeR3(gKexN, gKexR3);
  else mbedtls_rsa_free(&gRsa);
  if (usbDebug) Serial.printf("RelayProto: RSA-%u key %s in %lums\r\n", (unsigned) (RSA_BYTES * 8),
                              gRsaReady ? "generated" : "generation FAILED", millis() - t0);
}

static void sendKexFail(HardwareSerial &io, uint32_t id) {
  uint8_t b[4];
  putBe32(b, id);
  writeFrame(io, KEX_FAIL, b, sizeof(b));
}

bool handleKex(HardwareSerial &io, uint8_t type, const uint8_t *body, size_t bodyLen) {
  if (!isKex(type)) return false;
  uint32_t id = bodyLen >= 4 ? be32(body) : 0;

  if (type == KEX_REQ) {
    // A new connection always starts here - drop any previous session.
    secureReset();
    if (bodyLen != 4) return true;
    if (!gRsaReady) secureBegin();  // retry a failed boot-time generation
    if (!gRsaReady || mbedtls_ctr_drbg_random(&gDrbg, gKexRand, sizeof(gKexRand)) != 0) {
      if (usbDebug) Serial.println("RelayProto: KEX_REQ - no key available, sending KEX_FAIL");
      sendKexFail(io, id);
      return true;
    }
    uint8_t out[4 + 2 + 2 * RSA_BYTES + 16];
    putBe32(out, id);
    out[4] = (uint8_t) (RSA_BYTES >> 8);
    out[5] = (uint8_t) RSA_BYTES;
    memcpy(out + 6, gKexN, RSA_BYTES);
    memcpy(out + 6 + RSA_BYTES, gKexR3, RSA_BYTES);
    memcpy(out + 6 + 2 * RSA_BYTES, gKexRand, 16);
    gKexId = id;
    gKexPending = true;
    writeFrame(io, KEX_PUB, out, sizeof(out));
    if (usbDebug) Serial.println("RelayProto: KEX_REQ -> KEX_PUB");
    return true;
  }

  if (type == KEX_KEY) {
    bool pending = gKexPending;
    gKexPending = false;
    if (!pending || !gRsaReady || bodyLen != 4 + RSA_BYTES || id != gKexId) {
      if (usbDebug) Serial.println("RelayProto: unexpected KEX_KEY - sending KEX_FAIL");
      sendKexFail(io, id);
      return true;
    }
    uint8_t secret[16], key[16];
    size_t olen = 0;
    int rc = mbedtls_rsa_pkcs1_decrypt(&gRsa, mbedtls_ctr_drbg_random, &gDrbg, MBEDTLS_RSA_PRIVATE,
                                       &olen, body + 4, secret, sizeof(secret));
    if (rc != 0 || olen != sizeof(secret)) {
      if (usbDebug) Serial.printf("RelayProto: KEX_KEY decrypt failed (rc=%d len=%u) - sending KEX_FAIL\r\n", rc, (unsigned) olen);
      sendKexFail(io, id);
      return true;
    }
    mbedtls_aes_context tmp;
    mbedtls_aes_init(&tmp);
    mbedtls_aes_setkey_enc(&tmp, secret, 128);
    mbedtls_aes_crypt_ecb(&tmp, MBEDTLS_AES_ENCRYPT, gKexRand, key);
    mbedtls_aes_free(&tmp);
    mbedtls_aes_setkey_enc(&gAes, key, 128);
    memset(secret, 0, sizeof(secret));
    memset(key, 0, sizeof(key));
    gTxSeq = 0;
    gSecure = true;
    static const uint8_t ok[4] = {'R', 'P', 'O', 'K'};
    writeFrame(io, KEX_OK, ok, sizeof(ok));  // sent encrypted
    if (usbDebug) Serial.println("RelayProto: serial link secured");
    return true;
  }

  return true;  // KEX_PUB/KEX_OK/KEX_FAIL are WiRSa->Palm only - ignore
}

bool readByte(HardwareSerial &io, uint8_t &out, uint32_t timeoutMs) {
  uint32_t start = millis();
  while (millis() - start < timeoutMs) {
    if (io.available()) { out = (uint8_t) io.read(); return true; }
    yield();  // can legitimately spin for a long idle-frame timeout -
              // never starve WiFi/lwIP/watchdog housekeeping
  }
  return false;
}

bool readFrame(HardwareSerial &io, uint8_t &type, uint8_t *body, size_t bodyCap,
               size_t &bodyLen, uint32_t idleTimeoutMs, uint32_t byteTimeoutMs) {
  unsigned long tStart = millis();
  if (!readByte(io, type, idleTimeoutMs)) {
    if (usbDebug) Serial.printf("RelayProto: no frame-start byte after %lums (idle budget %lums)\r\n",
                                 millis() - tStart, (unsigned long) idleTimeoutMs);
    return false;
  }
  uint8_t lenHi, lenLo;
  if (!readByte(io, lenHi, byteTimeoutMs)) {
    if (usbDebug) Serial.printf("RelayProto: type=0x%02X arrived, then no length byte 1 within %lums\r\n",
                                 type, (unsigned long) byteTimeoutMs);
    return false;
  }
  if (!readByte(io, lenLo, byteTimeoutMs)) {
    if (usbDebug) Serial.printf("RelayProto: type=0x%02X, then no length byte 2 (total %lums)\r\n",
                                 type, millis() - tStart);
    return false;
  }
  uint16_t len = ((uint16_t) lenHi << 8) | lenLo;
  uint8_t chk = type ^ lenHi ^ lenLo;
  bool enc = (type & ENC_FLAG) != 0;
  uint8_t seqb[4] = {0};
  if (enc) {
    if (len < 4) return false;
    for (int i = 0; i < 4; i++) {
      if (!readByte(io, seqb[i], byteTimeoutMs)) return false;
      chk ^= seqb[i];
    }
    len -= 4;
  }

  if (len > bodyCap) {
    if (usbDebug) Serial.printf("RelayProto: dropped oversized frame (type=0x%02X len=%u > cap=%u)\r\n",
                                 type, len, (unsigned) bodyCap);
    uint8_t b;
    for (uint16_t i = 0; i < len; i++) { if (!readByte(io, b, byteTimeoutMs)) return false; }
    readByte(io, b, byteTimeoutMs);  // checksum byte, best-effort drain
    return false;
  }

  for (uint16_t i = 0; i < len; i++) {
    if (!readByte(io, body[i], byteTimeoutMs)) {
      if (usbDebug) Serial.printf("RelayProto: type=0x%02X len=%u, stalled mid-body at byte %u (total %lums)\r\n",
                                   type, len, i, millis() - tStart);
      return false;
    }
    chk ^= body[i];
  }
  uint8_t rxChk;
  if (!readByte(io, rxChk, byteTimeoutMs)) {
    if (usbDebug) Serial.printf("RelayProto: type=0x%02X len=%u, body complete but no checksum byte (total %lums)\r\n",
                                 type, len, millis() - tStart);
    return false;
  }
  bodyLen = len;
  if (rxChk != chk) {
    if (usbDebug) Serial.printf("RelayProto: checksum mismatch (type=0x%02X len=%u)\r\n", type, len);
    return false;
  }
  if (enc) {
    if (!gSecure) {
      if (usbDebug) Serial.printf("RelayProto: encrypted frame (type=0x%02X) without a session - dropped\r\n", type);
      return false;
    }
    if (len) xcrypt(body, len, DIR_PALM, be32(seqb), 0);
    type &= (uint8_t) ~ENC_FLAG;
  } else if (!isKex(type)) {
    if (usbDebug) Serial.printf("RelayProto: plaintext frame (type=0x%02X) - dropped, link must be secured\r\n", type);
    return false;
  }
  return true;
}

void writeFrame(HardwareSerial &io, uint8_t type, const uint8_t *body, size_t bodyLen) {
  // Only the plaintext key-exchange frames go out unencrypted; KEX_OK is
  // the first encrypted frame of a session.
  bool enc = !isKex(type) || type == KEX_OK;
  if (enc) {
    if (!gSecure) {
      if (usbDebug) Serial.printf("RelayProto: frame type=0x%02X not sent - link not secured\r\n", type);
      return;
    }
    uint32_t seq = gTxSeq++;
    size_t wireLen = bodyLen + 4;
    uint8_t hdr[7] = {(uint8_t) (type | ENC_FLAG), (uint8_t) (wireLen >> 8), (uint8_t) (wireLen & 0xFF)};
    putBe32(hdr + 3, seq);
    uint8_t chk = 0;
    for (int i = 0; i < 7; i++) chk ^= hdr[i];
    io.write(hdr, sizeof(hdr));
    uint8_t chunk[64];
    for (size_t off = 0; off < bodyLen; off += sizeof(chunk)) {
      size_t n = bodyLen - off;
      if (n > sizeof(chunk)) n = sizeof(chunk);
      memcpy(chunk, body + off, n);
      xcrypt(chunk, n, DIR_WIRSA, seq, (uint16_t) (off / 16));
      for (size_t i = 0; i < n; i++) chk ^= chunk[i];
      io.write(chunk, n);
    }
    io.write(chk);
    io.flush();
    return;
  }
  uint8_t lenHi = (uint8_t) (bodyLen >> 8), lenLo = (uint8_t) (bodyLen & 0xFF);
  uint8_t chk = type ^ lenHi ^ lenLo;
  io.write(type);
  io.write(lenHi);
  io.write(lenLo);
  if (bodyLen) io.write(body, bodyLen);
  for (size_t i = 0; i < bodyLen; i++) chk ^= body[i];
  io.write(chk);
  io.flush();  // see header comment - a real, confirmed real-hardware bug otherwise
}

bool writeFrameAcked(HardwareSerial &io, uint8_t type, const uint8_t *body, size_t bodyLen,
                      uint8_t ackType, uint8_t byeType,
                      uint8_t *scratchBody, size_t scratchCap,
                      int retries, uint32_t retryDelayMs,
                      uint32_t ackTimeoutMs, uint32_t byteTimeoutMs,
                      bool *peerSaidBye) {
  if (peerSaidBye) *peerSaidBye = false;
  for (int attempt = 0; attempt < retries; attempt++) {
    uint8_t ackFrameType; size_t ackLen;
    unsigned long tSendStart = millis();
    if (attempt > 0) delay(retryDelayMs);
    // Drain stale RX bytes before sending - see header comment (a late ack
    // for an already-abandoned attempt must never be misread as this one's).
    while (io.available()) io.read();
    writeFrame(io, type, body, bodyLen);
    if (usbDebug) Serial.printf("RelayProto: -> frame type=0x%02X len=%u sent, awaiting ack (attempt %d/%d)\r\n",
                                 type, (unsigned) bodyLen, attempt + 1, retries);
    bool gotFrame = readFrame(io, ackFrameType, scratchBody, scratchCap, ackLen, ackTimeoutMs, byteTimeoutMs);
    if (!gotFrame || ackFrameType != ackType) {
      if (usbDebug) {
        if (gotFrame) {
          Serial.printf("RelayProto: no ack for frame type=0x%02X after %lums - got a DIFFERENT frame instead: type=0x%02X len=%u\r\n",
                         type, millis() - tSendStart, ackFrameType, (unsigned) ackLen);
        } else {
          Serial.printf("RelayProto: no ack for frame type=0x%02X after %lums - readFrame itself failed\r\n",
                         type, millis() - tSendStart);
        }
      }
      if (gotFrame && ackFrameType == byeType) {
        if (peerSaidBye) *peerSaidBye = true;
        return false;  // peer already quit - no point burning the rest of the attempt budget
      }
      continue;  // genuine timeout/corruption - worth another attempt
    }
    if (usbDebug) Serial.printf("RelayProto: ack received for type=0x%02X after %lums\r\n", type, millis() - tSendStart);
    return true;
  }
  return false;
}

void Writer::u8(uint8_t v) { if (len < cap) buf[len++] = v; }
void Writer::u16(uint16_t v) { u8((uint8_t) (v >> 8)); u8((uint8_t) (v & 0xFF)); }
void Writer::u32(uint32_t v) { u16((uint16_t) (v >> 16)); u16((uint16_t) (v & 0xFFFF)); }
void Writer::bytesRaw(const uint8_t *p, size_t n) { for (size_t i = 0; i < n && len < cap; i++) buf[len++] = p[i]; }
void Writer::str(const String &s) {
  size_t avail = (len + 2 <= cap) ? (cap - len - 2) : 0;
  size_t n = s.length();
  if (n > avail) n = avail;
  if (n > 0xFFFF) n = 0xFFFF;
  u16((uint16_t) n);
  bytesRaw((const uint8_t *) s.c_str(), n);
}

uint8_t Reader::u8() { if (pos >= len) { ok = false; return 0; } return buf[pos++]; }
uint16_t Reader::u16() { uint16_t hi = u8(), lo = u8(); return (uint16_t) ((hi << 8) | lo); }
uint32_t Reader::u32() { uint32_t hi = u16(), lo = u16(); return ((uint32_t) hi << 16) | lo; }
bool Reader::bytesRaw(uint8_t *out, size_t n) {
  if (pos + n > len) { ok = false; return false; }
  memcpy(out, buf + pos, n); pos += n; return true;
}
String Reader::str() {
  uint16_t n = u16();
  if (!ok || pos + n > len) { ok = false; return String(); }
  String s; s.reserve(n);
  for (uint16_t i = 0; i < n; i++) s += (char) buf[pos + i];
  pos += n;
  return s;
}

}  // namespace RelayProto
