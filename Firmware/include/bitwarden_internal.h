// bitwarden_internal.h - what the files of BitWarden Relay mode share. Not for the rest of the firmware: that sees only bitwarden_relay.h.
//
//   bitwarden_queue.cpp   the download-ahead queue: frames made by the fetch, sent as the Palm takes them
//   bitwarden_crypto.cpp  key derivation, AES-CBC, RSA-OAEP unwrapping
//   bitwarden_login.cpp   prelogin and login (token request, two-factor provider choice)
//   bitwarden_vault.cpp   the vault: streaming JSON scan of /sync, one cipher at a time, decrypted and handed on
//   bitwarden_sync.cpp    TRUST and SYNC handling: login, fetch, send; the big-stack task
//   bitwarden_relay.cpp   the mode itself (relay_mode.h) and the OLED
//
// Wire protocol constants below MUST match docs/protocol/Protocol-wire.md (section 11) and apps/bitwarden (hackmaster_y2035 repo). One sync at a time:
// the scratch buffers and the per-sync state are shared by the files and reset by bwHandleSync.
#pragma once
#include "bitwarden_relay.h"
#include "pbkdf2_fast.h"
#include "globals.h"
#include "serial_io.h"
#include "display_menu.h"
#include "network.h"
#include "relay_proto.h"
#include "palm_ctl.h"
#include "mode_timer.h"
#include "relay_mode.h"
#include "relay_timing.h"
#include "palm_text.h"       // UTF-8 to Palm Latin: display text only, never a secret
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <mbedtls/md.h>
#include <mbedtls/aes.h>
#include <mbedtls/base64.h>
#include <mbedtls/pk.h>
#include <mbedtls/rsa.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>
extern bool usbDebug;
extern HardwareSerial PhysicalSerial;

#ifndef WIRSA_BW_QUEUE
#define WIRSA_BW_QUEUE 1
#endif

