// relay_proto.h - shared serial frame protocol for WiRSa's relay modes
// (BitWarden, CalDAV, and any future one). Originally extracted from
// bitwarden_relay.cpp's frame I/O layer; both existing relays now use it
// (bitwarden_relay.cpp via thin wrappers), and new relay modes should
// build on it rather than duplicating the framing/checksum/ack-retry logic.
//
// Frame format (identical across every relay mode that uses this):
//   type(1B) + lenHi(1B) + lenLo(1B) + body(len bytes) + checksum(1B)
//   checksum = XOR of type, lenHi, lenLo, and every body byte.
// Big-endian integers, uint16-length-prefixed strings (no NUL) - matches
// apps/caldav_sync/CDDVWire.c and apps/bitwarden/Wire.c on the Palm side.
#ifndef RELAY_PROTO_H
#define RELAY_PROTO_H

#include <Arduino.h>

namespace RelayProto {

// Blocking read of a single byte with a real deadline - yields every spin
// so a long wait (e.g. an idle-frame timeout) never starves WiFi/lwIP/
// watchdog housekeeping. Returns false on timeout.
bool readByte(HardwareSerial &io, uint8_t &out, uint32_t timeoutMs);

// Reads one complete frame. idleTimeoutMs bounds the wait for the frame's
// OWN first byte; byteTimeoutMs bounds the gap between any two bytes once
// the frame has started. Returns false on timeout, an oversized frame
// (silently drained, not just rejected - keeps the stream in sync for the
// next real frame), or a checksum mismatch. Never partial: on true,
// (type, body[0..bodyLen)) is a fully verified frame.
bool readFrame(HardwareSerial &io, uint8_t &type, uint8_t *body, size_t bodyCap,
               size_t &bodyLen, uint32_t idleTimeoutMs, uint32_t byteTimeoutMs);

// Writes one frame as a single logical unit, then flushes - write() alone
// only queues into the driver's TX ring buffer; skipping flush() here was
// a real, confirmed real-hardware bug (BitWarden Relay's own history: the
// Palm saw zero bytes of the very first acked frame, a clean timeout, not
// corruption) - mirrors the Palm's own SrmSendWait()-after-every-send
// convention on the other end of this same link.
void writeFrame(HardwareSerial &io, uint8_t type, const uint8_t *body, size_t bodyLen);

// Send-and-wait-for-ack with retry, mirroring the Palm's own per-frame
// ack-retry loop so neither side's patience runs out first. Drains any
// stale bytes sitting in the RX buffer before each (re)send - a late ack
// for an already-abandoned attempt would otherwise be misread as the ack
// for the retried send (both are identical empty-body ack frames).
// ackType: the frame type that counts as a successful ack.
// byeType: if the peer sends this INSTEAD of ackType, it has deliberately
//   ended the session already (own retries exhausted, or user cancelled) -
//   the retry loop aborts immediately (no point resending into dead air)
//   and sets *peerSaidBye = true if provided.
// scratchBody/scratchCap: caller-owned buffer for the ack frame's own body
//   (discarded either way - every relay's ack frames are empty-body, this
//   just needs somewhere to read into) - static/module-level, not a stack
//   local, matching this project's own established caution around
//   multi-KB buffers on a task stack.
bool writeFrameAcked(HardwareSerial &io, uint8_t type, const uint8_t *body, size_t bodyLen,
                      uint8_t ackType, uint8_t byeType,
                      uint8_t *scratchBody, size_t scratchCap,
                      int retries, uint32_t retryDelayMs,
                      uint32_t ackTimeoutMs, uint32_t byteTimeoutMs,
                      bool *peerSaidBye = nullptr);

// Body encode/decode helpers - uint16-length-prefixed strings, big-endian
// integers. Both are self-consistent even when truncated/overrun: Writer
// never writes a length prefix that doesn't match what actually got
// written (so a truncated field can never desync a receiver mid-frame),
// and Reader sets ok=false and stops consuming on the first overrun
// (check .ok after parsing a frame, same convention as the Palm's own
// CDDVR/BwReader).
// ---- Serial-link encryption (mandatory, no plaintext fallback) ----------
// Mirrors the Palm's apps/lib/relaysec.c. Key exchange (plaintext frames):
//   Palm  -> KEX_REQ  reqId(u32)
//   WiRSa -> KEX_PUB  reqId(u32) modLen(u16) modulus(big-endian)
//                     r3(modLen: R^3 mod modulus, R = 2^(8*modLen)) wRand(16)
//   Palm  -> KEX_KEY  reqId(u32) RSA-PKCS#1-v1.5(secret16)   [e = 3]
//   WiRSa -> KEX_OK   (encrypted) "RPOK"   or KEX_FAIL reqId(u32)
// Session key = AES-128-ECB(key = secret, block = wRand). Every other frame
// is encrypted: wire type = type | ENC_FLAG, wire body = seq(u32) +
// AES-128-CTR(body), counter block = dir,0,0,0,seq,0..0 (DIR_PALM for
// Palm->WiRSa, DIR_WIRSA for WiRSa->Palm), checksum over the bytes as sent.
// readFrame drops any plaintext frame that isn't a key-exchange frame and
// any encrypted frame without a session; writeFrame refuses to send a
// non-key-exchange frame without one. The RSA-1024 key pair lives in RAM,
// generated once per boot (secureBegin); nothing is pinned or stored.
enum : uint8_t {
  KEX_REQ = 0x70, KEX_PUB = 0x71, KEX_KEY = 0x72, KEX_OK = 0x73, KEX_FAIL = 0x74,
  ENC_FLAG = 0x80, DIR_PALM = 0x50, DIR_WIRSA = 0x57
};
inline bool isKex(uint8_t type) { return (type & 0xF0) == 0x70; }

// Generates the RSA key pair if this boot hasn't yet (call on relay-mode
// entry - can take a second or two), and clears any session.
void secureBegin();
// Clears the session (key, sequence, pending exchange).
void secureReset();
// Handles a key-exchange frame just returned by readFrame. Returns true if
// type was a key-exchange type (consumed - the caller skips its dispatch).
bool handleKex(HardwareSerial &io, uint8_t type, const uint8_t *body, size_t bodyLen);

struct Writer {
  uint8_t *buf; size_t cap; size_t len = 0;
  Writer(uint8_t *b, size_t c) : buf(b), cap(c) {}
  void u8(uint8_t v);
  void u16(uint16_t v);
  void u32(uint32_t v);
  void bytesRaw(const uint8_t *p, size_t n);
  void str(const String &s);
};

struct Reader {
  const uint8_t *buf; size_t len; size_t pos = 0; bool ok = true;
  Reader(const uint8_t *b, size_t l) : buf(b), len(l) {}
  uint8_t u8();
  uint16_t u16();
  uint32_t u32();
  bool bytesRaw(uint8_t *out, size_t n);
  String str();
};

}  // namespace RelayProto

#endif
