// HotSync Install Module
//
// Wire-compatible (not code-ported) reimplementation of dmitry.gr's SASD
// project's own embedded HotSync host client, for the ESP32/Arduino target.
// Reference source: ~/Downloads/sasdSource/SASD_release/software/serialSD/
// firmware/ - slk.c (SLP framing + CRC-16/XMODEM), frag.c (PADP-style
// fragmentation - this port needs the REAL multi-fragment version, unlike
// sasd.cpp's simplified single-fragment reimplementation, since resource/
// record payloads routinely exceed one 524-byte fragment), hotsync.c (the
// DLP opcode layer), prc.c (PRC file parsing - extended here to also read
// PDB files, which the reference deliberately does not support).
//
// WiRSa plays the desktop-cradle role: waits for a Palm's HotSync WAKE
// packet, negotiates a session, then pushes every .PRC/.PDB file found in
// two SD-card folders onto the device, then ends the sync. Full design/
// rationale in ~/WiRSa/HOTSYNC_PLAN.md.
//
// Unlike the C reference (which casts wire bytes onto __attribute__((packed))
// structs), every multi-byte field here is serialized/parsed by hand via the
// hsPut*/hsGet* helpers below - avoids all alignment/struct-packing risk on
// this target, matching the byte-array idiom sasd.cpp already established in
// this codebase.

#include "Arduino.h"
#include "hotsync.h"
#include "globals.h"         // zmCRC16Update - shared CRC-16/XMODEM table
#include "serial_io.h"
#include "display_menu.h"
#include "network.h"         // drawWifiStatusIcon() - hsDrawHeader() paints it too
#include <SD.h>

// ============================================================================
// Protocol constants
// ============================================================================

// ---- slk.c layer -------------------------------------------------------
#define SLK_MAGIC0        0xBE
#define SLK_MAGIC1        0xEF
#define SLK_MAGIC2        0xED
#define SLK_HDR_LEN       10   // magic[3] + dstSlk + srcSck + pktTyp + dataLen[2] + tid + csum
#define PKT_ADDR_DESKTOP_LINK 3
#define PKT_TYP_HOTSYNC   2
#define PKT_TYP_LOOPBACK  3

// ---- frag.c layer -------------------------------------------------------
#define FRAG_HDR_LEN      4    // typ + flags + ofst[2]
#define FRAG_TYP_DATA     1
#define FRAG_TYP_ACK      2
#define FRAG_FLAG_FIRST   0x80
#define FRAG_FLAG_LAST    0x40
#define FRAG_FLAG_OOM     0x20
#define FRAG_MAX_CHUNK_SZ 524
// The SASD reference used ~150ms/4 retries, tuned for its own bare-metal
// target. Real desktop tooling is far more patient - pilot-link's own PADP
// layer (libpisock/padp.c) uses PI_PADP_TX_TIMEOUT=2000ms x
// PI_PADP_TX_RETRIES=10 per packet. A first real-hardware HotSync attempt
// against this file's original 200ms x 4 (800ms total) budget failed with
// "handshake failed" and no other diagnostic - widened to something much
// closer to pilot-link's own proven numbers, since an under-patient sender
// silently timing out looks identical to a real protocol failure with no
// way to tell them apart from the log alone.
#define FRAG_MAX_RESENDS  8
#define FRAG_ACK_WAIT_MS  1000

// Largest single slk payload we ever need to hold at once: FragHdr(4) + one
// chunk of resource/record data (524) = 528 max for streaming sends (never
// buffered whole - see hsFragSendContinue). For non-streaming request/
// response envelopes (well under 128 bytes each) a smaller scratch buffer
// is used - HS_RXBUF_SIZE below.
#define HS_RXBUF_SIZE     256

// ---- DLP high-level opcodes (hotsync.c) ---------------------------------
#define HS_PKT_REPLY_MASK      0x80
#define HS_PKT_WAKE            0x01
#define HS_PKT_INIT            0x02
#define HS_PKT_GET_SYS_INFO    0x12
#define HS_PKT_DB_CREATE       0x18
#define HS_PKT_DB_CLOSE        0x19
#define HS_PKT_DB_DELETE       0x1a
#define HS_PKT_APP_INFO_WRITE  0x1c
#define HS_PKT_SORT_INFO_WRITE 0x1e
#define HS_PKT_REC_WRITE       0x21
#define HS_PKT_RES_WRITE       0x24
#define HS_PKT_OPEN_CONDUIT    0x2e
#define HS_PKT_SYNC_END        0x2f

#define HS_INIT_FLG_CHANGE_BAUDRATE 0x80

#define HS_ARG_MASK_SMALL   0x80
#define HS_ARG_MASK_LONG    0x40
#define HS_ARG_MASK_IDX     0x3f
#define HS_ARG_IDX_FIRST    0x20

#define HS_ERR_NOT_FOUND    5   // dlpErrNotFound (0x0005) - real Palm OS DLP status code
#define HS_ERR_EXISTS       9   // dlpErrExists   (0x0009)

#define HS_END_CAUSE_NORMAL 0
#define HS_END_CAUSE_OTHER  3

// ---- PRC/PDB header layout ----------------------------------------------
#define PALM_DB_HDR_LEN            0x4E    // fixed header size before the entry list
#define PALM_DB_ATTR_RESOURCE      0x0001  // set => PRC (resource DB), clear => PDB (record DB)
#define PALM_DB_ATTR_RESET         0x0020  // "reset after install" - see hsInstallFile's own comment for when this is actually forced
#define PALM_DB_RES_ENTRY_LEN      10      // type(4) + id(2) + ofst(4)
#define PALM_DB_REC_ENTRY_LEN      8       // ofst(4) + attrs(1) + uniqueID(3)
#define PALM_DB_MAX_ENTRIES        512     // generous vs. the LPC802 reference's MAX_RESOURCES=32 - ESP32 has ample RAM

// ============================================================================
// Byte serialization helpers - big-endian (network byte order), matching the
// real Palm HotSync wire protocol (confirmed against both the reference's
// own SASD_LITTLE_ENDIAN+TO_U16 byteswap build flag and pilot-link's slp.c
// get_short()/crc16() usage).
// ============================================================================

static uint16_t hsPut8(uint8_t *buf, uint16_t pos, uint8_t v) {
  buf[pos] = v;
  return pos + 1;
}
static uint16_t hsPut16(uint8_t *buf, uint16_t pos, uint16_t v) {
  buf[pos] = (uint8_t)(v >> 8);
  buf[pos + 1] = (uint8_t)v;
  return pos + 2;
}
static uint16_t hsPut32(uint8_t *buf, uint16_t pos, uint32_t v) {
  buf[pos] = (uint8_t)(v >> 24);
  buf[pos + 1] = (uint8_t)(v >> 16);
  buf[pos + 2] = (uint8_t)(v >> 8);
  buf[pos + 3] = (uint8_t)v;
  return pos + 4;
}
static uint16_t hsPutBytes(uint8_t *buf, uint16_t pos, const uint8_t *src, uint16_t n) {
  memcpy(buf + pos, src, n);
  return pos + n;
}
static uint16_t hsGet16(const uint8_t *buf, uint16_t pos) {
  return ((uint16_t) buf[pos] << 8) | buf[pos + 1];
}
static uint32_t hsGet32(const uint8_t *buf, uint16_t pos) {
  return ((uint32_t) buf[pos] << 24) | ((uint32_t) buf[pos + 1] << 16) |
         ((uint32_t) buf[pos + 2] << 8) | (uint32_t) buf[pos + 3];
}

// ============================================================================
// State
// ============================================================================

enum HsModeState { HS_WAITING, HS_DONE };
static HsModeState hsState = HS_WAITING;
static unsigned long hsDoneAtMs = 0;

static uint8_t  hsTxTid = 0;
static uint8_t  hsRxBuf[HS_RXBUF_SIZE];
static char     hsCurFileName[40];

// OLED status area state - see hsDrawStatus()'s own comment for the layout.
static char hsStateLine[16] = "Waiting";
static char hsLogLine1[22] = "";
static char hsLogLine2[22] = "";

// True if this session's SASD-folder install actually installed at least
// one PRC (not just found-and-skipped-as-already-existing) - set inside
// hsInstallWalk(), read at session end to pick "Init Complete" over the
// plain "Completed" state label.
static bool hsSasdInitTriggered = false;

extern bool usbDebug;

// ---- forward decls (static helpers) ----------------------------------------
static void hsDrawHeader();
static void hsDrawStatus();
static void hsSetState(const char *state);
static void hsLog(const char *line);
static bool hsReadByte(uint8_t *out, unsigned long deadlineMs);
static void hsSlkSend(uint8_t dst, uint8_t src, uint8_t pktTyp, uint8_t tid,
                       const uint8_t *payload, uint16_t len);
static bool hsSlkRecvOne(uint8_t *outDst, uint8_t *outSrc, uint8_t *outTyp, uint8_t *outTid,
                          uint8_t *payload, uint16_t payloadCap, uint16_t *payloadLenOut,
                          unsigned long deadlineMs);
static uint8_t hsNextTid();
static bool hsFragSendChunk(uint8_t dstSlk, uint8_t srcSlk, uint8_t tid,
                             uint8_t flags, uint16_t ofstField,
                             const uint8_t *data, uint16_t len);