namespace BitWardenRelay {

// The type numbers follow Protocol-wire.md section 6: the common frames at 0x01-0x06, BitWarden's own in 0x30-0x3F, the frames the Palm sends first (the junk filter's bound, BW_MAX_PALM_TYPE, is the highest).
enum : uint8_t {
  BW_HELLO = 0x01, BW_HELLO_ACK = 0x02, BW_BYE = 0x03,
  BW_ACK = 0x04,        // Palm -> WiRSa, empty body - see bwWriteFrameAcked
  BW_SYNC_DONE = 0x05,
  BW_ERR = 0x06,        // WiRSa -> Palm: code u8, message str, rememberToken str
  BW_TRUST = 0x30, BW_SYNC = 0x31,
  BW_TWO_FACTOR_SUBMIT   = 0x32, // Palm -> WiRSa, see docs/protocol/Protocol-wire.md
  BW_TRUST_RESULT = 0x33, BW_SYNC_ITEM = 0x34, BW_FOLDER = 0x35, BW_ORG = 0x36,
  BW_TWO_FACTOR_REQUIRED = 0x37, // WiRSa -> Palm, see docs/protocol/Protocol-wire.md
  // WiRSa -> Palm, body: `string statusText`. Cosmetic: a live status label for the one long stretch (prelogin, PBKDF2, login, api-sync fetch) that had no feedback between SYNC and the first
  // FOLDER/ORG/SYNC_ITEM/ERR/TWO_FACTOR_REQUIRED frame. See bwSendSyncStatus and Protocol-wire.md.
  BW_SYNC_STATUS = 0x38
};
static const uint8_t BW_MAX_PALM_TYPE = BW_TWO_FACTOR_SUBMIT;

enum : uint8_t {
  BW_ERR_DNS_FAIL = 1, BW_ERR_CONNECT_FAIL = 2, BW_ERR_TLS_FAIL = 3,
  BW_ERR_FINGERPRINT_MISMATCH = 4, BW_ERR_AUTH_FAILED = 5,
  BW_ERR_TWO_FACTOR_REQUIRED = 6, BW_ERR_SYNC_FETCH_FAILED = 7,
  BW_ERR_DECRYPT_FAILED = 8, BW_ERR_MALFORMED_KDF = 9
};

static const uint8_t  BW_PROTOCOL_VERSION = 2;   // 2: the type numbers of Protocol-wire.md section 6 (the Palm app refuses a WiRSa that reports less)
static const size_t BW_MAX_BODY = 4096;
using BwWriter = RelayProto::Writer;
using BwReader = RelayProto::Reader;

// structures the files exchange
#if WIRSA_BW_QUEUE
struct BwQNode {
  BwQNode *next;
  uint16_t len;
  uint16_t seq;                     // 1-based position among the queued frames (for the log)
  uint8_t type;
  bool final;                       // sent without an ack wait (SYNC_DONE/ERR)
  char label[32];                   // item name, log only
  uint8_t *data() { return (uint8_t *) (this + 1); }
};

struct BwQueueState {
  bool active = false;              // queue mode for this sync (PSRAM present)
  volatile bool started = false;    // first frame queued: the consumer owns the Palm line from here
  volatile bool cancel = false;     // consumer lost the Palm: stop producing
  volatile bool palmBye = false;
  volatile bool done = false;       // producer finished
  BwQNode *head = nullptr, *tail = nullptr;
  size_t bytes = 0;
  unsigned depth = 0;
  uint16_t pushed = 0;
  SemaphoreHandle_t mutex = nullptr;
  SemaphoreHandle_t avail = nullptr;   // counts queued nodes, plus one token when the producer is done
};
extern BwQueueState gQ;
#endif

struct BwCryptoContext {
  uint8_t masterKey[32];
  uint8_t stretchedEncKey[32];
  uint8_t stretchedMacKey[32];
  uint8_t userEncKey[32];
  uint8_t userMacKey[32];
};

struct BwKdfParams { uint8_t type = 1; uint32_t iterations = 600000; uint32_t memoryKiB = 0; uint8_t parallelism = 0; };

struct BwJsonScanner {
  Stream &s;
  int pushed;
  static const size_t kBufSize = 512;
  uint8_t buf[kBufSize];
  size_t bufPos, bufLen;
  explicit BwJsonScanner(Stream &stream) : s(stream), pushed(-2), bufPos(0), bufLen(0) {}

  // One readBytes() already waits up to the Stream's setTimeout() (45 s, see bwSyncAndStream) before returning 0, so one empty refill can be a long stall on a live connection (WiFi congestion, a slow
  // server assembling a large payload), not a dead one. Retry the refill a few times: a dead connection returns 0 at once each time (socket/TLS closure is detected immediately, it does not sit out the
  // timeout again), so this costs almost nothing there and adds resilience against a slow but live one.
  static const int kMaxRefillRetries = 3;

  int next() {
    if (pushed != -2) { int c = pushed; pushed = -2; return c; }
    if (bufPos >= bufLen) {
      for (int attempt = 0; attempt < kMaxRefillRetries; attempt++) {
        bufLen = s.readBytes(buf, kBufSize);  // respects Stream::setTimeout(); returns what is available, up to kBufSize
        bufPos = 0;
        if (bufLen > 0) break;
      }
      if (bufLen == 0) return -1;
    }
    return buf[bufPos++];
  }
  void pushBack(int c) { pushed = c; }

