// SASD (Serial-Attached SD): WiRSa plays the "device" of dmitry.gr's SASD protocol; it never initiates and only replies to the Palm's SASD.prc slot driver.
// Wire-compatible, not a code port. Reference: Src/proto.h (commands, structs), slk.c (framing, CRC-16/XMODEM), frag.c (the single-fragment wrapper every message rides in; payloads are
// under FRAG_MAX_CHUNK_SZ = 524, so it never fragments), lpc81x.c (the device role).
// The slk CRC reuses zmCRC16Update() (globals.h): the same CRC-16/XMODEM table (poly 0x1021, init 0) as the reference's slk.c.

#include "Arduino.h"
#include "sasd.h"
#include "mode_exit.h"
#include "globals.h"
#include "sd_mount.h"         // zmCRC16Update - shared CRC-16/XMODEM table
#include "serial_io.h"
#include "display_menu.h"
#include "network.h"         // drawWifiStatusIcon() - sasdShowStatus() paints it too
#include "ftp_server.h"      // ftpServerRunning - SASD<->FTP mutual exclusion
#include "palm_ctl.h"        // menuBaud()
#include <SD.h>
#include "sd_activity.h"
#include "sd_usage.h"
#include "vdisk.h"
#include "settings.h"      // sasdDiskMode

#define R_OK_STAT 0   // matches modem.cpp's own resultCodes_t enum (R_OK_STAT==0);
                       // local define, to avoid pulling in that private enum (as slip_mode.cpp does)

// ---- protocol constants (Src/proto.h) ----
#define SASD_PKT_TYP           0x10  // PKT_TYP_SASD
#define SASD_PKT_ADDR          0x08  // PKT_ADDR_SASD (both src and dst)

#define SASD_CMD_PING          0x00
#define SASD_CMD_GET_INFO      0x01
#define SASD_CMD_READ          0x02
#define SASD_CMD_WRITE         0x03
#define SASD_CMD_RESET_STATE   0x04
#define SASD_CMD_SET_BAUD      0x05
// CMD_FW_UPDATE_START and CMD_GET_BATT are not implemented: only SlotCustomControl reaches them, not the VFS read/write path this mode exists for.

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
// No fixed maximum: CMD_SET_BAUD caps the rate at WiRSa's configured baud (bauds[serialSpeed]).

// ---- frag.c layer ----
#define FRAG_TYP_DATA           1
#define FRAG_TYP_ACK             2
#define FRAG_FLAG_FIRST        0x80
#define FRAG_FLAG_LAST          0x40

// ---- slk.c layer ----
#define SLK_MAGIC0  0xBE
#define SLK_MAGIC1  0xEF
#define SLK_MAGIC2  0xED
#define SLK_HDR_LEN 10   // magic[3] + dstSlk + srcSck + pktTyp + dataLen[2] + tid + csum

// Largest slk payload: 4-byte FragHdr + 1-byte NOTICE_XFER tag + 512-byte sector = 517 (the reference's SD_PROTO_MAX_MSG).
#define SASD_MAX_PAYLOAD 528

// ---- State ----

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
static bool         sasdVirtual = false;     // serving the flash virtual card (vdisk.h) because no SD card answered