static bool hsFragSendOne(uint8_t tid, const uint8_t *data, uint16_t len);
static bool hsFragRecv(uint8_t *dst, uint16_t dstCap, uint16_t *lenInOut,
                        unsigned long deadlineMs, uint8_t *outTid);

struct HsFragSendState {
  uint16_t curPos;
  uint16_t totalLen;
  uint8_t  tid;
};
static bool hsFragSendStart(HsFragSendState *st, uint8_t tid, uint16_t totalLen);
static bool hsFragSendContinue(HsFragSendState *st, const uint8_t *data, uint16_t len);

static bool hsDlpTxRx(uint8_t pktTyp, const uint8_t *argData, uint16_t argLen,
                       uint16_t *errOut, uint16_t *rspArgOfst, uint16_t *rspArgLen);
static bool hsRspParseOneArg(const uint8_t *buf, uint16_t rxLen, uint16_t *argOfst, uint16_t *argLen);

static uint32_t hsHandshake(const uint8_t *wakePayload, uint16_t wakeLen, uint32_t desiredBaud);
static bool hsOpenConduit();
static bool hsGetSysInfo(uint32_t *romVerOut);
static bool hsEndSync(uint16_t cause);
static bool hsDbDelete(const uint8_t name[32], uint16_t *errOut);
static bool hsDbCreate(const uint8_t name[32], uint32_t type, uint32_t crid, uint16_t flags,
                        uint16_t version, uint8_t *handleOut, uint16_t *errOut);
static bool hsDbClose(uint8_t handle);
static bool hsResWriteBegin(uint8_t handle, uint32_t resType, uint16_t resID, uint16_t len);
static bool hsResWriteContinue(const uint8_t *data, uint16_t len);
static bool hsResWriteFinish();
static bool hsRecWriteBegin(uint8_t handle, uint16_t size, uint32_t recID, uint8_t recAttrs, uint8_t category);
static bool hsRecWriteContinue(const uint8_t *data, uint16_t len);
static bool hsRecWriteFinish();
static bool hsInfoWriteBegin(uint8_t handle, bool appInfo, uint16_t size);
static bool hsInfoWriteContinue(const uint8_t *data, uint16_t len);
static bool hsInfoWriteFinish(bool appInfo);

static void hsInstallWalk(bool *anyFailOut);
static bool hsFolderExists(const char *path);
static bool hsInstallExt(const char *path, const char *ext, bool deleteFirst, bool forceReset,
                          uint16_t *installedCountOut, const char *moveToDir);
static bool hsInstallFile(File &f, bool deleteFirst, bool forceReset, bool *wasInstalledOut);

static HsFragSendState gStreamSt;   // reused for whichever Write*Begin/Continue is active

// ============================================================================
// OLED header - live baud (not the configured showHeader() value - see
// HOTSYNC_PLAN.md round 4) top-right, same boxed-header chrome as sasd.cpp's
// sasdDrawStatusLines()/slip_mode.cpp's slipShowStatus().
// ============================================================================

static void hsDrawHeader() {
  display.fillRect(0, 0, 128, 16, SSD1306_WHITE);
  display.drawLine(1, 1, 126, 1, SSD1306_BLACK);
  display.drawLine(1, 14, 126, 14, SSD1306_BLACK);
  display.drawLine(1, 1, 1, 14, SSD1306_BLACK);
  display.drawLine(126, 1, 126, 14, SSD1306_BLACK);

  display.setCursor(4, 4);
  display.setTextColor(SSD1306_BLACK);
  {
    String line = "HotSync";
    display.print(line);
    display.print(padLeft(liveBaudStr(PhysicalSerial.baudRate()), 20 - line.length()));
  }
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 17);
  display.display();
  drawWifiStatusIcon();  // WiFi status on every title bar
}

// ============================================================================
// OLED status area - plain lines against the black background under the
// title, not the SASD-style white busy-dialog box (showMessage()) - that
// box wastes most of the screen on chrome/padding for what's really just a
// few lines of text. Layout, all left-aligned starting at x=0:
//   y=19  "State: <Waiting|Connected|Syncing|Completed>"
//   y=29  "Details:" (static label)
//   y=39  scrolling detail log line 1 (older)
//   y=49  scrolling detail log line 2 (newer)
// hsLog() shifts line2 into line1 and puts the new entry in line2, so the
// two visible lines scroll up as new detail events come in - callers only
// ever see the latest 2, matching "up to 2 lines...scrolling up" exactly.
// ============================================================================

static void hsDrawStatus() {
  display.fillRect(0, 17, 128, 47, SSD1306_BLACK);
  display.setTextColor(SSD1306_WHITE);

  display.setCursor(0, 19);
  display.print("State: ");
  display.print(hsStateLine);

  display.setCursor(0, 29);
  display.print("Details:");

  display.setCursor(0, 39);
  display.print(hsLogLine1);

  display.setCursor(0, 49);
  display.print(hsLogLine2);

  display.display();
}

static void hsSetState(const char *state) {
  strncpy(hsStateLine, state, sizeof(hsStateLine) - 1);
  hsStateLine[sizeof(hsStateLine) - 1] = 0;
  hsDrawStatus();
}

static void hsLog(const char *line) {
  memcpy(hsLogLine1, hsLogLine2, sizeof(hsLogLine1));   // shift up - same-sized buffers
  strncpy(hsLogLine2, line, sizeof(hsLogLine2) - 1);
  hsLogLine2[sizeof(hsLogLine2) - 1] = 0;
  hsDrawStatus();
}

// ============================================================================
// slk.c layer - single SLP packet send/receive
// ============================================================================

// Reads one byte with an absolute-deadline timeout. Returns false on timeout.
static bool hsReadByte(uint8_t *out, unsigned long deadlineMs) {
  while (!PhysicalSerial.available()) {
    if ((long) (millis() - deadlineMs) >= 0) return false;
  }
  return PhysicalSerial.readBytes(out, 1) == 1;
}

static uint8_t hsSum9(const uint8_t *hdr9) {
  uint8_t s = 0;
  for (int i = 0; i < SLK_HDR_LEN - 1; i++) s += hdr9[i];
  return s;
}

// Sends one complete slk packet: 10-byte header + payload + 2-byte CRC.
static void hsSlkSend(uint8_t dst, uint8_t src, uint8_t pktTyp, uint8_t tid,
                       const uint8_t *payload, uint16_t len) {
  uint8_t hdr[SLK_HDR_LEN];
  uint16_t crc = 0;
  uint8_t crcOut[2];

  hdr[0] = SLK_MAGIC0; hdr[1] = SLK_MAGIC1; hdr[2] = SLK_MAGIC2;
  hdr[3] = dst; hdr[4] = src; hdr[5] = pktTyp;
  hdr[6] = (uint8_t) (len >> 8); hdr[7] = (uint8_t) (len & 0xFF);
  hdr[8] = tid;
  hdr[9] = hsSum9(hdr);

  for (int i = 0; i < SLK_HDR_LEN; i++) crc = zmCRC16Update(crc, hdr[i]);
  for (uint16_t i = 0; i < len; i++) crc = zmCRC16Update(crc, payload[i]);
  crcOut[0] = (uint8_t) (crc >> 8);
  crcOut[1] = (uint8_t) (crc & 0xFF);

  PhysicalSerial.write(hdr, SLK_HDR_LEN);
  if (len) PhysicalSerial.write(payload, len);
  PhysicalSerial.write(crcOut, 2);
}

