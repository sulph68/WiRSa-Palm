// HotSync Install: WiRSa plays the desktop cradle. It waits for a Palm's HotSync WAKE, negotiates a session, installs every .PRC/.PDB in two folders of the
// selected disk (SD card or virtual card, per the Disk setting) and ends the sync. Design: apps/WiRSa-Palm.md.
// Wire-compatible with dmitry.gr's SASD host client (slk.c SLP framing and CRC-16/XMODEM, frag.c fragmentation, hotsync.c DLP layer, prc.c); not a code port.
// Unlike sasd.cpp this needs the real multi-fragment PADP, since resource and record payloads exceed one 524-byte fragment, and it reads PDB files too.
// Multi-byte fields are built and parsed by hand (hsPut*/hsGet*) rather than cast onto packed structs, to avoid alignment risk.

#include "Arduino.h"
#include "hotsync.h"
#include "globals.h"         // zmCRC16Update - shared CRC-16/XMODEM table
#include "serial_io.h"
#include "display_menu.h"
#include "network.h"         // drawWifiStatusIcon() - hsDrawHeader() paints it too
#include "palm_ctl.h"
#include "mode_timer.h"
#include "settings.h"   // modeAfterExitSec
#include "relay_proto.h"   // CTL_LEAD
#include <SD.h>
#include "sd_activity.h"
#include "sd_mount.h"
#include "vdisk.h"
#include "vdfs.h"

// The disk HotSync installs from (the Disk setting, settings.h), chosen by hotsyncPrepareDisk() at mode start.
static fs::FS *hsFs = &SD;
static bool hsVirtual = false;

// ---- Protocol constants ----

// ---- slk.c layer ----
#define SLK_MAGIC0        0xBE
#define SLK_MAGIC1        0xEF
#define SLK_MAGIC2        0xED
#define SLK_HDR_LEN       10   // magic[3] + dstSlk + srcSck + pktTyp + dataLen[2] + tid + csum
#define PKT_ADDR_DESKTOP_LINK 3
#define PKT_TYP_HOTSYNC   2
#define PKT_TYP_LOOPBACK  3

// ---- frag.c layer ----
#define FRAG_HDR_LEN      4    // typ + flags + ofst[2]
#define FRAG_TYP_DATA     1
#define FRAG_TYP_ACK      2
#define FRAG_FLAG_FIRST   0x80
#define FRAG_FLAG_LAST    0x40
#define FRAG_FLAG_OOM     0x20
#define FRAG_MAX_CHUNK_SZ 524
// Resend budget and reply wait, as pilot-link's PADP (libpisock/padp.c): PI_PADP_TX_TIMEOUT 2 s x PI_PADP_TX_RETRIES 10 per fragment, and PI_PADP_RX_BLOCK_TO 30 s for the
// Palm's reply. A Palm deleting or creating a big database, or compacting its heap, takes seconds before it answers; the old 3 s wait gave up on it, went on to the next
// request and never acked the late reply, and the Palm dropped the connection.
#define FRAG_MAX_RESENDS  10
#define FRAG_ACK_WAIT_MS  2000
#define HS_REPLY_WAIT_MS  30000

// Largest slk payload held at once: FragHdr 4 + one 524-byte data chunk (streaming sends are never buffered whole, see hsFragSendContinue). Request/response envelopes
// are far smaller (HS_RXBUF_SIZE).
#define HS_RXBUF_SIZE     256

// ---- DLP opcodes (hotsync.c) ----
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

// ---- PRC/PDB header layout ----
#define PALM_DB_HDR_LEN            0x4E    // fixed header size before the entry list
#define PALM_DB_ATTR_RESOURCE      0x0001  // set => PRC (resource DB), clear => PDB (record DB)
#define PALM_DB_ATTR_RESET         0x0020  // "reset after install" - see hsInstallFile's own comment for when this is actually forced
#define PALM_DB_RES_ENTRY_LEN      10      // type(4) + id(2) + ofst(4)
#define PALM_DB_REC_ENTRY_LEN      8       // ofst(4) + attrs(1) + uniqueID(3)
#define PALM_DB_MAX_ENTRIES        512     // generous vs. the LPC802 reference's MAX_RESOURCES=32 - ESP32 has ample RAM

// ---- Big-endian byte helpers (the HotSync wire order, as in pilot-link's slp.c) ----

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

// ---- State ----

enum HsModeState { HS_WAITING, HS_DONE };
static HsModeState hsState = HS_WAITING;
static unsigned long hsDoneAtMs = 0;

static uint8_t  hsTxTid = 0;
static uint8_t  hsRxBuf[HS_RXBUF_SIZE];

// The Palm stopped answering (a fragment was never acked, or a reply never came): every later request fails at once, the install walk stops and EndSync is not sent, so
// WiRSa does not go on talking to a Palm that has gone (pilot-link marks the socket PI_SOCK_CONN_BREAK the same way).
static bool hsLinkDead = false;

// Lost-ack repair, as pilot-link's padp_tx(): the last fragment we acked (a repeat of it means our ack was lost: ack it again), and a packet that arrived while we waited
// for an ack but was already the reply (the Palm's ack was lost: take it as acked and keep the packet for the receiver).
struct HsLastAck { bool valid; uint8_t tid, flags; uint16_t ofst; };
static HsLastAck hsLastAck = { false, 0, 0, 0 };
static bool hsPendValid = false;
static uint8_t hsPendDst, hsPendSrc, hsPendTyp, hsPendTid;
static uint16_t hsPendLen;
static uint8_t hsPend[FRAG_HDR_LEN + FRAG_MAX_CHUNK_SZ];
static char     hsCurFileName[40];