// One sector source for the protocol, the capacity scan and the status screen: the SD card, or the virtual card.
static uint32_t sasdSecCount() { return sasdVirtual ? vdiskSectors() : (uint32_t) SD.numSectors(); }
static bool sasdRd(uint8_t *buf, uint32_t sec) { return sasdVirtual ? vdiskRead(buf, sec, 1) : SD.readRAW(buf, sec); }
static bool sasdWr(uint8_t *buf, uint32_t sec) { return sasdVirtual ? vdiskWrite(buf, sec, 1) : SD.writeRAW(buf, sec); }
static uint64_t     sasdTotalBytes = 0, sasdUsedBytes = 0;
// FAT32 direct-scan state (see sasdParseFat32Layout): SD.usedBytes() cannot be used for the refresh.
static bool         sasdFat32Ready = false;
static uint32_t     sasdFatStartSector = 0;   // absolute sector of the first FAT copy
static uint32_t     sasdFatSectorCount = 0;   // sectors per FAT copy
static uint32_t     sasdTotalClusters = 0;    // real usable data clusters (excludes reserved entries 0/1)
static uint32_t     sasdBytesPerCluster = 0;
static int          sasdPreviousMenuMode = MODE_MAIN;
static uint32_t     sasdActivitySec = 0;
// How quiet the Palm is (log only): when the last byte arrived, the byte total, the longest silence, and when the last "still quiet" line was written.
static unsigned long sasdLastRxMs = 0, sasdQuietLoggedMs = 0, sasdLongestQuietMs = 0;
static uint32_t      sasdRxByteCount = 0;
// Debounced capacity-refresh trigger (see sasdIsIdleEnoughToScan).
static unsigned long sasdLastFileOpMillis = 0;
static bool          sasdCapacityStale = false;
static uint32_t     sasdBytesIn = 0, sasdBytesOut = 0;
static uint32_t     sasdCrcErrors = 0, sasdProtoErrors = 0;
static uint32_t     sasdSpeedLastBytes = 0;
static unsigned long sasdSpeedLastMillis = 0;
static float        sasdSpeedKBs = 0.0f;

// from main.cpp / globals.h
extern bool usbDebug;
extern WirsaDisplay display;

// ---- forward declarations ----
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

// ---- slk framing layer ----

static void sasdRxReset() {
  sasdRx.state = SASD_RX_SYNC;
  memset(sasdRx.window, 0, sizeof(sasdRx.window));
}

static uint8_t sasdSum9(const uint8_t *hdr9) {
  uint8_t s = 0;
  for (int i = 0; i < SLK_HDR_LEN - 1; i++) s += hdr9[i];
  return s;
}

// Sends one slk packet: 10-byte header, payload, 2-byte CRC.
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

// ---- frag.c layer: single-fragment ACK-with-payload only. WiRSa never sends a standalone DATA message: every reply IS the ack of the Palm's request, as in lpc81x.c. ----

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

// ---- Byte-at-a-time receive state machine (as slip.cpp's slipReceiveByte()), fed one byte per call from sasdLoop() so nothing blocks loop(). Returns true when a full slk
// packet has been received and CRC-verified into sasdRx. ----

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
          // Cannot be a real SASD message (or the link is corrupt): keep scanning, avoid an overrun.
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

        // Ready for the next packet either way: a CRC mismatch drops this one and resyncs, as the reference's slkRx() does. The Palm retries (up to FRAG_MAX_RESENDS over ~600 ms),
        // so an occasional drop recovers; frequent ones mean the link runs faster than it can sustain.
        sasdRxReset();

        if (crcRxed == crcCalc) return true;

        sasdCrcErrors++;
        if (logOn(LOG_WARN)) {
          logPrintf(LOG_WARN, "SASD: *** CRC error #%lu (got %04X, expected %04X, %u-byte packet) - dropped, Palm will retry ***\r\n",
                         (unsigned long) sasdCrcErrors, crcRxed, crcCalc, (unsigned) sasdRx.dataLen);
        }
      }
      break;
  }
  return false;
}