// Receives ONE full slk packet (header+payload+CRC-verified) within an
// absolute deadline. On a header-checksum mismatch or CRC mismatch, keeps
// scanning for the next valid packet rather than failing outright, matching
// slk.c's own do-while retry-on-corruption loop - still bounded by the same
// overall deadline. Returns false only on timeout.
static bool hsSlkRecvOne(uint8_t *outDst, uint8_t *outSrc, uint8_t *outTyp, uint8_t *outTid,
                          uint8_t *payload, uint16_t payloadCap, uint16_t *payloadLenOut,
                          unsigned long deadlineMs) {
  uint8_t window[SLK_HDR_LEN] = {0};
  // Tracks whether ANY byte at all arrived before the deadline - lets a
  // timeout log distinguish "the other end sent nothing" from "something
  // arrived but never formed a valid header", which look identical without
  // this (both just return false) but mean very different things when
  // diagnosing a stuck handshake.
  uint32_t bytesSeen = 0;

  for (;;) {
    // slide the header window one byte at a time until magic+checksum lock
    for (;;) {
      uint8_t b;
      if (!hsReadByte(&b, deadlineMs)) {
        if (usbDebug) Serial.printf("HotSync: SLP recv timeout (%lu byte(s) seen, no valid header)\r\n", (unsigned long) bytesSeen);
        return false;
      }
      bytesSeen++;
      for (int i = 0; i < SLK_HDR_LEN - 1; i++) window[i] = window[i + 1];
      window[SLK_HDR_LEN - 1] = b;
      if (window[0] == SLK_MAGIC0 && window[1] == SLK_MAGIC1 && window[2] == SLK_MAGIC2 &&
          window[9] == hsSum9(window)) {
        break;
      }
    }

    uint16_t dataLen = hsGet16(window, 6);
    if (dataLen > payloadCap) {
      // Can't be a real packet at this length (or link corruption) - keep
      // scanning rather than trust it; don't reset the window, it'll flush
      // out naturally as new bytes shift through.
      continue;
    }

    uint16_t got = 0;
    while (got < dataLen) {
      if (!hsReadByte(&payload[got], deadlineMs)) {
        if (usbDebug) Serial.printf("HotSync: SLP recv timeout mid-payload (%u/%u bytes, header locked)\r\n", (unsigned) got, (unsigned) dataLen);
        return false;
      }
      got++;
    }
    uint8_t crcBuf[2];
    if (!hsReadByte(&crcBuf[0], deadlineMs) || !hsReadByte(&crcBuf[1], deadlineMs)) {
      if (usbDebug) Serial.println("HotSync: SLP recv timeout mid-CRC (header+payload complete)");
      return false;
    }

    uint16_t crcRxed = ((uint16_t) crcBuf[0] << 8) | crcBuf[1];
    uint16_t crcCalc = 0;
    for (int i = 0; i < SLK_HDR_LEN; i++) crcCalc = zmCRC16Update(crcCalc, window[i]);
    for (uint16_t i = 0; i < dataLen; i++) crcCalc = zmCRC16Update(crcCalc, payload[i]);

    if (crcRxed != crcCalc) {
      if (usbDebug) Serial.printf("HotSync: SLP CRC mismatch (got %04X, exp %04X) - dropped\r\n", crcRxed, crcCalc);
      continue;   // corrupted packet - resync and keep waiting for a good one
    }

    *outDst = window[3];
    *outSrc = window[4];
    *outTyp = window[5];
    *outTid = window[8];
    *payloadLenOut = dataLen;
    return true;
  }
}

static uint8_t hsNextTid() {
  hsTxTid++;
  if (hsTxTid == 0xff) hsTxTid = 1;
  return hsTxTid;
}

// ============================================================================
// frag.c layer - real multi-fragment PADP-style reliability wrapper
// ============================================================================

// Sends one fragment and waits (with resend) for its matching ACK.
static bool hsFragSendChunk(uint8_t dstSlk, uint8_t srcSlk, uint8_t tid,
                             uint8_t flags, uint16_t ofstField,
                             const uint8_t *data, uint16_t len) {
  uint8_t out[FRAG_HDR_LEN + FRAG_MAX_CHUNK_SZ];
  uint16_t pos = 0;
  pos = hsPut8(out, pos, FRAG_TYP_DATA);
  pos = hsPut8(out, pos, flags);
  pos = hsPut16(out, pos, ofstField);
  pos = hsPutBytes(out, pos, data, len);

  for (uint8_t attempt = 0; attempt < FRAG_MAX_RESENDS; attempt++) {
    if (attempt > 0 && usbDebug) {
      Serial.printf("HotSync: resend attempt %u/%u (tid=%u flags=0x%02x ofst=%u len=%u)\r\n",
                     (unsigned) attempt + 1, (unsigned) FRAG_MAX_RESENDS, tid, flags, ofstField, len);
    }
    hsSlkSend(dstSlk, srcSlk, PKT_TYP_HOTSYNC, tid, out, pos);

    unsigned long deadline = millis() + FRAG_ACK_WAIT_MS;
    for (;;) {
      uint8_t gDst, gSrc, gTyp, gTid;
      // A Palm's own ACK packets carry 2 extra bytes of payload beyond the
      // bare 4-byte FragHdr (a known PalmOS quirk, also flagged in the SASD
      // reference's frag.c). Sized generously here and only the first 4
      // bytes (typ/flags/ofst) are actually inspected below - any trailing
      // bytes, garbage or otherwise, are simply ignored.
      uint8_t rxBuf[16];
      uint16_t rxLen;
      if (!hsSlkRecvOne(&gDst, &gSrc, &gTyp, &gTid, rxBuf, sizeof(rxBuf), &rxLen, deadline))
        break;   // timed out waiting for this attempt's ack - resend

      if (rxLen < FRAG_HDR_LEN) continue;
      uint8_t gFragTyp = rxBuf[0];
      uint8_t gFragFlags = rxBuf[1];
      uint16_t gFragOfst = hsGet16(rxBuf, 2);

      // Must be addressed back to us, same pktTyp/tid, an ACK. Does not
      // require the echoed ofst/size field to match what we sent - matches
      // pilot-link's own desktop-side implementation (padp_tx(),
      // libpisock/padp.c), which matches an ack by (type==ACK, txid==ours)
      // only, never by the size field.
      if (gDst != srcSlk || gSrc != dstSlk || gTyp != PKT_TYP_HOTSYNC || gTid != tid) {
        if (usbDebug && (gTyp == PKT_TYP_HOTSYNC)) {
          Serial.printf("HotSync: ignoring unrelated pkt dst=%u src=%u tid=%u (want tid=%u)\r\n",
                         gDst, gSrc, gTid, tid);
        }
        continue;
      }
      if (gFragTyp != FRAG_TYP_ACK) continue;

      if (gFragFlags & FRAG_FLAG_OOM) {
        if (usbDebug) Serial.println("HotSync: device reported OOM, aborting send");
        return false;   // device out of memory - no point retrying
      }
      return true;
    }
  }
  if (usbDebug) {
    Serial.printf("HotSync: no ack after %u attempts (tid=%u flags=0x%02x ofst=%u len=%u) - giving up\r\n",
                   (unsigned) FRAG_MAX_RESENDS, tid, flags, ofstField, len);
  }
  return false;
}

static bool hsFragSendStart(HsFragSendState *st, uint8_t tid, uint16_t totalLen) {
  st->curPos = 0;
  st->totalLen = totalLen;
  st->tid = tid;
  return true;
}

// May be called repeatedly with arbitrary-sized increments (e.g. a small SD
// read-buffer at a time) - the FRAG_MAX_CHUNK_SZ chunk boundary and FIRST/
// LAST flags are computed purely from st->curPos/st->totalLen, independent
// of how much data the caller hands over per call. This is what lets a large
// PRC resource stream straight from SD without ever being buffered whole.
static bool hsFragSendContinue(HsFragSendState *st, const uint8_t *data, uint16_t len) {
  if (!len) return true;
  if ((uint32_t) st->totalLen - st->curPos < len) return false;   // caller bug - overrun

  uint16_t ofst = 0;
  while (ofst < len) {
    uint16_t now = len - ofst;
    if (now > FRAG_MAX_CHUNK_SZ) now = FRAG_MAX_CHUNK_SZ;

    uint8_t flags = 0;
    uint16_t ofstField;
    if (st->curPos == 0) {
      flags |= FRAG_FLAG_FIRST;
      ofstField = st->totalLen;
    } else {
      ofstField = st->curPos;
    }
    if (st->curPos + now == st->totalLen) flags |= FRAG_FLAG_LAST;

    if (!hsFragSendChunk(PKT_ADDR_DESKTOP_LINK, PKT_ADDR_DESKTOP_LINK, st->tid,
                          flags, ofstField, data + ofst, now))
      return false;

    ofst += now;
    st->curPos += now;
  }
  return true;
}

// One-shot convenience: send data (<= FRAG_MAX_CHUNK_SZ, true for every
// non-streaming request in this file) as a single FIRST|LAST fragment.
static bool hsFragSendOne(uint8_t tid, const uint8_t *data, uint16_t len) {
  HsFragSendState st;
  return hsFragSendStart(&st, tid, len) && hsFragSendContinue(&st, data, len);
}

