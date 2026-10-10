// SASD Module - Serial-Attached SD
//
// Wire-compatible (not code-ported) reimplementation of dmitry.gr's SASD
// protocol for the ESP32/Arduino target. Reference source:
// ~/palmos_sdk/Sample_Source/serialSD/ - Src/proto.h (commands/structs),
// firmware/slk.c (framing + CRC-16/XMODEM), firmware/frag.c (the thin
// single-fragment reliability wrapper every SASD message rides in - never
// actually fragments here since every SASD payload is well under
// FRAG_MAX_CHUNK_SZ=524), firmware/lpc81x.c (the reference "device" role
// this file plays: WiRSa never initiates, it only replies to whatever the
// Palm's SASD.prc slot driver sends).
//
// Reuses this codebase's own zmCRC16Update() (main.cpp, declared in
// globals.h - relocated there from file_transfer.cpp when that was removed
// as unreachable dead code) for the slk-layer CRC - its table is
// byte-for-byte the same CRC-16/XMODEM (poly 0x1021, init 0) table the
// reference protocol uses, confirmed by direct comparison against
// firmware/slk.c's own x86 reference table.

#include "Arduino.h"
#include "sasd.h"
#include "globals.h"         // zmCRC16Update - shared CRC-16/XMODEM table
#include "serial_io.h"
#include "display_menu.h"
#include "network.h"         // drawWifiStatusIcon() - sasdShowStatus() paints it too
#include "ftp_server.h"      // ftpServerRunning - SASD<->FTP mutual exclusion
#include <SD.h>

#define R_OK_STAT 0   // matches modem.cpp's own resultCodes_t enum (R_OK_STAT==0);
                       // local define to avoid pulling in that private enum,
                       // same pattern slip_mode.cpp already uses

// ---- protocol constants (Src/proto.h) --------------------------------------
#define SASD_PKT_TYP           0x10  // PKT_TYP_SASD
#define SASD_PKT_ADDR          0x08  // PKT_ADDR_SASD (both src and dst)

#define SASD_CMD_PING          0x00
#define SASD_CMD_GET_INFO      0x01
#define SASD_CMD_READ          0x02
#define SASD_CMD_WRITE         0x03
#define SASD_CMD_RESET_STATE   0x04
#define SASD_CMD_SET_BAUD      0x05
// CMD_FW_UPDATE_START/CMD_GET_BATT intentionally not implemented - only
// reachable via SlotCustomControl, never the VFS read/write path this mode
// exists for.

#define SASD_RSP_PONG          0x40
#define SASD_RSP_OK            0x41
#define SASD_RSP_ERROR         0x42
#define SASD_RSP_INFO          0x43

#define SASD_NOTICE_CTS        0x80
#define SASD_NOTICE_XFER       0x81

#define SASD_ERR_BOUNDS        0x01
#define SASD_ERR_SEQUENCE      0x02
#define SASD_ERR_STATE         0x03
#define SASD_ERR_SEQ2          0x04
#define SASD_ERR_CARD          0x05

#define SASD_FLAG_COMMS        0x00000001UL
#define SASD_FLAG_HAVE_CARD    0x00000002UL
#define SASD_FLAG_IS_NEW_VER   0x00000010UL
#define SASD_FLAG_IS_HIGH_CAP  0x00000020UL

#define SASD_BLOCK_SIZE        512
#define SASD_BAUD_INITIAL      9600
// No fixed max here - CMD_SET_BAUD caps the negotiated rate at whatever the
// user has configured as WiRSa's own baud rate (bauds[serialSpeed]).

// ---- frag.c layer -----------------------------------------------------------
#define FRAG_TYP_DATA           1
#define FRAG_TYP_ACK             2
#define FRAG_FLAG_FIRST        0x80
#define FRAG_FLAG_LAST          0x40

// ---- slk.c layer ------------------------------------------------------------
#define SLK_MAGIC0  0xBE
#define SLK_MAGIC1  0xEF
#define SLK_MAGIC2  0xED
#define SLK_HDR_LEN 10   // magic[3] + dstSlk + srcSck + pktTyp + dataLen[2] + tid + csum

// Largest real slk payload = 4-byte FragHdr + 1-byte NOTICE_XFER tag +
// 512-byte sector = 517 bytes; matches the reference's own SD_PROTO_MAX_MSG.
#define SASD_MAX_PAYLOAD 528

// ============================================================================
// State
// ============================================================================

enum SasdRxState { SASD_RX_SYNC, SASD_RX_PAYLOAD, SASD_RX_CRC };

struct SasdRxCtx {
  SasdRxState state;
  uint8_t     window[SLK_HDR_LEN];
  uint8_t     dstSlk, srcSck, pktTyp, tid;
  uint16_t    dataLen;
  uint8_t     payload[SASD_MAX_PAYLOAD];
  uint16_t    payloadPos;
  uint8_t     crcBuf[2];
  uint8_t     crcPos;
};

enum SasdOp { SASD_OP_NONE, SASD_OP_READ, SASD_OP_WRITE };
struct SasdOpState {
  SasdOp   op;
  uint32_t firstSec, numSec, doneSec;
};

static SasdRxCtx    sasdRx;
static SasdOpState  sasdOp = { SASD_OP_NONE, 0, 0, 0 };
static uint8_t      sasdPrevTid = 0;
static bool         sasdHavePrevTid = false;
static bool         sasdCardReady = false;
static uint64_t     sasdTotalBytes = 0, sasdUsedBytes = 0;
// FAT32-direct-scan state - see sasdParseFat32Layout's own comment for why
// this exists instead of just calling SD.usedBytes() every refresh.
static bool         sasdFat32Ready = false;
static uint32_t     sasdFatStartSector = 0;   // absolute sector of the first FAT copy
static uint32_t     sasdFatSectorCount = 0;   // sectors per FAT copy
static uint32_t     sasdTotalClusters = 0;    // real usable data clusters (excludes reserved entries 0/1)
static uint32_t     sasdBytesPerCluster = 0;
static int          sasdPreviousMenuMode = MODE_MAIN;
static uint32_t     sasdActivitySec = 0;
// Debounced capacity-refresh trigger - see sasdIsIdleEnoughToScan's own
// comment for why this replaced an sasdOp.op-based "idle" check.
static unsigned long sasdLastFileOpMillis = 0;
static bool          sasdCapacityStale = false;
static uint32_t     sasdBytesIn = 0, sasdBytesOut = 0;
static uint32_t     sasdCrcErrors = 0, sasdProtoErrors = 0;
static uint32_t     sasdSpeedLastBytes = 0;
static unsigned long sasdSpeedLastMillis = 0;
static float        sasdSpeedKBs = 0.0f;