  int nextNonWs() {
    for (;;) {
      int c = next();
      if (c < 0 || (c != ' ' && c != '\t' && c != '\r' && c != '\n')) return c;
    }
  }
};

struct BwOrgKey { uint8_t encKey[32]; uint8_t macKey[32]; bool valid; };

// ---- Shared state ----
extern unsigned long gBwNegotiatedAt;
extern unsigned gBwSessOk, gBwSessFail, gBwSessItems;
extern bool gBwProdOk;
extern unsigned gBwProdItems, gBwProdFolders, gBwProdOrgs, gBwSkippedItems;
extern uint8_t gFrameBody[BW_MAX_BODY];
extern uint8_t gScratchCt[BW_MAX_BODY];
extern uint8_t gScratchPt[BW_MAX_BODY];
extern uint8_t gScratchMacInput[16 + BW_MAX_BODY];


// Ack frames are empty-body; this is only somewhere to read them into. Separate from gFrameBody so a retry always resends the original frame.
static uint8_t gBwAckScratch[32];

// idleTimeoutMs affects only the wait for this frame's own first byte; it defaults to the inter-frame timeout, and bwHandleSync's 2FA wait passes RelayTiming::BW_TWO_FACTOR_TIMEOUT_MS, as that wait is for a human.
inline bool bwReadFrame(uint8_t &type, uint8_t *body, size_t bodyCap, size_t &bodyLen,
                         uint32_t idleTimeoutMs = RelayTiming::FRAME_IDLE_TIMEOUT_MS) {
  return RelayProto::readFrame(PhysicalSerial, type, body, bodyCap, bodyLen,
                               idleTimeoutMs, RelayTiming::BYTE_TIMEOUT_MS);
}

inline void bwWriteFrame(uint8_t type, const uint8_t *body, size_t bodyLen) {
  RelayProto::writeFrame(PhysicalSerial, type, body, bodyLen);
}

// FOLDER/ORG/SYNC_ITEM are flow-controlled: send, then block for the Palm's BW_ACK before the next frame. 5 attempts at 500 ms mirror the Palm's SendItemAckWithRetry (Sync.c), so neither side's
// patience runs out first. palmSaidBye is set when the Palm sent BW_BYE instead of an ack.
inline bool bwWriteFrameAcked(uint8_t type, const uint8_t *body, size_t bodyLen, bool *palmSaidBye = nullptr,
                              int attempts = RelayTiming::BW_ACK_ATTEMPTS) {
  return RelayProto::writeFrameAcked(PhysicalSerial, type, body, bodyLen,
                                     BW_ACK, BW_BYE,
                                     gBwAckScratch, sizeof(gBwAckScratch),
                                     attempts, RelayTiming::ACK_RETRY_DELAY_MS, RelayTiming::ITEM_ACK_TIMEOUT_MS, RelayTiming::BYTE_TIMEOUT_MS,
                                     palmSaidBye);
}

// ---- Functions the files share ----

// ---- bitwarden_queue.cpp ----
#if WIRSA_BW_QUEUE
void bwQFree(BwQNode *n);
void bwQReset();
bool bwQPush(uint8_t type, const uint8_t *body, size_t len, bool final, const char *label = nullptr);
BwQNode *bwQPop();
void bwQConsume();
bool bwStreamSend(uint8_t type, const uint8_t *body, size_t len, bool *palmSaidBye = nullptr,
                         const char *label = nullptr);
void bwEmitFinal(uint8_t type, const uint8_t *body, size_t len);
#endif

#if !(WIRSA_BW_QUEUE)
bool bwStreamSend(uint8_t type, const uint8_t *body, size_t len, bool *palmSaidBye = nullptr,
                         const char *label = nullptr);
void bwEmitFinal(uint8_t type, const uint8_t *body, size_t len);
#endif


// ---- bitwarden_crypto.cpp ----
void bwHmacSha256(const uint8_t *key, size_t keyLen, const uint8_t *msg, size_t msgLen, uint8_t out[32]);
void bwPbkdf2Sha256(const uint8_t *pw, size_t pwLen, const uint8_t *salt, size_t saltLen,
                            uint32_t iterations, uint8_t out[32]);
void bwHkdfExpandSha256(const uint8_t prk[32], const char *info, uint8_t out[32]);
bool bwBase64Decode(const String &in, uint8_t *out, size_t outCap, size_t &outLen);
String bwBase64Encode(const uint8_t *in, size_t inLen);
bool bwAesCbcDecrypt(const uint8_t key32[32], const uint8_t iv16[16],
                             const uint8_t *ct, size_t ctLen, uint8_t *ptOut, size_t &ptLen);
bool bwDecryptEncString(const String &encString, const uint8_t encKey[32], const uint8_t macKey[32], String &plainOut);
bool bwRsaOaepUnwrap(mbedtls_pk_context &pk, const String &wrapped, uint8_t out[64]);
bool bwParsePrivateKey(const uint8_t *der, size_t derLen, mbedtls_pk_context &pk);
String urlEncode(const String &s);
void bwDeriveMasterKey(const String &password, const String &email, uint32_t iterations, uint8_t out[32]);
String bwDeriveMasterPasswordHash(const uint8_t masterKey[32], const String &password);
void bwDeriveStretchedKeys(const uint8_t masterKey[32], uint8_t encKeyOut[32], uint8_t macKeyOut[32]);

// ---- bitwarden_login.cpp ----
bool bwPrelogin(WiFiClientSecure &tls, const String &host, const String &email, BwKdfParams &kdf, String &errOut);
void bwPickTwoFactorProvider(JsonDocument &doc, uint8_t &outProvider, String &outLabel);
bool bwLogin(WiFiClientSecure &tls, const String &host, const String &email, const String &masterPasswordHashB64,
                     const String &twoFactorToken, uint8_t twoFactorProvider,
                     String &accessToken, String &keyEncString, String &outNewTwoFactorToken,
                     uint8_t &outTwoFactorProvider, String &outTwoFactorLabel,
                     String &errOut, uint8_t &errCode);

// ---- bitwarden_vault.cpp ----
bool bwScanJsonValue(BwJsonScanner &sc, String &out, size_t maxLen);
void bwBuildCipherFilter(JsonDocument &filter);
bool bwFindCappedStringField(const String &json, const char *fieldName, String &out, size_t maxLen);
bool bwProcessOneCipher(const String &itemJson, JsonDocument &filter,
                                const uint8_t userEncKey[32], const uint8_t userMacKey[32],
                                const String gFolderIds[], int folderCount,
                                const String gOrgIds[], int orgCount,
                                const BwOrgKey orgKeys[],
                                uint16_t &itemCount, String &errOut, uint8_t &errCode);
bool bwStreamSyncBody(Stream &s, const uint8_t userEncKey[32], const uint8_t userMacKey[32],
                              uint16_t &itemCount, uint16_t &folderCount, uint16_t &orgCount,
                              uint16_t &skipFolders, uint16_t &skipOrgs, uint16_t &skipCiphers,
                              String &errOut, uint8_t &errCode, unsigned long tSyncStart);
bool bwSyncAndStream(WiFiClientSecure &tls, const String &host, const String &accessToken,
                             const uint8_t userEncKey[32], const uint8_t userMacKey[32],
                             uint16_t &itemCount, uint16_t &folderCount, uint16_t &orgCount,
                             uint16_t &skipFolders, uint16_t &skipOrgs, uint16_t &skipCiphers,
                             String &errOut, uint8_t &errCode,
                             unsigned long tSyncStart);

// ---- bitwarden_sync.cpp ----
String bwUrlToHost(const String &url, uint16_t &port);
void bwHandleTrust(const uint8_t *body, size_t bodyLen);
void bwSendSyncStatus(const char *text);
void bwHandleSync(const uint8_t *body, size_t bodyLen);
void bwReportDelivery();
void bwHandleSyncTask(void *);
void bwHandleSyncBigStack(const uint8_t *body, size_t bodyLen);

// ---- bitwarden_relay.cpp ----
void bwDrawHeader();
void bwDrawStatus(const char *state);
void bwRevertToIdleBaud();

}  // namespace BitWardenRelay