// Receives one (possibly multi-fragment) message addressed to us, auto-
// acking each fragment as it arrives. Loosely matches slk address+type only
// (NOT tid against any prior request) - matching the reference's own
// hotsyncPrvRx, since this protocol is strictly sequential (never
// pipelined), so "the next well-formed message addressed to us" IS the
// reply we're waiting for. Returns false on timeout or a malformed stream.
static bool hsFragRecv(uint8_t *dst, uint16_t dstCap, uint16_t *lenInOut,
                        unsigned long deadlineMs, uint8_t *outTid) {
  uint16_t cap = *lenInOut;
  uint16_t done = 0;
  uint16_t expected = 0;
  bool haveFirst = false;
  uint8_t lockedDst = 0, lockedSrc = 0, lockedTyp = 0, lockedTid = 0;
  uint8_t rawBuf[FRAG_HDR_LEN + FRAG_MAX_CHUNK_SZ];

  for (;;) {
    uint8_t gDst, gSrc, gTyp, gTid;
    uint16_t rawLen;
    if (!hsSlkRecvOne(&gDst, &gSrc, &gTyp, &gTid, rawBuf, sizeof(rawBuf), &rawLen, deadlineMs))
      return false;

    if (gTyp == PKT_TYP_LOOPBACK) continue;   // not expected here, but tolerate/ignore
    if (rawLen < FRAG_HDR_LEN) continue;

    uint8_t fragTyp = rawBuf[0];
    uint8_t fragFlags = rawBuf[1];
    uint16_t fragOfst = hsGet16(rawBuf, 2);
    const uint8_t *fragData = rawBuf + FRAG_HDR_LEN;
    uint16_t fragDataLen = rawLen - FRAG_HDR_LEN;

    if (fragTyp != FRAG_TYP_DATA) continue;   // stray ACK or similar - ignore

    if (gDst != PKT_ADDR_DESKTOP_LINK || gSrc != PKT_ADDR_DESKTOP_LINK || gTyp != PKT_TYP_HOTSYNC)
      continue;   // not addressed to the hotsync desktop-link socket - ignore

    uint16_t nowOfst;
    if (!haveFirst) {
      if (!(fragFlags & FRAG_FLAG_FIRST)) continue;   // waiting for the start of a new message
      expected = fragOfst;   // FIRST fragment's ofst field carries the TOTAL length
      if (expected > cap) return false;   // won't fit - caller's buffer too small
      lockedDst = gDst; lockedSrc = gSrc; lockedTyp = gTyp; lockedTid = gTid;
      haveFirst = true;
      nowOfst = 0;
    } else {
      if (gDst != lockedDst || gSrc != lockedSrc || gTyp != lockedTyp || gTid != lockedTid) continue;
      nowOfst = fragOfst;
    }

    if (nowOfst != done) continue;   // out of order - drop, sender will resend
    if ((uint32_t) expected - done < fragDataLen) return false;   // sender overran - protocol error
    bool isLastFrag = (fragFlags & FRAG_FLAG_LAST) != 0;
    bool shouldBeLast = (expected - done == fragDataLen);
    if (isLastFrag != shouldBeLast) return false;   // malformed LAST flag - protocol error

    // ack it (echo the same flags/ofst back, addressed the other way)
    {
      uint8_t ackHdr[FRAG_HDR_LEN];
      uint16_t p = 0;
      p = hsPut8(ackHdr, p, FRAG_TYP_ACK);
      p = hsPut8(ackHdr, p, fragFlags);
      p = hsPut16(ackHdr, p, fragOfst);
      hsSlkSend(gSrc, gDst, gTyp, gTid, ackHdr, p);
    }

    if (fragDataLen && done + fragDataLen <= dstCap) memcpy(dst + done, fragData, fragDataLen);
    done += fragDataLen;

    if (done == expected) {
      *lenInOut = done;
      if (outTid) *outTid = lockedTid;
      return true;
    }
  }
}

// ============================================================================
// DLP high-level request/response envelope
// ============================================================================

// Sends a single-arg (or no-arg, argLen==0) HL DLP request using the "tiny"
// arg wrapper (every non-streaming request here is comfortably under 255
// bytes) and waits for its response. Returns false only on a transport-level
// failure (no usable reply at all) - a non-zero DLP status in *errOut is
// still a successful transaction as far as this function is concerned; the
// caller decides what a given status code means.
static bool hsDlpTxRx(uint8_t pktTyp, const uint8_t *argData, uint16_t argLen,
                       uint16_t *errOut, uint16_t *rspArgOfst, uint16_t *rspArgLen) {
  uint8_t txBuf[HS_RXBUF_SIZE];
  uint16_t pos = 0;
  pos = hsPut8(txBuf, pos, pktTyp);
  pos = hsPut8(txBuf, pos, argLen ? 1 : 0);
  if (argLen) {
    pos = hsPut8(txBuf, pos, HS_ARG_IDX_FIRST);
    pos = hsPut8(txBuf, pos, (uint8_t) argLen);
    pos = hsPutBytes(txBuf, pos, argData, argLen);
  }
  // Every real request in the reference is padded (with zero bytes, outside
  // the formal arg length) to a minimum of 4 bytes and to an even total
  // length - e.g. its own argless OpenConduit request tacks on a 2-byte
  // "sbz" field despite nArgs=0, and DbClose's 1-byte arg gets one extra
  // padding byte. Matching both rules generically here covers every call
  // site without needing to special-case each one individually.
  while (pos < 4 || (pos & 1)) pos = hsPut8(txBuf, pos, 0);

  uint8_t tid = hsNextTid();
  if (!hsFragSendOne(tid, txBuf, pos)) return false;

  uint16_t rxLen = sizeof(hsRxBuf);
  if (!hsFragRecv(hsRxBuf, sizeof(hsRxBuf), &rxLen, millis() + 3000, NULL)) return false;

  if (rxLen < 4) return false;
  uint8_t rspTyp = hsRxBuf[0];
  if (rspTyp != (HS_PKT_REPLY_MASK | pktTyp)) return false;
  *errOut = hsGet16(hsRxBuf, 2);

  if (rspArgOfst && rspArgLen) {
    if (!hsRspParseOneArg(hsRxBuf, rxLen, rspArgOfst, rspArgLen)) {
      *rspArgOfst = 0;
      *rspArgLen = 0;
    }
  }
  return true;
}

// Parses at most one response arg (every opcode used in this file returns 0
// or 1 args) - a simplification of the reference's fully general N-arg
// parser, justified since nothing here ever needs more than one.
static bool hsRspParseOneArg(const uint8_t *buf, uint16_t rxLen, uint16_t *argOfst, uint16_t *argLen) {
  uint8_t nArgs = buf[1];
  if (nArgs == 0) { *argOfst = 4; *argLen = 0; return true; }
  if (nArgs != 1 || rxLen < 4 + 2) return false;

  uint8_t idByte = buf[4];
  if ((idByte & HS_ARG_MASK_IDX) != HS_ARG_IDX_FIRST) return false;

  if (idByte & HS_ARG_MASK_SMALL) {
    if (rxLen < 4 + 4) return false;
    uint16_t len = hsGet16(buf, 4 + 2);
    if (4 + 4 + len > rxLen) return false;
    *argOfst = 4 + 4;
    *argLen = len;
    return true;
  }
  if (idByte & HS_ARG_MASK_LONG) return false;   // long format unused/unsupported here
  uint8_t len = buf[4 + 1];
  if (4 + 2 + len > rxLen) return false;
  *argOfst = 4 + 2;
  *argLen = len;
  return true;
}

// ---- WAKE/INIT handshake ------------------------------------------------

// wakePayload is the already-defragmented raw WAKE packet body (struct
// HsInitPkt equivalent: pktTyp,flags,ver[2],sbz[2],maxBaud[4] = 10 bytes).
// Sends the INIT reply and returns the baud rate to switch to (0 on error -
// matches hotsyncInit's own "0 = fail" convention).
static uint32_t hsHandshake(const uint8_t *wakePayload, uint16_t wakeLen, uint32_t desiredBaud) {
  if (wakeLen < 10) {
    if (usbDebug) Serial.printf("HotSync: WAKE too short (%u bytes, need 10)\r\n", (unsigned) wakeLen);
    return 0;
  }
  // Validation matches pilot-link's own cmp_rx_handshake (libpisock/cmp.c)
  // rather than the SASD reference firmware's stricter check (which also
  // requires minor version <=3 and reserved==0) - a Palm is only
  // guaranteed to send major version 1 in the high byte, and pilot-link
  // itself never gates on the minor version or reserved bytes.
  if (wakePayload[0] != HS_PKT_WAKE || wakePayload[2] != 1) {
    if (usbDebug) {
      Serial.printf("HotSync: WAKE rejected - type=0x%02x ver=%u.%u sbz=%u\r\n",
                     wakePayload[0], wakePayload[2], wakePayload[3], hsGet16(wakePayload, 4));
    }
    return 0;
  }

  uint32_t maxBaud = hsGet32(wakePayload, 6);
  uint32_t curBaud = 9600;
  uint8_t flags = 0;

  if (maxBaud > curBaud && desiredBaud > curBaud) {
    flags |= HS_INIT_FLG_CHANGE_BAUDRATE;
    curBaud = (desiredBaud > maxBaud) ? maxBaud : desiredBaud;
  }

  if (usbDebug) {
    Serial.printf("HotSync: WAKE ok (ver=%u.%u, maxBaud=%lu) - sending INIT (curBaud=%lu)\r\n",
                   wakePayload[2], wakePayload[3], (unsigned long) maxBaud, (unsigned long) curBaud);
  }

  uint8_t init[10];
  uint16_t pos = 0;
  pos = hsPut8(init, pos, HS_PKT_INIT);
  pos = hsPut8(init, pos, flags);
  pos = hsPut8(init, pos, 1);
  pos = hsPut8(init, pos, 0);
  pos = hsPut16(init, pos, 0);
  pos = hsPut32(init, pos, curBaud);

  if (usbDebug) {
    // Confirm the actual UART rate at the moment we transmit (rules out a
    // baud mismatch) and dump the exact CMP payload bytes about to go out
    // (rules out an encoding bug) - both invisible from any higher-level log.
    Serial.printf("HotSync: TX baud=%lu, INIT payload:", (unsigned long) PhysicalSerial.baudRate());
    for (uint16_t i = 0; i < pos; i++) Serial.printf(" %02X", init[i]);
    Serial.println();
  }

  if (!hsFragSendOne(hsNextTid(), init, pos)) {
    if (usbDebug) Serial.println("HotSync: INIT reply never got acked - handshake abandoned");
    return 0;
  }

  if (usbDebug) Serial.println("HotSync: INIT acked");

  if (flags & HS_INIT_FLG_CHANGE_BAUDRATE) delay(100);   // let both ends settle before switching

  return curBaud;
}