// OLED status area state, see hsDrawStatus().
static char hsStateLine[16] = "Waiting";
static char hsLogLine1[22] = "";
static char hsLogLine2[22] = "";

// True if this session's SASD-folder install put in at least one PRC (not only skipped existing ones); picks "Init Complete" over "Completed" at the end.
static bool hsSasdInitTriggered = false;

extern bool usbDebug;

// ---- forward declarations ----
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
static uint32_t hsInstalledCount = 0, hsSkippedCount = 0;   // this session, for the summary line
static unsigned long hsSessionStartMs = 0;

static bool hsInstallFile(File &f, bool deleteFirst, bool forceReset, bool *wasInstalledOut);

static HsFragSendState gStreamSt;   // reused for whichever Write*Begin/Continue is active

// ---- OLED header: the live baud top right, in the chrome of sasdDrawStatusLines() and slipShowStatus() ----

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

// ---- OLED status area: plain lines under the title (a white dialog box wastes most of the screen) ----
// y=19 "State: <Waiting|Connected|Syncing|Completed>", y=29 "Details:", y=39 and y=49 the two newest detail lines.
// hsLog() shifts line 2 into line 1 and puts the new entry in line 2.

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

// ---- slk.c layer: one SLP packet ----

// Reads one byte before an absolute deadline; false on timeout.
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

// Sends one slk packet: 10-byte header, payload, 2-byte CRC.
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