// External global variables (from main.cpp / globals.h)
extern bool usbDebug;
extern Adafruit_SSD1306 display;

// ---- forward decls (static helpers) ----------------------------------------
static void sasdRxReset();
static uint8_t sasdSum9(const uint8_t *hdr9);
static void sasdSlkSend(uint8_t tid, const uint8_t *payload, uint16_t len);
static void sasdSendAck(uint8_t tid, uint8_t reqFlags, uint16_t reqOfst,
                          const uint8_t *replyData, uint16_t replyLen);
static void sasdSendError(uint8_t tid, uint8_t flags, uint16_t ofst, uint8_t errType);
static bool sasdTryParseBootSector(uint32_t baseLBA);
static bool sasdParseFat32Layout();
static bool sasdScanFreeClusters(uint32_t *outFree);
static void sasdMarkFileActivity();
static bool sasdIsIdleEnoughToScan();
static bool sasdReceiveByte(uint8_t byte);
static void sasdProcessPacket();
static void sasdDispatchCommand(uint8_t tid, uint8_t flags, uint16_t ofst,
                                  const uint8_t *data, uint16_t len, bool isDupe);
static void sasdUpdateActiveDisplay();
static void sasdScanCapacity();

// ============================================================================
// slk framing layer
// ============================================================================

static void sasdRxReset() {
  sasdRx.state = SASD_RX_SYNC;
  memset(sasdRx.window, 0, sizeof(sasdRx.window));
}

static uint8_t sasdSum9(const uint8_t *hdr9) {
  uint8_t s = 0;
  for (int i = 0; i < SLK_HDR_LEN - 1; i++) s += hdr9[i];
  return s;
}

// Sends one complete slk packet: 10-byte header + payload + 2-byte CRC.
static void sasdSlkSend(uint8_t tid, const uint8_t *payload, uint16_t len) {
  uint8_t hdr[SLK_HDR_LEN];
  uint16_t crc = 0;
  uint8_t crcOut[2];

  hdr[0] = SLK_MAGIC0; hdr[1] = SLK_MAGIC1; hdr[2] = SLK_MAGIC2;
  hdr[3] = SASD_PKT_ADDR; hdr[4] = SASD_PKT_ADDR; hdr[5] = SASD_PKT_TYP;
  hdr[6] = (uint8_t)(len >> 8); hdr[7] = (uint8_t)(len & 0xFF);
  hdr[8] = tid;
  hdr[9] = sasdSum9(hdr);

  for (int i = 0; i < SLK_HDR_LEN; i++) crc = zmCRC16Update(crc, hdr[i]);
  for (uint16_t i = 0; i < len; i++) crc = zmCRC16Update(crc, payload[i]);
  crcOut[0] = (uint8_t)(crc >> 8);
  crcOut[1] = (uint8_t)(crc & 0xFF);

  PhysicalSerial.write(hdr, SLK_HDR_LEN);
  if (len) PhysicalSerial.write(payload, len);
  PhysicalSerial.write(crcOut, 2);

  sasdBytesOut += SLK_HDR_LEN + len + 2;
}

// ============================================================================
// frag.c layer - single-fragment ACK-with-payload only (WiRSa never sends a
// standalone DATA message - every SASD reply IS the ack for the Palm's own
// request, matching lpc81x.c's own fragAck-only usage throughout).
// ============================================================================

static void sasdSendAck(uint8_t tid, uint8_t reqFlags, uint16_t reqOfst,
                          const uint8_t *replyData, uint16_t replyLen) {
  uint8_t buf[4 + SASD_MAX_PAYLOAD];
  buf[0] = FRAG_TYP_ACK;
  buf[1] = reqFlags;
  buf[2] = (uint8_t)(reqOfst >> 8);
  buf[3] = (uint8_t)(reqOfst & 0xFF);
  if (replyLen) memcpy(buf + 4, replyData, replyLen);
  sasdSlkSend(tid, buf, 4 + replyLen);
}

static void sasdSendError(uint8_t tid, uint8_t flags, uint16_t ofst, uint8_t errType) {
  uint8_t ed[8] = {0};
  ed[0] = SASD_RSP_ERROR;
  ed[1] = errType;
  sasdSendAck(tid, flags, ofst, ed, sizeof(ed));
}

// ============================================================================
// Byte-at-a-time receive state machine, same idiom as slip.cpp's
// slipReceiveByte() - fed one byte per call from sasdLoop() so nothing here
// ever blocks the main Arduino loop(). Returns true once a full slk packet
// (header + payload + CRC) has been received and CRC-verified into sasdRx.
// ============================================================================