static bool hsOpenConduit() {
  uint16_t err;
  if (!hsDlpTxRx(HS_PKT_OPEN_CONDUIT, NULL, 0, &err, NULL, NULL)) return false;
  return err == 0;
}

static bool hsGetSysInfo(uint32_t *romVerOut) {
  uint8_t arg[4];
  uint16_t p = 0;
  p = hsPut16(arg, p, 1);   // hotsync version major=1
  p = hsPut16(arg, p, 2);   //                  minor=2
  uint16_t err, argOfst, argLen;
  if (!hsDlpTxRx(HS_PKT_GET_SYS_INFO, arg, p, &err, &argOfst, &argLen)) return false;
  if (err || argLen < 4) return false;
  *romVerOut = hsGet32(hsRxBuf, argOfst);
  return true;
}

static bool hsEndSync(uint16_t cause) {
  uint8_t arg[2];
  hsPut16(arg, 0, cause);
  uint16_t err;
  if (!hsDlpTxRx(HS_PKT_SYNC_END, arg, sizeof(arg), &err, NULL, NULL)) return false;
  return err == 0;
}

// ---- DB create/delete/close ---------------------------------------------

static bool hsDbDelete(const uint8_t name[32], uint16_t *errOut) {
  uint8_t arg[34];
  uint16_t p = 0;
  p = hsPut8(arg, p, 0);      // cardNo
  p = hsPut8(arg, p, 0);      // openMode (unused for delete)
  p = hsPutBytes(arg, p, name, 32);
  return hsDlpTxRx(HS_PKT_DB_DELETE, arg, p, errOut, NULL, NULL);
}

static bool hsDbCreate(const uint8_t name[32], uint32_t type, uint32_t crid, uint16_t flags,
                        uint16_t version, uint8_t *handleOut, uint16_t *errOut) {
  // crid(4)+type(4)+cardNo(1)+rfu(1)+flags(2)+version(2)+name(32) = 46 bytes
  uint8_t arg[46];
  uint16_t p = 0;
  p = hsPut32(arg, p, crid);
  p = hsPut32(arg, p, type);
  p = hsPut8(arg, p, 0);       // cardNo
  p = hsPut8(arg, p, 0);       // rfu
  p = hsPut16(arg, p, flags);
  p = hsPut16(arg, p, version);
  p = hsPutBytes(arg, p, name, 32);

  uint16_t argOfst, argLen;
  if (!hsDlpTxRx(HS_PKT_DB_CREATE, arg, p, errOut, &argOfst, &argLen)) return false;
  if (*errOut) return true;   // transaction ok, but device reported an error - caller inspects *errOut
  if (argLen < 1) return false;
  *handleOut = hsRxBuf[argOfst];
  return true;
}

static bool hsDbClose(uint8_t handle) {
  uint8_t arg[1] = { handle };
  uint16_t err;
  if (!hsDlpTxRx(HS_PKT_DB_CLOSE, arg, sizeof(arg), &err, NULL, NULL)) return false;
  return err == 0;
}

// ---- resource/record/info-block streaming writes -------------------------

static bool hsResWriteBegin(uint8_t handle, uint32_t resType, uint16_t resID, uint16_t len) {
  // header(2) + small-wrapper(4) + HsResWriteReq(dbHandle1+rfu1+resTyp4+resID2+resSize2=10) = 16 bytes
  uint8_t hdr[16];
  uint16_t p = 0;
  p = hsPut8(hdr, p, HS_PKT_RES_WRITE);
  p = hsPut8(hdr, p, 1);
  p = hsPut8(hdr, p, HS_ARG_IDX_FIRST | HS_ARG_MASK_SMALL);
  p = hsPut8(hdr, p, 0);
  p = hsPut16(hdr, p, (uint16_t) (10 + len));
  p = hsPut8(hdr, p, handle);
  p = hsPut8(hdr, p, 0);
  p = hsPut32(hdr, p, resType);
  p = hsPut16(hdr, p, resID);
  p = hsPut16(hdr, p, len);

  uint8_t tid = hsNextTid();
  return hsFragSendStart(&gStreamSt, tid, (uint16_t) (p + len)) &&
         hsFragSendContinue(&gStreamSt, hdr, p);
}
static bool hsResWriteContinue(const uint8_t *data, uint16_t len) {
  return hsFragSendContinue(&gStreamSt, data, len);
}
static bool hsResWriteFinish() {
  uint16_t rxLen = sizeof(hsRxBuf);
  if (!hsFragRecv(hsRxBuf, sizeof(hsRxBuf), &rxLen, millis() + 5000, NULL)) return false;
  if (rxLen < 4 || hsRxBuf[0] != (HS_PKT_REPLY_MASK | HS_PKT_RES_WRITE)) return false;
  return hsGet16(hsRxBuf, 2) == 0;
}

static bool hsRecWriteBegin(uint8_t handle, uint16_t size, uint32_t recID, uint8_t recAttrs, uint8_t category) {
  // header(2) + small-wrapper(4) + HsRecWriteReq(dbHandle1+flags1+recID4+recAttrs1+category1=8) = 14 bytes
  uint8_t hdr[14];
  uint16_t p = 0;
  p = hsPut8(hdr, p, HS_PKT_REC_WRITE);
  p = hsPut8(hdr, p, 1);
  p = hsPut8(hdr, p, HS_ARG_IDX_FIRST | HS_ARG_MASK_SMALL);
  p = hsPut8(hdr, p, 0);
  p = hsPut16(hdr, p, (uint16_t) (8 + size));
  p = hsPut8(hdr, p, handle);
  p = hsPut8(hdr, p, 0x80);
  p = hsPut32(hdr, p, recID);
  p = hsPut8(hdr, p, recAttrs);
  p = hsPut8(hdr, p, category);

  uint8_t tid = hsNextTid();
  return hsFragSendStart(&gStreamSt, tid, (uint16_t) (p + size)) &&
         hsFragSendContinue(&gStreamSt, hdr, p);
}
static bool hsRecWriteContinue(const uint8_t *data, uint16_t len) {
  return hsFragSendContinue(&gStreamSt, data, len);
}
static bool hsRecWriteFinish() {
  uint16_t rxLen = sizeof(hsRxBuf);
  if (!hsFragRecv(hsRxBuf, sizeof(hsRxBuf), &rxLen, millis() + 5000, NULL)) return false;
  if (rxLen < 4 || hsRxBuf[0] != (HS_PKT_REPLY_MASK | HS_PKT_REC_WRITE)) return false;
  return hsGet16(hsRxBuf, 2) == 0;
}

static bool hsInfoWriteBegin(uint8_t handle, bool appInfo, uint16_t size) {
  uint8_t hdr[2 + 4 + 4];
  uint16_t p = 0;
  p = hsPut8(hdr, p, appInfo ? HS_PKT_APP_INFO_WRITE : HS_PKT_SORT_INFO_WRITE);
  p = hsPut8(hdr, p, 1);
  p = hsPut8(hdr, p, HS_ARG_IDX_FIRST | HS_ARG_MASK_SMALL);
  p = hsPut8(hdr, p, 0);
  p = hsPut16(hdr, p, (uint16_t) (4 + size));
  p = hsPut8(hdr, p, handle);
  p = hsPut8(hdr, p, 0);
  p = hsPut16(hdr, p, size);

  uint8_t tid = hsNextTid();
  return hsFragSendStart(&gStreamSt, tid, (uint16_t) (p + size)) &&
         hsFragSendContinue(&gStreamSt, hdr, p);
}
static bool hsInfoWriteContinue(const uint8_t *data, uint16_t len) {
  return hsFragSendContinue(&gStreamSt, data, len);
}
static bool hsInfoWriteFinish(bool appInfo) {
  uint16_t rxLen = sizeof(hsRxBuf);
  if (!hsFragRecv(hsRxBuf, sizeof(hsRxBuf), &rxLen, millis() + 5000, NULL)) return false;
  uint8_t want = HS_PKT_REPLY_MASK | (appInfo ? HS_PKT_APP_INFO_WRITE : HS_PKT_SORT_INFO_WRITE);
  if (rxLen < 4 || hsRxBuf[0] != want) return false;
  return hsGet16(hsRxBuf, 2) == 0;
}

// ============================================================================
// PRC/PDB file reader - shared header parse to PALM_DB_HDR_LEN (0x4E), then
// branch on PALM_DB_ATTR_RESOURCE into a resource-entry (10B) or record-
// entry (8B) reader. Extends the reference's prc.c (PRC-only) to also read
// PDB files, plus optional AppInfo/SortInfo block extraction.
// ============================================================================

struct HsDbEntry {
  uint32_t ofst;          // file offset of this entry's data (both kinds)
  uint32_t type;          // resource only: resource type
  uint16_t id;             // resource only: resource ID
  uint8_t  recAttrs;       // record only: raw on-disk attrs byte (category in low nibble)
  uint32_t recUniqueId;    // record only: 24-bit on-disk unique ID, zero-extended
};

