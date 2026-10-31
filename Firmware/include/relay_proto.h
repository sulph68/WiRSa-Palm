// relay_proto.h - the shared serial frame layer of WiRSa's relay modes (BitWarden, CalDAV, Mesh and any future one), extracted from bitwarden_relay.cpp's frame I/O. New relay modes build on it
// instead of duplicating the framing, checksum and ack-retry logic.
// Frame: type (1 B), lenHi, lenLo, body (len bytes), checksum (XOR of type, both length bytes and every body byte). Big-endian integers, uint16-length-prefixed strings (no NUL), as the Palm's
// apps/caldav_sync/CDDVWire.c and apps/bitwarden/Wire.c. The whole wire is specified in docs/protocol/Protocol-wire.md.
#ifndef RELAY_PROTO_H
#define RELAY_PROTO_H

#include <Arduino.h>

namespace RelayProto {

// Blocking read of one byte with a real deadline; yields on every spin so a long wait (an idle-frame timeout) never starves WiFi, lwIP or the watchdog. False on timeout.
bool readByte(HardwareSerial &io, uint8_t &out, uint32_t timeoutMs);

// Reads one complete frame. idleTimeoutMs bounds the wait for the frame's own first byte; byteTimeoutMs bounds the gap between bytes once it has started. False on timeout, on an oversized frame
// (drained, not only rejected, to keep the stream in step) or on a checksum mismatch. Never partial: on true, (type, body[0..bodyLen)) is a fully verified frame.
bool readFrame(HardwareSerial &io, uint8_t &type, uint8_t *body, size_t bodyCap,
               size_t &bodyLen, uint32_t idleTimeoutMs, uint32_t byteTimeoutMs);

// Writes one frame as a unit, then flushes: write() only queues into the driver's TX ring, and skipping flush() was a hardware bug (the Palm saw no byte of the first acked frame: a clean timeout,
// not corruption). It mirrors the Palm's SrmSendWait() after every send.
void writeFrame(HardwareSerial &io, uint8_t type, const uint8_t *body, size_t bodyLen);

// Send and wait for the ack, with retry, mirroring the Palm's per-frame ack-retry loop so neither side's patience runs out first. Stale bytes in the RX buffer are drained before each (re)send: a
// late ack of an abandoned attempt would otherwise pass for the ack of the retry (both are identical empty-body frames).
// retries/retryDelayMs/ackTimeoutMs: the tries, and the sleep before each retry. ackTimeoutMs is the whole time of one try, so on a retry the sleep is part of it (the ack is awaited ackTimeoutMs - retryDelayMs).
// ackType: the frame type that counts as the ack.
// byeType: if the peer sends this INSTEAD of the ack it has ended the session (its retries exhausted, or the user cancelled): the loop aborts at once and sets *peerSaidBye if given.
// scratchBody/scratchCap: a caller-owned buffer for the ack's body (discarded: acks are empty); static, not on the task stack (multi-KB buffers stay off it).
// onOther (optional): a frame that is neither the ack nor the BYE arrives while the ack is awaited (the peer sent something of its own at that moment). Without a handler it counts as a failed attempt
// and the frame is lost; with one, onOther(type, body, len) gets it (body is in scratchBody, valid only during the call: copy it) and returns true if kept, and the wait for the ack goes on.
using OtherFrameFn = bool (*)(uint8_t type, const uint8_t *body, size_t len);
bool writeFrameAcked(HardwareSerial &io, uint8_t type, const uint8_t *body, size_t bodyLen,
                      uint8_t ackType, uint8_t byeType,
                      uint8_t *scratchBody, size_t scratchCap,
                      int retries, uint32_t retryDelayMs,
                      uint32_t ackTimeoutMs, uint32_t byteTimeoutMs,
                      bool *peerSaidBye = nullptr, OtherFrameFn onOther = nullptr);

// Body encode/decode helpers: uint16-length-prefixed strings and big-endian integers, consistent even when truncated or overrun. Writer never writes a length prefix that does not match what was
// written (a truncated field cannot desync a receiver), and Reader sets ok = false and stops on the first overrun (check .ok after parsing a frame, as the Palm's CDDVR/BwReader).
// ---- Serial-link encryption (mandatory, no plaintext fallback) ----
// Mirrors the Palm's apps/lib/relaysec.c; Protocol-wire.md section 4 has the layout. Key exchange in plaintext frames: KEX_REQ (reqId), KEX_PUB (reqId, modLen, modulus, R^3 mod modulus, wRand 16),
// KEX_KEY (reqId, RSA-PKCS#1-v1.5 of a 16-byte secret, e = 3), then KEX_OK (encrypted, "RPOK") or KEX_FAIL (reqId). Session key = AES-128-ECB(key = secret, block = wRand). Every other frame is
// encrypted: wire type = type | ENC_FLAG, wire body = seq (u32) + AES-128-CTR(body), counter block = dir, 0, 0, 0, seq, 0..0 (DIR_PALM Palm -> WiRSa, DIR_WIRSA WiRSa -> Palm), checksum over the bytes
// as sent. readFrame drops a plaintext frame that is not a key-exchange frame and an encrypted one without a session; writeFrame refuses a non-key-exchange frame without a session. The RSA-1024
// key pair lives in RAM, made once per boot (secureBegin); nothing is pinned or stored.
enum : uint8_t {
  KEX_REQ = 0x70, KEX_PUB = 0x71, KEX_KEY = 0x72, KEX_OK = 0x73, KEX_FAIL = 0x74,
  ENC_FLAG = 0x80, DIR_PALM = 0x50, DIR_WIRSA = 0x57
};
inline bool isKex(uint8_t type) { return (type & 0xF0) == 0x70; }

// Makes the RSA key pair if this boot has not (call on relay-mode entry; it can take a second or two) and clears any session.
void secureBegin();
// Clears the session (key, sequence, pending exchange).
void secureReset();
// Handles a key-exchange frame just returned by readFrame. True if type was a key-exchange type (consumed: the caller skips its dispatch).
bool handleKex(HardwareSerial &io, uint8_t type, const uint8_t *body, size_t bodyLen);

// ---- Palm control link (palm_ctl.cpp) ----
// Off by default (every relay mode's own behaviour). While on, writeFrame sends CTL_LEAD ahead of every frame, and plaintext non-key-exchange frames are sent and accepted while the link is not
// secured (only the password commands run secured).
enum : uint8_t { CTL_LEAD = 0x16 };
void setControlLink(bool on);
bool isSecure();
// readFrame for a caller that already read and checked the type byte.
bool readFrameRest(HardwareSerial &io, uint8_t &type, uint8_t *body, size_t bodyCap,
                   size_t &bodyLen, uint32_t byteTimeoutMs);

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