static bool sasdReceiveByte(uint8_t byte) {
  switch (sasdRx.state) {
    case SASD_RX_SYNC: {
      for (int i = 0; i < SLK_HDR_LEN - 1; i++) sasdRx.window[i] = sasdRx.window[i + 1];
      sasdRx.window[SLK_HDR_LEN - 1] = byte;

      if (sasdRx.window[0] == SLK_MAGIC0 && sasdRx.window[1] == SLK_MAGIC1 &&
          sasdRx.window[2] == SLK_MAGIC2 && sasdRx.window[9] == sasdSum9(sasdRx.window)) {
        sasdRx.dstSlk = sasdRx.window[3];
        sasdRx.srcSck = sasdRx.window[4];
        sasdRx.pktTyp = sasdRx.window[5];
        sasdRx.dataLen = ((uint16_t)sasdRx.window[6] << 8) | sasdRx.window[7];
        sasdRx.tid = sasdRx.window[8];

        if (sasdRx.dataLen > SASD_MAX_PAYLOAD) {
          // Can't be a real SASD message (or the link is corrupted) - keep
          // scanning rather than risk a buffer overrun.
          break;
        }
        sasdRx.payloadPos = 0;
        sasdRx.crcPos = 0;
        sasdRx.state = sasdRx.dataLen ? SASD_RX_PAYLOAD : SASD_RX_CRC;
      }
      break;
    }

    case SASD_RX_PAYLOAD:
      sasdRx.payload[sasdRx.payloadPos++] = byte;
      if (sasdRx.payloadPos >= sasdRx.dataLen) sasdRx.state = SASD_RX_CRC;
      break;

    case SASD_RX_CRC:
      sasdRx.crcBuf[sasdRx.crcPos++] = byte;
      if (sasdRx.crcPos >= 2) {
        uint16_t crcRxed = ((uint16_t)sasdRx.crcBuf[0] << 8) | sasdRx.crcBuf[1];
        uint16_t crcCalc = 0;
        for (int i = 0; i < SLK_HDR_LEN; i++) crcCalc = zmCRC16Update(crcCalc, sasdRx.window[i]);
        for (uint16_t i = 0; i < sasdRx.dataLen; i++) crcCalc = zmCRC16Update(crcCalc, sasdRx.payload[i]);

        // Ready for the next packet regardless of outcome - a CRC mismatch
        // just means dropping this one and resyncing, same as the
        // reference's own slkRx() outer retry loop. The Palm will retry
        // (up to FRAG_MAX_RESENDS times over ~600ms) so an occasional drop
        // is recoverable - frequent ones are the real symptom of running
        // the link faster than it can reliably sustain.
        sasdRxReset();

        if (crcRxed == crcCalc) return true;

        sasdCrcErrors++;
        if (usbDebug) {
          UsbDebugPrint("");
          Serial.printf("SASD: *** CRC error #%lu (got %04X, expected %04X, %u-byte packet) - dropped, Palm will retry ***\r\n",
                         (unsigned long) sasdCrcErrors, crcRxed, crcCalc, (unsigned) sasdRx.dataLen);
        }
      }
      break;
  }
  return false;
}

// ============================================================================
// Dispatch - mirrors lpc81x.c's parseAndReplyToSasdPacket()
// ============================================================================

static void sasdProcessPacket() {
  if (sasdRx.dstSlk != SASD_PKT_ADDR || sasdRx.srcSck != SASD_PKT_ADDR ||
      sasdRx.pktTyp != SASD_PKT_TYP) {
    return;  // not a SASD packet - ignore (e.g. link noise)
  }
  if (sasdRx.dataLen < 4) return;  // too short to hold a FragHdr

  uint8_t fragTyp   = sasdRx.payload[0];
  uint8_t fragFlags = sasdRx.payload[1];
  uint16_t fragOfst = ((uint16_t)sasdRx.payload[2] << 8) | sasdRx.payload[3];
  const uint8_t *cmdData = sasdRx.payload + 4;
  uint16_t cmdLen = sasdRx.dataLen - 4;

  if (fragTyp != FRAG_TYP_DATA) return;  // only ever expect commands from the Palm

  bool isDupe = sasdHavePrevTid && (sasdRx.tid == sasdPrevTid);

  sasdDispatchCommand(sasdRx.tid, fragFlags, fragOfst, cmdData, cmdLen, isDupe);

  sasdPrevTid = sasdRx.tid;
  sasdHavePrevTid = true;
  sasdBytesIn += SLK_HDR_LEN + sasdRx.dataLen + 2;
}