static struct {
  uint8_t     name[32];
  uint16_t    dbAttrs;
  uint16_t    version;
  uint32_t    type, crid;
  uint32_t    appInfoOfst, sortInfoOfst;
  bool        isResourceDb;
  uint16_t    numEntries;
  HsDbEntry   entries[PALM_DB_MAX_ENTRIES];
  uint32_t    fileSize;
} hsFile;

// Reads exactly `n` bytes or fails. Small helper since File::read() can short-read.
static bool hsFileReadExact(File &f, uint8_t *buf, uint16_t n) {
  return f.read(buf, n) == n;
}

static bool hsParseDbHeader(File &f, uint32_t size) {
  if (size < PALM_DB_HDR_LEN) return false;

  uint8_t hdr[PALM_DB_HDR_LEN];
  if (!hsFileReadExact(f, hdr, PALM_DB_HDR_LEN)) return false;

  memcpy(hsFile.name, hdr, 32);
  hsFile.name[31] = 0;   // ensure NUL-terminated even if the file's name field is exactly 32 non-null bytes
  hsFile.dbAttrs = hsGet16(hdr, 32);
  hsFile.version = hsGet16(hdr, 34);
  // offsets 0x24-0x33: crDate/modDate/bkupDate/modNum - not needed, skipped implicitly (not re-read)
  hsFile.appInfoOfst = hsGet32(hdr, 0x34);
  hsFile.sortInfoOfst = hsGet32(hdr, 0x38);
  hsFile.type = hsGet32(hdr, 0x3C);
  hsFile.crid = hsGet32(hdr, 0x40);
  // 0x44-0x47: uniqueIDSeed - ignored
  uint32_t nextRecList = hsGet32(hdr, 0x48);
  if (nextRecList) return false;   // segmented record lists unsupported (matches reference)
  hsFile.numEntries = hsGet16(hdr, 0x4C);
  hsFile.isResourceDb = (hsFile.dbAttrs & PALM_DB_ATTR_RESOURCE) != 0;
  hsFile.fileSize = size;

  if (hsFile.numEntries > PALM_DB_MAX_ENTRIES) return false;

  uint16_t entryLen = hsFile.isResourceDb ? PALM_DB_RES_ENTRY_LEN : PALM_DB_REC_ENTRY_LEN;
  for (uint16_t i = 0; i < hsFile.numEntries; i++) {
    uint8_t e[PALM_DB_REC_ENTRY_LEN > PALM_DB_RES_ENTRY_LEN ? PALM_DB_REC_ENTRY_LEN : PALM_DB_RES_ENTRY_LEN];
    if (!hsFileReadExact(f, e, entryLen)) return false;

    if (hsFile.isResourceDb) {
      hsFile.entries[i].type = hsGet32(e, 0);
      hsFile.entries[i].id = hsGet16(e, 4);
      hsFile.entries[i].ofst = hsGet32(e, 6);
    } else {
      hsFile.entries[i].ofst = hsGet32(e, 0);
      hsFile.entries[i].recAttrs = e[4];
      hsFile.entries[i].recUniqueId = ((uint32_t) e[5] << 16) | ((uint32_t) e[6] << 8) | e[7];
    }

    if (hsFile.entries[i].ofst > size) return false;
    if (i && hsFile.entries[i - 1].ofst >= hsFile.entries[i].ofst) return false;   // must be strictly increasing
  }

  uint32_t dataStart = PALM_DB_HDR_LEN + (uint32_t) hsFile.numEntries * entryLen;
  if (hsFile.numEntries && hsFile.entries[0].ofst < dataStart) return false;

  return true;
}

static uint32_t hsEntrySize(uint16_t idx) {
  uint32_t nextOfst = (idx == hsFile.numEntries - 1) ? hsFile.fileSize : hsFile.entries[idx + 1].ofst;
  return nextOfst - hsFile.entries[idx].ofst;
}

// ============================================================================
// Install driver
// ============================================================================

enum HsStreamTarget { HS_STREAM_RES, HS_STREAM_REC, HS_STREAM_INFO };

// Streams one entry's raw bytes (resource, record, or AppInfo/SortInfo block
// data) from `f` straight into whichever Write*Continue is currently active,
// via a small fixed buffer - never holds a whole entry in RAM.
static bool hsStreamEntryData(File &f, uint32_t ofst, uint32_t len, HsStreamTarget target) {
  if (!f.seek(ofst)) return false;
  uint8_t buf[256];
  uint32_t done = 0;
  while (done < len) {
    uint32_t now = len - done;
    if (now > sizeof(buf)) now = sizeof(buf);
    if (!hsFileReadExact(f, buf, (uint16_t) now)) return false;
    bool ok;
    switch (target) {
      case HS_STREAM_REC:  ok = hsRecWriteContinue(buf, (uint16_t) now); break;
      case HS_STREAM_INFO: ok = hsInfoWriteContinue(buf, (uint16_t) now); break;
      default:              ok = hsResWriteContinue(buf, (uint16_t) now); break;
    }
    if (!ok) return false;
    done += now;
  }
  return true;
}

// Installs one already-open PRC/PDB file. `deleteFirst` selects the
// INSTALL-folder policy (delete-then-reinstall) vs the SASD-folder policy
// (skip-if-exists, no delete attempted). `forceReset` controls whether
// PALM_DB_ATTR_RESET gets OR'd onto the file's own on-disk attrs.
// `*wasInstalledOut`, if non-NULL, is set true iff this file was actually
// created (not skipped because it already existed) - needed by the SASD
// folder's own "any PRC installed -> force-override the PDB pass" rule.
static bool hsInstallFile(File &f, bool deleteFirst, bool forceReset, bool *wasInstalledOut) {
  if (wasInstalledOut) *wasInstalledOut = false;

  uint32_t size = f.size();
  if (!hsParseDbHeader(f, size)) {
    if (usbDebug) Serial.printf("HotSync: '%s' - not a valid PRC/PDB header\r\n", f.name());
    return false;
  }

  if (deleteFirst) {
    uint16_t delErr;
    if (!hsDbDelete(hsFile.name, &delErr)) {
      if (usbDebug) Serial.printf("HotSync: delete '%s' - transport failure\r\n", f.name());
    } else if (delErr == HS_ERR_NOT_FOUND) {
      if (usbDebug) Serial.printf("HotSync: '%s' - not previously installed\r\n", f.name());
    } else if (delErr == 0) {
      if (usbDebug) Serial.printf("HotSync: '%s' - deleted existing copy\r\n", f.name());
    } else if (usbDebug) {
      Serial.printf("HotSync: delete '%s' - err 0x%04x (continuing anyway)\r\n", f.name(), delErr);
    }
  }

  // Real desktop installers (pilot-link's own pi_file_install(), libpisock/
  // pi-file.c) pass the DB's on-disk attrs straight through - `flags =
  // pf->info.flags;` - and only OR in the reset bit for one deliberate,
  // narrow Graffiti-specific special case, never unconditionally. An
  // earlier pass here always OR'd in PALM_DB_ATTR_RESET (copied from the
  // SASD reference firmware's OWN deliberate choice, appropriate for ITS
  // bootstrap-a-blank-device use case) for every install, which caused a
  // real, unwanted Palm reset after installing an ordinary INSTALL-folder
  // app that never asked for one. Now only forced for the SASD folder,
  // matching the plan's own "drivers" framing for that folder specifically -
  // the INSTALL folder passes the file's own attrs through unmodified, same
  // as a real desktop install would.
  uint16_t flags = hsFile.dbAttrs;
  if (forceReset) flags |= PALM_DB_ATTR_RESET;
  uint8_t handle;
  uint16_t crErr;
  if (!hsDbCreate(hsFile.name, hsFile.type, hsFile.crid, flags, hsFile.version, &handle, &crErr)) {
    if (usbDebug) Serial.printf("HotSync: create '%s' - transport failure\r\n", f.name());
    return false;
  }
  if (crErr == HS_ERR_EXISTS && !deleteFirst) {
    if (usbDebug) Serial.printf("HotSync: '%s' - already installed, skipping\r\n", f.name());
    return true;   // not a failure - this is the expected skip path
  }
  if (crErr) {
    if (usbDebug) Serial.printf("HotSync: create '%s' - err 0x%04x\r\n", f.name(), crErr);
    return false;
  }

  // Create genuinely succeeded (not a skip) from here on - counts as
  // "installed" for the caller's purposes even if a later write step fails,
  // since the DB now exists on the device and installation was attempted.
  if (wasInstalledOut) *wasInstalledOut = true;

  bool ok = true;

  if (hsFile.appInfoOfst) {
    uint32_t nextOfst = hsFile.numEntries ? hsFile.entries[0].ofst : size;
    uint32_t len = (hsFile.sortInfoOfst ? hsFile.sortInfoOfst : nextOfst) - hsFile.appInfoOfst;
    if (hsInfoWriteBegin(handle, true, (uint16_t) len) &&
        hsStreamEntryData(f, hsFile.appInfoOfst, len, HS_STREAM_INFO) &&
        hsInfoWriteFinish(true)) {
      // ok
    } else {
      if (usbDebug) Serial.printf("HotSync: '%s' - AppInfo block write failed\r\n", f.name());
      ok = false;
    }
  }
  if (ok && hsFile.sortInfoOfst) {
    uint32_t nextOfst = hsFile.numEntries ? hsFile.entries[0].ofst : size;
    uint32_t len = nextOfst - hsFile.sortInfoOfst;
    if (hsInfoWriteBegin(handle, false, (uint16_t) len) &&
        hsStreamEntryData(f, hsFile.sortInfoOfst, len, HS_STREAM_INFO) &&
        hsInfoWriteFinish(false)) {
      // ok
    } else {
      if (usbDebug) Serial.printf("HotSync: '%s' - SortInfo block write failed\r\n", f.name());
      ok = false;
    }
  }

  for (uint16_t i = 0; ok && i < hsFile.numEntries; i++) {
    uint32_t entrySize = hsEntrySize(i);
    if (entrySize >> 16) { ok = false; break; }   // single-entry size cap - matches reference's own limitation

    if (hsFile.isResourceDb) {
      ok = hsResWriteBegin(handle, hsFile.entries[i].type, hsFile.entries[i].id, (uint16_t) entrySize) &&
           hsStreamEntryData(f, hsFile.entries[i].ofst, entrySize, HS_STREAM_RES) &&
           hsResWriteFinish();
    } else {
      uint8_t recAttrs = hsFile.entries[i].recAttrs & 0xF0;
      uint8_t category = hsFile.entries[i].recAttrs & 0x0F;
      ok = hsRecWriteBegin(handle, (uint16_t) entrySize, hsFile.entries[i].recUniqueId, recAttrs, category) &&
           hsStreamEntryData(f, hsFile.entries[i].ofst, entrySize, HS_STREAM_REC) &&
           hsRecWriteFinish();
    }
    if (!ok && usbDebug) Serial.printf("HotSync: '%s' - entry %u write failed\r\n", f.name(), (unsigned) i);
  }

  if (!hsDbClose(handle) && usbDebug) Serial.printf("HotSync: '%s' - close failed\r\n", f.name());

  if (ok && usbDebug) Serial.printf("HotSync: '%s' - installed\r\n", f.name());
  return ok;
}