// ---- Dispatch, as lpc81x.c's parseAndReplyToSasdPacket() ----

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
      // SdInfoData: cmd 1, manuf 1, oem 2, numBlocks 4, flags 4, snum 4, hwVers 1, fwVers 3 = 20 bytes, big-endian.
      uint8_t info[20] = {0};
      info[0] = SASD_RSP_INFO;
      uint32_t flagsVal = SASD_FLAG_COMMS;
      if (sasdCardReady) {
        // SD.h does not expose the SDSC/SDHC/MMC/read-only distinctions of the reference driver; "new version, high capacity" is right for the SDHC/SDXC cards this project targets.
        flagsVal |= SASD_FLAG_HAVE_CARD | SASD_FLAG_IS_NEW_VER | SASD_FLAG_IS_HIGH_CAP;
        uint32_t numBlocks = sasdSecCount();
        info[4] = (uint8_t)(numBlocks >> 24); info[5] = (uint8_t)(numBlocks >> 16);
        info[6] = (uint8_t)(numBlocks >> 8);  info[7] = (uint8_t)(numBlocks);
      }
      info[8] = (uint8_t)(flagsVal >> 24); info[9] = (uint8_t)(flagsVal >> 16);
      info[10] = (uint8_t)(flagsVal >> 8); info[11] = (uint8_t)(flagsVal);
      // manuf, oem and snum (offsets 1-3 and 12-15) stay 0: SD.h does not expose them.
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
      uint32_t totalSecs = sasdCardReady ? sasdSecCount() : 0;

      if (!sasdCardReady || secStart >= totalSecs || !secNum || totalSecs - secStart < secNum) {
        sasdProtoErrors++;
        if (logOn(LOG_WARN)) {
          logPrintf(LOG_WARN, "SASD: %s BOUNDS error - sector %lu+%lu, card has %lu sectors\r\n",
                         isWrite ? "WRITE" : "READ", (unsigned long) secStart,
                         (unsigned long) secNum, (unsigned long) totalSecs);
        }
        sasdSendError(tid, flags, ofst, SASD_ERR_BOUNDS);
        return;
      }
      if (sasdOp.op != SASD_OP_NONE && sasdOp.numSec && sasdOp.doneSec &&
          sasdOp.numSec != sasdOp.doneSec) {
        // a previous READ/WRITE was started and never finished cleanly
        sasdProtoErrors++;
        if (logOn(LOG_WARN)) {
          logPrintf(LOG_WARN, "SASD: SEQUENCE error - previous op only did %lu/%lu sectors\r\n",
                         (unsigned long) sasdOp.doneSec, (unsigned long) sasdOp.numSec);
        }
        sasdSendError(tid, flags, ofst, SASD_ERR_SEQUENCE);
        return;
      }

      sasdOp.op = isWrite ? SASD_OP_WRITE : SASD_OP_READ;
      sasdOp.firstSec = secStart;
      sasdOp.numSec = secNum;
      sasdOp.doneSec = 0;

      if (logOn(LOG_VERBOSE)) {
        logPrintf(LOG_VERBOSE, "SASD: %s start - sector %lu, %lu sector(s)\r\n",
                       isWrite ? "WRITE" : "READ", (unsigned long) secStart, (unsigned long) secNum);
      }

      uint8_t ok = SASD_RSP_OK;
      sasdSendAck(tid, flags, ofst, &ok, 1);
      break;
    }

    case SASD_NOTICE_CTS: {
      // the Palm asks for the next sector of a READ in progress
      if (len != 1) return;
      if (sasdOp.op != SASD_OP_READ) { sasdSendError(tid, flags, ofst, SASD_ERR_STATE); return; }

      // A duplicate means the Palm missed our ack and retried: not fatal (the same sector is resent), but frequent ones mean the link is too fast, as with CRC errors.
      if (isDupe && logOn(LOG_VERBOSE)) {
        logPrintf(LOG_VERBOSE, "SASD: dupe CTS (tid=%u) - Palm didn't see our last ack in time\r\n", tid);
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
      sdActivity();
      if (!sasdRd(blockBuf + 1, secNow)) {
        sasdProtoErrors++;
        if (logOn(LOG_WARN)) { logPrintf(LOG_WARN, "SASD: SD read FAILED at sector %lu\r\n", (unsigned long) secNow); }
        sasdSendError(tid, flags, ofst, SASD_ERR_CARD);
        return;
      }
      sasdSendAck(tid, flags, ofst, blockBuf, sizeof(blockBuf));
      sasdActivitySec = secNow;
      sasdMarkFileActivity();
      if (sasdOp.doneSec == sasdOp.numSec && logOn(LOG_VERBOSE)) {
        logPrintf(LOG_VERBOSE, "SASD: READ complete - %lu sector(s)\r\n", (unsigned long) sasdOp.numSec);
      }
      break;
    }

    case SASD_NOTICE_XFER: {
      // the Palm delivers the next sector of a WRITE in progress
      if (len != 1 + SASD_BLOCK_SIZE) return;
      if (sasdOp.op != SASD_OP_WRITE) { sasdSendError(tid, flags, ofst, SASD_ERR_STATE); return; }

      if (isDupe && logOn(LOG_VERBOSE)) {
        logPrintf(LOG_VERBOSE, "SASD: dupe XFER (tid=%u) - Palm didn't see our last ack in time\r\n", tid);
      }

      if (!isDupe) {
        if (sasdOp.doneSec >= sasdOp.numSec) { sasdSendError(tid, flags, ofst, SASD_ERR_SEQ2); return; }
        uint32_t secNow = sasdOp.firstSec + sasdOp.doneSec++;
        sdActivity();
        if (!sasdWr((uint8_t *)(data + 1), secNow)) {
          sasdProtoErrors++;
          if (logOn(LOG_WARN)) { logPrintf(LOG_WARN, "SASD: SD write FAILED at sector %lu\r\n", (unsigned long) secNow); }
          sasdSendError(tid, flags, ofst, SASD_ERR_CARD);
          return;
        }
        // A reset ends SASD, so a write must be on flash before it is acknowledged as complete.
        if (sasdVirtual && sasdOp.doneSec == sasdOp.numSec && !vdiskFlush()) {
          sasdProtoErrors++;
          if (logOn(LOG_WARN)) { logPrintf(LOG_WARN, "SASD: virtual card flash write FAILED near sector %lu\r\n", (unsigned long) secNow); }
          sasdSendError(tid, flags, ofst, SASD_ERR_CARD);
          return;
        }
        sasdActivitySec = secNow;
        sasdMarkFileActivity();
        if (sasdOp.doneSec == sasdOp.numSec && logOn(LOG_VERBOSE)) {
          logPrintf(LOG_VERBOSE, "SASD: WRITE complete - %lu sector(s)\r\n", (unsigned long) sasdOp.numSec);
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
      // Cap at the baud the user configured for WiRSa (Settings > Baud Rate), not the UART maximum: 115200 was unreliable on the user's cable (CRC errors, then a failed
      // transfer), while the configured rate is the one verified to work.
      uint32_t maxAllowed = (uint32_t) bauds[serialSpeed];
      uint32_t requested = rate;
      if (rate > maxAllowed) rate = maxAllowed;

      uint8_t reply[8];
      memcpy(reply, data, 8);
      reply[0] = SASD_RSP_OK;
      reply[4] = (uint8_t)(rate >> 24); reply[5] = (uint8_t)(rate >> 16);
      reply[6] = (uint8_t)(rate >> 8);  reply[7] = (uint8_t)(rate);
      sasdSendAck(tid, flags, ofst, reply, sizeof(reply));

      if (logOn(LOG_INFO)) {
        logPrintf(LOG_INFO, "SASD: baud negotiate - Palm offered %lu, capped to %lu (WiRSa config max)\r\n",
                       (unsigned long) requested, (unsigned long) rate);
      }

      // Switch live as network.cpp's setBaudRate() does: drain TX at the OLD rate first, so the ack itself is not corrupted.
      PhysicalSerial.flush();
      delay(50);
      PhysicalSerial.end();
      delay(200);
      PhysicalSerial.begin(rate, SERIAL_8N1, RXD2, TXD2);
      sasdRxReset();
      break;
    }

    default:
      // unknown or unimplemented command (e.g. CMD_FW_UPDATE_START, CMD_GET_BATT): ignored, as the reference's default case
      break;
  }
}

// ---- Mode entry/exit and main loop ----

// Capacity scan, why it walks the FAT: SD.usedBytes()/totalBytes() use FatFs's f_getfree(), which for FAT32 returns a free-cluster count CACHED in the mounted FATFS and
// refreshed only by FatFs's own file operations. SASD writes raw sectors (SD.writeRAW) that never touch FatFs, so the cache is stale after the first read (confirmed in the SD.cpp source).
// Reading the FSInfo free-cluster hint did not update live either (the card may have no partition table, or the writer does not maintain the hint). So this counts the free
// entries (zero) in the actual FAT, the one thing that must be right for the filesystem to work. It is slow (reads the whole FAT), so it runs only periodically (sasdLoop's 10 s
// timer) and only when idle (sasdIsIdleEnoughToScan), never on the 1 s display tick, so it cannot stall the protocol mid-transfer.
static bool sasdTryParseBootSector(uint32_t baseLBA) {
  static uint8_t sec[512];
  if (!sasdRd(sec, baseLBA)) return false;
  if (sec[510] != 0x55 || sec[511] != 0xAA) return false;       // boot sector sig
  if (memcmp(sec + 82, "FAT32   ", 8) != 0) return false;       // BS_FilSysType - rules out
                                                                  // so an MBR is not misread as a BPB

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
  // Superfloppy first (no partition table, boot sector at sector 0): the SD Association's recommended format for cards this size.
  if (sasdTryParseBootSector(0)) return true;

  // Else an MBR-partitioned card: the partition table is at offset 446 (16 bytes per entry); the first entry's starting LBA is the little-endian 4 bytes at offset 8 (absolute 454).
  static uint8_t mbr[512];
  if (!sasdRd(mbr, 0)) return false;
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

    if (!sasdRd(sec, sasdFatStartSector + fatSector)) return false;

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

static bool sasdVirtualCapacity() {
  return vdiskUsage(&sasdUsedBytes, &sasdTotalBytes);
}

static void sasdScanCapacity() {
  if (sasdVirtual) {
    if (!sasdVirtualCapacity()) sasdUsedBytes = 0;
    return;
  }
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
    // Fallback: FatFs's own cached value, possibly stale, but better than nothing.
    sasdUsedBytes = SD.usedBytes();
  }

  if (logOn(LOG_VERBOSE)) {
    logPrintf(LOG_VERBOSE, "SASD: capacity scan (%s, %lums) - Used: %lu MB, Total: %lu MB\r\n",
                   viaFatScan ? "FAT walk" : "SD.usedBytes() fallback",
                   (unsigned long) (millis() - startMs),
                   (unsigned long) (sasdUsedBytes / (1024 * 1024)),
                   (unsigned long) (sasdTotalBytes / (1024 * 1024)));
  }
}

// Idle detection for the scan: gating on sasdOp.op == SASD_OP_NONE never fired after the first transfer, because sasdOp.op is reset only at mode entry/exit or
// CMD_RESET_STATE, not when a READ/WRITE finishes (the reference recognises "done" by doneSec == numSec), so Used stayed at its first value. Instead track the time of the last real
// file operation (CTS/XFER, sasdMarkFileActivity, not PING/GET_INFO/SET_BAUD) and scan once that has been quiet for a while. That is both the "safe to scan" signal (no mid-transfer
// Palm timeout) and the "something may have changed" trigger.
static void sasdMarkFileActivity() {
  sasdLastFileOpMillis = millis();
  sasdCapacityStale = true;
}

static bool sasdIsIdleEnoughToScan() {
  return sasdCapacityStale && (millis() - sasdLastFileOpMillis > 10000);
}

void enterSasdMode() {
  // SASD and the FTP server are mutually exclusive: SASD does raw sector I/O around the FAT, the FTP server writes through it, so both at once risks corrupting the card (two
  // unsynchronised FAT views), and FTP's SD.begin() re-mount could pull the filesystem from under a write. ftp_server.cpp has the mirror check before FTP Server > Start.
  if (ftpServerRunning) {
    SerialPrintLn("Cannot enter SASD mode: FTP server is running.");
    showMessage("Cannot start.\nStop FTP first");
    delay(1500);
    // As ftpServerStart()'s blocked entry: force a full redraw to a sane menu, not leave this message until the next input (mainLoop() does not redraw every call).
    msgFlag = true;
    mainMenu(false);
    return;
  }

  sasdPreviousMenuMode = menuMode;
  menuMode = MODE_PLAYBACK;   // repurposed slot - see globals.h

  // SASD needs a clean binary channel, as SLIP.
  setBinaryMode(true);

  SerialPrintLn("\r\nEntering SASD Mode...");

  sasdCardReady = sasdDiskMode != SASD_DISK_VIRTUAL && sdMount();
  sasdVirtual = false;
  sasdFat32Ready = false;
  if (!sasdCardReady && sasdDiskMode != SASD_DISK_SD && vdiskPresent() && vdiskOpen()) {
    // No SD card (or the virtual card was chosen): serve the flash virtual card (FAT16, formatted on first use).
    if (vdiskPrepare((uint32_t) ESP.getEfuseMac())) {
      sasdVirtual = sasdCardReady = true;
      LOGI("SASD: serving the virtual card (%lu KB) - %s\r\n", (unsigned long) (vdiskSectors() >> 1),
           sasdDiskMode == SASD_DISK_VIRTUAL ? "chosen in Setup" : "no SD card");
    } else {
      vdiskClose();
      LOGW("SASD: no SD card and the virtual card could not be set up\r\n");
    }
  }
  if (sasdVirtual) {
    sasdScanCapacity();
  } else if (!sasdCardReady) {
    SerialPrintLn("WARNING: no SD card - SASD will report no-card to the Palm");
    sasdTotalBytes = 0;
  } else {
    // SD.totalBytes() goes through f_getfree(), which computes total and free counts together: if the free-cluster cache is not valid at mount, a full FAT scan hides in what looks
    // like a cheap boot-sector read. sasdParseFat32Layout() already reads the cluster count and size from the boot sector, so the total comes from that, with the normal API only as a fallback.
    sasdFat32Ready = sasdParseFat32Layout();
    if (sasdFat32Ready) {
      sasdTotalBytes = (uint64_t) sasdTotalClusters * sasdBytesPerCluster;
    } else {
      sasdTotalBytes = SD.totalBytes();
    }
    if (logOn(LOG_VERBOSE)) {
      logPrintf(LOG_VERBOSE, "SASD: capacity tracking via %s\r\n",
                     sasdFat32Ready ? "direct FAT scan (live, ~10s after activity settles)" : "SD.usedBytes() (may not update live)");
    }

    // One synchronous scan here so Used shows a real number on entry, not 0 or stale until the first idle refresh; one scan is fine, a scan on every loop tick was the old problem.
    sasdScanCapacity();
  }

  if (!sasdCardReady) {
    // Nothing to serve (no card, and the virtual card unavailable or not chosen): do not start, so the Palm's driver is not left talking to a "no card" device.
    LOGW("SASD: not started - no SD card%s\r\n", sasdDiskMode == SASD_DISK_SD ? " (SASD disk is set to SD card)" : "");
    setBinaryMode(false);
    menuMode = sasdPreviousMenuMode;
    showMessage("\nPLEASE INSERT\n   SD CARD");
    delay(1500);
    msgFlag = true;
    mainMenu(false);
    return;
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
                                // the next scan happens once real activity marks it stale
  sasdRxReset();
  sasdLastRxMs = sasdQuietLoggedMs = millis();
  sasdLongestQuietMs = 0;
  sasdRxByteCount = 0;

  // A SASD conversation always starts at 9600 (the reference's SD_BAUDRATE_INITIAL); the Palm negotiates up with CMD_SET_BAUD, capped at bauds[serialSpeed].
  PhysicalSerial.flush();
  PhysicalSerial.end();
  delay(200);
  PhysicalSerial.begin(SASD_BAUD_INITIAL, SERIAL_8N1, RXD2, TXD2);

  SerialPrintLn("SASD Active - waiting for the Palm to connect");
  SerialPrintLn("Press +++ or BACK button to exit");

  if (logOn(LOG_INFO)) {
    if (sasdCardReady) {
      logPrintf(LOG_INFO, "SASD: mode entered - SD card ready, %lu sectors, max baud %lu (WiRSa config)\r\n",
                     (unsigned long) sasdSecCount(), (unsigned long) bauds[serialSpeed]);
    } else {
      logPrintf(LOG_INFO, "SASD: mode entered - SD card NOT PRESENT, max baud %lu (WiRSa config)\r\n",
                     (unsigned long) bauds[serialSpeed]);
    }
  }

  sasdShowStatus();
}

void exitSasdMode() {
  setBinaryMode(false);
  SerialPrintLn("\r\nExiting SASD Mode...");

  if (logOn(LOG_INFO)) {
    logPrintf(LOG_INFO, "SASD: mode exit - %lu bytes in, %lu bytes out, %lu CRC error(s), %lu protocol error(s); %lu byte(s) seen on the line, last one %lu s ago, longest silence %lu s\r\n",
                   (unsigned long) sasdBytesIn, (unsigned long) sasdBytesOut,
                   (unsigned long) sasdCrcErrors, (unsigned long) sasdProtoErrors,
                   (unsigned long) sasdRxByteCount, (millis() - sasdLastRxMs) / 1000UL,
                   (unsigned long) (max(sasdLongestQuietMs, millis() - sasdLastRxMs) / 1000UL));
  }

  sasdOp.op = SASD_OP_NONE;
  if (sasdVirtual) { vdiskClose(); sasdVirtual = false; }

  // Leftover SASD bytes leaked into the next mode and were read as menu keystrokes. The Palm does not know WiRSa left and keeps sending for a while, and flush() drains only TX,
  // so drain RX before and after the baud switch (bytes arriving during the ~200 ms gap at the old rate are garbage at the new one).
  // That alone only partly worked ("tolerable, not fixed"), unlike HotSync's exit, because HotSync ends with a DLP EndSync round trip before touching the baud, so nothing is in flight.
  // SASD has no goodbye (every SASD_CMD_* is Palm-initiated; checked lpc81x.c), so the command in flight at BACK gets no reply and is retried for up to ~600 ms (FRAG_MAX_RESENDS = 4 at
  // ~150 ms in the reference). One snapshot catches only what is already buffered, not a retry burst. So the AFTER drain is a time-bounded loop that discards whatever arrives for 1 s
  // after reopening the interactive baud, past the retry window, without hanging BACK for long.
  while (PhysicalSerial.available()) PhysicalSerial.read();

  // restore a normal interactive baud for the menu system
  PhysicalSerial.flush();
  PhysicalSerial.end();
  delay(200);
  PhysicalSerial.begin(menuBaud(), WIRSA_SERIAL_FORMAT, RXD2, TXD2);   // the menu's current rate (palm_ctl.h)

  unsigned long drainUntil = millis() + 1000;
  while ((long) (millis() - drainUntil) < 0) {
    while (PhysicalSerial.available()) PhysicalSerial.read();
  }

  SerialPrintLn("SASD Mode Stopped");
}

void sasdLoop() {
  // SASD draws its own title and status, bypassing showHeader()'s border and clear, so leaving takes the full-redraw path (ModeExit::toMenu), the same latent bug as HotSync's hsExit().
  if (ModeExit::backPressed()) {
    exitSasdMode();
    ModeExit::toMenu();
    return;
  }

  if (BTNUP || BTNDN || BTNEN) {
    waitSwitches();
    sasdShowStatus();
  }

  // "+++" on the USB serial only: PhysicalSerial carries binary SASD traffic that must not be read as commands.
  if (ModeExit::escapeTyped()) {
    exitSasdMode();
    ModeExit::toMenu();
    return;
  }

  // Feed every available byte to the receive state machine; each validated packet is dispatched and acked at once (the passive "device" role).
  while (PhysicalSerial.available()) {
    uint8_t b = (uint8_t) PhysicalSerial.read();
    unsigned long now = millis(), gap = now - sasdLastRxMs;
    if (gap > sasdLongestQuietMs) sasdLongestQuietMs = gap;
    if (gap > 5000) LOGI("SASD: Palm traffic again after %lu s quiet (first byte 0x%02X, %lu byte(s) seen so far)\r\n",
                         gap / 1000UL, (unsigned) b, (unsigned long) sasdRxByteCount);
    sasdLastRxMs = now;
    sasdRxByteCount++;
    if (sasdReceiveByte(b)) sasdProcessPacket();
  }
  // SASD timeout (Config > Exit Timers): nothing from the Palm for that long. The Palm is off or its driver let go of the port (measured: silent, no polling, no noise), so go back to
  // the main menu, where WiRSaCtl works again. No check for an operation under way: every operation is driven by the Palm's packets, and sasdOp stays set after a read or write until RESET_STATE.
  if (millis() - sasdLastRxMs > (unsigned long) sasdTimeoutMin * 60000UL) {
    LOGI("SASD: no byte from the Palm for %lu s (timeout %u min) - back to the main menu\r\n", (millis() - sasdLastRxMs) / 1000UL, (unsigned) sasdTimeoutMin);
    exitSasdMode();
    ModeExit::toMenu();
    return;
  }
  {   // a minute at a time while the line stays silent
    unsigned long now = millis();
    if (now - sasdLastRxMs > 60000UL && now - sasdQuietLoggedMs >= 60000UL) {
      sasdQuietLoggedMs = now;
      LOGI("SASD: no byte from the Palm for %lu min (%lu byte(s) seen since entry)\r\n", (now - sasdLastRxMs) / 60000UL, (unsigned long) sasdRxByteCount);
    }
  }

  // The capacity scan is a full FAT walk: only after the card has been quiet (no CTS/XFER) for 15 s, so it cannot stall sasdLoop() into a Palm timeout, and once per idle period.
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

// ---- Status display: the boxed-header chrome of slip_mode.cpp's slipShowStatus(), trimmed to 3 content rows (with the header, 4 rows look cramped on a 128x64 OLED).
// Sector-level activity is USB-serial only (the READ/WRITE/CTS/XFER debug logging in sasdDispatchCommand), which leaves room for readable used/total capacity and a computed KB/s. ----

// Auto-scaling size (B/KB/MB/GB) as an OS file browser shows it; a fixed "always GB" read "0.0GB" for small values. decimals lets callers ask for more precision where the value changes
// (Used: a few KB) and less where it does not (Total), which also keeps the "used/total" line within the OLED width.
static String sasdHumanBytes(uint64_t bytes, int decimals) { return sdHumanBytes(bytes, decimals); }   // sd_usage.cpp: shared with the WiRSaControl SD row

static void sasdDrawStatusLines() {
  display.fillRect(0, 0, 128, 16, SSD1306_WHITE);
  display.drawLine(1, 1, 126, 1, SSD1306_BLACK);
  display.drawLine(1, 14, 126, 14, SSD1306_BLACK);
  display.drawLine(1, 1, 1, 14, SSD1306_BLACK);
  display.drawLine(126, 1, 126, 14, SSD1306_BLACK);

  display.setCursor(4, 4);
  display.setTextColor(SSD1306_BLACK);
  // Baud top right, placed like showHeader()'s "WiRSa <menu>" with right-padded baud, but the LIVE PhysicalSerial.baudRate(): SASD changes its UART rate mid-session (9600 listen to
  // the negotiated rate), so the configured baudDisp[serialSpeed] would read wrong for seconds.
  {
    String line = "SASDCard";
    display.print(line);
    display.print(padLeft(liveBaudStr(PhysicalSerial.baudRate()), 20 - line.length()));
  }
  display.setTextColor(SSD1306_WHITE);

  display.setCursor(3, 19);
  if (sasdCardReady) {
    display.print(sasdVirtual ? "VD: " : "SD: ");
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
  // The same idle check as sasdLoop's periodic trigger: if the card has been quiet long enough, scan now; if the Palm is still transferring, redraw with the last values rather than risk a blocking scan.
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

  // Capacity is refreshed by sasdLoop's 10 s timer, not here; this redraws the last scan.
  display.clearDisplay();
  sasdDrawStatusLines();
  display.display();
  // clearDisplay() above wipes the WiFi icon, and maintainWiFi()'s redraw is edge-triggered, so repaint it here (as sasdShowStatus()).
  drawWifiStatusIcon();
}