static void sasdDispatchCommand(uint8_t tid, uint8_t flags, uint16_t ofst,
                                  const uint8_t *data, uint16_t len, bool isDupe) {
  if (len < 1) return;
  uint8_t cmd = data[0];

  switch (cmd) {
    case SASD_CMD_PING: {
      if (len != 32) return;   // sizeof(SdPingData) = cmd(1) + garbage[31]
      uint8_t reply[32];
      memcpy(reply, data, 32);
      reply[0] = SASD_RSP_PONG;
      sasdSendAck(tid, flags, ofst, reply, sizeof(reply));
      break;
    }

    case SASD_CMD_GET_INFO: {
      if (len != 1) return;
      // SdInfoData layout: cmd(1) manuf(1) oem(2) numBlocks(4) flags(4)
      // snum(4) hwVers(1) fwVers(3) = 20 bytes, all big-endian.
      uint8_t info[20] = {0};
      info[0] = SASD_RSP_INFO;
      uint32_t flagsVal = SASD_FLAG_COMMS;
      if (sasdCardReady) {
        // WiRSa's SD.h wrapper doesn't expose the CID-level SDSC/SDHC/MMC/
        // read-only distinctions the reference firmware's own low-level
        // driver reports - reporting "new version, high capacity"
        // unconditionally is correct for the SDHC/SDXC cards this project
        // actually targets, rather than guessing wrong.
        flagsVal |= SASD_FLAG_HAVE_CARD | SASD_FLAG_IS_NEW_VER | SASD_FLAG_IS_HIGH_CAP;
        uint32_t numBlocks = (uint32_t) SD.numSectors();
        info[4] = (uint8_t)(numBlocks >> 24); info[5] = (uint8_t)(numBlocks >> 16);
        info[6] = (uint8_t)(numBlocks >> 8);  info[7] = (uint8_t)(numBlocks);
      }
      info[8] = (uint8_t)(flagsVal >> 24); info[9] = (uint8_t)(flagsVal >> 16);
      info[10] = (uint8_t)(flagsVal >> 8); info[11] = (uint8_t)(flagsVal);
      // manuf/oem/snum (offsets 1-3, 12-15) stay 0 - not exposed by SD.h.
      info[16] = 1;  // hwVers - arbitrary WiRSa id, no shared hw-rev registry
      info[17] = 1; info[18] = 0; info[19] = 0;  // fwVers maj.min.bfx
      sasdSendAck(tid, flags, ofst, info, sizeof(info));
      break;
    }

    case SASD_CMD_READ:
    case SASD_CMD_WRITE: {
      if (len != 12) return;   // SdReadWriteReq: cmd(1) rfu(3) sector(4) numSec(4)
      uint32_t secStart = ((uint32_t)data[4] << 24) | ((uint32_t)data[5] << 16) |
                           ((uint32_t)data[6] << 8) | data[7];
      uint32_t secNum   = ((uint32_t)data[8] << 24) | ((uint32_t)data[9] << 16) |
                           ((uint32_t)data[10] << 8) | data[11];
      bool isWrite = (cmd == SASD_CMD_WRITE);
      uint32_t totalSecs = sasdCardReady ? (uint32_t) SD.numSectors() : 0;

      if (!sasdCardReady || secStart >= totalSecs || !secNum || totalSecs - secStart < secNum) {
        sasdProtoErrors++;
        if (usbDebug) {
          UsbDebugPrint("");
          Serial.printf("SASD: %s BOUNDS error - sector %lu+%lu, card has %lu sectors\r\n",
                         isWrite ? "WRITE" : "READ", (unsigned long) secStart,
                         (unsigned long) secNum, (unsigned long) totalSecs);
        }
        sasdSendError(tid, flags, ofst, SASD_ERR_BOUNDS);
        return;
      }
      if (sasdOp.op != SASD_OP_NONE && sasdOp.numSec && sasdOp.doneSec &&
          sasdOp.numSec != sasdOp.doneSec) {
        // A previous READ/WRITE was started but never finished cleanly.
        sasdProtoErrors++;
        if (usbDebug) {
          UsbDebugPrint("");
          Serial.printf("SASD: SEQUENCE error - previous op only did %lu/%lu sectors\r\n",
                         (unsigned long) sasdOp.doneSec, (unsigned long) sasdOp.numSec);
        }
        sasdSendError(tid, flags, ofst, SASD_ERR_SEQUENCE);
        return;
      }

      sasdOp.op = isWrite ? SASD_OP_WRITE : SASD_OP_READ;
      sasdOp.firstSec = secStart;
      sasdOp.numSec = secNum;
      sasdOp.doneSec = 0;

      if (usbDebug) {
        UsbDebugPrint("");
        Serial.printf("SASD: %s start - sector %lu, %lu sector(s)\r\n",
                       isWrite ? "WRITE" : "READ", (unsigned long) secStart, (unsigned long) secNum);
      }

      uint8_t ok = SASD_RSP_OK;
      sasdSendAck(tid, flags, ofst, &ok, 1);
      break;
    }

    case SASD_NOTICE_CTS: {
      // Palm requesting the next sector of an in-progress READ.
      if (len != 1) return;
      if (sasdOp.op != SASD_OP_READ) { sasdSendError(tid, flags, ofst, SASD_ERR_STATE); return; }

      // A dupe means the Palm never saw our previous ack in time and
      // retried - not fatal (we just resend the same sector), but frequent
      // dupes are a real symptom of the link running faster than it can
      // reliably sustain, same root cause as CRC errors.
      if (isDupe && usbDebug) {
        UsbDebugPrint("");
        Serial.printf("SASD: dupe CTS (tid=%u) - Palm didn't see our last ack in time\r\n", tid);
      }

      uint32_t secNow;
      if (!isDupe) {
        if (sasdOp.doneSec >= sasdOp.numSec) { sasdSendError(tid, flags, ofst, SASD_ERR_SEQ2); return; }
        secNow = sasdOp.doneSec++;
      } else {
        if (!sasdOp.doneSec) { sasdSendError(tid, flags, ofst, SASD_ERR_SEQ2); return; }
        secNow = sasdOp.doneSec - 1;
      }
      secNow += sasdOp.firstSec;

      static uint8_t blockBuf[1 + SASD_BLOCK_SIZE];
      blockBuf[0] = SASD_NOTICE_XFER;
      if (!SD.readRAW(blockBuf + 1, secNow)) {
        sasdProtoErrors++;
        if (usbDebug) { UsbDebugPrint(""); Serial.printf("SASD: SD read FAILED at sector %lu\r\n", (unsigned long) secNow); }
        sasdSendError(tid, flags, ofst, SASD_ERR_CARD);
        return;
      }
      sasdSendAck(tid, flags, ofst, blockBuf, sizeof(blockBuf));
      sasdActivitySec = secNow;
      sasdMarkFileActivity();
      if (sasdOp.doneSec == sasdOp.numSec && usbDebug) {
        UsbDebugPrint(""); Serial.printf("SASD: READ complete - %lu sector(s)\r\n", (unsigned long) sasdOp.numSec);
      }
      break;
    }

    case SASD_NOTICE_XFER: {
      // Palm delivering the next sector of an in-progress WRITE.
      if (len != 1 + SASD_BLOCK_SIZE) return;
      if (sasdOp.op != SASD_OP_WRITE) { sasdSendError(tid, flags, ofst, SASD_ERR_STATE); return; }

      if (isDupe && usbDebug) {
        UsbDebugPrint("");
        Serial.printf("SASD: dupe XFER (tid=%u) - Palm didn't see our last ack in time\r\n", tid);
      }

      if (!isDupe) {
        if (sasdOp.doneSec >= sasdOp.numSec) { sasdSendError(tid, flags, ofst, SASD_ERR_SEQ2); return; }
        uint32_t secNow = sasdOp.firstSec + sasdOp.doneSec++;
        if (!SD.writeRAW((uint8_t *)(data + 1), secNow)) {
          sasdProtoErrors++;
          if (usbDebug) { UsbDebugPrint(""); Serial.printf("SASD: SD write FAILED at sector %lu\r\n", (unsigned long) secNow); }
          sasdSendError(tid, flags, ofst, SASD_ERR_CARD);
          return;
        }
        sasdActivitySec = secNow;
        sasdMarkFileActivity();
        if (sasdOp.doneSec == sasdOp.numSec && usbDebug) {
          UsbDebugPrint(""); Serial.printf("SASD: WRITE complete - %lu sector(s)\r\n", (unsigned long) sasdOp.numSec);
        }
      }
      uint8_t ok = SASD_RSP_OK;
      sasdSendAck(tid, flags, ofst, &ok, 1);
      break;
    }

    case SASD_CMD_RESET_STATE: {
      sasdOp.op = SASD_OP_NONE;
      uint8_t ok = SASD_RSP_OK;
      sasdSendAck(tid, flags, ofst, &ok, 1);
      break;
    }

    case SASD_CMD_SET_BAUD: {
      if (len != 8) return;   // SdBaudData: cmd(1) rfu(3) rate(4)
      uint32_t rate = ((uint32_t)data[4] << 24) | ((uint32_t)data[5] << 16) |
                       ((uint32_t)data[6] << 8) | data[7];
      // Cap at whatever baud rate the user has configured for WiRSa itself
      // (Settings > Baud Rate), not the raw UART hardware max - real-hardware
      // testing showed 115200 was unreliable on the user's cable/setup
      // (manifesting as CRC errors and eventually a failed transfer), while
      // their configured rate is the one they've already verified works.
      uint32_t maxAllowed = (uint32_t) bauds[serialSpeed];
      uint32_t requested = rate;
      if (rate > maxAllowed) rate = maxAllowed;

      uint8_t reply[8];
      memcpy(reply, data, 8);
      reply[0] = SASD_RSP_OK;
      reply[4] = (uint8_t)(rate >> 24); reply[5] = (uint8_t)(rate >> 16);
      reply[6] = (uint8_t)(rate >> 8);  reply[7] = (uint8_t)(rate);
      sasdSendAck(tid, flags, ofst, reply, sizeof(reply));

      if (usbDebug) {
        UsbDebugPrint("");
        Serial.printf("SASD: baud negotiate - Palm offered %lu, capped to %lu (WiRSa config max)\r\n",
                       (unsigned long) requested, (unsigned long) rate);
      }

      // Same live baud-switch pattern already proven in network.cpp's
      // setBaudRate()/modem.cpp's baudLoop() - drain TX at the OLD rate
      // first, THEN switch, so the ack itself isn't corrupted mid-flight.
      PhysicalSerial.flush();
      delay(50);
      PhysicalSerial.end();
      delay(200);
      PhysicalSerial.begin(rate, SERIAL_8N1, RXD2, TXD2);
      sasdRxReset();
      break;
    }

    default:
      // Unknown/unimplemented command (e.g. CMD_FW_UPDATE_START, CMD_GET_BATT)
      // - silently ignored, matching the reference's own default case.
      break;
  }
}