// Case-insensitive suffix compare, written by hand rather than relying on
// strcasecmp() being in scope (not used elsewhere in this codebase, and its
// declaration's availability without an explicit <strings.h> include isn't
// worth gambling on here).
static bool hsHasExtension(const char *name, const char *ext) {
  size_t nameLen = strlen(name), extLen = strlen(ext);
  if (nameLen < extLen) return false;
  const char *suffix = name + (nameLen - extLen);
  for (size_t i = 0; i < extLen; i++) {
    char a = suffix[i], b = ext[i];
    if (a >= 'a' && a <= 'z') a -= 32;
    if (b >= 'a' && b <= 'z') b -= 32;
    if (a != b) return false;
  }
  return true;
}

// True if the file's own basename starts with '.' - macOS AppleDouble
// resource-fork sidecar files (e.g. "._Converter.prc", written alongside
// the real file whenever macOS copies onto a non-HFS+ filesystem like a FAT
// SD card) and ordinary Unix-style hidden files both match this. Without
// this check "._Converter.prc" would ALSO pass hsHasExtension(name,".PRC")
// (it's a real suffix match) and get installed as if it were a real PRC -
// checked by basename, not raw name, so this is robust regardless of
// whether File::name() returns a bare name or a full "/dir/name" path.
static bool hsIsDotfile(const char *name) {
  const char *slash = strrchr(name, '/');
  const char *base = slash ? slash + 1 : name;
  return base[0] == '.';
}

// True if `path` exists and is a directory (a lightweight existence check,
// separate from actually scanning it - callers need this ahead of time to
// decide whether to log "Install from X" and, for SASD, whether the PDB
// pass should even be considered).
static bool hsFolderExists(const char *path) {
  File dir = SD.open(path);
  bool exists = dir && dir.isDirectory();
  if (dir) dir.close();
  return exists;
}

// Creates `path` as a directory if it doesn't already exist. SD.mkdir() on
// the ESP32 FS layer is not guaranteed idempotent (some backends fail if the
// target already exists), so this checks first via hsFolderExists() rather
// than relying on mkdir()'s own return value to distinguish "already there"
// from "really failed".
static bool hsEnsureFolder(const char *path) {
  if (hsFolderExists(path)) return true;
  if (!SD.mkdir(path)) {
    if (usbDebug) Serial.printf("HotSync: could not create %s\r\n", path);
    return false;
  }
  if (usbDebug) Serial.printf("HotSync: created %s\r\n", path);
  return true;
}

// Moves a just-installed file from the INSTALL folder into INSTALLED (see
// hsInstallExt's `moveToDir` param) so a routine sync doesn't keep
// reinstalling the same app/database every time. `srcPath` must be the
// file's full path (File::path(), not File::name() - the latter is only
// the basename) since it has to be a valid standalone argument to
// SD.rename() after the File handle that opened it is closed. Best-effort:
// a failure here is logged but does NOT fail the overall install (the
// install itself already succeeded - worst case the file just gets
// reinstalled, and this retried, next sync).
static void hsMoveToInstalled(const char *srcPath, const char *installedDir) {
  if (!hsEnsureFolder(installedDir)) return;

  const char *slash = strrchr(srcPath, '/');
  const char *base = slash ? slash + 1 : srcPath;
  char destPath[96];
  snprintf(destPath, sizeof(destPath), "%s/%s", installedDir, base);

  // A same-named file may already sit in INSTALLED from an earlier move
  // (e.g. the exact same app re-uploaded to INSTALL and reinstalled) -
  // SD.rename() fails if the destination already exists on this FS
  // backend, so clear the old archived copy first. This is the archive
  // copy only, not the live on-device DB - safe to just overwrite.
  if (SD.exists(destPath)) SD.remove(destPath);

  if (SD.rename(srcPath, destPath)) {
    if (usbDebug) Serial.printf("HotSync: moved '%s' -> %s\r\n", base, installedDir);
  } else if (usbDebug) {
    Serial.printf("HotSync: could not move '%s' to %s (left in place)\r\n", base, installedDir);
  }
}

// Walks one folder for ONE extension, applying the given overwrite policy
// to each file found. `*installedCountOut`, if non-NULL, is incremented for
// every file actually installed (not skipped-as-already-existing) - the
// SASD folder's own PRC-triggers-PDB-override rule needs this per-extension,
// which is why this no longer bundles both passes like the earlier
// `hsInstallFolder()` (renamed/split) did. `moveToDir`, if non-NULL, is
// where a successfully-installed file gets relocated to afterward (the
// INSTALL folder's own "don't reinstall every sync" behavior) - NULL for
// the SASD folder, whose files are meant to stay in place and always be
// checked for (see hsInstallFile's own comment on that folder's intent).
static bool hsInstallExt(const char *path, const char *ext, bool deleteFirst, bool forceReset,
                          uint16_t *installedCountOut, const char *moveToDir) {
  File dir = SD.open(path);
  if (!dir || !dir.isDirectory()) {
    if (dir) dir.close();
    if (usbDebug) Serial.printf("HotSync: %s not found on SD card - skipping\r\n", path);
    return true;   // folder doesn't exist - not an error, just nothing to do
  }

  bool anyFail = false;
  if (usbDebug) Serial.printf("HotSync: scanning %s for %s files\r\n", path, ext);
  uint16_t foundCount = 0;
  for (;;) {
    File f = dir.openNextFile();
    if (!f) break;
    if (f.isDirectory()) { f.close(); continue; }
    if (hsIsDotfile(f.name())) { f.close(); continue; }
    if (!hsHasExtension(f.name(), ext)) { f.close(); continue; }

    foundCount++;
    strncpy(hsCurFileName, f.name(), sizeof(hsCurFileName) - 1);
    hsCurFileName[sizeof(hsCurFileName) - 1] = 0;
    hsLog(hsCurFileName);

    // Captured before f.close() - path() isn't guaranteed valid on a
    // closed handle, and it's needed below regardless of install outcome.
    char srcPath[96];
    strncpy(srcPath, f.path(), sizeof(srcPath) - 1);
    srcPath[sizeof(srcPath) - 1] = 0;

    bool wasInstalled = false;
    bool installOk = hsInstallFile(f, deleteFirst, forceReset, &wasInstalled);
    if (!installOk) anyFail = true;
    else if (wasInstalled && installedCountOut) (*installedCountOut)++;
    f.close();

    if (installOk && wasInstalled && moveToDir) hsMoveToInstalled(srcPath, moveToDir);
  }
  // Explicit even-when-zero result distinguishes an empty folder/no-
  // matching-files pass from a scan that failed to enumerate anything.
  if (usbDebug && foundCount == 0) {
    Serial.printf("HotSync: no %s files found in %s\r\n", ext, path);
  }
  dir.close();
  return !anyFail;
}