// Receives one slk packet (header and CRC verified) before an absolute deadline. A bad header checksum or CRC keeps scanning for the next valid packet, as slk.c does.
// False only on timeout.
static bool hsSlkRecvOne(uint8_t *outDst, uint8_t *outSrc, uint8_t *outTyp, uint8_t *outTid,
                          uint8_t *payload, uint16_t payloadCap, uint16_t *payloadLenOut,
                          unsigned long deadlineMs) {
  if (hsPendValid && hsPendLen <= payloadCap) {   // kept by hsFragSendChunk: the reply that came in place of an ack
    hsPendValid = false;
    *outDst = hsPendDst; *outSrc = hsPendSrc; *outTyp = hsPendTyp; *outTid = hsPendTid;
    memcpy(payload, hsPend, hsPendLen);
    *payloadLenOut = hsPendLen;
    return true;
  }
  uint8_t window[SLK_HDR_LEN] = {0};
  // Whether any byte arrived at all, so a timeout log can tell "nothing sent" from "something arrived but never formed a header".
  uint32_t bytesSeen = 0;

  for (;;) {
    // slide the header window a byte at a time until magic and checksum lock
    for (;;) {
      uint8_t b;
      if (!hsReadByte(&b, deadlineMs)) {
        LOGV("HotSync: SLP recv timeout (%lu byte(s) seen, no valid header)\r\n", (unsigned long) bytesSeen);
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
      // Not a real packet at this length (or corruption): keep scanning; the window flushes out as new bytes shift in.
      continue;
    }

    uint16_t got = 0;
    while (got < dataLen) {
      if (!hsReadByte(&payload[got], deadlineMs)) {
        LOGV("HotSync: SLP recv timeout mid-payload (%u/%u bytes, header locked)\r\n", (unsigned) got, (unsigned) dataLen);
        return false;
      }
      got++;
    }
    uint8_t crcBuf[2];
    if (!hsReadByte(&crcBuf[0], deadlineMs) || !hsReadByte(&crcBuf[1], deadlineMs)) {
      LOGV("HotSync: SLP recv timeout mid-CRC (header+payload complete)");
      return false;
    }

    uint16_t crcRxed = ((uint16_t) crcBuf[0] << 8) | crcBuf[1];
    uint16_t crcCalc = 0;
    for (int i = 0; i < SLK_HDR_LEN; i++) crcCalc = zmCRC16Update(crcCalc, window[i]);
    for (uint16_t i = 0; i < dataLen; i++) crcCalc = zmCRC16Update(crcCalc, payload[i]);

    if (crcRxed != crcCalc) {
      LOGW("HotSync: SLP CRC mismatch (got %04X, exp %04X) - dropped\r\n", crcRxed, crcCalc);
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

// ---- frag.c layer: multi-fragment PADP-style reliability ----

// Sends one fragment and waits (resending) for its ACK.
static bool hsFragSendChunk(uint8_t dstSlk, uint8_t srcSlk, uint8_t tid,
                             uint8_t flags, uint16_t ofstField,
                             const uint8_t *data, uint16_t len) {
  uint8_t out[FRAG_HDR_LEN + FRAG_MAX_CHUNK_SZ];
  uint16_t pos = 0;
  pos = hsPut8(out, pos, FRAG_TYP_DATA);
  pos = hsPut8(out, pos, flags);
  pos = hsPut16(out, pos, ofstField);
  pos = hsPutBytes(out, pos, data, len);
  if (hsLinkDead) return false;

  for (uint8_t attempt = 0; attempt < FRAG_MAX_RESENDS; attempt++) {
    if (attempt > 0 && logOn(LOG_VERBOSE)) {
      logPrintf(LOG_VERBOSE, "HotSync: resend attempt %u/%u (tid=%u flags=0x%02x ofst=%u len=%u)\r\n",
                     (unsigned) attempt + 1, (unsigned) FRAG_MAX_RESENDS, tid, flags, ofstField, len);
    }
    hsSlkSend(dstSlk, srcSlk, PKT_TYP_HOTSYNC, tid, out, pos);

    unsigned long deadline = millis() + FRAG_ACK_WAIT_MS;
    for (;;) {
      uint8_t gDst, gSrc, gTyp, gTid;
      // A Palm's ACK carries 2 bytes beyond the 4-byte FragHdr (a known PalmOS quirk); only the first 4 are read. Room for a whole data fragment, which may be the reply.
      static uint8_t rxBuf[FRAG_HDR_LEN + FRAG_MAX_CHUNK_SZ];
      uint16_t rxLen;
      if (!hsSlkRecvOne(&gDst, &gSrc, &gTyp, &gTid, rxBuf, sizeof(rxBuf), &rxLen, deadline))
        break;   // timed out waiting for this attempt's ack - resend

      if (rxLen < FRAG_HDR_LEN) continue;
      uint8_t gFragTyp = rxBuf[0];
      uint8_t gFragFlags = rxBuf[1];
      uint16_t gFragOfst = hsGet16(rxBuf, 2);

      // It must be addressed to us, same pktTyp and tid, an ACK. The echoed ofst/size is not compared, as in pilot-link's padp_tx(), which matches on (ACK, txid) only.
      if (gDst != srcSlk || gSrc != dstSlk || gTyp != PKT_TYP_HOTSYNC ||
          (gTid != tid && gFragTyp != FRAG_TYP_DATA)) {
        if (logOn(LOG_VERBOSE) && (gTyp == PKT_TYP_HOTSYNC)) {
          logPrintf(LOG_VERBOSE, "HotSync: ignoring unrelated pkt dst=%u src=%u tid=%u (want tid=%u)\r\n",
                         gDst, gSrc, gTid, tid);
        }
        continue;
      }
      if (gFragTyp == FRAG_TYP_DATA && gTyp == PKT_TYP_HOTSYNC && gDst == srcSlk) {
        if (gTid == tid && (flags & FRAG_FLAG_LAST)) {
          // The reply to this request, so the Palm had our last fragment and its ack was lost ("PADP TX Missing Ack"): done, and the receiver gets this packet.
          LOGV("HotSync: ack lost, the reply came instead (tid=%u)\r\n", tid);
          hsPendDst = gDst; hsPendSrc = gSrc; hsPendTyp = gTyp; hsPendTid = gTid; hsPendLen = rxLen;
          memcpy(hsPend, rxBuf, rxLen);
          hsPendValid = true;
          return true;
        }
        if (hsLastAck.valid && gTid == hsLastAck.tid && gFragFlags == hsLastAck.flags && gFragOfst == hsLastAck.ofst) {
          // A repeat of the last fragment we received: our ack was lost ("PADP TX resending lost ACK").
          LOGV("HotSync: Palm repeated tid=%u - acking it again\r\n", gTid);
          uint8_t ackHdr[FRAG_HDR_LEN];
          uint16_t p = 0;
          p = hsPut8(ackHdr, p, FRAG_TYP_ACK);
          p = hsPut8(ackHdr, p, gFragFlags);
          p = hsPut16(ackHdr, p, gFragOfst);
          hsSlkSend(gSrc, gDst, gTyp, gTid, ackHdr, p);
          continue;
        }
      }
      if (gTid != tid) continue;
      if (gFragTyp != FRAG_TYP_ACK) continue;

      if (gFragFlags & FRAG_FLAG_OOM) {
        LOGW("HotSync: device reported OOM, aborting send");
        return false;   // device out of memory - no point retrying
      }
      return true;
    }
  }
  LOGW("HotSync: no ack after %u tries (tid=%u) - the Palm has gone, session abandoned\r\n", (unsigned) FRAG_MAX_RESENDS, tid);
  hsLinkDead = true;
  return false;
}

static bool hsFragSendStart(HsFragSendState *st, uint8_t tid, uint16_t totalLen) {
  st->curPos = 0;
  st->totalLen = totalLen;
  st->tid = tid;
  return true;
}

// May be called with any size increment (e.g. one SD read buffer): chunk boundaries and FIRST/LAST flags come from st->curPos and st->totalLen alone, so a large PRC
// resource streams from the card without being buffered whole.
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

// One-shot: sends data (<= FRAG_MAX_CHUNK_SZ, true of every non-streaming request here) as a single FIRST|LAST fragment.
static bool hsFragSendOne(uint8_t tid, const uint8_t *data, uint16_t len) {
  HsFragSendState st;
  return hsFragSendStart(&st, tid, len) && hsFragSendContinue(&st, data, len);
}

// Receives one (possibly multi-fragment) message addressed to us, acking each fragment. Matches the slk address and type only, not the tid: the protocol is strictly
// sequential, so the next well-formed message to us is the reply (as the reference's hotsyncPrvRx). False on timeout or a malformed stream.
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

    if (fragTyp != FRAG_TYP_DATA) continue;   // stray ACK, or a tickle (the Palm is busy): keep waiting

    if (gDst != PKT_ADDR_DESKTOP_LINK || gSrc != PKT_ADDR_DESKTOP_LINK || gTyp != PKT_TYP_HOTSYNC)
      continue;   // not addressed to the hotsync desktop-link socket - ignore

    // A repeat of a fragment already acked (our ack was lost): ack it again, as pilot-link does, or the Palm resends until it gives up.
    if (hsLastAck.valid && gTid == hsLastAck.tid && fragFlags == hsLastAck.flags && fragOfst == hsLastAck.ofst &&
        !(haveFirst && gTid == lockedTid && fragOfst == done)) {
      uint8_t ackHdr[FRAG_HDR_LEN];
      uint16_t p = 0;
      p = hsPut8(ackHdr, p, FRAG_TYP_ACK);
      p = hsPut8(ackHdr, p, fragFlags);
      p = hsPut16(ackHdr, p, fragOfst);
      hsSlkSend(gSrc, gDst, gTyp, gTid, ackHdr, p);
      LOGV("HotSync: repeated fragment tid=%u - acked again\r\n", gTid);
      continue;
    }

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

    // ack it: echo the flags and ofst back, addressed the other way
    {
      uint8_t ackHdr[FRAG_HDR_LEN];
      uint16_t p = 0;
      p = hsPut8(ackHdr, p, FRAG_TYP_ACK);
      p = hsPut8(ackHdr, p, fragFlags);
      p = hsPut16(ackHdr, p, fragOfst);
      hsSlkSend(gSrc, gDst, gTyp, gTid, ackHdr, p);
      hsLastAck.valid = true; hsLastAck.tid = gTid; hsLastAck.flags = fragFlags; hsLastAck.ofst = fragOfst;
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

// ---- DLP request/response envelope ----

// Sends a single-arg (or no-arg) DLP request in the "tiny" arg wrapper (all are under 255 bytes) and waits for the response. False only on a transport failure; a non-zero
// DLP status in *errOut is still a completed transaction, and the caller decides what it means.
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
  // The reference pads every request with zeros (outside the arg length) to at least 4 bytes and an even length (its argless OpenConduit carries a 2-byte "sbz" field,
  // DbClose's 1-byte arg gets a pad). Doing both here covers every call site.
  while (pos < 4 || (pos & 1)) pos = hsPut8(txBuf, pos, 0);

  if (hsLinkDead) return false;
  uint8_t tid = hsNextTid();
  if (!hsFragSendOne(tid, txBuf, pos)) return false;

  uint16_t rxLen = sizeof(hsRxBuf);
  if (!hsFragRecv(hsRxBuf, sizeof(hsRxBuf), &rxLen, millis() + HS_REPLY_WAIT_MS, NULL)) {
    LOGW("HotSync: no reply to request 0x%02x in %u s - the Palm has gone, session abandoned\r\n", pktTyp, (unsigned) (HS_REPLY_WAIT_MS / 1000));
    hsLinkDead = true;
    return false;
  }

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

// Parses at most one response arg (every opcode used here returns 0 or 1), a simplification of the reference's N-arg parser.
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

// ---- WAKE/INIT handshake ----

// wakePayload is the defragmented WAKE body (pktTyp, flags, ver[2], sbz[2], maxBaud[4] = 10 bytes). Sends the INIT reply and returns the baud to switch to (0 = failed, as hotsyncInit).
static uint32_t hsHandshake(const uint8_t *wakePayload, uint16_t wakeLen, uint32_t desiredBaud) {
  if (wakeLen < 10) {
    LOGV("HotSync: WAKE too short (%u bytes, need 10)\r\n", (unsigned) wakeLen);
    return 0;
  }
  // Validated as pilot-link's cmp_rx_handshake is, not the stricter SASD firmware check (minor version <= 3, reserved == 0): a Palm only guarantees major version 1.
  if (wakePayload[0] != HS_PKT_WAKE || wakePayload[2] != 1) {
    if (logOn(LOG_WARN)) {
      logPrintf(LOG_WARN, "HotSync: WAKE rejected - type=0x%02x ver=%u.%u sbz=%u\r\n",
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

  if (logOn(LOG_VERBOSE)) {
    logPrintf(LOG_VERBOSE, "HotSync: WAKE ok (ver=%u.%u, maxBaud=%lu) - sending INIT (curBaud=%lu)\r\n",
                   wakePayload[2], wakePayload[3], (unsigned long) maxBaud, (unsigned long) curBaud);
  }

  uint8_t init[10];
  uint16_t pos = 0;
  // CMP version: the Palm's, but at most 1.2, as pilot-link's cmp_tx() (PI_CMP_VERSION); this code used to answer 1.0 to every Palm.
  uint16_t ver = hsGet16(wakePayload, 2);
  if (ver > 0x0102) ver = 0x0102;
  pos = hsPut8(init, pos, HS_PKT_INIT);
  pos = hsPut8(init, pos, flags);
  pos = hsPut16(init, pos, ver);
  pos = hsPut16(init, pos, 0);
  pos = hsPut32(init, pos, curBaud);

  if (logOn(LOG_VERBOSE)) {
    // Log the real UART rate at the moment of sending (rules out a baud mismatch) and the exact CMP payload (rules out an encoding bug).
    char hex[3 * 40 + 1];
    size_t hp = 0;
    for (uint16_t i = 0; i < pos && hp + 4 < sizeof(hex); i++) hp += snprintf(hex + hp, sizeof(hex) - hp, " %02X", init[i]);
    logPrintf(LOG_VERBOSE, "HotSync: TX baud=%lu, INIT payload:%s", (unsigned long) PhysicalSerial.baudRate(), hex);
  }

  if (!hsFragSendOne(hsNextTid(), init, pos)) {
    LOGW("HotSync: INIT reply never got acked - handshake abandoned");
    return 0;
  }

  LOGV("HotSync: INIT acked");

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

// ---- DB create/delete/close ----

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
  // crid 4 + type 4 + cardNo 1 + rfu 1 + flags 2 + version 2 + name 32 = 46 bytes
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
  if (*errOut) return true;  // transaction ok, but the device reported an error: the caller inspects *errOut
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

// ---- resource/record/info-block streaming writes ----

static bool hsResWriteBegin(uint8_t handle, uint32_t resType, uint16_t resID, uint16_t len) {
  // header 2 + small-wrapper 4 + HsResWriteReq (dbHandle 1, rfu 1, resTyp 4, resID 2, resSize 2) = 16 bytes
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
  if (!hsFragRecv(hsRxBuf, sizeof(hsRxBuf), &rxLen, millis() + HS_REPLY_WAIT_MS, NULL)) { hsLinkDead = true; return false; }
  if (rxLen < 4 || hsRxBuf[0] != (HS_PKT_REPLY_MASK | HS_PKT_RES_WRITE)) return false;
  return hsGet16(hsRxBuf, 2) == 0;
}

static bool hsRecWriteBegin(uint8_t handle, uint16_t size, uint32_t recID, uint8_t recAttrs, uint8_t category) {
  // header 2 + small-wrapper 4 + HsRecWriteReq (dbHandle 1, flags 1, recID 4, recAttrs 1, category 1) = 14 bytes
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
  if (!hsFragRecv(hsRxBuf, sizeof(hsRxBuf), &rxLen, millis() + HS_REPLY_WAIT_MS, NULL)) { hsLinkDead = true; return false; }
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
  if (!hsFragRecv(hsRxBuf, sizeof(hsRxBuf), &rxLen, millis() + HS_REPLY_WAIT_MS, NULL)) { hsLinkDead = true; return false; }
  uint8_t want = HS_PKT_REPLY_MASK | (appInfo ? HS_PKT_APP_INFO_WRITE : HS_PKT_SORT_INFO_WRITE);
  if (rxLen < 4 || hsRxBuf[0] != want) return false;
  return hsGet16(hsRxBuf, 2) == 0;
}

// ---- PRC/PDB reader: the header up to PALM_DB_HDR_LEN (0x4E), then a resource-entry (10 B) or record-entry (8 B) reader by PALM_DB_ATTR_RESOURCE.
// Extends the reference's PRC-only prc.c to PDB files and the optional AppInfo/SortInfo blocks. ----

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

// Reads exactly n bytes or fails (File::read() can short-read).
static bool hsFileReadExact(File &f, uint8_t *buf, uint16_t n) {
  sdActivity();
  return f.read(buf, n) == n;
}

static bool hsParseDbHeader(File &f, uint32_t size) {
  if (size < PALM_DB_HDR_LEN) return false;

  uint8_t hdr[PALM_DB_HDR_LEN];
  if (!hsFileReadExact(f, hdr, PALM_DB_HDR_LEN)) return false;

  memcpy(hsFile.name, hdr, 32);
  hsFile.name[31] = 0;  // NUL-terminate even if the name field is 32 non-null bytes
  hsFile.dbAttrs = hsGet16(hdr, 32);
  hsFile.version = hsGet16(hdr, 34);
  // 0x24-0x33: crDate, modDate, bkupDate, modNum are not needed
  hsFile.appInfoOfst = hsGet32(hdr, 0x34);
  hsFile.sortInfoOfst = hsGet32(hdr, 0x38);
  hsFile.type = hsGet32(hdr, 0x3C);
  hsFile.crid = hsGet32(hdr, 0x40);
  // 0x44-0x47: uniqueIDSeed, ignored
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

// ---- Install driver ----

enum HsStreamTarget { HS_STREAM_RES, HS_STREAM_REC, HS_STREAM_INFO };

// Streams one entry's bytes (resource, record or AppInfo/SortInfo data) from f into the active Write*Continue through a small fixed buffer; an entry is never held whole in RAM.
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

// Installs one open PRC/PDB. deleteFirst: INSTALL-folder policy (delete, then reinstall) or SASD-folder policy (skip if it exists). forceReset: OR PALM_DB_ATTR_RESET onto
// the file's attrs. *wasInstalledOut (if not NULL) is true only if the file was created, not skipped (the SASD folder's "any PRC installed forces the PDB pass" rule needs it).
static bool hsInstallFile(File &f, bool deleteFirst, bool forceReset, bool *wasInstalledOut) {
  if (wasInstalledOut) *wasInstalledOut = false;

  uint32_t size = f.size();
  if (!hsParseDbHeader(f, size)) {
    LOGW("HotSync: '%s' - not a valid PRC/PDB header\r\n", f.name());
    return false;
  }

  if (deleteFirst) {
    uint16_t delErr;
    if (!hsDbDelete(hsFile.name, &delErr)) {
      LOGW("HotSync: delete '%s' - transport failure\r\n", f.name());
    } else if (delErr == HS_ERR_NOT_FOUND) {
      LOGV("HotSync: '%s' - not previously installed\r\n", f.name());
    } else if (delErr == 0) {
      LOGV("HotSync: '%s' - deleted existing copy\r\n", f.name());
    } else if (logOn(LOG_WARN)) {
      logPrintf(LOG_WARN, "HotSync: delete '%s' - err 0x%04x (continuing anyway)\r\n", f.name(), delErr);
    }
  }

  // pilot-link's pi_file_install() passes the DB's own attrs through and sets the reset bit only in one narrow Graffiti case. Always setting it (the SASD firmware does,
  // to bootstrap a blank device) reset the Palm after an ordinary INSTALL-folder app. So it is forced for the SASD folder only.
  uint16_t flags = hsFile.dbAttrs;
  if (forceReset) flags |= PALM_DB_ATTR_RESET;
  uint8_t handle;
  uint16_t crErr;
  if (!hsDbCreate(hsFile.name, hsFile.type, hsFile.crid, flags, hsFile.version, &handle, &crErr)) {
    LOGW("HotSync: create '%s' - transport failure\r\n", f.name());
    return false;
  }
  if (crErr == HS_ERR_EXISTS && !deleteFirst) {
    hsSkippedCount++;
    LOGV("HotSync: '%s' - already installed, skipping\r\n", f.name());
    return true;   // not a failure - this is the expected skip path
  }
  if (crErr) {
    LOGW("HotSync: create '%s' - err 0x%04x\r\n", f.name(), crErr);
    return false;
  }

  // From here the DB exists on the device, so it counts as installed even if a later write fails.
  if (wasInstalledOut) *wasInstalledOut = true;

  bool ok = true;

  if (hsFile.appInfoOfst) {
    uint32_t nextOfst = hsFile.numEntries ? hsFile.entries[0].ofst : size;
    uint32_t len = (hsFile.sortInfoOfst ? hsFile.sortInfoOfst : nextOfst) - hsFile.appInfoOfst;
    if (hsInfoWriteBegin(handle, true, (uint16_t) len) &&
        hsStreamEntryData(f, hsFile.appInfoOfst, len, HS_STREAM_INFO) &&
        hsInfoWriteFinish(true)) {
    } else {
      LOGW("HotSync: '%s' - AppInfo block write failed\r\n", f.name());
      ok = false;
    }
  }
  if (ok && hsFile.sortInfoOfst) {
    uint32_t nextOfst = hsFile.numEntries ? hsFile.entries[0].ofst : size;
    uint32_t len = nextOfst - hsFile.sortInfoOfst;
    if (hsInfoWriteBegin(handle, false, (uint16_t) len) &&
        hsStreamEntryData(f, hsFile.sortInfoOfst, len, HS_STREAM_INFO) &&
        hsInfoWriteFinish(false)) {
    } else {
      LOGW("HotSync: '%s' - SortInfo block write failed\r\n", f.name());
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
    if (!ok && logOn(LOG_WARN)) logPrintf(LOG_WARN, "HotSync: '%s' - entry %u write failed\r\n", f.name(), (unsigned) i);
  }

  if (!hsDbClose(handle) && logOn(LOG_WARN)) logPrintf(LOG_WARN, "HotSync: '%s' - close failed\r\n", f.name());

  if (ok) {
    hsInstalledCount++;
    LOGI("HotSync: '%s' - installed\r\n", f.name());
  }
  return ok;
}

// Case-insensitive suffix compare, by hand: strcasecmp() is not used elsewhere and needs <strings.h>.
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

// True if the basename starts with '.': macOS AppleDouble sidecars ("._Converter.prc", written when macOS copies onto FAT) and hidden files. Without it "._Converter.prc"
// matches ".PRC" and is installed as a PRC. By basename, since File::name() may return a bare name or a full path.
static bool hsIsDotfile(const char *name) {
  const char *slash = strrchr(name, '/');
  const char *base = slash ? slash + 1 : name;
  return base[0] == '.';
}

// True if path exists and is a directory (a cheap check, ahead of scanning it).
static bool hsFolderExists(const char *path) {
  sdActivity();
  File dir = hsFs->open(path);
  bool exists = dir && dir.isDirectory();
  if (dir) dir.close();
  return exists;
}

// Creates path as a directory if missing. SD.mkdir() may fail when the target exists on some backends, so it checks first.
static bool hsEnsureFolder(const char *path) {
  if (hsFolderExists(path)) return true;
  if (!hsFs->mkdir(path)) {
    LOGW("HotSync: could not create %s\r\n", path);
    return false;
  }
  LOGV("HotSync: created %s\r\n", path);
  return true;
}

// Moves a just-installed file from INSTALL into INSTALLED (hsInstallExt's moveToDir) so a routine sync does not reinstall it. srcPath is the full path (File::path(),
// not name()), as SD.rename() needs after the handle is closed. Best-effort: a failure is logged and the install still counts (the file is just reinstalled next sync).
// Picks and mounts the disk by the Disk setting: the SD card, the virtual card, or (Auto) the SD card if one answers, else the virtual card. Then makes sure the install
// folders exist on it. False (nothing mounted) when there is no disk.
bool hotsyncPrepareDisk() {
  hsFs = &SD;
  hsVirtual = false;
  bool haveSd = sasdDiskMode != SASD_DISK_VIRTUAL && sdMount();
  if (!haveSd) {
    if (sasdDiskMode == SASD_DISK_SD || !vdiskPresent() || !vdfsMount()) return false;
    hsFs = &vdfs();
    hsVirtual = true;
  }
  LOGI("HotSync: installing from the %s\r\n", hsVirtual ? "virtual card (ESP32)" : "SD card");
  static const char *const kDirs[] = { "/PALM", "/PALM/PROGRAMS", "/PALM/PROGRAMS/INSTALL",
                                       "/PALM/PROGRAMS/SASD", "/PALM/PROGRAMS/INSTALLED" };
  for (size_t i = 0; i < sizeof(kDirs) / sizeof(kDirs[0]); i++)
    if (!hsEnsureFolder(kDirs[i])) break;
  return true;
}

static void hsMoveToInstalled(const char *srcPath, const char *installedDir) {
  if (!hsEnsureFolder(installedDir)) return;

  const char *slash = strrchr(srcPath, '/');
  const char *base = slash ? slash + 1 : srcPath;
  char destPath[96];
  snprintf(destPath, sizeof(destPath), "%s/%s", installedDir, base);

  // A same-named file may already sit in INSTALLED from an earlier move, and rename fails if the destination exists: clear the archived copy first (not the on-device DB).
  if (hsFs->exists(destPath)) hsFs->remove(destPath);

  if (hsFs->rename(srcPath, destPath)) {
    LOGV("HotSync: moved '%s' -> %s\r\n", base, installedDir);
  } else if (logOn(LOG_WARN)) {
    logPrintf(LOG_WARN, "HotSync: could not move '%s' to %s (left in place)\r\n", base, installedDir);
  }
}

// Walks one folder for one extension with the given overwrite policy. *installedCountOut (if not NULL) counts files actually installed, not skipped (per extension, for the
// SASD PRC-triggers-PDB rule). moveToDir (if not NULL) is where an installed file goes afterwards (INSTALL folder); NULL for the SASD folder, whose files stay in place.
static bool hsInstallExt(const char *path, const char *ext, bool deleteFirst, bool forceReset,
                          uint16_t *installedCountOut, const char *moveToDir) {
  sdActivity();
  File dir = hsFs->open(path);
  if (!dir || !dir.isDirectory()) {
    if (dir) dir.close();
    LOGV("HotSync: %s not found on SD card - skipping\r\n", path);
    return true;   // folder doesn't exist - not an error, just nothing to do
  }

  bool anyFail = false;
  LOGV("HotSync: scanning %s for %s files\r\n", path, ext);
  uint16_t foundCount = 0;
  for (;;) {
    if (hsLinkDead) { anyFail = true; break; }   // the Palm has gone: nothing more to send
    File f = dir.openNextFile();
    if (!f) break;
    if (f.isDirectory()) { f.close(); continue; }
    if (hsIsDotfile(f.name())) { f.close(); continue; }
    if (!hsHasExtension(f.name(), ext)) { f.close(); continue; }

    foundCount++;
    strncpy(hsCurFileName, f.name(), sizeof(hsCurFileName) - 1);
    hsCurFileName[sizeof(hsCurFileName) - 1] = 0;
    hsLog(hsCurFileName);

    // Captured before f.close(): path() is not valid on a closed handle.
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
  // An explicit zero tells an empty folder from a scan that failed to list anything.
  if (logOn(LOG_VERBOSE) && foundCount == 0) {
    logPrintf(LOG_VERBOSE, "HotSync: no %s files found in %s\r\n", ext, path);
  }
  dir.close();
  return !anyFail;
}

static void hsInstallWalk(bool *anyFailOut) {
  bool anyFail = false;

  // ---- SASD folder: PRCs are skip-if-exists ("drivers", forceReset). PDBs are installed only if a PRC was actually installed this run, and then delete-then-reinstall,
  // since a fresh driver's data files must match it. A run that installed nothing new must not touch PDBs, so a sync does not clobber user data. ----
  hsLog("Checking SASD Dir");
  if (hsFolderExists("/PALM/PROGRAMS/SASD")) {
    hsLog("Install from SASD");
    uint16_t prcInstalled = 0;
    if (!hsInstallExt("/PALM/PROGRAMS/SASD", ".PRC", false, true, &prcInstalled, NULL)) anyFail = true;
    if (prcInstalled > 0) {
      const char *msg = "Initialization install triggered. PDB files will be installed.";
      LOGI("HotSync: %s\r\n", msg);
      hsLog(msg);  // the OLED truncates to ~21 chars
      hsSasdInitTriggered = true;
      uint16_t pdbInstalled = 0;
      if (!hsInstallExt("/PALM/PROGRAMS/SASD", ".PDB", true /* force override */, true, &pdbInstalled, NULL))
        anyFail = true;
    }
    // else: no PRC installed this run, so the PDB pass is skipped (not even scanned)
  }

  // ---- INSTALL folder: general apps, always delete-then-reinstall, never forces a reset (as pilot-link's pi_file_install()). Installed files move to INSTALLED so a sync does
  // not reinstall them. ----
  hsLog("Checking INSTALL Dir");
  if (!hsLinkDead && hsFolderExists("/PALM/PROGRAMS/INSTALL")) {
    hsLog("Install from INSTALL");
    if (!hsInstallExt("/PALM/PROGRAMS/INSTALL", ".PRC", true, false, NULL, "/PALM/PROGRAMS/INSTALLED")) anyFail = true;
    if (!hsInstallExt("/PALM/PROGRAMS/INSTALL", ".PDB", true, false, NULL, "/PALM/PROGRAMS/INSTALLED")) anyFail = true;
  }

  *anyFailOut = anyFail;
}

// ---- Session driver: handshake, install and EndSync run synchronously, as the ZMODEM loops do. Every step is bounded by the frag layer's ack wait and resends, so it
// cannot hang, though it blocks loop() meanwhile. ----

static bool hsRunSession(const uint8_t *wakePayload, uint16_t wakeLen) {
  hsLinkDead = false;
  hsPendValid = false;
  uint32_t br = hsHandshake(wakePayload, wakeLen, (uint32_t) bauds[serialSpeed]);
  if (!br) {
    LOGW("HotSync: handshake failed");
    return false;  // stay in Waiting: never connected, nothing to show as Completed
  }

  if (br != PhysicalSerial.baudRate()) {
    // As pilot-link's s_changebaud(): 200 ms, the new rate once the output has drained, 200 ms. updateBaudRate() keeps the driver and pins (begin() reinstalls the driver,
    // which can glitch the TX line just as the Palm starts listening at the new rate).
    PhysicalSerial.flush();
    delay(200);
    PhysicalSerial.updateBaudRate(br);
    delay(200);
    while (PhysicalSerial.available()) PhysicalSerial.read();
  }
  hsDrawHeader();
  hsSetState("Connected");
  LOGI("HotSync: connected at %lu baud\r\n", (unsigned long) br);
  hsInstalledCount = hsSkippedCount = 0;
  hsSessionStartMs = millis();

  if (!hsOpenConduit() && logOn(LOG_WARN)) logPrintf(LOG_WARN, "HotSync: OpenConduit failed (continuing)");

  uint32_t romVer = 0;
  hsGetSysInfo(&romVer);  // best-effort: some devices refuse later requests without it; the value is unused

  hsSetState("Syncing");
  bool anyFail = false;
  hsInstallWalk(&anyFail);

  bool lost = hsLinkDead;
  if (lost) {
    LOGW("HotSync: the Palm stopped answering - no EndSync, back to waiting\r\n");
  } else {
    bool endOk = hsEndSync(anyFail ? HS_END_CAUSE_OTHER : HS_END_CAUSE_NORMAL);
    if (logOn(LOG_WARN)) {
      logPrintf(LOG_WARN, "HotSync: EndSync %s (cause=%s)\r\n", endOk ? "acked" : "not acked/failed",
                     anyFail ? "OTHER (install had failures)" : "NORMAL");
    }
  }

  // A DB installed with PALM_DB_ATTR_RESET makes the Palm reset itself after the sync to load the new code (standard HotSync behaviour), hence the noise on the line after
  // EndSync. Drain RX before and after the switch back, as exitSasdMode() does for stray bytes leaking into the next mode's key parser; bytes arriving at the old rate are garbage at the new one.
  while (PhysicalSerial.available()) PhysicalSerial.read();
  PhysicalSerial.flush();
  PhysicalSerial.end();
  delay(200);
  PhysicalSerial.begin(9600, SERIAL_8N1, RXD2, TXD2);   // back to the fixed WAKE-listen baud
  while (PhysicalSerial.available()) PhysicalSerial.read();

  hsDrawHeader();
  hsSetState(lost ? "Palm lost" : hsSasdInitTriggered ? "Init Complete" : "Completed");
  hsLog(lost ? "Connection lost" : "Hotsync completed");
  LOGI("HotSync: session complete - %lu installed, %lu already there%s, %lus\r\n", (unsigned long) hsInstalledCount,
       (unsigned long) hsSkippedCount, anyFail ? ", WITH ERRORS" : "", (unsigned long) ((millis() - hsSessionStartMs) / 1000));
  return true;
}

// Peeks for a WAKE: returns at once if nothing arrived (loop() stays responsive), else blocks up to 1 s for the rest. WAKE is at the fixed 9600 listen baud (~25 ms for ~26 bytes).
static bool hsPollForWake(uint8_t *outPayload, uint16_t *outLen) {
  if (!PhysicalSerial.available()) return false;

  LOGV("HotSync: bytes arriving, trying to sync on a WAKE packet...");

  uint16_t len = *outLen;
  uint8_t tid;
  unsigned long deadline = millis() + 1000;
  // A new session: forget the last ack, or a WAKE that looks like the previous session's (same tid, flags and length) would be taken for a repeat and only acked again.
  hsLastAck.valid = false;
  hsPendValid = false;
  if (!hsFragRecv(outPayload, len, &len, deadline, &tid)) {
    LOGV("HotSync: gave up on this byte burst (no valid packet synced in time)");
    return false;
  }
  LOGV("HotSync: got a %u-byte packet, checking if it's a valid WAKE\r\n", (unsigned) len);

  *outLen = len;
  return true;
}

// ---- Public API ----

// Back to the main menu on the shared Exit Timers (mode_timer.h): no HotSync within the "unused" time of entering, or the "after" time once one finished.
static ModeExitTimer hsTimer;

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
  modeTimerStart(hsTimer);
  LOGI("HotSync: mode entered - waiting for the Palm");
  hsDrawHeader();
  hsSetState("Waiting");
}

static void hsExit() {
  if (hsVirtual) { vdfsUnmount(); hsVirtual = false; hsFs = &SD; }
  // Drain before and after the baud switch, as after EndSync: BTNBK or the 15 s timeout can fire while the Palm still has traffic in flight.
  while (PhysicalSerial.available()) PhysicalSerial.read();
  PhysicalSerial.flush();
  PhysicalSerial.end();
  delay(200);
  PhysicalSerial.begin(menuBaud(), WIRSA_SERIAL_FORMAT, RXD2, TXD2);   // the menu's current rate (palm_ctl.h)
  while (PhysicalSerial.available()) PhysicalSerial.read();

  // This mode draws its own title and status (hsDrawHeader()/hsDrawStatus()) and never touches lastMenu. showHeader() redraws fully only when lastMenu != menuMode, so
  // coming straight from the main menu would leave HotSync's pixels (and a damaged border) on screen. Force the full redraw other mode changes get (msgFlag, as modem.cpp did).
  msgFlag = true;
  LOGI("HotSync: mode exit");
  mainMenu(false);
}

// BOOT pressed again while still waiting for the Palm: leave, as BACK. Ignored once a session has started.
void hotsyncCancelWait() {
  if (menuMode == MODE_FILE && hsState == HS_WAITING) hsExit();
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
    // Palm control link (palm_ctl.h): PING/STATUS from WiRSaCtl, and EXIT (only while waiting).
    if (palmCtlModeHook('H', true, 0)) {
      hsExit();
      return;
    }
    if (modeTimerExpired(hsTimer)) {
      LOGI("HotSync: exit timer - back to the main menu");
      hsExit();
      return;
    }
#if WIRSA_FEATURE_PALMCTL
    // A control frame is for palmCtlModeHook on the next pass; the WAKE scanner below would swallow it.
    if (PhysicalSerial.available() && PhysicalSerial.peek() == RelayProto::CTL_LEAD) return;
#endif
    uint8_t wakeBuf[32];
    uint16_t wakeLen = sizeof(wakeBuf);
    if (hsPollForWake(wakeBuf, &wakeLen)) {
      bool ran = hsRunSession(wakeBuf, wakeLen);   // blocks for the whole session
      if (ran) modeTimerSessionEnded(hsTimer);
      // "After" timer 0 = never leave: listen for the next HotSync. A packet that was not a real WAKE keeps waiting too.
      if (ran && modeAfterExitSec) {
        hsState = HS_DONE;
        hsDoneAtMs = millis();
      }
    }
  } else if (hsState == HS_DONE) {
    if (modeTimerExpired(hsTimer)) hsExit();
  }
}