// ============================================================================
// Mode entry/exit + main loop
// ============================================================================

// Real-hardware finding #1: SD.usedBytes()/totalBytes() both go through
// FatFs's f_getfree(), which for FAT32 uses a free-cluster count CACHED in
// the mounted FATFS struct - only refreshed by FatFs's own file operations
// (create/write/delete through ITS OWN API). SASD's entire point is writing
// raw sectors directly (SD.writeRAW, driven by the Palm's own commands),
// which never goes through FatFs at all - so that cache goes stale after the
// first read and never updates again, no matter how often SD.usedBytes() is
// called. Confirmed by reading the actual library source (SD.cpp).
//
// Real-hardware finding #2: a first attempt at fixing this by reading the
// FAT32 FSInfo sector's free-cluster HINT directly also didn't update live -
// most likely because whatever wrote the card either isn't a superfloppy
// (no partition table, boot sector at LBA 0) or the FSInfo hint just isn't
// maintained by the writer. Rather than depend on either assumption, this
// walks the ACTUAL FAT allocation table and counts free entries directly
// (a zero entry = free cluster) - the one thing that MUST be correct for the
// filesystem to function at all, regardless of what wrote it or whether it
// bothers with the FSInfo hint. Slower (reads the whole FAT, not just one
// sector) - deliberately only run periodically (sasdLoop's 10s timer) and
// only when idle (see sasdOp check at each call site), not on every 1s
// display tick, to avoid blocking SASD's own protocol responsiveness
// mid-transfer.
static bool sasdTryParseBootSector(uint32_t baseLBA) {
  static uint8_t sec[512];
  if (!SD.readRAW(sec, baseLBA)) return false;
  if (sec[510] != 0x55 || sec[511] != 0xAA) return false;       // boot sector sig
  if (memcmp(sec + 82, "FAT32   ", 8) != 0) return false;       // BS_FilSysType - rules out
                                                                  // misreading an MBR as a BPB

  uint16_t bytesPerSec = sec[11] | ((uint16_t) sec[12] << 8);
  uint8_t  secPerClus  = sec[13];
  uint16_t rsvdSecCnt  = sec[14] | ((uint16_t) sec[15] << 8);
  uint8_t  numFATs     = sec[16];
  uint32_t fatSz32     = sec[36] | ((uint32_t) sec[37] << 8) | ((uint32_t) sec[38] << 16) | ((uint32_t) sec[39] << 24);
  uint32_t totSec32    = sec[32] | ((uint32_t) sec[33] << 8) | ((uint32_t) sec[34] << 16) | ((uint32_t) sec[35] << 24);
  if (!bytesPerSec || !secPerClus || !numFATs || !fatSz32 || !totSec32) return false;

  sasdFatStartSector = baseLBA + rsvdSecCnt;
  sasdFatSectorCount = fatSz32;
  sasdBytesPerCluster = (uint32_t) bytesPerSec * secPerClus;
  uint32_t dataSectors = totSec32 - (rsvdSecCnt + (uint32_t) numFATs * fatSz32);
  sasdTotalClusters = dataSectors / secPerClus;
  return true;
}

static bool sasdParseFat32Layout() {
  // Superfloppy format (no partition table, boot sector IS sector 0) first -
  // the SD Association's own recommended format for cards this size.
  if (sasdTryParseBootSector(0)) return true;

  // Fall back to an MBR-partitioned card: partition table starts at offset
  // 446 (16 bytes/entry); the first entry's starting LBA is the 4-byte
  // little-endian value at offset 8 within that entry (absolute offset 454).
  static uint8_t mbr[512];
  if (!SD.readRAW(mbr, 0)) return false;
  if (mbr[510] != 0x55 || mbr[511] != 0xAA) return false;
  uint32_t partLBA = mbr[454] | ((uint32_t) mbr[455] << 8) | ((uint32_t) mbr[456] << 16) | ((uint32_t) mbr[457] << 24);
  if (!partLBA) return false;
  return sasdTryParseBootSector(partLBA);
}