static void hsInstallWalk(bool *anyFailOut) {
  bool anyFail = false;

  // ---- SASD folder: PRC is skip-if-exists (files are "drivers", matching
  // the original SASD hardware's own intent for that folder - see
  // hsInstallFile's comment, forceReset=true). PDB files are only installed
  // at all if at least one PRC was actually installed this run (not just
  // found-and-skipped) - and when that happens, the PDB pass force-
  // overrides (delete-then-reinstall) rather than skip-if-exists, since a
  // freshly-(re)installed driver's own data files need to match it. A run
  // where nothing new was installed must not touch PDBs at all, so a
  // routine sync doesn't clobber user data alongside a driver that was
  // already present.
  hsLog("Checking SASD Dir");
  if (hsFolderExists("/PALM/PROGRAMS/SASD")) {
    hsLog("Install from SASD");
    uint16_t prcInstalled = 0;
    if (!hsInstallExt("/PALM/PROGRAMS/SASD", ".PRC", false, true, &prcInstalled, NULL)) anyFail = true;
    if (prcInstalled > 0) {
      const char *msg = "Initialization install triggered. PDB files will be installed.";
      if (usbDebug) Serial.printf("HotSync: %s\r\n", msg);
      hsLog(msg);   // OLED line naturally truncates to ~21 chars, same as any other long entry
      hsSasdInitTriggered = true;
      uint16_t pdbInstalled = 0;
      if (!hsInstallExt("/PALM/PROGRAMS/SASD", ".PDB", true /* force override */, true, &pdbInstalled, NULL))
        anyFail = true;
    }
    // else: no PRC actually installed this run - PDB pass is skipped
    // entirely (not even scanned), per the explicit requirement above.
  }

  // ---- INSTALL folder: general app installs, always delete-then-reinstall,
  // never forces a reset - matches pilot-link's own pi_file_install(),
  // which never does either. Successfully installed files are moved to a
  // sibling INSTALLED folder afterward so a routine sync doesn't keep
  // reinstalling the same app/database every time - unlike SASD's
  // "drivers" folder, these files aren't meant to always be present in INSTALL.
  hsLog("Checking INSTALL Dir");
  if (hsFolderExists("/PALM/PROGRAMS/INSTALL")) {
    hsLog("Install from INSTALL");
    if (!hsInstallExt("/PALM/PROGRAMS/INSTALL", ".PRC", true, false, NULL, "/PALM/PROGRAMS/INSTALLED")) anyFail = true;
    if (!hsInstallExt("/PALM/PROGRAMS/INSTALL", ".PDB", true, false, NULL, "/PALM/PROGRAMS/INSTALLED")) anyFail = true;
  }

  *anyFailOut = anyFail;
}

// ============================================================================
// Session driver - runs the whole handshake->install->endsync sequence
// synchronously (same blocking-with-timeouts precedent already used by this
// codebase's own ZMODEM transfer loops), since every step is individually
// bounded by the frag layer's own ack-wait/resend timeouts - it cannot hang
// indefinitely even though it blocks the Arduino loop() for its duration.
// ============================================================================

static void hsRunSession(const uint8_t *wakePayload, uint16_t wakeLen) {
  uint32_t br = hsHandshake(wakePayload, wakeLen, (uint32_t) bauds[serialSpeed]);
  if (!br) {
    if (usbDebug) Serial.println("HotSync: handshake failed");
    return;   // stay in Waiting - never really connected, nothing to show as Completed
  }

  PhysicalSerial.begin(br, SERIAL_8N1, RXD2, TXD2);
  hsDrawHeader();
  hsSetState("Connected");
  if (usbDebug) Serial.printf("HotSync: connected at %lu baud\r\n", (unsigned long) br);

  if (!hsOpenConduit() && usbDebug) Serial.println("HotSync: OpenConduit failed (continuing)");

  uint32_t romVer = 0;
  hsGetSysInfo(&romVer);   // best-effort - some devices refuse further requests without this, but we don't act on the value

  hsSetState("Syncing");
  bool anyFail = false;
  hsInstallWalk(&anyFail);

  bool endOk = hsEndSync(anyFail ? HS_END_CAUSE_OTHER : HS_END_CAUSE_NORMAL);
  if (usbDebug) {
    Serial.printf("HotSync: EndSync %s (cause=%s)\r\n", endOk ? "acked" : "not acked/failed",
                   anyFail ? "OTHER (install had failures)" : "NORMAL");
  }

  // A freshly-installed/updated DB with PALM_DB_ATTR_RESET (set on every
  // create in hsInstallFile) makes real Palm OS auto-reset itself once the
  // sync ends, to load the new code - this is standard desktop-HotSync
  // install behavior, not something WiRSa requests, and it's exactly why
  // the line goes noisy right after EndSync (reset/reboot chatter from the
  // Palm's own side). Drain RX both before and after the baud switch back -
  // same real-hardware fix already proven for SASD mode's own "stray bytes
  // leak into the next mode's keystroke parser" bug (see exitSasdMode()) -
  // anything arriving during the brief gap at the OLD rate would also just
  // be garbage at the NEW rate.
  while (PhysicalSerial.available()) PhysicalSerial.read();
  PhysicalSerial.flush();
  PhysicalSerial.end();
  delay(200);
  PhysicalSerial.begin(9600, SERIAL_8N1, RXD2, TXD2);   // back to the fixed WAKE-listen baud
  while (PhysicalSerial.available()) PhysicalSerial.read();

  hsDrawHeader();
  hsSetState(hsSasdInitTriggered ? "Init Complete" : "Completed");
  hsLog("Hotsync completed");
  if (usbDebug) Serial.println("HotSync: session complete");
}

// Non-blocking-ish peek for a WAKE packet: returns immediately if nothing
// has arrived yet (so hotsyncLoop() stays responsive), otherwise blocks up
// to 1s to receive the rest of whatever just started arriving - safe since
// WAKE always transmits at the fixed 9600 listen baud (~25ms for the whole
// ~26-byte packet), so 1s is a very generous margin, not a real stall risk.
static bool hsPollForWake(uint8_t *outPayload, uint16_t *outLen) {
  if (!PhysicalSerial.available()) return false;

  if (usbDebug) Serial.println("HotSync: bytes arriving, trying to sync on a WAKE packet...");

  uint16_t len = *outLen;
  uint8_t tid;
  unsigned long deadline = millis() + 1000;
  if (!hsFragRecv(outPayload, len, &len, deadline, &tid)) {
    if (usbDebug) Serial.println("HotSync: gave up on this byte burst (no valid packet synced in time)");
    return false;
  }
  if (usbDebug) Serial.printf("HotSync: got a %u-byte packet, checking if it's a valid WAKE\r\n", (unsigned) len);

  *outLen = len;
  return true;
}

// ============================================================================
// Public API
// ============================================================================

void enterHotsyncMode() {
  menuMode = MODE_FILE;   // repurposed slot - see globals.h
  hsState = HS_WAITING;
  hsDoneAtMs = 0;
  hsCurFileName[0] = 0;
  hsTxTid = 0;
  hsLogLine1[0] = 0;
  hsLogLine2[0] = 0;
  hsSasdInitTriggered = false;

  PhysicalSerial.begin(9600, SERIAL_8N1, RXD2, TXD2);   // protocol-mandated fixed listen baud
  hsDrawHeader();
  hsSetState("Waiting");
}

static void hsExit() {
  // Same drain-before-and-after-the-baud-switch fix as hsRunSession's own
  // post-EndSync transition (see its comment) - needed here too since BTNBK
  // or the 15s auto-timeout can both fire while the Palm still has residual
  // traffic in flight (mid-transfer abort, or reset/reboot chatter right
  // after a completed sync that hasn't fully drained yet).
  while (PhysicalSerial.available()) PhysicalSerial.read();
  PhysicalSerial.flush();
  PhysicalSerial.end();
  delay(200);
  PhysicalSerial.begin(bauds[serialSpeed], WIRSA_SERIAL_FORMAT, RXD2, TXD2);
  while (PhysicalSerial.available()) PhysicalSerial.read();

  // hotsync.cpp draws its own title/status directly (hsDrawHeader()/
  // hsDrawStatus()), bypassing showHeader()'s normal border+clear entirely -
  // it never touches `lastMenu`. showHeader() only does its own full
  // display.clearDisplay() + left/right/bottom border redraw when
  // `lastMenu != menuMode`; if the user came here straight from the main
  // menu (lastMenu was already MODE_MAIN before HotSync ever ran), that
  // check is false and showHeader() only clears a small selection-highlight
  // rect, leaving HotSync's own leftover pixels - including a missing/
  // corrupted border - on screen. Force the same "full redraw" path every
  // other real mode transition already gets, matching the established
  // `msgFlag=true; //force full menu redraw` idiom used elsewhere in this
  // codebase (modem.cpp) for exactly this situation.
  msgFlag = true;
  mainMenu(false);
}

void hotsyncLoop() {
  readSwitches();
  if (BTNBK) {
    waitSwitches();
    hsExit();
    return;
  }
  waitSwitches();

  if (hsState == HS_WAITING) {
    uint8_t wakeBuf[32];
    uint16_t wakeLen = sizeof(wakeBuf);
    if (hsPollForWake(wakeBuf, &wakeLen)) {
      hsRunSession(wakeBuf, wakeLen);
      hsState = HS_DONE;
      hsDoneAtMs = millis();
    }
  } else if (hsState == HS_DONE) {
    if (millis() - hsDoneAtMs > 15000) hsExit();
  }
}
