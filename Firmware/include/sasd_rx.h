// sasd_rx.h - the receive side of SASD's slk packets: a byte-at-a-time state machine with no hardware in it (host test: test_host/sasd_rx_test.cpp).
//
// An slk packet is a 10 byte header (BE EF ED, dst, src, type, length[2], tid, checksum), the payload and a CRC-16/XMODEM over both. feed() takes one byte at a time and says when a
// packet has been verified. Two things go beyond the reference reader's slkRx():
//
//  * A packet that is dropped (CRC mismatch, or its rest never came) may have swallowed the start of a real one: over a wireless link a damaged long packet and the Palm's resend
//    of a short request can run into each other. replay() drops the packet in progress and puts its bytes, minus the first, back in front of the input, so the receiver looks for a
//    header inside them.
//  * stalled() says when a packet in progress has been silent for STALL_MS: its rest is not coming, and holding the receiver in it would swallow the Palm's resends (measured: 46 s).
//
// Used by sasd.cpp, which owns the Ctx and does the reading:  for (;;) { if (!pushed(c, b)) { if (!available) break; b = read(); } if (feed(c, b) == PACKET) handle(c); }
#pragma once
#include <stdint.h>
#include <string.h>

uint16_t zmCRC16Update(uint16_t crc, uint8_t b);   // CRC-16/XMODEM, one byte (globals.h)

namespace SasdRx {

constexpr uint8_t MAGIC0 = 0xBE, MAGIC1 = 0xEF, MAGIC2 = 0xED;
constexpr int HDR_LEN = 10;                 // magic[3] + dstSlk + srcSck + pktTyp + dataLen[2] + tid + csum
constexpr uint16_t MAX_PAYLOAD = 528;       // the reference's SD_PROTO_MAX_MSG
constexpr unsigned long STALL_MS = 100;     // no byte for this long inside a packet: it is not going to finish

enum State : uint8_t { SYNC, PAYLOAD, CRC };
enum Result : uint8_t { NONE, PACKET, CRC_ERROR };

struct Ctx {
  State    state;
  uint8_t  window[HDR_LEN];
  uint8_t  dstSlk, srcSck, pktTyp, tid;
  uint16_t dataLen;
  uint8_t  payload[MAX_PAYLOAD];
  uint16_t payloadPos;
  uint8_t  crcBuf[2];
  uint8_t  crcPos;
  uint16_t crcRxed, crcCalc;                // of the last CRC_ERROR, for the log
  uint8_t  push[1536];                      // bytes to look at again, ahead of the input
  uint16_t pushPos, pushLen;
  uint32_t replays;                         // packets dropped and looked into again (CRC errors and stalls)
};

inline uint8_t sum9(const uint8_t *h) {
  uint8_t s = 0;
  for (int i = 0; i < HDR_LEN - 1; i++) s += h[i];
  return s;
}

// Ready for a new header. The bytes waiting to be looked at again stay.
inline void reset(Ctx &c) {
  c.state = SYNC;
  memset(c.window, 0, sizeof(c.window));
}

// Ready for a new header and nothing waiting (mode entry, a change of baud rate).
inline void flush(Ctx &c) {
  reset(c);
  c.pushPos = c.pushLen = 0;
}

// The packet in progress is dropped; its bytes except the first are put back in front of whatever is still waiting.
inline void replay(Ctx &c) {
  uint8_t tmp[HDR_LEN + MAX_PAYLOAD + 2];
  uint16_t n = 0;
  if (c.state != SYNC) {
    memcpy(tmp, c.window, HDR_LEN); n = HDR_LEN;
    uint16_t pl = c.state == PAYLOAD ? c.payloadPos : c.dataLen;
    memcpy(tmp + n, c.payload, pl); n += pl;
    if (c.state == CRC) { memcpy(tmp + n, c.crcBuf, c.crcPos); n += c.crcPos; }
  }
  reset(c);
  if (n < 2) return;
  uint16_t add = n - 1;
  uint16_t rest = c.pushLen > c.pushPos ? c.pushLen - c.pushPos : 0;
  if ((uint32_t) add + rest > sizeof(c.push)) rest = sizeof(c.push) - add;
  memmove(c.push + add, c.push + c.pushPos, rest);
  memcpy(c.push, tmp + 1, add);
  c.pushPos = 0; c.pushLen = add + rest;
  c.replays++;
}

// The next byte waiting to be looked at again, if any.
inline bool pushed(Ctx &c, uint8_t &b) {
  if (c.pushPos >= c.pushLen) return false;
  b = c.push[c.pushPos++];
  if (c.pushPos >= c.pushLen) c.pushPos = c.pushLen = 0;
  return true;
}

// A packet in progress that has had no byte for STALL_MS (quietMs: the time since the last byte read from the port), with nothing waiting to be looked at again.
inline bool stalled(const Ctx &c, unsigned long quietMs) {
  return c.state != SYNC && c.pushPos >= c.pushLen && quietMs > STALL_MS;
}

// One byte in. PACKET: a verified packet is in c (dstSlk, srcSck, pktTyp, tid, dataLen, payload). CRC_ERROR: the packet was dropped and its bytes are waiting to be looked at again.
inline Result feed(Ctx &c, uint8_t byte) {
  switch (c.state) {
    case SYNC: {
      for (int i = 0; i < HDR_LEN - 1; i++) c.window[i] = c.window[i + 1];
      c.window[HDR_LEN - 1] = byte;

      if (c.window[0] == MAGIC0 && c.window[1] == MAGIC1 && c.window[2] == MAGIC2 && c.window[9] == sum9(c.window)) {
        c.dstSlk = c.window[3];
        c.srcSck = c.window[4];
        c.pktTyp = c.window[5];
        c.dataLen = ((uint16_t) c.window[6] << 8) | c.window[7];
        c.tid = c.window[8];

        if (c.dataLen > MAX_PAYLOAD) break;      // cannot be a real SASD message (or the link is corrupt): keep scanning, avoid an overrun
        c.payloadPos = 0;
        c.crcPos = 0;
        c.state = c.dataLen ? PAYLOAD : CRC;
      }
      break;
    }

    case PAYLOAD:
      c.payload[c.payloadPos++] = byte;
      if (c.payloadPos >= c.dataLen) c.state = CRC;
      break;

    case CRC:
      c.crcBuf[c.crcPos++] = byte;
      if (c.crcPos >= 2) {
        uint16_t rx = ((uint16_t) c.crcBuf[0] << 8) | c.crcBuf[1];
        uint16_t calc = 0;
        for (int i = 0; i < HDR_LEN; i++) calc = zmCRC16Update(calc, c.window[i]);
        for (uint16_t i = 0; i < c.dataLen; i++) calc = zmCRC16Update(calc, c.payload[i]);
        if (rx == calc) { reset(c); return PACKET; }
        c.crcRxed = rx; c.crcCalc = calc;
        replay(c);                               // the Palm retries; meanwhile a real packet may sit inside the dropped one
        return CRC_ERROR;
      }
      break;
  }
  return NONE;
}

}  // namespace SasdRx