static bool sasdScanFreeClusters(uint32_t *outFree) {
  static uint8_t sec[512];
  const uint32_t entriesPerSector = 512 / 4;
  uint32_t totalEntries = sasdTotalClusters + 2;   // FAT array size; entries 0/1 are reserved
  uint32_t free = 0;

  for (uint32_t fatSector = 0; fatSector < sasdFatSectorCount; fatSector++) {
    uint32_t firstEntryInSector = fatSector * entriesPerSector;
    if (firstEntryInSector >= totalEntries) break;

    if (!SD.readRAW(sec, sasdFatStartSector + fatSector)) return false;

    uint32_t startIdx = (fatSector == 0) ? 2 : 0;   // skip reserved entries 0/1
    uint32_t endIdx = entriesPerSector;
    if (firstEntryInSector + endIdx > totalEntries) endIdx = totalEntries - firstEntryInSector;

    for (uint32_t i = startIdx; i < endIdx; i++) {
      uint32_t off = i * 4;
      uint32_t val = (sec[off] | ((uint32_t) sec[off + 1] << 8) | ((uint32_t) sec[off + 2] << 16) |
                       ((uint32_t) sec[off + 3] << 24)) & 0x0FFFFFFFUL;
      if (val == 0) free++;
    }
  }
  *outFree = free;
  return true;
}

static void sasdScanCapacity() {
  if (!sasdCardReady) {
    sasdUsedBytes = 0;
    return;
  }

  unsigned long startMs = millis();
  uint32_t freeClusters;
  bool viaFatScan = sasdFat32Ready && sasdScanFreeClusters(&freeClusters);
  if (viaFatScan) {
    uint64_t freeBytes = (uint64_t) freeClusters * sasdBytesPerCluster;
    sasdUsedBytes = (freeBytes < sasdTotalBytes) ? (sasdTotalBytes - freeBytes) : 0;
  } else {
    // Fallback - FatFs's own cached value, which may already be stale by
    // the time we get here, but is still better than showing nothing.
    sasdUsedBytes = SD.usedBytes();
  }

  if (usbDebug) {
    UsbDebugPrint("");
    Serial.printf("SASD: capacity scan (%s, %lums) - Used: %lu MB, Total: %lu MB\r\n",
                   viaFatScan ? "FAT walk" : "SD.usedBytes() fallback",
                   (unsigned long) (millis() - startMs),
                   (unsigned long) (sasdUsedBytes / (1024 * 1024)),
                   (unsigned long) (sasdTotalBytes / (1024 * 1024)));
  }
}

// Real-hardware finding #3: an earlier "only scan when sasdOp.op ==
// SASD_OP_NONE" gate never actually fired after the first file operation -
// sasdOp.op is only ever reset to SASD_OP_NONE on mode entry/exit or an
// explicit CMD_RESET_STATE, NOT when a READ/WRITE cleanly finishes (matches
// the reference firmware's own state machine, which relies on doneSec==
// numSec to recognize "done", not on nulling curOp). So sasdOp.op stayed
// stuck non-NONE for the rest of the session after the very first transfer,
// permanently blocking any further scan - exactly matching "still only
// showing the initial used space." Replaced with a real activity-based
// idle signal: track the time of the last actual file operation (CTS/XFER -
// see sasdMarkFileActivity, called from those handlers, not from protocol
// housekeeping like PING/GET_INFO/SET_BAUD) and only scan once that's been
// quiet for a while. This solves both problems the sasdOp.op gate was for
// at once: it's simultaneously the right "safe to scan without risking a
// mid-transfer Palm timeout" signal AND the right "something might have
// changed, worth a fresh look" trigger.
static void sasdMarkFileActivity() {
  sasdLastFileOpMillis = millis();
  sasdCapacityStale = true;
}

static bool sasdIsIdleEnoughToScan() {
  return sasdCapacityStale && (millis() - sasdLastFileOpMillis > 10000);
}

void enterSasdMode() {
  // SASD and the FTP server are mutually exclusive - SASD does raw-sector
  // I/O bypassing the FAT filesystem entirely, while the FTP server writes
  // through the normal FAT layer; running both at once risks real card
  // corruption (two unsynchronized FAT views of the same card), and FTP's
  // SD.begin() history of re-mounting could also yank the filesystem out
  // from under an FTP write in progress. See ftp_server.cpp's mirror check
  // before FTP Server > Start.
  if (ftpServerRunning) {
    SerialPrintLn("Cannot enter SASD mode: FTP server is running.");
    showMessage("Cannot start.\nStop FTP first");
    delay(1500);
    // Match ftpServerStart()'s own blocked-entry behavior: force a full
    // redraw back to a sane menu instead of leaving this message on screen
    // until the next unrelated input (mainLoop() doesn't otherwise redraw
    // on every call).
    msgFlag = true;
    mainMenu(false);
    return;
  }

  sasdPreviousMenuMode = menuMode;
  menuMode = MODE_PLAYBACK;   // repurposed slot - see globals.h

  // SASD needs a clean binary channel, same as SLIP.
  setBinaryMode(true);

  SerialPrintLn("\r\nEntering SASD Mode...");

  sasdCardReady = SD.begin(SD_CS, SPI, 20000000);
  sasdFat32Ready = false;
  if (!sasdCardReady) {
    SerialPrintLn("WARNING: no SD card - SASD will report no-card to the Palm");
    sasdTotalBytes = 0;
  } else {
    // SD.totalBytes() goes through FatFs's f_getfree(), which computes BOTH
    // total and free cluster counts in one call even though totalBytes()
    // only returns one of them - if the free-cluster cache isn't already
    // valid at mount time, that's a full FAT scan hiding inside a call that
    // looks like a cheap boot-sector read. sasdParseFat32Layout() already
    // extracts the real cluster count/size straight from the boot sector
    // via our own raw reads, so total capacity is computed directly from
    // that instead - only falls back to the (possibly slower) normal API if
    // our own parse fails.
    sasdFat32Ready = sasdParseFat32Layout();
    if (sasdFat32Ready) {
      sasdTotalBytes = (uint64_t) sasdTotalClusters * sasdBytesPerCluster;
    } else {
      sasdTotalBytes = SD.totalBytes();
    }
    if (usbDebug) {
      UsbDebugPrint("");
      Serial.printf("SASD: capacity tracking via %s\r\n",
                     sasdFat32Ready ? "direct FAT scan (live, ~10s after activity settles)" : "SD.usedBytes() (may not update live)");
    }

    // One synchronous scan here so Used shows a real number immediately on
    // entry, rather than 0/stale until the first idle-triggered refresh -
    // a single scan's cost is acceptable; it's a REPEATED scan on every
    // loop tick that was the actual problem fixed previously.
    sasdScanCapacity();
  }

  sasdOp.op = SASD_OP_NONE;
  sasdOp.firstSec = sasdOp.numSec = sasdOp.doneSec = 0;
  sasdHavePrevTid = false;
  sasdActivitySec = 0;
  sasdBytesIn = sasdBytesOut = 0;
  sasdCrcErrors = sasdProtoErrors = 0;
  sasdSpeedLastBytes = 0;
  sasdSpeedLastMillis = 0;
  sasdSpeedKBs = 0.0f;
  sasdLastFileOpMillis = millis();
  sasdCapacityStale = false;   // just scanned above (if a card is present) - the
                                // next scan happens once real activity marks it stale again
  sasdRxReset();

  // A fresh SASD conversation always starts at 9600, per the reference
  // protocol's own SD_BAUDRATE_INITIAL convention - the Palm negotiates up
  // from there via CMD_SET_BAUD once it's ready, capped at bauds[serialSpeed]
  // (WiRSa's own configured baud rate, see SASD_CMD_SET_BAUD).
  PhysicalSerial.flush();
  PhysicalSerial.end();
  delay(200);
  PhysicalSerial.begin(SASD_BAUD_INITIAL, SERIAL_8N1, RXD2, TXD2);

  SerialPrintLn("SASD Active - waiting for the Palm to connect");
  SerialPrintLn("Press +++ or BACK button to exit");

  if (usbDebug) {
    UsbDebugPrint("");
    if (sasdCardReady) {
      Serial.printf("SASD: mode entered - SD card ready, %lu sectors, max baud %lu (WiRSa config)\r\n",
                     (unsigned long) SD.numSectors(), (unsigned long) bauds[serialSpeed]);
    } else {
      Serial.printf("SASD: mode entered - SD card NOT PRESENT, max baud %lu (WiRSa config)\r\n",
                     (unsigned long) bauds[serialSpeed]);
    }
  }

  sasdShowStatus();
}

void exitSasdMode() {
  setBinaryMode(false);
  SerialPrintLn("\r\nExiting SASD Mode...");

  if (usbDebug) {
    UsbDebugPrint("");
    Serial.printf("SASD: mode exit - %lu bytes in, %lu bytes out, %lu CRC error(s), %lu protocol error(s)\r\n",
                   (unsigned long) sasdBytesIn, (unsigned long) sasdBytesOut,
                   (unsigned long) sasdCrcErrors, (unsigned long) sasdProtoErrors);
  }

  sasdOp.op = SASD_OP_NONE;

  // Real-hardware finding: leftover SASD protocol bytes were leaking into
  // whatever mode came next and getting misread as keystroke commands by the
  // menu system's own char-based input parser. The Palm doesn't know WiRSa
  // exited and may keep sending for a bit; PhysicalSerial.flush() only
  // drains the TX side, never the RX buffer. Drain RX explicitly, both
  // before and after the baud switch (anything that arrives during the
  // ~200ms gap at the OLD rate would also be garbage at the NEW rate).
  //
  // This alone was only a partial mitigation on real hardware ("tolerable,
  // not fixed" per direct user feedback), unlike the equivalent fix for
  // HotSync mode's own exit, which turned out clean. The difference: HotSync
  // formally tells the Palm the session is over via a DLP EndSync
  // request/response round trip *before* touching the baud, so nothing is
  // still in flight by the time it drains. SASD's wire protocol has no such
  // goodbye message at all (checked the reference lpc81x.c - every SASD_CMD_*
  // is Palm-initiated, WiRSa only ever replies, there is no device-to-Palm
  // session-end signal to send) - exiting is always abrupt from the Palm's
  // side, so whatever command it had in flight when BACK was pressed gets no
  // reply and it will retry, up to this same protocol family's own known
  // retry budget (FRAG_MAX_RESENDS=4 at ~150ms/attempt in the reference,
  // matching hotsync.cpp's pre-widened numbers) - up to ~600ms of retries
  // for one stranded command. A single before/after snapshot only catches
  // whatever's ALREADY buffered at that instant, not a retry burst trickling
  // in over the following several hundred ms. Widen the AFTER drain into a
  // time-bounded loop that keeps discarding anything arriving for 1s past
  // reopening the interactive baud - comfortably past that retry-exhaustion
  // window, without hanging BACK's responsiveness for long.
  while (PhysicalSerial.available()) PhysicalSerial.read();

  // Restore a normal interactive baud rate for the menu system.
  PhysicalSerial.flush();
  PhysicalSerial.end();
  delay(200);
  PhysicalSerial.begin(bauds[serialSpeed], WIRSA_SERIAL_FORMAT, RXD2, TXD2);

  unsigned long drainUntil = millis() + 1000;
  while ((long) (millis() - drainUntil) < 0) {
    while (PhysicalSerial.available()) PhysicalSerial.read();
  }

  SerialPrintLn("SASD Mode Stopped");
}

void sasdLoop() {
  readSwitches();

  if (BTNBK) {
    waitSwitches();
    exitSasdMode();
    // SASD is only ever entered from the main menu now (MODEM mode, the
    // other historical entry point, is gone) - always return there. SASD
    // draws its own title/status directly (sasdDrawStatusLines() etc.),
    // bypassing showHeader()'s normal border+clear and never touching
    // `lastMenu` - same latent bug class found and fixed in HotSync mode's
    // own exit (see hotsync.cpp's hsExit()): if the user came here straight
    // from the main menu, showHeader()'s `lastMenu != menuMode` check is
    // false and it only clears a small selection-highlight rect, leaving a
    // corrupted/missing border behind. Force the full-redraw path, same
    // `msgFlag=true` idiom already used elsewhere in this codebase.
    msgFlag = true;
    mainMenu(false);
    return;
  }

  if (BTNUP || BTNDN || BTNEN) {
    waitSwitches();
    sasdShowStatus();
  }

  // +++ escape on USB Serial only - PhysicalSerial carries binary SASD
  // traffic that shouldn't be interpreted as commands, same convention
  // slip_mode.cpp's slipActiveLoop() already uses.
  static char escapeBuffer[4] = {0};
  static int escapePos = 0;
  static unsigned long lastEscapeTime = 0;
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '+') {
      if (millis() - lastEscapeTime > 1000) escapePos = 0;
      escapeBuffer[escapePos++] = c;
      lastEscapeTime = millis();
      if (escapePos >= 3) {
        SerialPrintLn("\r\nOK");
        exitSasdMode();
        msgFlag = true; mainMenu(false);   // force full redraw - see BTNBK path's own comment
        return;
      }
    } else {
      escapePos = 0;
    }
  }

  // Feed every available byte through the receive state machine; each fully
  // validated packet is dispatched (and acked) immediately, matching the
  // reference's own always-passive "device" role.
  while (PhysicalSerial.available()) {
    uint8_t b = (uint8_t) PhysicalSerial.read();
    if (sasdReceiveByte(b)) sasdProcessPacket();
  }

  // Capacity is a full FAT walk (see sasdScanCapacity's own comment) - only
  // once the card has been quiet (no CTS/XFER) for 15s, so it never blocks
  // sasdLoop() long enough to make the Palm time out mid-transfer, and only
  // fires once per idle period rather than hammering the card every tick
  // (see sasdIsIdleEnoughToScan/sasdMarkFileActivity's own comments).
  if (sasdIsIdleEnoughToScan()) {
    sasdScanCapacity();
    sasdCapacityStale = false;
  }

  static unsigned long lastDisplayUpdate = 0;
  if (millis() - lastDisplayUpdate > 1000) {
    sasdUpdateActiveDisplay();
    lastDisplayUpdate = millis();
  }
}

// ============================================================================
// Status display - same boxed-header chrome as slip_mode.cpp's
// slipShowStatus() (WiRSa's established status-screen convention), trimmed
// to 3 content rows rather than SLIP's 4: real-hardware testing showed that
// with the header, 4 content rows read as cramped on a 128x64 OLED. Sector-
// level activity is USB-serial-only now (see the READ/WRITE/CTS/XFER debug
// logging in sasdDispatchCommand) - not useful at a glance on a small
// screen, and dropping it is what makes room for the header + human-
// readable used/total capacity + a real computed KB/s speed to fit
// comfortably.
// ============================================================================

// Auto-scaling human-readable size (B/KB/MB/GB), matching how every OS file
// browser formats sizes - a fixed "always GB" display looked wrong/useless
// for small used values (e.g. "0.0GB" on a mostly-empty card). `decimals`
// lets callers ask for finer precision on values that actually change (Used
// - Palm files are often just a few KB) vs. coarser on ones that don't
// (Total - a fixed card size), which also keeps the combined "used/total"
// OLED line from running off the right edge.
static String sasdHumanBytes(uint64_t bytes, int decimals) {
  static const char *units[] = {"B", "KB", "MB", "GB"};
  float val = (float) bytes;
  int unitIdx = 0;
  while (val >= 1024.0f && unitIdx < 3) {
    val /= 1024.0f;
    unitIdx++;
  }
  String s = String(val, unitIdx == 0 ? 0 : decimals);   // whole bytes stay integers
  s += units[unitIdx];
  return s;
}

static void sasdDrawStatusLines() {
  display.fillRect(0, 0, 128, 16, SSD1306_WHITE);
  display.drawLine(1, 1, 126, 1, SSD1306_BLACK);
  display.drawLine(1, 14, 126, 14, SSD1306_BLACK);
  display.drawLine(1, 1, 1, 14, SSD1306_BLACK);
  display.drawLine(126, 1, 126, 14, SSD1306_BLACK);

  display.setCursor(4, 4);
  display.setTextColor(SSD1306_BLACK);
  // Baud rate in the top-right corner, positioned like showHeader()'s own
  // "WiRSa <menu>" + right-padded baud text (display_menu.cpp) - but showing
  // the LIVE PhysicalSerial.baudRate(), not the configured baudDisp[serialSpeed]
  // preference: SASD actively changes its own UART rate mid-session (9600
  // listen -> negotiated/capped rate), so the configured setting can read
  // wrong for several seconds after a real rate change - live is the useful
  // value here, deliberately deviating from the plain-menu convention.
  // Replaces the old separate "Baud: " body line (Speed: moved up into its
  // place, below).
  {
    String line = "SASDCard";
    display.print(line);
    display.print(padLeft(liveBaudStr(PhysicalSerial.baudRate()), 20 - line.length()));
  }
  display.setTextColor(SSD1306_WHITE);

  display.setCursor(3, 19);
  if (sasdCardReady) {
    display.print("SD: ");
    display.print(sasdHumanBytes(sasdUsedBytes, 3));
    display.print("/");
    display.print(sasdHumanBytes(sasdTotalBytes, 1));
  } else {
    display.print("SD: NOT PRESENT");
  }

  display.setCursor(3, 33);
  display.print("Speed: ");
  display.print(sasdSpeedKBs, 3);
  display.print(" KB/s");
}

void sasdShowStatus() {
  // Same idle check as sasdLoop's own periodic trigger - if the card's been
  // quiet long enough to be worth a fresh scan, do it now rather than
  // waiting for the next loop tick; if the Palm is still actively
  // transferring, this just redraws with the last-known values instead of
  // risking a multi-second blocking scan mid-transfer.
  if (sasdIsIdleEnoughToScan()) {
    sasdScanCapacity();
    sasdCapacityStale = false;
  }
  display.clearDisplay();
  sasdDrawStatusLines();
  display.display();
  drawWifiStatusIcon();  // WiFi status on every title bar
  lastMenu = -1;  // force menu redraw on exit
}

static void sasdUpdateActiveDisplay() {
  unsigned long now = millis();
  uint32_t totalBytes = sasdBytesIn + sasdBytesOut;
  if (sasdSpeedLastMillis) {
    unsigned long elapsedMs = now - sasdSpeedLastMillis;
    if (elapsedMs > 0) {
      uint32_t delta = totalBytes - sasdSpeedLastBytes;   // wraps safely if it ever overflows
      sasdSpeedKBs = (float) delta / 1024.0f / ((float) elapsedMs / 1000.0f);
    }
  }
  sasdSpeedLastBytes = totalBytes;
  sasdSpeedLastMillis = now;

  // Capacity itself is refreshed by sasdLoop's own 10s timer, not here -
  // this just redraws with whatever the last scan found.
  display.clearDisplay();
  sasdDrawStatusLines();
  display.display();
  // clearDisplay() above wipes the WiFi icon too, and maintainWiFi()'s own
  // periodic redraw is edge-triggered (only fires on a state/animation
  // change), so it won't notice and repaint this - has to be explicit here,
  // same as sasdShowStatus().
  drawWifiStatusIcon();
}
