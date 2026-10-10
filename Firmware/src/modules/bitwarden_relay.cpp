// BitWarden Relay mode - see include/bitwarden_relay.h for the architecture
// summary and apps/bitwarden/PROTOCOL.md (hackmaster_y2035 repo) for the
// authoritative wire protocol this file implements one half of.
//
// Network/crypto pipeline: TOFU cert discovery, OAuth login, /api/sync
// fetch+decrypt, streamed back over PhysicalSerial in cleartext. KEX
// (session-key wire encryption) is stubbed (see BW_KEX below) - HELLO_ACK
// advertises capFlags bit0=0 so a well-behaved Palm client won't attempt
// it against this build.
//
// WiFiClientSecure::verify() must be called after connect() (it checks the
// peer cert of an already-open connection, it does not configure a policy
// for future connects). HTTPClient::connect() reuses an already-connected
// client rather than reconnecting, so one connect()+setInsecure()+verify()
// per SYNC is reused across the prelogin/token/sync requests that follow it.

#include "bitwarden_relay.h"
#include "globals.h"
#include "serial_io.h"
#include "display_menu.h"
#include "network.h"
#include "relay_proto.h"
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

// ============================================================================
// Wire protocol constants (PROTOCOL.md)
// ============================================================================

enum : uint8_t {
  BW_HELLO = 0x01, BW_HELLO_ACK = 0x02,
  BW_TRUST = 0x03, BW_TRUST_RESULT = 0x04,
  BW_SYNC = 0x05, BW_FOLDER = 0x0C, BW_ORG = 0x0D, BW_SYNC_ITEM = 0x06,
  BW_SYNC_DONE = 0x07, BW_SYNC_ERROR = 0x08,
  BW_KEX = 0x09, BW_KEX_ACK = 0x0A,
  BW_BYE = 0x0B,
  BW_ITEM_ACK = 0x0E,   // Palm -> WiRSa, empty body - see bwWriteFrameAcked
  BW_TWO_FACTOR_REQUIRED = 0x0F, // WiRSa -> Palm, see PROTOCOL.md
  BW_TWO_FACTOR_SUBMIT   = 0x10, // Palm -> WiRSa, see PROTOCOL.md
  // WiRSa -> Palm, body: `string statusText`. Purely cosmetic - live status-
  // label update for the one long stretch (prelogin/PBKDF2/login/api-sync-
  // fetch) that used to have zero feedback between SYNC and the first real
  // FOLDER/ORG/SYNC_ITEM/SYNC_ERROR/TWO_FACTOR_REQUIRED frame. See
  // bwSendSyncStatus and PROTOCOL.md.
  BW_SYNC_STATUS = 0x11
};

enum : uint8_t {
  BW_ERR_DNS_FAIL = 1, BW_ERR_CONNECT_FAIL = 2, BW_ERR_TLS_FAIL = 3,
  BW_ERR_FINGERPRINT_MISMATCH = 4, BW_ERR_AUTH_FAILED = 5,
  BW_ERR_TWO_FACTOR_REQUIRED = 6, BW_ERR_SYNC_FETCH_FAILED = 7,
  BW_ERR_DECRYPT_FAILED = 8, BW_ERR_MALFORMED_KDF = 9
};

static const uint8_t  BW_PROTOCOL_VERSION = 1;
static const uint32_t BW_BYTE_TIMEOUT_MS  = 3000;  // gap between bytes of one frame
static const uint32_t BW_IDLE_TIMEOUT_MS  = 60000; // waiting for a new frame to start

// Baud negotiation, mirrors WiRSa's own SASD mode (sasd.cpp's
// SASD_CMD_SET_BAUD) and apps/caldav_sync/caldav_relay.cpp's identical
// scheme. Every fresh connection - both this relay's own PhysicalSerial
// and apps/bitwarden/Serial.h's bwBaudIdle on the Palm side - starts at
// this fixed, known-safe rate. BW_HELLO/BW_HELLO_ACK negotiate up from here.
static const uint32_t BW_BAUD_IDLE = 9600;
// If the Palm negotiates a faster rate but never follows up with BW_SYNC
// within this window, revert to BW_BAUD_IDLE so the next connection
// attempt isn't stuck listening at an unexpected rate.
static const uint32_t BW_NEGOTIATE_TIMEOUT_MS = 15000;
// Set when BW_HELLO successfully negotiates a faster rate; bitwardenRelayLoop's
// own tick checks this against BW_NEGOTIATE_TIMEOUT_MS.
static unsigned long gBwNegotiatedAt = 0;
// Forward decl - real definition lives with the other OLED helpers further
// down, but bwHandleHello (well above that point) needs it for the live
// baud readout right after negotiating.
static void bwDrawHeader();
// Waiting for TWO_FACTOR_SUBMIT specifically needs much longer than the
// normal inter-frame idle timeout above - this one is for a HUMAN to notice
// the prompt, check their phone/email, and type a code via Graffiti/
// keyboard, not for WiRSa's own processing. Matches the Palm's own
// bwIdleTimeoutTicks (Serial.c, 180s) rather than inventing a new budget.
static const uint32_t BW_TWO_FACTOR_TIMEOUT_MS = 180000;
// Each BwStoreAddItem commit on the Palm is a two-pass write (raw fields,
// then a second DmGetRecord+per-field-encrypt+DmWrite pass) into a PDB
// that's been growing the whole sync, and Palm hardware's DmNewRecord
// free-space search can slow down as the file grows - occasionally past a
// too-short ack-wait even while the Palm is still legitimately working.
//
// 10s (not the Palm's own 180s idle budget) - long enough that a normal
// ack (well under a second) is never mistaken for a stall, short enough
// that a genuine one gets noticed and retried (bwHandleSync's own
// reconnect-and-resume loop) instead of the user staring at a frozen
// dialog. This does mean WiRSa can give up on an ack while the Palm is
// still legitimately working on it - see bwWriteFrameAcked's own
// drain-before-send comment for how a stale, late-arriving ack from that
// abandoned attempt is kept from being misread as the ack for a retried send.
static const uint32_t BW_ITEM_ACK_TIMEOUT_MS = 10000;

// Frame body ceiling. Generous for every request field and most vault
// fields; an unusually long "notes" field is truncated (see BwWriter::str)
// rather than corrupting the frame - a documented v1 limitation
// (PROTOCOL.md), not silent data loss on the wire itself.
static const size_t BW_MAX_BODY = 4096;

// Static (not stack) scratch buffers - this relay processes one frame/one
// field at a time, never concurrently, so module-level reuse is safe and
// avoids ever putting a multi-KB buffer on the loop task's stack (this
// project has hit real stack-overflow crashes from exactly that pattern
// elsewhere - see the lazyFTP auto-reconnect crash history).
static uint8_t gFrameBody[BW_MAX_BODY];
static uint8_t gScratchCt[BW_MAX_BODY];
static uint8_t gScratchPt[BW_MAX_BODY];
static uint8_t gScratchMacInput[16 + BW_MAX_BODY];

static bool bwWifiReady = false;

// ============================================================================
// Frame I/O - raw PhysicalSerial, bypassing SerialRead/SerialWrite's
// USB/Physical/console multiplexing on purpose (same reasoning as
// SASD/HotSync: this UART carries a binary protocol while the mode is
// active, and must not be mixed with the local text menu channel).
// ============================================================================

// The framing layer itself (byte/frame read, checksummed write, ack-retry,
// Writer/Reader) lives in relay_proto.cpp, shared with the CalDAV relay.
// These thin wrappers pin this relay's own port, timeouts and frame types
// so every call site below stays exactly as it was.

using BwWriter = RelayProto::Writer;
using BwReader = RelayProto::Reader;

// Ack frames are empty-body; this is only somewhere to read them into. Kept
// separate from gFrameBody so a retry always resends the original frame.
static uint8_t gBwAckScratch[32];

// idleTimeoutMs only affects the wait for this frame's OWN first byte -
// defaults to the normal inter-frame timeout; bwHandleSync's 2FA wait passes
// BW_TWO_FACTOR_TIMEOUT_MS since that wait is for a human.
static bool bwReadFrame(uint8_t &type, uint8_t *body, size_t bodyCap, size_t &bodyLen,
                         uint32_t idleTimeoutMs = BW_IDLE_TIMEOUT_MS) {
  return RelayProto::readFrame(PhysicalSerial, type, body, bodyCap, bodyLen,
                               idleTimeoutMs, BW_BYTE_TIMEOUT_MS);
}

static void bwWriteFrame(uint8_t type, const uint8_t *body, size_t bodyLen) {
  RelayProto::writeFrame(PhysicalSerial, type, body, bodyLen);
}

// FOLDER/ORG/SYNC_ITEM are flow-controlled: send, then block for the Palm's
// BW_ITEM_ACK before the next frame. 5 attempts / 500ms mirror the Palm's
// own SendItemAckWithRetry (Sync.c) so neither side's patience runs out
// first. palmSaidBye is set when the Palm sent BW_BYE instead of an ack.
static bool bwWriteFrameAcked(uint8_t type, const uint8_t *body, size_t bodyLen, bool *palmSaidBye = nullptr) {
  return RelayProto::writeFrameAcked(PhysicalSerial, type, body, bodyLen,
                                     BW_ITEM_ACK, BW_BYE,
                                     gBwAckScratch, sizeof(gBwAckScratch),
                                     5, 500, BW_ITEM_ACK_TIMEOUT_MS, BW_BYTE_TIMEOUT_MS,
                                     palmSaidBye);
}

// ============================================================================
// Crypto primitives (mbedTLS, bundled with the ESP32 Arduino core - no new
// vendored crypto code, matching this codebase's own convention of adopting
// small proven pieces rather than writing large systems from scratch).
// ============================================================================

static void bwHmacSha256(const uint8_t *key, size_t keyLen, const uint8_t *msg, size_t msgLen, uint8_t out[32]) {
  const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  mbedtls_md_context_t ctx;
  mbedtls_md_init(&ctx);
  mbedtls_md_setup(&ctx, info, 1 /* hmac */);
  mbedtls_md_hmac_starts(&ctx, key, keyLen);
  mbedtls_md_hmac_update(&ctx, msg, msgLen);
  mbedtls_md_hmac_finish(&ctx, out);
  mbedtls_md_free(&ctx);
}

// PBKDF2-HMAC-SHA256, hand-rolled on top of mbedtls_md's HMAC context
// directly (rather than mbedtls_pkcs5_pbkdf2_hmac[_ext]) so it compiles
// unchanged regardless of which mbedTLS 2.x/3.x API a given arduino-esp32
// core version exposes - this sandbox can't compile-test against real
// hardware, so the more portable primitive is preferred over a possibly
// faster but version-fragile library call.
static void bwPbkdf2Sha256(const uint8_t *pw, size_t pwLen, const uint8_t *salt, size_t saltLen,
                            uint32_t iterations, uint8_t out[32]) {
  const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  mbedtls_md_context_t ctx;
  mbedtls_md_init(&ctx);
  mbedtls_md_setup(&ctx, info, 1);

  // Single 32-byte output (dkLen == hLen): RFC 8018 block index 1 only.
  uint8_t saltBlock[256];
  size_t sLen = saltLen < (sizeof(saltBlock) - 4) ? saltLen : (sizeof(saltBlock) - 4);
  memcpy(saltBlock, salt, sLen);
  saltBlock[sLen+0] = 0; saltBlock[sLen+1] = 0; saltBlock[sLen+2] = 0; saltBlock[sLen+3] = 1;

  uint8_t u[32], t[32];
  mbedtls_md_hmac_starts(&ctx, pw, pwLen);
  mbedtls_md_hmac_update(&ctx, saltBlock, sLen + 4);
  mbedtls_md_hmac_finish(&ctx, u);
  memcpy(t, u, 32);
  for (uint32_t i = 1; i < iterations; i++) {
    mbedtls_md_hmac_starts(&ctx, pw, pwLen);
    mbedtls_md_hmac_update(&ctx, u, 32);
    mbedtls_md_hmac_finish(&ctx, u);
    for (int j = 0; j < 32; j++) t[j] ^= u[j];
  }
  memcpy(out, t, 32);
  mbedtls_md_free(&ctx);
}

// RFC 5869 HKDF-Expand only (PRK supplied directly, no Extract step) -
// Bitwarden's own construction: PRK = masterKey (32 bytes), info = "enc" or
// "mac" (ASCII), single 32-byte output block T(1).
static void bwHkdfExpandSha256(const uint8_t prk[32], const char *info, uint8_t out[32]) {
  size_t infoLen = strlen(info);
  uint8_t buf[64];
  memcpy(buf, info, infoLen);
  buf[infoLen] = 0x01;
  bwHmacSha256(prk, 32, buf, infoLen + 1, out);
}

static bool bwBase64Decode(const String &in, uint8_t *out, size_t outCap, size_t &outLen) {
  size_t olen = 0;
  int rc = mbedtls_base64_decode(out, outCap, &olen, (const unsigned char *) in.c_str(), in.length());
  if (rc != 0) return false;
  outLen = olen;
  return true;
}

static String bwBase64Encode(const uint8_t *in, size_t inLen) {
  size_t needed = 0;
  mbedtls_base64_encode(nullptr, 0, &needed, in, inLen);
  uint8_t *buf = (uint8_t *) malloc(needed + 1);
  if (!buf) return String();
  size_t actual = 0;
  mbedtls_base64_encode(buf, needed, &actual, in, inLen);
  buf[actual] = 0;
  String s = String((char *) buf);
  free(buf);
  return s;
}

static bool bwAesCbcDecrypt(const uint8_t key32[32], const uint8_t iv16[16],
                             const uint8_t *ct, size_t ctLen, uint8_t *ptOut, size_t &ptLen) {
  if (ctLen == 0 || (ctLen % 16) != 0) return false;
  mbedtls_aes_context aes;
  mbedtls_aes_init(&aes);
  if (mbedtls_aes_setkey_dec(&aes, key32, 256) != 0) { mbedtls_aes_free(&aes); return false; }
  uint8_t ivCopy[16];
  memcpy(ivCopy, iv16, 16); // mbedtls_aes_crypt_cbc mutates its iv argument
  int rc = mbedtls_aes_crypt_cbc(&aes, MBEDTLS_AES_DECRYPT, ctLen, ivCopy, ct, ptOut);
  mbedtls_aes_free(&aes);
  if (rc != 0) return false;
  uint8_t pad = ptOut[ctLen - 1];
  if (pad == 0 || pad > 16 || pad > ctLen) return false;
  for (size_t i = 0; i < pad; i++) if (ptOut[ctLen - 1 - i] != pad) return false;
  ptLen = ctLen - pad;
  return true;
}

// Bitwarden EncString format: "{encType}.{b64 iv}|{b64 ct}|{b64 mac}".
// encType 2 = AES-256-CBC + HMAC-SHA256, the only type a personal vault
// uses and the only one this relay decrypts - anything else is treated as
// a decrypt failure rather than guessed at.
static bool bwDecryptEncString(const String &encString, const uint8_t encKey[32], const uint8_t macKey[32], String &plainOut) {
  int dot = encString.indexOf('.');
  if (dot < 0) return false;
  if (encString.substring(0, dot) != "2") return false;
  String rest = encString.substring(dot + 1);
  int p1 = rest.indexOf('|');
  int p2 = (p1 < 0) ? -1 : rest.indexOf('|', p1 + 1);
  if (p1 < 0 || p2 < 0) return false;

  String ivB64 = rest.substring(0, p1);
  String ctB64 = rest.substring(p1 + 1, p2);
  String macB64 = rest.substring(p2 + 1);

  uint8_t iv[16]; size_t ivLen;
  if (!bwBase64Decode(ivB64, iv, sizeof(iv), ivLen) || ivLen != 16) return false;

  size_t ctLen;
  if (!bwBase64Decode(ctB64, gScratchCt, sizeof(gScratchCt), ctLen)) return false;

  uint8_t mac[32]; size_t macLen;
  if (!bwBase64Decode(macB64, mac, sizeof(mac), macLen) || macLen != 32) return false;

  memcpy(gScratchMacInput, iv, 16);
  memcpy(gScratchMacInput + 16, gScratchCt, ctLen);
  uint8_t macCalc[32];
  bwHmacSha256(macKey, 32, gScratchMacInput, 16 + ctLen, macCalc);
  uint8_t diff = 0;
  for (int i = 0; i < 32; i++) diff |= (macCalc[i] ^ mac[i]);
  if (diff != 0) return false;

  size_t ptLen;
  if (!bwAesCbcDecrypt(encKey, iv, gScratchCt, ctLen, gScratchPt, ptLen)) return false;

  String s; s.reserve(ptLen);
  for (size_t i = 0; i < ptLen; i++) s += (char) gScratchPt[i];
  plainOut = s;
  return true;
}

// ---- organization key unwrapping (RSA-OAEP) --------------------------
// Organization-owned items are encrypted under the organization's own
// symmetric key, wrapped for this specific user via RSA - not the plain
// AES-CBC+HMAC EncString scheme every personal item uses, so a personal-
// key decrypt attempt on an org item fails HMAC/produces garbage.
//
// The chain: profile.privateKey is the user's own RSA private key, itself
// wrapped the normal way (type "2." EncString, symmetric, under the user's
// own encKey/macKey - bwDecryptEncString handles it directly). Each
// profile.organizations[].key is that ORG's own 64-byte symmetric key
// (encKey+macKey, same shape as every other key in this scheme),
// RSA-OAEP-wrapped under the user's RSA PUBLIC key - type "3." (OAEP-
// SHA256) or "4." (OAEP-SHA1), Bitwarden's own two RSA EncString variants.
// Unlike the "2." format there's no separate iv/mac field - OAEP is a
// single ciphertext blob and is already integrity-checked by its own
// padding scheme.
static bool bwRsaOaepUnwrap(mbedtls_pk_context &pk, const String &wrapped, uint8_t out[64]) {
  int dot = wrapped.indexOf('.');
  if (dot < 0) return false;
  String typeStr = wrapped.substring(0, dot);
  mbedtls_md_type_t md;
  if (typeStr == "3") md = MBEDTLS_MD_SHA256;
  else if (typeStr == "4") md = MBEDTLS_MD_SHA1;
  else return false; // types 5/6 (OAEP + extra HMAC layer) - not issued for org keys, not supported

  uint8_t ct[256]; size_t ctLen;
  if (!bwBase64Decode(wrapped.substring(dot + 1), ct, sizeof(ct), ctLen)) return false;

  mbedtls_rsa_context *rsa = mbedtls_pk_rsa(pk);
  if (!rsa) return false;
  mbedtls_rsa_set_padding(rsa, MBEDTLS_RSA_PKCS_V21, md);

  mbedtls_entropy_context entropy;
  mbedtls_ctr_drbg_context ctr_drbg;
  mbedtls_entropy_init(&entropy);
  mbedtls_ctr_drbg_init(&ctr_drbg);
  static const char *pers = "bw_org_unwrap";
  bool ok = false;
  if (mbedtls_ctr_drbg_seed(&ctr_drbg, mbedtls_entropy_func, &entropy,
                             (const unsigned char *) pers, strlen(pers)) == 0) {
    size_t olen = 0;
    int ret = mbedtls_pk_decrypt(&pk, ct, ctLen, out, &olen, 64,
                                  mbedtls_ctr_drbg_random, &ctr_drbg);
    ok = (ret == 0 && olen == 64);
  }
  mbedtls_ctr_drbg_free(&ctr_drbg);
  mbedtls_entropy_free(&entropy);
  return ok;
}

// Parses profile.privateKey (already unwrapped to raw PKCS8 DER bytes via
// bwDecryptEncString) into a reusable mbedtls_pk_context for unwrapping
// every org's own key in one sync pass. Caller must mbedtls_pk_free(pk)
// when done (see bwStreamSyncBody).
static bool bwParsePrivateKey(const uint8_t *der, size_t derLen, mbedtls_pk_context &pk) {
  /* This mbedtls version's mbedtls_pk_parse_key takes no rng args (older,
     pre-blinding-required signature). No unencrypted-DER key needs a
     password either. */
  return mbedtls_pk_parse_key(&pk, der, derLen, NULL, 0) == 0;
}

static String urlEncode(const String &s) {
  String out; out.reserve(s.length() * 3);
  const char *hex = "0123456789ABCDEF";
  for (size_t i = 0; i < s.length(); i++) {
    uint8_t c = (uint8_t) s[i];
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') out += (char) c;
    else { out += '%'; out += hex[c >> 4]; out += hex[c & 0xF]; }
  }
  return out;
}

// ============================================================================
// Master-password derivation (Bitwarden's own construction)
// ============================================================================

struct BwCryptoContext {
  uint8_t masterKey[32];
  uint8_t stretchedEncKey[32];
  uint8_t stretchedMacKey[32];
  uint8_t userEncKey[32];
  uint8_t userMacKey[32];
};

static void bwDeriveMasterKey(const String &password, const String &email, uint32_t iterations, uint8_t out[32]) {
  String emailLower = email; emailLower.toLowerCase();
  bwPbkdf2Sha256((const uint8_t *) password.c_str(), password.length(),
                 (const uint8_t *) emailLower.c_str(), emailLower.length(), iterations, out);
}

static String bwDeriveMasterPasswordHash(const uint8_t masterKey[32], const String &password) {
  uint8_t hash[32];
  bwPbkdf2Sha256(masterKey, 32, (const uint8_t *) password.c_str(), password.length(), 1, hash);
  return bwBase64Encode(hash, 32);
}

static void bwDeriveStretchedKeys(const uint8_t masterKey[32], uint8_t encKeyOut[32], uint8_t macKeyOut[32]) {
  bwHkdfExpandSha256(masterKey, "enc", encKeyOut);
  bwHkdfExpandSha256(masterKey, "mac", macKeyOut);
}

// ============================================================================
// Vaultwarden HTTP calls. All three share one already-connected,
// already-pin-verified WiFiClientSecure (see bwHandleSync) - HTTPClient's
// own connect() reuses a client that's already connected() rather than
// reconnecting, so this is one TLS session for the whole SYNC, not three.
// ============================================================================

struct BwKdfParams { uint8_t type = 1; uint32_t iterations = 600000; uint32_t memoryKiB = 0; uint8_t parallelism = 0; };

static bool bwPrelogin(WiFiClientSecure &tls, const String &host, const String &email, BwKdfParams &kdf, String &errOut) {
  HTTPClient http;
  if (!http.begin(tls, "https://" + host + "/identity/accounts/prelogin")) { errOut = "prelogin begin() failed"; return false; }
  http.addHeader("Content-Type", "application/json");
  JsonDocument reqDoc;
  reqDoc["email"] = email;
  String body; serializeJson(reqDoc, body);
  int code = http.POST(body);
  if (code < 0) { errOut = "prelogin connect failed (" + String(code) + ")"; http.end(); return false; }
  if (code != 200) { errOut = "prelogin HTTP " + String(code); http.end(); return false; }
  // tls directly, not http.getStream() - see bwSyncAndStream's own note on
  // HTTPClient::getStream() silently substituting an empty placeholder
  // Stream when its own connected() bookkeeping disagrees with reality.
  // This response is always tiny (just KDF params), so it was never at
  // risk of the /api/sync-sized NoMemory bug, but it shares the exact same
  // getStream() fragility - fixed here too for consistency, not because it
  // was ever observed failing.
  JsonDocument doc;
  DeserializationError je = deserializeJson(doc, tls);
  http.end();
  if (je) { errOut = "prelogin JSON parse failed: " + String(je.c_str()); return false; }
  kdf.type = (uint8_t) (doc["kdf"] | 0);
  kdf.iterations = doc["kdfIterations"] | 600000;
  kdf.memoryKiB = doc["kdfMemory"] | 0;
  kdf.parallelism = doc["kdfParallelism"] | 0;
  return true;
}

// Examines a 400 response body that has TwoFactorProviders/TwoFactorProviders2
// (account has 2FA enabled, or the just-submitted code was wrong - Vaultwarden
// uses the same response shape for both, see PROTOCOL.md's own note on this)
// and picks ONE provider to ask the Palm for (prefers Authenticator app=0,
// then Email=1, then whatever the server listed first) plus a short human
// label. v1 has no "pick your 2FA method" UI, so this always auto-picks
// rather than surfacing the full provider list to the Palm.
static void bwPickTwoFactorProvider(JsonDocument &doc, uint8_t &outProvider, String &outLabel) {
  int provider = -1;
  JsonVariant providers2 = doc["TwoFactorProviders2"];
  if (!providers2.isNull()) {
    JsonObject obj = providers2.as<JsonObject>();
    if (obj["0"].is<JsonVariant>()) provider = 0;
    else if (obj["1"].is<JsonVariant>()) provider = 1;
    else for (JsonPair kv : obj) { provider = atoi(kv.key().c_str()); break; }
    if (provider == 1) {
      String emailHint = obj["1"]["Email"] | "";
      outLabel = emailHint.length() ? ("Email code sent to " + emailHint) : "Email code";
    } else if (provider == 0) {
      outLabel = "Authenticator app code";
    } else if (provider >= 0) {
      outLabel = "Two-factor code (provider " + String(provider) + ")";
    }
  } else {
    JsonArray arr = doc["TwoFactorProviders"].as<JsonArray>();
    for (JsonVariant v : arr) { int p = v.as<int>(); if (provider < 0 || p == 0) provider = p; if (p == 0) break; }
    outLabel = (provider == 0) ? "Authenticator app code"
             : (provider == 1) ? "Email code"
             : "Two-factor code (provider " + String(provider < 0 ? 0 : provider) + ")";
  }
  outProvider = (uint8_t) (provider < 0 ? 0 : provider); // defensive - shouldn't happen, the caller already checked TwoFactorProviders(2) is present
}

// twoFactorToken/twoFactorProvider are empty/0 on a genuinely fresh attempt
// (account has never completed 2FA from this Palm, or the remembered token
// was rejected); the caller (bwHandleSync) seeds them from the Palm's own
// stored "remember this device" token (provider 5) before ever trying an
// interactive prompt, and refills them from a fresh TWO_FACTOR_SUBMIT on
// retry. outTwoFactorProvider/outTwoFactorLabel are only meaningful when
// this returns false with errCode == BW_ERR_TWO_FACTOR_REQUIRED.
// outNewTwoFactorToken is set (only) when a SUCCESSFUL response includes a
// TwoFactorToken - the caller reports this back to the Palm to persist for
// next time (SYNC_DONE's own updatedTwoFactorToken - see PROTOCOL.md).
// twoFactorRemember=1 is now always sent (not just on an interactive
// code) so a login using a still-valid remembered token also gets it
// renewed, matching every real Bitwarden client's own behavior.
static bool bwLogin(WiFiClientSecure &tls, const String &host, const String &email, const String &masterPasswordHashB64,
                     const String &twoFactorToken, uint8_t twoFactorProvider,
                     String &accessToken, String &keyEncString, String &outNewTwoFactorToken,
                     uint8_t &outTwoFactorProvider, String &outTwoFactorLabel,
                     String &errOut, uint8_t &errCode) {
  HTTPClient http;
  if (!http.begin(tls, "https://" + host + "/identity/connect/token")) {
    errCode = BW_ERR_CONNECT_FAIL; errOut = "token begin() failed"; return false;
  }
  http.addHeader("Content-Type", "application/x-www-form-urlencoded");
  http.addHeader("Device-Type", "0");
  String form = "grant_type=password&username=" + urlEncode(email) +
                "&password=" + urlEncode(masterPasswordHashB64) +
                "&scope=api+offline_access&client_id=connector" +
                "&deviceType=21&deviceName=WiRSa&deviceIdentifier=wirsa-bitwarden-relay";
  if (twoFactorToken.length()) {
    // Vaultwarden's two_factor_remember field is Option<i32>, not a bool -
    // the literal string "1" is required; "true" silently fails to parse
    // and is treated as absent, so the remember-token is never issued.
    // Field names are fine as camelCase - Vaultwarden matches them
    // case-insensitively against its own snake_case names.
    form += "&twoFactorToken=" + urlEncode(twoFactorToken) +
            "&twoFactorProvider=" + String(twoFactorProvider) +
            "&twoFactorRemember=1";
  }
  int code = http.POST(form);
  if (code < 0) { errCode = BW_ERR_CONNECT_FAIL; errOut = "token connect failed (" + String(code) + ")"; http.end(); return false; }

  if (code == 400) {
    // tls directly, not http.getStream() - see bwSyncAndStream's own note.
    JsonDocument doc;
    deserializeJson(doc, tls);
    http.end();
    if (!doc["TwoFactorProviders"].isNull() || !doc["TwoFactorProviders2"].isNull()) {
      errCode = BW_ERR_TWO_FACTOR_REQUIRED;
      errOut = twoFactorToken.length() ? "incorrect two-factor code" : "two-factor authentication required";
      bwPickTwoFactorProvider(doc, outTwoFactorProvider, outTwoFactorLabel);
    } else {
      errCode = BW_ERR_AUTH_FAILED;
      errOut = "invalid email or master password";
    }
    return false;
  }
  if (code != 200) { errCode = BW_ERR_AUTH_FAILED; errOut = "token HTTP " + String(code); http.end(); return false; }

  // tls directly, not http.getStream() - see bwSyncAndStream's own note.
  JsonDocument doc;
  DeserializationError je = deserializeJson(doc, tls);
  http.end();
  if (je) { errCode = BW_ERR_AUTH_FAILED; errOut = "token JSON parse failed: " + String(je.c_str()); return false; }

  accessToken = doc["access_token"] | "";
  keyEncString = doc["Key"] | "";
  outNewTwoFactorToken = doc["TwoFactorToken"] | ""; // present only when twoFactorRemember=1 was honored
  // Key names only (never values - access_token/Key/refresh_token are live
  // session secrets) - lets a missing expected field be diagnosed from the
  // debug log without exposing session data.
  if (usbDebug && twoFactorToken.length()) {
    Serial.print("BW: token response keys:");
    for (JsonPair kv : doc.as<JsonObject>()) Serial.printf(" %s", kv.key().c_str());
    Serial.printf("\r\n");
  }
  if (accessToken.length() == 0 || keyEncString.length() == 0) {
    errCode = BW_ERR_AUTH_FAILED; errOut = "token response missing access_token/Key";
    return false;
  }
  return true;
}

// A large vault's /api/sync response (multiple MB) can exceed this
// device's usable heap. ArduinoJson's Filter feature only bounds which
// fields get kept per array element - it still has to hold the whole
// filtered ciphers array in one JsonDocument simultaneously, so an account
// with enough items still runs out of memory regardless of filtering.
//
// Peak memory instead needs to be bounded by one cipher at a time,
// matching the same "decrypt one item, send it, discard it, move on"
// discipline this file already uses for the WiRSa->Palm direction (see
// bwWriteFrameAcked). ArduinoJson has no built-in SAX/streaming-array API,
// so this is a small hand-written JSON scanner: BwJsonScanner reads bytes
// off the live stream (`tls` directly - see bwSyncAndStream's own note on
// why not http.getStream()); bwScanJsonValue finds the start/end of
// exactly one JSON value by tracking brace/bracket depth and string
// boundaries (including escaped quotes) without needing to understand or
// unescape what's inside a string. Each captured value's raw text is then
// handed to ArduinoJson's own deserializeJson() for a small, bounded,
// single-item parse, reusing its string unescaping and field-extraction
// logic, rescoped to run once per cipher instead of once for the whole array.
//
// Reading one byte at a time off a multi-MB stream multiplies per-call
// mbedTLS/lwIP overhead enough to plausibly trip a per-read idle timeout
// on a slow patch. Buffering internally cuts the number of real stream
// reads by up to 512x - refills happen via one readBytes() call for
// however much is actually available (not a fixed wait), so this doesn't
// add latency, only removes the per-byte overhead multiplication.
struct BwJsonScanner {
  Stream &s;
  int pushed;
  static const size_t kBufSize = 512;
  uint8_t buf[kBufSize];
  size_t bufPos, bufLen;
  explicit BwJsonScanner(Stream &stream) : s(stream), pushed(-2), bufPos(0), bufLen(0) {}

  // A single readBytes() call already waits up to the Stream's own
  // setTimeout() (45s, see bwSyncAndStream) for any data before returning
  // 0, so one empty refill can mean a genuine long stall on an otherwise-
  // live connection (brief WiFi congestion, a slow server-side moment
  // assembling a large payload) rather than a dead one. Retry the refill a
  // few times before giving up: a truly dead connection returns 0
  // immediately on each attempt (socket/TLS closure is detected right
  // away, it doesn't sit out the full timeout again), so this costs almost
  // nothing extra in that case while extending resilience against a
  // slow-but-alive one.
  static const int kMaxRefillRetries = 3;

  int next() {
    if (pushed != -2) { int c = pushed; pushed = -2; return c; }
    if (bufPos >= bufLen) {
      for (int attempt = 0; attempt < kMaxRefillRetries; attempt++) {
        bufLen = s.readBytes(buf, kBufSize); // respects Stream::setTimeout(); returns whatever's actually available, up to kBufSize
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

// Captures the exact source text of one JSON value (object/array/string/
// number/true/false/null) starting at the next non-whitespace byte, into
// `out`, verbatim/unescaped. maxLen is a defensive ceiling - a single item
// legitimately exceeding it is worth failing loudly rather than silently
// truncating (which would corrupt whatever gets parsed from `out` next).
static bool bwScanJsonValue(BwJsonScanner &sc, String &out, size_t maxLen) {
  out = String();
  int c = sc.nextNonWs();
  if (c < 0) {
    if (usbDebug) Serial.println("BW: bwScanJsonValue: stream ended before any value started");
    return false;
  }

  if (c == '"') {
    out += (char) c;
    bool escape = false;
    for (;;) {
      int d = sc.next();
      if (d < 0) {
        if (usbDebug) Serial.printf("BW: bwScanJsonValue: stream ended mid-string (%u bytes captured)\r\n", (unsigned) out.length());
        return false;
      }
      out += (char) d;
      if (out.length() > maxLen) {
        if (usbDebug) Serial.printf("BW: bwScanJsonValue: string exceeded %u-byte cap\r\n", (unsigned) maxLen);
        return false;
      }
      if (escape) { escape = false; continue; }
      if (d == '\\') { escape = true; continue; }
      if (d == '"') return true;
    }
  }

  if (c == '{' || c == '[') {
    char open = (char) c;
    char close = (open == '{') ? '}' : ']';
    // Growing this byte-by-byte from empty means a full-size cipher object
    // (routinely 1-2KB) triggers many incremental String reallocations -
    // real cost on a heap that's already fragmented from hundreds of prior
    // per-item allocations deep into a long sync. Reserving a reasonable
    // starting size up front doesn't fix a genuine low-memory condition,
    // but cuts realloc COUNT (and therefore fragmentation churn) for the
    // common case; String::reserve() is a plain capacity hint with no
    // failure mode worse than "did nothing" if it can't get the space.
    out.reserve(512);
    out += open;
    int depth = 1;
    bool inString = false, escape = false;
    for (;;) {
      int d = sc.next();
      if (d < 0) {
        if (usbDebug) Serial.printf("BW: bwScanJsonValue: stream ended mid-value (%u bytes captured, depth=%d)\r\n", (unsigned) out.length(), depth);
        return false;
      }
      out += (char) d;
      if (out.length() > maxLen) {
        if (usbDebug) Serial.printf("BW: bwScanJsonValue: value exceeded %u-byte cap (depth=%d) - raising kMaxSyncItemJson may be needed\r\n", (unsigned) maxLen, depth);
        return false;
      }
      // A single value can now be up to kMaxSyncItemJson (256KB) - give the
      // WiFi/lwIP tasks a scheduling slice periodically rather than only
      // once per completed item, same reasoning as the per-item yield()
      // calls elsewhere in this file.
      if ((out.length() & 0x3FF) == 0) yield();
      if (inString) {
        if (escape) { escape = false; continue; }
        if (d == '\\') { escape = true; continue; }
        if (d == '"') inString = false;
        continue;
      }
      if (d == '"') { inString = true; continue; }
      // Only the outer bracket TYPE is counted; a differently-typed bracket
      // nested inside (e.g. a `[` inside this `{...}`) is always internally
      // balanced by the time we'd see its own close, so ignoring it here
      // nets to zero effect on finding where THIS value ends.
      if (d == open) depth++;
      else if (d == close) { depth--; if (depth == 0) return true; }
    }
  }

  // Bare literal (number, true, false, null) - read until a delimiter,
  // pushing the delimiter back since the caller (expecting ',' / '}' / ']'
  // next) still needs to see it.
  out += (char) c;
  for (;;) {
    int d = sc.next();
    if (d < 0) return true; // EOF right after a bare literal is fine
    if (d == ',' || d == '}' || d == ']' || d == ' ' || d == '\t' || d == '\r' || d == '\n') {
      sc.pushBack(d);
      return true;
    }
    out += (char) d;
    if (out.length() > maxLen) {
      if (usbDebug) Serial.printf("BW: bwScanJsonValue: bare literal exceeded %u-byte cap\r\n", (unsigned) maxLen);
      return false;
    }
  }
}

// A single item's raw JSON (attachments/fields[]/passwordHistory[]/a long
// notes field, all captured verbatim before re-parsing) can be large.
// Since peak memory is bounded to one item at a time (see
// bwStreamSyncBody), there's no reason to keep this cap tight; 256KB is a
// hard ceiling against a pathological/corrupt response, not a realistic
// per-item size otherwise.
static const size_t kMaxSyncItemJson = 262144;

// Iterates a JSON array sitting at the scanner's current position (right
// after the array's own ':'), calling fn(rawElementText) once per element.
// Returns false if fn returns false (caller-detected failure, e.g. a lost
// Palm ack) or the array is malformed/truncated.
template <typename Fn>
static bool bwStreamArray(BwJsonScanner &sc, Fn fn) {
  if (sc.nextNonWs() != '[') return false;
  int peek = sc.nextNonWs();
  if (peek < 0) return false;
  if (peek == ']') return true; // empty array
  sc.pushBack(peek);
  for (;;) {
    String itemJson;
    if (!bwScanJsonValue(sc, itemJson, kMaxSyncItemJson)) return false;
    if (!fn(itemJson)) return false;
    int sep = sc.nextNonWs();
    if (sep == ']') return true;
    if (sep != ',') return false;
  }
}

// An org-owned item carries substantially more metadata than a personal
// one (collections, attachments, custom fields[], passwordHistory[],
// permissions, etc.) - deserializeJson() can run out of heap building a
// full unfiltered DOM for one such item. This filter bounds each item's
// own peak memory to the ~30 fields actually used, no matter how much
// surrounding data a large outlier item carries.
static void bwBuildCipherFilter(JsonDocument &filter) {
  filter["deletedDate"] = true;
  filter["archivedDate"] = true;
  filter["type"] = true;
  filter["id"] = true;
  filter["folderId"] = true;
  filter["organizationId"] = true;
  filter["key"] = true;
  filter["name"] = true;
  // "notes" is deliberately not in this filter, unlike every other kept
  // field here - see bwFindCappedStringField: it's the one field with no
  // natural size bound, large enough on its own in an outlier item to
  // blow the memory budget even with every other field filtered out.
  // Extracted separately, capped, after this (now smaller) parse succeeds.
  filter["login"]["username"] = true;
  filter["login"]["password"] = true;
  filter["login"]["totp"] = true;
  filter["login"]["uris"][0]["uri"] = true;
  JsonObject card = filter["card"].to<JsonObject>();
  card["cardholderName"] = true; card["brand"] = true; card["number"] = true;
  card["expMonth"] = true; card["expYear"] = true; card["code"] = true;
  JsonObject identity = filter["identity"].to<JsonObject>();
  identity["title"] = true; identity["firstName"] = true; identity["middleName"] = true;
  identity["lastName"] = true; identity["username"] = true; identity["company"] = true;
  identity["ssn"] = true; identity["passportNumber"] = true; identity["licenseNumber"] = true;
  identity["email"] = true; identity["phone"] = true; identity["address1"] = true;
  identity["address2"] = true; identity["address3"] = true; identity["city"] = true;
  identity["state"] = true; identity["postalCode"] = true; identity["country"] = true;
}

// "notes" needs a different approach than the rest of the filter: Filter
// can only include or exclude a whole value, not cap its length, and an
// outlier item's notes value alone can be large enough to blow the memory
// budget even with every other field filtered out. This finds
// "fieldName":"..." via plain substring search (safe here specifically
// because it's only ever used for a known top-level, non-nested field name
// like "notes" - not a general JSON path search) and captures the raw
// (still JSON-escaped) value, bounded at maxLen bytes. Returns false (and
// leaves out unset) if the field is absent OR its raw value exceeds
// maxLen - the caller treats an oversized notes field as "skip it for this
// one item" rather than risking another NoMemory decrypting/holding a
// truly enormous value that violates the Palm's own bwMaxField(512) cap by
// orders of magnitude anyway.
static bool bwFindCappedStringField(const String &json, const char *fieldName, String &out, size_t maxLen) {
  String marker = String("\"") + fieldName + "\":\"";
  int pos = json.indexOf(marker);
  if (pos < 0) return false;
  pos += marker.length();
  String raw;
  bool escape = false;
  int i;
  for (i = pos; i < (int) json.length(); i++) {
    char c = json[i];
    if (raw.length() > maxLen) return false; // too large - caller skips this field
    if (escape) { raw += c; escape = false; continue; }
    if (c == '\\') { raw += c; escape = true; continue; }
    if (c == '"') break; // real closing quote
    raw += c;
  }
  if (i >= (int) json.length()) return false; // ran off the end - malformed/truncated
  // Wrap as a standalone JSON string literal and let ArduinoJson do the
  // actual unescaping - correctness over reimplementing JSON string
  // unescaping by hand for what should be a rare, small parse.
  JsonDocument tmp;
  String literal = "\"" + raw + "\"";
  if (deserializeJson(tmp, literal)) return false;
  out = tmp.as<String>();
  return true;
}

// One organization's own symmetric key, unwrapped via RSA from
// profile.organizations[].key (see bwRsaOaepUnwrap's own comment) -
// `valid` false means unwrapping failed (missing/malformed key, private
// key parse failure, etc.), in which case org-owned items for this org
// fall back to the user's personal key (matches this relay's prior,
// admittedly-wrong behavior - a decrypt failure the Palm already handles
// as unreadable/blank, rather than a hard sync error).
struct BwOrgKey { uint8_t encKey[32]; uint8_t macKey[32]; bool valid; };

// Processes one cipher against a small per-item JsonDocument (parsed from
// one array element's raw text, filtered - see bwBuildCipherFilter)
// instead of iterating a JsonArray inside one giant whole-response
// JsonDocument. `filter` is built once by the caller (bwStreamSyncBody)
// and reused across every item.
static bool bwProcessOneCipher(const String &itemJson, JsonDocument &filter,
                                const uint8_t userEncKey[32], const uint8_t userMacKey[32],
                                const String gFolderIds[], int folderCount,
                                const String gOrgIds[], int orgCount,
                                const BwOrgKey orgKeys[],
                                uint16_t &itemCount, String &errOut, uint8_t &errCode) {
  JsonDocument cdoc;
  DeserializationError je = deserializeJson(cdoc, itemJson, DeserializationOption::Filter(filter));
  if (je) {
    if (usbDebug) {
      // Logging both head and tail of the captured text distinguishes a
      // filtered-parse failure (tail ends in a clean closing '}') from
      // bwScanJsonValue's own brace-depth tracking having stopped at the
      // wrong place (tail doesn't) - both look identical from the head alone.
      size_t n = itemJson.length();
      String head = itemJson.substring(0, n < 100 ? n : 100);
      String tail = itemJson.substring(n < 100 ? 0 : n - 100);
      Serial.printf("BW: malformed cipher JSON - error=%s, captured %u bytes\r\nBW:   head: %s\r\nBW:   tail: %s\r\n",
                     je.c_str(), (unsigned) n, head.c_str(), tail.c_str());
    }
    errCode = BW_ERR_SYNC_FETCH_FAILED; errOut = "malformed cipher JSON"; return false;
  }
  JsonObject c = cdoc.as<JsonObject>();

  if (!c["deletedDate"].isNull()) return true; // skip trashed items - not a failure
  // Bitwarden's native per-item Archive feature (distinct from Trash above) -
  // archivedDate non-null means the item is archived. Unlike a trashed item,
  // an archived one is still forwarded (just flagged), matching the "Archived
  // Items" filter view on the Palm side (MainForm.c's own gArchiveFilter) -
  // the user still owns/can read it, it's just hidden from the normal list.
  uint8_t archived = c["archivedDate"].isNull() ? 0 : 1;

  // itemType is sent to the Palm as its own byte, then the Palm's own
  // switch(itemType) decides how many/which rStr fields to read for the
  // type-specific tail below - but that tail's actual SHAPE is decided
  // here independently, by which of login/card/identity is present in the
  // JSON, not by c["type"]. Trusting c["type"] on its own risked sending a
  // tail shape the itemType byte doesn't match (e.g. c["type"] missing/
  // wrong while c["card"] is genuinely present) - the Palm would then read
  // the wrong number of fields for what's actually on the wire, desyncing
  // every read after it. Deriving itemType from the SAME sub-object
  // presence check that decides the tail shape below guarantees the two
  // can never disagree, regardless of what c["type"] itself says (falls
  // back to it only for note/unknown, which has no distinguishing
  // sub-object of its own).
  uint8_t itemType = (uint8_t) (c["type"] | 1);
  if (!c["identity"].isNull()) itemType = 4;
  else if (!c["card"].isNull()) itemType = 3;
  else if (!c["login"].isNull()) itemType = 1;
  String guid = c["id"] | "";
  // Logged before the send (not just on successful ack) so whichever item
  // the Palm chokes on is identifiable even when no ack ever arrives -
  // itemType and which of login/card/identity is present, since a
  // mismatch between the two would send a tail shape the Palm's
  // itemType-based switch doesn't expect.
  if (usbDebug) {
    Serial.printf("BW: about to send item id=%s itemType=%u has(login=%d,card=%d,identity=%d)\r\n",
                  guid.c_str(), itemType,
                  (int) !c["login"].isNull(), (int) !c["card"].isNull(), (int) !c["identity"].isNull());
  }
  String folderId = c["folderId"].isNull() ? String() : c["folderId"].as<String>();
  uint16_t folderIndex = 0xFFFF;
  if (folderId.length()) {
    for (int i = 0; i < folderCount; i++) if (gFolderIds[i] == folderId) { folderIndex = (uint16_t) i; break; }
  }

  // An org-owned item's own organizationId maps to ORG's own index the same
  // way folderId maps to FOLDER's - an item is either personal (folderIndex
  // meaningful) or org-owned (orgIndex meaningful), never both, matching
  // Bitwarden's own model (org items aren't filed into personal folders).
  String orgId = c["organizationId"].isNull() ? String() : c["organizationId"].as<String>();
  uint16_t orgIndex = 0xFFFF;
  if (orgId.length()) {
    for (int i = 0; i < orgCount; i++) if (gOrgIds[i] == orgId) { orgIndex = (uint16_t) i; break; }
    // Only logs for items that actually claim an organizationId (keeps log
    // volume sane). orgIndex==0xFFFF means this item's organizationId
    // didn't match any of the orgCount IDs collected from
    // profile.organizations - points at how gOrgIds was populated, not the
    // Palm's own filter logic.
    if (usbDebug) {
      Serial.printf("BW: item orgId=%s -> orgIndex=%d (orgCount known=%d)\r\n",
                    orgId.c_str(), (int) orgIndex, orgCount);
    }
  }

  // Base key for this item: the organization's own key if org-owned (and
  // successfully unwrapped via RSA - see bwRsaOaepUnwrap), otherwise the
  // user's personal key - an org item decrypted with the personal key
  // fails its HMAC check and yields nothing.
  const uint8_t *baseEncKey = userEncKey, *baseMacKey = userMacKey;
  if (orgIndex != 0xFFFF && orgKeys[orgIndex].valid) {
    baseEncKey = orgKeys[orgIndex].encKey;
    baseMacKey = orgKeys[orgIndex].macKey;
  }

  // A per-item wrapped key (cipher.key) is rare for a personal vault's own
  // items but real for some org-shared items - unwrap it (same AES-256-CBC+
  // HMAC EncString decrypt as everything else) using the BASE key above
  // (not always the user's key - an org item's own per-item key is itself
  // wrapped under the ORG's key, not the user's personal one) and use the
  // result in place of the base key for just this item. */
  const uint8_t *itemEncKey = baseEncKey, *itemMacKey = baseMacKey;
  uint8_t ownEncKey[32], ownMacKey[32];
  if (!c["key"].isNull()) {
    String wrappedKeyEnc = c["key"].as<String>();
    String rawKeyStr;
    if (bwDecryptEncString(wrappedKeyEnc, baseEncKey, baseMacKey, rawKeyStr) && rawKeyStr.length() >= 64) {
      memcpy(ownEncKey, rawKeyStr.c_str(), 32);
      memcpy(ownMacKey, rawKeyStr.c_str() + 32, 32);
      itemEncKey = ownEncKey; itemMacKey = ownMacKey;
    }
  }

  String name, notes;
  String nameEnc = c["name"] | "";
  if (nameEnc.length()) bwDecryptEncString(nameEnc, itemEncKey, itemMacKey, name);
  {
    // 8192 raw (still-encrypted) bytes is generous headroom over anything
    // the Palm's own bwMaxField(512) wire/storage cap could ever use after
    // decrypt (EncString's AES-CBC+base64 overhead means encrypted length
    // runs a bit larger than plaintext, never anywhere near this) - real
    // legitimate notes fit comfortably under this; a genuine outlier this
    // large gets skipped for just this one item rather than risking
    // another NoMemory failure for the whole sync.
    String notesEnc;
    if (bwFindCappedStringField(itemJson, "notes", notesEnc, 8192)) {
      bwDecryptEncString(notesEnc, itemEncKey, itemMacKey, notes);
    } else if (usbDebug && itemJson.indexOf("\"notes\":\"") >= 0) {
      Serial.println("BW: notes field present but exceeded the 8192-byte raw cap (or malformed) - skipped for this item");
    }
    // The Palm's own BwStoreAddItem silently drops a whole secure-note item
    // if it ends up with an empty name and empty notes (a deliberate
    // optimization for genuinely-blank notes) - this catches that
    // combination for a secure-note item right before it would happen.
    if (usbDebug && itemType == 2 && name.length() == 0 && notes.length() == 0) {
      Serial.printf("BW: secure note id=%s has EMPTY name AND notes after extraction - "
                    "Palm will silently drop this item. raw notes field present in JSON: %s\r\n",
                    guid.c_str(), itemJson.indexOf("\"notes\":\"") >= 0 ? "yes" : "no");
    }
  }

  // Decrypts obj[key] into out if present, else leaves out empty - the
  // repeated shape every one of Card/Identity's real fields below needs
  // (see PROTOCOL.md's SYNC_ITEM section for why these are now dedicated
  // per-field wire slots instead of the old 4-generic-slots reuse hack).
  auto field = [&](JsonObject &obj, const char *key, String &out) {
    if (!obj[key].isNull()) bwDecryptEncString(obj[key].as<String>(), itemEncKey, itemMacKey, out);
  };

  BwWriter w(gFrameBody, sizeof(gFrameBody));
  w.str(guid);
  w.u8(itemType);
  w.u16(folderIndex);
  w.u16(orgIndex);
  w.u8(archived);
  w.str(name);
  w.str(notes);

  if (!c["login"].isNull()) {
    JsonObject login = c["login"];
    String username, password, uri, totp;
    field(login, "username", username);
    field(login, "password", password);
    field(login, "totp", totp);
    if (!login["uris"].isNull()) {
      JsonArray uris = login["uris"].as<JsonArray>();
      for (JsonObject u : uris) {
        if (!u["uri"].isNull()) { bwDecryptEncString(u["uri"].as<String>(), itemEncKey, itemMacKey, uri); break; } // first URI only, v1
      }
    }
    w.str(username); w.str(password); w.str(uri); w.str(totp);
  } else if (!c["card"].isNull()) {
    JsonObject card = c["card"];
    String cardholderName, brand, number, expMonth, expYear, code;
    field(card, "cardholderName", cardholderName);
    field(card, "brand", brand);
    field(card, "number", number);
    field(card, "expMonth", expMonth);
    field(card, "expYear", expYear);
    field(card, "code", code);
    w.str(cardholderName); w.str(brand); w.str(number); w.str(expMonth); w.str(expYear); w.str(code);
  } else if (!c["identity"].isNull()) {
    JsonObject identity = c["identity"];
    String title, firstName, middleName, lastName, username, company, ssn, passportNumber,
           licenseNumber, email, phone, address1, address2, address3, city, state, postalCode, country;
    field(identity, "title", title);
    field(identity, "firstName", firstName);
    field(identity, "middleName", middleName);
    field(identity, "lastName", lastName);
    field(identity, "username", username);
    field(identity, "company", company);
    field(identity, "ssn", ssn);
    field(identity, "passportNumber", passportNumber);
    field(identity, "licenseNumber", licenseNumber);
    field(identity, "email", email);
    field(identity, "phone", phone);
    field(identity, "address1", address1);
    field(identity, "address2", address2);
    field(identity, "address3", address3);
    field(identity, "city", city);
    field(identity, "state", state);
    field(identity, "postalCode", postalCode);
    field(identity, "country", country);
    w.str(title); w.str(firstName); w.str(middleName); w.str(lastName); w.str(username);
    w.str(company); w.str(ssn); w.str(passportNumber); w.str(licenseNumber); w.str(email);
    w.str(phone); w.str(address1); w.str(address2); w.str(address3); w.str(city);
    w.str(state); w.str(postalCode); w.str(country);
  }
  // itemType 2 (secure note): nothing extra - notes above already covers it.

  {
    bool palmSaidBye = false;
    if (!bwWriteFrameAcked(BW_SYNC_ITEM, gFrameBody, w.len, &palmSaidBye)) {
      // Always terminal, not retried via bwHandleSync's outer reconnect+
      // refetch loop: bwWriteFrameAcked already retries this exact frame in
      // place up to 5 times, so reaching here means either the Palm
      // explicitly quit (palmSaidBye) or 5 genuine attempts all failed - in
      // both cases the Palm has almost certainly already closed its serial
      // port, making a further whole-fetch retry pointless.
      errCode = BW_ERR_CONNECT_FAIL;
      errOut = palmSaidBye ? "Palm ended the session" : "Palm lost mid-sync (item ack) after 5 attempts";
      return false;
    }
  }
  itemCount++;
  if (usbDebug) {
    Serial.printf("BW: item #%u '%s' type=%u notes=%u chars sent\r\n",
                   itemCount, name.c_str(), (unsigned) itemType, (unsigned) notes.length());
  }
  yield(); // a big vault can take a while - keep WiFi/watchdog serviced between items
  return true;
}

static const int kMaxSyncFolders = 64;
static const int kMaxSyncOrgs = 16;

// Single forward pass over the top-level /api/sync object. folders and
// profile.organizations must be seen (and their id->index maps built)
// before ciphers, since cipher items reference them by GUID and this
// design can't seek backward on a live stream - real Vaultwarden responses
// have consistently put profile/folders ahead of ciphers (matches the
// official SyncResponseModel's own field order), but this fails loudly
// with a clear error instead of silently emitting wrong folderIndex/
// orgIndex if that ever isn't true for some server build.
// skipFolders/skipOrgs/skipCiphers: how many of each array's elements were
// already successfully forwarded to (and acked by) the Palm in a PRIOR
// attempt at this SAME sync session - see bwHandleSync's own retry-loop
// comment. 0/0/0 on a first attempt. itemCount/folderCount/orgCount are
// OWNED BY THE CALLER now (not reset here) for exactly the same reason:
// they need to keep accumulating across retries, both as the running
// totals reported in the eventual SYNC_DONE and as the stable per-category
// send-index every folder/org already got assigned (a later retry must
// keep assigning the SAME indices to the SAME folders/orgs, since cipher
// items reference them by that index and may have already been sent
// referencing it).
static bool bwStreamSyncBody(Stream &s, const uint8_t userEncKey[32], const uint8_t userMacKey[32],
                              uint16_t &itemCount, uint16_t &folderCount, uint16_t &orgCount,
                              uint16_t &skipFolders, uint16_t &skipOrgs, uint16_t &skipCiphers,
                              String &errOut, uint8_t &errCode, unsigned long tSyncStart) {
  static String gFolderIds[kMaxSyncFolders];
  static String gOrgIds[kMaxSyncOrgs];
  static String gOrgWrappedKeys[kMaxSyncOrgs];	/* profile.organizations[].key, raw RSA-OAEP EncString text */
  static BwOrgKey gOrgKeys[kMaxSyncOrgs];
  String privateKeyEnc;	/* profile.privateKey, raw (still-quoted) JSON text - unwrapped once profile parsing finishes */
  for (int i = 0; i < kMaxSyncOrgs; i++) gOrgKeys[i].valid = false;
  bool sawFolders = false, sawProfile = false;
  BwJsonScanner sc(s);

  // errOut/errCode are the same shared variables bwHandleSync's login retry
  // loop used earlier in this call (bwLogin legitimately sets
  // errCode=BW_ERR_TWO_FACTOR_REQUIRED on the first, expected-to-fail
  // attempt) - force a clean slate on entry so this function's own
  // "if (errCode == 0) fall back to a generic message" checks below don't
  // mistake a stale value for one meaningfully set here.
  errCode = 0;
  errOut = String();

  {
    int first = sc.nextNonWs();
    if (first != '{') {
      // Logs the actual first byte seen and elapsed time, distinguishing a
      // real stall (first == -1: BwJsonScanner's buffered read timed out
      // after headers/Content-Length were already received - the server
      // hasn't started writing the body yet) from genuinely wrong bytes
      // (leading garbage/BOM, an HTML error page despite HTTP 200,
      // undechunked transfer-encoding framing, or an empty/EOF body).
      if (usbDebug) {
        Serial.printf("BW: sync response first byte: %d ('%c') - expected '{' (0x7B) - t+%lums since sync start\r\n",
                      first, (first >= 32 && first < 127) ? (char) first : '?', millis() - tSyncStart);
      }
      errCode = BW_ERR_SYNC_FETCH_FAILED; errOut = "sync response is not a JSON object"; return false;
    }
  }

  for (;;) {
    int d = sc.nextNonWs();
    if (d == '}') break;
    if (d != '"') { errCode = BW_ERR_SYNC_FETCH_FAILED; errOut = "malformed sync JSON (expected key)"; return false; }

    String key;
    for (;;) {
      int e = sc.next();
      if (e < 0) { errCode = BW_ERR_SYNC_FETCH_FAILED; errOut = "sync stream ended mid-key"; return false; }
      if (e == '\\') {
        key += (char) e;
        int f = sc.next();
        if (f < 0) { errCode = BW_ERR_SYNC_FETCH_FAILED; errOut = "sync stream ended mid-key"; return false; }
        key += (char) f;
        continue;
      }
      if (e == '"') break;
      key += (char) e;
    }
    if (sc.nextNonWs() != ':') { errCode = BW_ERR_SYNC_FETCH_FAILED; errOut = "malformed sync JSON (expected ':')"; return false; }

    if (key == "folders") {
      // elemIndex: raw position within THIS attempt's array walk, reset to
      // 0 every call (unlike folderCount, which persists across retries as
      // the stable per-folder send-index - see this function's own header
      // comment). Folders before skipFolders were already sent+acked in a
      // prior attempt at this same sync session; still parsed/decrypted (so
      // gFolderIds stays fully populated for cipher folderIndex lookups
      // below) but not re-sent to the Palm, so a retry can't create
      // duplicate folder records.
      //
      // skipFolders is advanced right here, per folder, the moment each
      // one is confirmed (successfully acked, or was already-skipped) -
      // advancing it only once after the whole array finished would leave
      // it stale on a mid-array ack failure, causing a retry to re-send
      // already-acked folders and create duplicate records on the Palm.
      int elemIndex = 0;
      /* folderAckFailed distinguishes an ack failure (bwWriteFrameAcked
       * already retried 5x in place - see its own comment - so this is
       * always terminal now, not retried again via this function's own
       * outer loop) from a malformed-JSON array element (a different,
       * genuinely retryable failure - see BW_SYNC_ITEM's own comment on
       * why the outer loop no longer retries ack failures specifically). */
      bool folderAckFailed = false;
      bool folderPalmSaidBye = false;
      bool arrOk = bwStreamArray(sc, [&](String &itemJson) -> bool {
        JsonDocument fd;
        if (deserializeJson(fd, itemJson)) return false;
        bool alreadySent = (elemIndex < (int) skipFolders);
        elemIndex++;
        if (folderCount < kMaxSyncFolders) {
          String id = fd["id"] | "";
          gFolderIds[folderCount] = id;
          if (!alreadySent) {
            String nameEnc = fd["name"] | "";
            String name;
            if (nameEnc.length() == 0 || !bwDecryptEncString(nameEnc, userEncKey, userMacKey, name)) name = "(undecryptable)";
            BwWriter w(gFrameBody, sizeof(gFrameBody));
            w.u16(folderCount);
            w.str(name);
            if (!bwWriteFrameAcked(BW_FOLDER, gFrameBody, w.len, &folderPalmSaidBye)) { folderAckFailed = true; return false; }
          }
          folderCount++;
          skipFolders = folderCount; // advance the resume point now, not after the whole array
          yield();
        }
        return true;
      });
      if (!arrOk) {
        if (folderAckFailed) {
          errCode = BW_ERR_CONNECT_FAIL;
          errOut = folderPalmSaidBye ? "Palm ended the session" : "Palm lost mid-sync (folder ack) after 5 attempts";
        } else {
          errCode = BW_ERR_SYNC_FETCH_FAILED;
          errOut = "malformed folders array";
        }
        return false;
      }
      sawFolders = true;
    } else if (key == "profile") {
      if (sc.nextNonWs() != '{') { errCode = BW_ERR_SYNC_FETCH_FAILED; errOut = "malformed sync JSON (profile)"; return false; }
      for (;;) {
        int pd = sc.nextNonWs();
        if (pd == '}') break;
        if (pd != '"') { errCode = BW_ERR_SYNC_FETCH_FAILED; errOut = "malformed sync JSON (profile key)"; return false; }
        String pkey;
        for (;;) {
          int e = sc.next();
          if (e < 0) { errCode = BW_ERR_SYNC_FETCH_FAILED; errOut = "sync stream ended mid-key"; return false; }
          if (e == '\\') {
            pkey += (char) e;
            int f = sc.next();
            if (f < 0) { errCode = BW_ERR_SYNC_FETCH_FAILED; errOut = "sync stream ended mid-key"; return false; }
            pkey += (char) f;
            continue;
          }
          if (e == '"') break;
          pkey += (char) e;
        }
        if (sc.nextNonWs() != ':') { errCode = BW_ERR_SYNC_FETCH_FAILED; errOut = "malformed sync JSON (profile ':')"; return false; }

        if (pkey == "organizations") {
          // Organizations: NOT end-to-end encrypted in Bitwarden's own model
          // (the server manages org membership/metadata directly) - relay
          // the name as /api/sync returns it, no decrypt step.
          // elemIndex/alreadySent: same retry-resume shape as "folders"
          // above, including the same skipOrgs-advances-per-item fix (see
          // that comment for the duplicate-record bug this closes).
          int elemIndex = 0;
          /* orgAckFailed - see folderAckFailed's own comment above for why
           * this is tracked separately from a malformed-JSON element. */
          bool orgAckFailed = false;
          bool orgPalmSaidBye = false;
          bool arrOk = bwStreamArray(sc, [&](String &itemJson) -> bool {
            JsonDocument od;
            if (deserializeJson(od, itemJson)) return false;
            bool alreadySent = (elemIndex < (int) skipOrgs);
            elemIndex++;
            if (orgCount < kMaxSyncOrgs) {
              String id = od["id"] | "";
              gOrgIds[orgCount] = id;
              gOrgWrappedKeys[orgCount] = od["key"] | "";	/* RSA-OAEP EncString - unwrapped after profile parsing finishes, see below - needed even for an already-sent org, since gOrgKeys is rebuilt fresh every attempt */
              if (!alreadySent) {
                String name = od["name"] | "(unnamed)";
                BwWriter w(gFrameBody, sizeof(gFrameBody));
                w.u16(orgCount);
                w.str(name);
                if (!bwWriteFrameAcked(BW_ORG, gFrameBody, w.len, &orgPalmSaidBye)) { orgAckFailed = true; return false; }
              }
              orgCount++;
              skipOrgs = orgCount; // advance the resume point now, not after the whole array
              yield();
            }
            return true;
          });
          if (!arrOk) {
            if (orgAckFailed) {
              errCode = BW_ERR_CONNECT_FAIL;
              errOut = orgPalmSaidBye ? "Palm ended the session" : "Palm lost mid-sync (org ack) after 5 attempts";
            } else {
              errCode = BW_ERR_SYNC_FETCH_FAILED;
              errOut = "malformed organizations array";
            }
            return false;
          }
        } else if (pkey == "privateKey") {
          // Captured raw (still JSON-quoted) here, unwrapped once the whole
          // profile object finishes parsing (organizations may appear
          // before OR after this key - either order needs both pieces
          // available first). Quotes stripped below before decrypting.
          if (!bwScanJsonValue(sc, privateKeyEnc, 4096)) {
            errCode = BW_ERR_SYNC_FETCH_FAILED; errOut = "malformed sync JSON (profile.privateKey)"; return false;
          }
          if (privateKeyEnc.length() >= 2) privateKeyEnc = privateKeyEnc.substring(1, privateKeyEnc.length() - 1);
        } else {
          String discard;
          if (!bwScanJsonValue(sc, discard, 65536)) {
            errCode = BW_ERR_SYNC_FETCH_FAILED; errOut = "malformed sync JSON (profile." + pkey + ")"; return false;
          }
        }
        int psep = sc.nextNonWs();
        if (psep == ',') continue;
        if (psep == '}') break;
        errCode = BW_ERR_SYNC_FETCH_FAILED; errOut = "malformed sync JSON (profile separator)"; return false;
      }
      sawProfile = true;
      // Unwrap every org's own key now that both profile.privateKey and
      // profile.organizations are fully known (order-independent - either
      // can appear first in the real JSON). A failure here (missing
      // privateKey, parse failure, one org's key not unwrapping) just
      // leaves that org's gOrgKeys[i].valid false - bwProcessOneCipher
      // already falls back to the user's own key in that case, matching
      // this relay's prior (if wrong) behavior rather than failing the
      // whole sync over one org's key.
      if (orgCount > 0 && privateKeyEnc.length()) {
        String privateKeyDer;
        if (bwDecryptEncString(privateKeyEnc, userEncKey, userMacKey, privateKeyDer) && privateKeyDer.length()) {
          mbedtls_pk_context pk;
          mbedtls_pk_init(&pk);
          if (bwParsePrivateKey((const uint8_t *) privateKeyDer.c_str(), privateKeyDer.length(), pk)) {
            for (int i = 0; i < orgCount; i++) {
              if (gOrgWrappedKeys[i].length()) {
                uint8_t raw[64];
                if (bwRsaOaepUnwrap(pk, gOrgWrappedKeys[i], raw)) {
                  memcpy(gOrgKeys[i].encKey, raw, 32);
                  memcpy(gOrgKeys[i].macKey, raw + 32, 32);
                  gOrgKeys[i].valid = true;
                }
              }
              if (usbDebug) Serial.printf("BW: org[%d] key unwrap %s\r\n", i, gOrgKeys[i].valid ? "OK" : "FAILED");
            }
          } else if (usbDebug) {
            Serial.println("BW: profile.privateKey failed to parse as an RSA key - org items will fall back to the personal key");
          }
          mbedtls_pk_free(&pk);
        } else if (usbDebug) {
          Serial.println("BW: profile.privateKey failed to decrypt - org items will fall back to the personal key");
        }
      }
      privateKeyEnc = String();	/* private key material - don't linger in RAM longer than needed */
    } else if (key == "ciphers") {
      if (!sawFolders || !sawProfile) {
        errCode = BW_ERR_SYNC_FETCH_FAILED;
        errOut = "sync response has 'ciphers' before 'folders'/'profile' - unsupported key order";
        return false;
      }
      if (usbDebug) Serial.printf("BW: [t+%lums] streaming ciphers...\r\n", millis() - tSyncStart);
      JsonDocument cipherFilter;
      bwBuildCipherFilter(cipherFilter);
      // elemIndex/skipCiphers: same retry-resume shape as folders/orgs
      // above, but skips the WHOLE bwProcessOneCipher call (not just the
      // frame send) for an already-processed element - unlike a folder/org
      // name, decrypting a full cipher is real, non-trivial work, worth
      // skipping entirely on a retry rather than just suppressing the send.
      // Correct as long as the retried /api/sync response returns the same
      // items in the same order as the failed attempt did, which holds for
      // the short (seconds), no-intervening-write window a retry runs in.
      // skipCiphers is advanced per item, right after each one is
      // confirmed, not only once after the whole array finishes - the
      // latter would leave it stale on a mid-array ack failure, causing a
      // retry to re-decrypt and re-send already-confirmed items and create
      // duplicate records on the Palm.
      int elemIndex = 0;
      bool arrOk = bwStreamArray(sc, [&](String &itemJson) -> bool {
        if (elemIndex < (int) skipCiphers) { elemIndex++; return true; }
        bool itemOk = bwProcessOneCipher(itemJson, cipherFilter, userEncKey, userMacKey, gFolderIds, folderCount, gOrgIds, orgCount,
                                          gOrgKeys, itemCount, errOut, errCode);
        elemIndex++;
        if (itemOk) skipCiphers = (uint16_t) elemIndex;
        return itemOk;
      });
      if (!arrOk) {
        if (errCode == 0) { errCode = BW_ERR_SYNC_FETCH_FAILED; errOut = "malformed ciphers array"; }
        return false;
      }
    } else {
      // Unknown/uninteresting top-level field (collections, sends,
      // policies, domains, object, ...) - skip its value wholesale without
      // materializing it.
      String discard;
      if (!bwScanJsonValue(sc, discard, 4UL * 1024 * 1024)) {
        errCode = BW_ERR_SYNC_FETCH_FAILED; errOut = "malformed sync JSON (skipping '" + key + "')"; return false;
      }
    }

    int sep = sc.nextNonWs();
    if (sep == ',') continue;
    if (sep == '}') break;
    errCode = BW_ERR_SYNC_FETCH_FAILED; errOut = "malformed sync JSON (top-level separator)"; return false;
  }

  return true;
}

static bool bwSyncAndStream(WiFiClientSecure &tls, const String &host, const String &accessToken,
                             const uint8_t userEncKey[32], const uint8_t userMacKey[32],
                             uint16_t &itemCount, uint16_t &folderCount, uint16_t &orgCount,
                             uint16_t &skipFolders, uint16_t &skipOrgs, uint16_t &skipCiphers,
                             String &errOut, uint8_t &errCode,
                             unsigned long tSyncStart) {
  HTTPClient http;
  if (!http.begin(tls, "https://" + host + "/api/sync?excludeDomains=true")) {
    errCode = BW_ERR_CONNECT_FAIL; errOut = "sync begin() failed"; return false;
  }
  http.addHeader("Authorization", "Bearer " + accessToken);
  // /api/sync's response size is unbounded (grows with vault size - a real
  // account measured at 2.2MB) and this device profile has separately been
  // observed taking >90s on PBKDF2 alone, so a short read timeout is its
  // own failure mode independent of network health. 45s here (both the
  // HTTPClient-level and the underlying socket-level timeout - two
  // different timers, both need raising).
  http.setTimeout(45000);
  int code = http.GET();
  if (code < 0) { errCode = BW_ERR_CONNECT_FAIL; errOut = "sync connect failed (" + String(code) + ")"; http.end(); return false; }
  if (code != 200) { errCode = BW_ERR_SYNC_FETCH_FAILED; errOut = "sync HTTP " + String(code); http.end(); return false; }
  if (usbDebug) Serial.printf("BW: [t+%lums] /api/sync HTTP 200, Content-Length=%d, connected=%d, streaming item-by-item\r\n",
                               millis() - tSyncStart, http.getSize(), (int) http.connected());

  // Parses straight off `tls` (the same WiFiClientSecure this whole
  // bwHandleSync call already owns end to end), not http.getStream() -
  // getStream() can return an empty stream even with a live,
  // connected()==true socket; reading from the object already held
  // sidesteps HTTPClient's own stream-access bookkeeping entirely.
  bool ok = bwStreamSyncBody(tls, userEncKey, userMacKey, itemCount, folderCount, orgCount,
                              skipFolders, skipOrgs, skipCiphers, errOut, errCode, tSyncStart);
  http.end();
  if (!ok && errOut.length() == 0) { errCode = BW_ERR_SYNC_FETCH_FAILED; errOut = "sync failed"; }
  return ok;
}

// ============================================================================
// Frame handlers
// ============================================================================

static String bwUrlToHost(const String &url, uint16_t &port) {
  String host = url;
  int schemeEnd = host.indexOf("://");
  if (schemeEnd >= 0) host = host.substring(schemeEnd + 3);
  int slash = host.indexOf('/');
  if (slash >= 0) host = host.substring(0, slash);
  int colon = host.indexOf(':');
  port = 443;
  if (colon >= 0) { port = (uint16_t) host.substring(colon + 1).toInt(); host = host.substring(0, colon); }
  return host;
}

static void bwHandleHello(const uint8_t *body, size_t bodyLen) {
  // BW_HELLO's body carries the Palm's offered rate (BwGetBaud on that
  // side) as a big-endian uint32, mirroring caldav_relay.cpp's identical
  // scheme. A malformed/missing field means no ack at all - not backward-
  // compatible, matching this project's convention that Palm app + WiRSa
  // firmware are always rebuilt/reflashed together.
  BwReader r(body, bodyLen);
  uint32_t offered = r.u32();
  if (!r.ok) {
    if (usbDebug) Serial.println("BW: malformed HELLO (short body, old Palm build?) - ignored, no ack sent");
    return;
  }

  uint32_t negotiated = offered;
  uint32_t maxAllowed = (uint32_t) bauds[serialSpeed];
  if (negotiated > maxAllowed) negotiated = maxAllowed;
  if (negotiated == 0) negotiated = BW_BAUD_IDLE;
  if (usbDebug) {
    Serial.printf("BW: baud negotiate - Palm offered %lu, capped to %lu (WiRSa config max)\r\n",
                  (unsigned long) offered, (unsigned long) negotiated);
  }

  BwWriter w(gFrameBody, sizeof(gFrameBody));
  w.u8(BW_PROTOCOL_VERSION);
  w.u8(0x00); // capFlags: bit0 (KEX) not implemented in this build - Phase 1
  w.str(String("WiRSa-BW/1"));
  w.u32(negotiated);  // appended, not interleaved - see this function's own header comment
  // bwWriteFrame flushes internally before returning (its own guarantee,
  // relay_proto.h's writeFrame has the identical contract) - the ack is
  // fully on the wire, at the OLD rate the Palm is still listening at,
  // before we touch PhysicalSerial below.
  bwWriteFrame(BW_HELLO_ACK, gFrameBody, w.len);
  if (usbDebug) Serial.println("BW: HELLO -> HELLO_ACK");

  PhysicalSerial.begin(negotiated, WIRSA_SERIAL_FORMAT, RXD2, TXD2);
  gBwNegotiatedAt = millis();
  bwDrawHeader();  // live baud readout - title bar reflects the negotiated rate
}

// Discovery only - setInsecure() so the handshake completes regardless of
// trust, purely to read whatever certificate the server presents. This
// connection is never reused for anything else (see bwHandleSync for the
// real pin-enforcing connection).
static void bwHandleTrust(const uint8_t *body, size_t bodyLen) {
  BwReader r(body, bodyLen);
  String url = r.str();
  if (!r.ok) { if (usbDebug) Serial.println("BW: malformed TRUST frame"); return; }

  uint16_t port;
  String host = bwUrlToHost(url, port);

  WiFiClientSecure tls;
  tls.setInsecure();
  tls.setTimeout(8); // seconds (WiFiClientSecure::setTimeout takes seconds, not ms)
  // WiFiClientSecure::setTimeout() above is a non-virtual method that
  // shadows Stream::setTimeout() (different signature, seconds vs ms) - it
  // only sets WiFiClientSecure's own connect/handshake budget, and never
  // touches Stream::_timeout (stuck at Arduino's hardcoded 1000ms default),
  // which is what readBytes()/readStringUntil() actually use. Set it
  // explicitly through a Stream& reference, where static dispatch resolves
  // to the base method instead. Confirmed real via the vendored ESP32 core
  // source and already fixed the same way in caldav_relay.cpp.
  ((Stream &) tls).setTimeout(8000);
  tls.setHandshakeTimeout(8); // seconds - otherwise silently defaults to 120s

  BwWriter w(gFrameBody, sizeof(gFrameBody));
  if (!tls.connect(host.c_str(), port)) {
    if (usbDebug) Serial.printf("BW: TRUST connect failed to %s:%u\r\n", host.c_str(), port);
    w.u8(2); // connect_fail
    uint8_t zero[32] = {0}; w.bytesRaw(zero, 32);
    w.str(""); w.str("");
    bwWriteFrame(BW_TRUST_RESULT, gFrameBody, w.len);
    return;
  }

  uint8_t fp[32];
  bool haveFp = tls.getFingerprintSHA256(fp);
  tls.stop();

  if (usbDebug) {
    char hex[65];
    for (int i = 0; i < 32; i++) sprintf(hex + i * 2, "%02X", fp[i]);
    hex[64] = 0;
    Serial.printf("BW: TRUST %s -> fingerprint %s\r\n", host.c_str(), haveFp ? hex : "(unavailable)");
  }

  w.u8(haveFp ? 0 : 3); // 0=ok, 3=tls_handshake_fail
  uint8_t zero[32] = {0};
  w.bytesRaw(haveFp ? fp : zero, 32);
  // Subject/issuer CN intentionally left empty in v1 - NetworkClientSecure
  // doesn't expose a stable public accessor for these across arduino-esp32
  // versions this sandbox can't compile-verify against; the fingerprint is
  // what TOFU actually pins on, these are just a nicer display later.
  w.str(""); w.str("");
  bwWriteFrame(BW_TRUST_RESULT, gFrameBody, w.len);
}

// Live status-label update for the Palm during the one long stretch
// (prelogin/PBKDF2/login/api-sync-fetch, all inside a single bwHandleSync
// call with no other frame in between) that used to be one silent gap - see
// BW_SYNC_STATUS's own comment. Purely cosmetic: the Palm just displays
// whatever text arrives and does NOT shorten its receive-timeout override
// upon receiving one (unlike FOLDER/ORG/SYNC_ITEM), since a status ping
// doesn't mean steady data is now flowing - see Sync.c's own handling.
static void bwSendSyncStatus(const char *text) {
  BwWriter w(gFrameBody, sizeof(gFrameBody));
  w.str(String(text));
  bwWriteFrame(BW_SYNC_STATUS, gFrameBody, w.len);
}

static void bwHandleSync(const uint8_t *body, size_t bodyLen) {
  // The Palm's receive-loop idle timeout (Serial.c's bwIdleTimeoutTicks,
  // 180s) starts counting right after it sends this SYNC request, well
  // before WiRSa's own PBKDF2/login/api-sync-fetch work happens. These
  // prints measure elapsed time through that pipeline directly so a run
  // that's eating close to or past the budget is identifiable from the log.
  unsigned long tSyncStart = millis();
  BwReader r(body, bodyLen);
  String url = r.str();
  uint8_t pinnedFp[32];
  r.bytesRaw(pinnedFp, 32);
  String email = r.str();
  String masterPassword = r.str();
  uint8_t kdfTypeIn = r.u8();
  uint32_t kdfIterationsIn = r.u32();
  uint32_t kdfMemoryIn = r.u32();
  uint8_t kdfParallelismIn = r.u8();
  String rememberedTwoFactorToken = r.str(); // see PROTOCOL.md's own SYNC/TWO_FACTOR_REQUIRED note
  if (!r.ok) {
    if (usbDebug) Serial.println("BW: malformed SYNC frame");
    masterPassword = String();
    return;
  }

  // `token`: Vaultwarden rotates (invalidates) the account's remember-
  // device token the moment a login succeeds, regardless of what happens
  // afterward - a sync that fails mid-progress still had a successful
  // login just before, so the Palm's old token is already dead and this is
  // the only copy of the new one. Reported here (not just via SYNC_DONE,
  // which a mid-progress failure never reaches) so a failed sync doesn't
  // force a fresh interactive 2FA prompt on the next attempt. Same
  // "empty = no change" convention as SYNC_DONE's own field - see PROTOCOL.md.
  auto sendErr = [&](uint8_t code, const String &msg, const String &token = String()) {
    BwWriter w(gFrameBody, sizeof(gFrameBody));
    w.u8(code); w.str(msg); w.str(token);
    bwWriteFrame(BW_SYNC_ERROR, gFrameBody, w.len);
  };

  uint16_t port;
  String host = bwUrlToHost(url, port);

  WiFiClientSecure tls;
  tls.setInsecure(); // TOFU: accept whatever cert is presented during the handshake
  tls.setTimeout(45); // seconds - shared across prelogin/login/sync; sync's
                       // /api/sync body is unbounded in size and this device
                       // has been observed taking >90s on PBKDF2 alone, so
                       // 15s was too tight (see bwSyncAndStream's own note)
  // BUG (confirmed against the vendored ESP32 core source, same one already
  // fixed in caldav_relay.cpp): WiFiClientSecure::setTimeout() above is
  // non-virtual and shadows Stream::setTimeout() (different signature -
  // seconds vs ms) - it only sets WiFiClientSecure's own connect/write-
  // check budget, never Stream::_timeout, which stays at Arduino's
  // hardcoded 1000ms default. BwJsonScanner::next() reads through `s`
  // (a Stream&, see bwStreamSyncBody's own signature) via readBytes(),
  // which uses THAT stuck-at-1000ms field, not the 45s just set above -
  // so the real per-refill wait was ~1s (~3s total across
  // kMaxRefillRetries), not 45s. Root cause of the real-device
  // "malformed cipher JSON - error=IncompleteInput" failures on large
  // items (more refills needed while scanning one big captured value =
  // more chances to hit an ordinary sub-second WiFi/TLS scheduling gap
  // that this stuck timeout couldn't tolerate) - confirmed via a real
  // debug log where the same oversized item failed identically on all 3
  // retry attempts, each capturing a slightly different byte count right
  // around the same ~17KB mark. Set explicitly through a Stream&
  // reference, where static dispatch resolves to the base method instead.
  ((Stream &) tls).setTimeout(45000);
  tls.setHandshakeTimeout(45); // seconds - otherwise silently defaults to 120s

  if (!tls.connect(host.c_str(), port)) {
    if (usbDebug) Serial.printf("BW: SYNC connect failed to %s:%u\r\n", host.c_str(), port);
    sendErr(BW_ERR_CONNECT_FAIL, "connect failed");
    masterPassword = String();
    return;
  }

  char fpHex[65];
  for (int i = 0; i < 32; i++) sprintf(fpHex + i * 2, "%02X", pinnedFp[i]);
  fpHex[64] = 0;
  // Fingerprint-only check (domain_name = nullptr): TOFU trust is the exact
  // certificate bytes the user already accepted, not a CN/SAN match - many
  // self-hosted Vaultwarden certs are self-signed with no matching SAN,
  // which would otherwise fail a legitimately-pinned reconnect.
  if (!tls.verify(fpHex, nullptr)) {
    if (usbDebug) Serial.printf("BW: SYNC fingerprint mismatch for %s\r\n", host.c_str());
    tls.stop();
    sendErr(BW_ERR_FINGERPRINT_MISMATCH, "certificate fingerprint does not match the pinned value");
    masterPassword = String();
    return;
  }

  BwKdfParams kdf;
  String errMsg;
  if (kdfTypeIn == 0) {
    if (!bwPrelogin(tls, host, email, kdf, errMsg)) {
      if (usbDebug) Serial.printf("BW: prelogin failed: %s\r\n", errMsg.c_str());
      tls.stop();
      sendErr(BW_ERR_MALFORMED_KDF, errMsg);
      masterPassword = String();
      return;
    }
  } else {
    kdf.type = kdfTypeIn; kdf.iterations = kdfIterationsIn; kdf.memoryKiB = kdfMemoryIn; kdf.parallelism = kdfParallelismIn;
  }

  if (kdf.type != 0) { // 0 = PBKDF2-SHA256; 1 = Argon2id, not yet supported
    if (usbDebug) Serial.println("BW: account uses Argon2id - not supported by this relay build yet");
    tls.stop();
    sendErr(BW_ERR_MALFORMED_KDF, "this account uses Argon2id - not yet supported by this relay build");
    masterPassword = String();
    return;
  }

  if (usbDebug) Serial.printf("BW: deriving master key (PBKDF2-SHA256, %lu iterations)\r\n", (unsigned long) kdf.iterations);
  bwSendSyncStatus("Iterating...");

  BwCryptoContext ctx;
  bwDeriveMasterKey(masterPassword, email, kdf.iterations, ctx.masterKey);
  if (usbDebug) Serial.printf("BW: [t+%lums] PBKDF2 done\r\n", millis() - tSyncStart);
  bwSendSyncStatus("Fetching vault..."); // matches the Palm's own pre-SYNC guess label for this whole phase - see Sync.c
  String masterPasswordHash = bwDeriveMasterPasswordHash(ctx.masterKey, masterPassword);
  bwDeriveStretchedKeys(ctx.masterKey, ctx.stretchedEncKey, ctx.stretchedMacKey);
  masterPassword = String(); // raw password no longer needed - drop the RAM copy now

  // 2FA round trip (PROTOCOL.md's own TWO_FACTOR_REQUIRED/TWO_FACTOR_SUBMIT
  // section) - bwLogin itself is stateless per call; this loop is what keeps
  // retrying it with whatever code the Palm just submitted, up to a bounded
  // number of attempts, without re-doing the TLS connect/prelogin/PBKDF2
  // work above (that's all still valid - only the token exchange itself
  // needs to be retried).
  //
  // If the Palm sent a remembered "trust this device" token, the very
  // first attempt below uses it (provider 5 = Bitwarden's own Remember
  // pseudo-provider) instead of starting from an empty/interactive attempt
  // - if the account has already completed 2FA once from this Palm and
  // the token hasn't expired, this succeeds immediately with no prompt at
  // all, matching every real Bitwarden client. If it's rejected (expired/
  // invalid), errCode comes back BW_ERR_TWO_FACTOR_REQUIRED exactly like a
  // wrong interactive code would, and falls through to the normal
  // TWO_FACTOR_REQUIRED prompt cycle below - no special-casing needed
  // there, it already treats "current attempt failed but still within
  // budget" uniformly regardless of why the previous one failed.
  String accessToken, keyEncString;
  uint8_t errCode = 0;
  String twoFactorToken = rememberedTwoFactorToken;
  uint8_t twoFactorProvider = rememberedTwoFactorToken.length() ? 5 : 0;
  String newRememberToken; // what to report back in SYNC_DONE - empty = no change
  bool loggedIn = false;
  static const int kMaxTwoFactorAttempts = 3;
  for (int attempt = 0; ; attempt++) {
    // Reconnects before every attempt, not just the first. This loop
    // reuses the same tls connection opened before prelogin; a long idle
    // gap (a high-iteration PBKDF2, or an interactive 2FA code being a
    // human-timescale wait) is enough for the server or an intervening
    // proxy to close it, so the next request on it would otherwise fail at
    // the transport level. Cheap relative to everything else in this flow,
    // and guarantees the request that follows always has a live socket.
    // Same reconnect+re-verify shape as the sync-retry loop's own recovery path.
    tls.stop();
    if (!tls.connect(host.c_str(), port) || !tls.verify(fpHex, nullptr)) {
      errCode = BW_ERR_CONNECT_FAIL; errMsg = "reconnect for login failed";
      break;
    }
    uint8_t rxProvider = 0;
    String rxLabel;
    String outTfToken;
    if (bwLogin(tls, host, email, masterPasswordHash, twoFactorToken, twoFactorProvider,
                accessToken, keyEncString, outTfToken, rxProvider, rxLabel, errMsg, errCode)) {
      loggedIn = true;
      // Logs unconditionally (not just when outTfToken is present) so a
      // login succeeding via provider 5 (the remembered token itself)
      // also confirms this branch is reached, distinguishing "this code
      // path never runs" from "the server legitimately never returns a
      // TwoFactorToken even on success".
      if (usbDebug) {
        Serial.printf("BW: login succeeded (attempt %d, provider used=%u), TwoFactorToken %s",
                      attempt, twoFactorProvider, outTfToken.length() ? "present, length=" : "ABSENT");
        if (outTfToken.length()) Serial.printf("%u", (unsigned) outTfToken.length());
        Serial.printf("\r\n");
      }
      if (outTfToken.length()) newRememberToken = outTfToken;
      break;
    }
    if (errCode != BW_ERR_TWO_FACTOR_REQUIRED || attempt >= kMaxTwoFactorAttempts) break;

    if (usbDebug) Serial.printf("BW: two-factor required (provider %u: %s)\r\n", rxProvider, rxLabel.c_str());
    {
      BwWriter w(gFrameBody, sizeof(gFrameBody));
      w.u8(rxProvider);
      w.str(rxLabel);
      bwWriteFrame(BW_TWO_FACTOR_REQUIRED, gFrameBody, w.len);
    }

    uint8_t rxType; size_t rxLen;
    if (!bwReadFrame(rxType, gFrameBody, sizeof(gFrameBody), rxLen, BW_TWO_FACTOR_TIMEOUT_MS)) {
      errCode = BW_ERR_AUTH_FAILED; errMsg = "timed out waiting for the two-factor code";
      break;
    }
    if (rxType == BW_BYE) {
      if (usbDebug) Serial.println("BW: two-factor entry cancelled by Palm");
      masterPasswordHash = String();
      memset(&ctx, 0, sizeof(ctx));
      tls.stop();
      return;
    }
    if (rxType != BW_TWO_FACTOR_SUBMIT) {
      errCode = BW_ERR_AUTH_FAILED; errMsg = "unexpected frame while waiting for the two-factor code";
      break;
    }
    BwReader tr(gFrameBody, rxLen);
    twoFactorToken = tr.str();
    twoFactorProvider = rxProvider;
    if (!tr.ok) {
      errCode = BW_ERR_AUTH_FAILED; errMsg = "malformed two-factor submit frame";
      break;
    }
    if (usbDebug) Serial.println("BW: retrying login with submitted two-factor code");
  }
  if (!loggedIn) {
    if (usbDebug) Serial.printf("BW: login failed: %s\r\n", errMsg.c_str());
    sendErr(errCode, errMsg);
    memset(&ctx, 0, sizeof(ctx));
    tls.stop();
    return;
  }
  masterPasswordHash = String();
  // errMsg/errCode may still hold BW_ERR_TWO_FACTOR_REQUIRED from the
  // loop's own first, expected-to-fail bwLogin attempt - loggedIn==true
  // means that was resolved, not a real failure. Clear them so nothing
  // downstream (bwSyncAndStream reuses these same variables) mistakes this
  // stale leftover for a fresh error.
  errMsg = String();
  errCode = 0;

  String userKeyRaw;
  if (!bwDecryptEncString(keyEncString, ctx.stretchedEncKey, ctx.stretchedMacKey, userKeyRaw) || userKeyRaw.length() < 64) {
    if (usbDebug) Serial.println("BW: failed to unwrap the account key (likely wrong master password)");
    sendErr(BW_ERR_DECRYPT_FAILED, "failed to unwrap the account key (likely wrong master password)", newRememberToken);
    memset(&ctx, 0, sizeof(ctx));
    tls.stop();
    return;
  }
  memcpy(ctx.userEncKey, userKeyRaw.c_str(), 32);
  memcpy(ctx.userMacKey, userKeyRaw.c_str() + 32, 32);
  userKeyRaw = String();

  if (usbDebug) Serial.printf("BW: [t+%lums] authenticated, fetching /api/sync\r\n", millis() - tSyncStart);

  // Retries here cover a genuine Vaultwarden-side stream stall or
  // malformed-JSON condition (BwJsonScanner's own retry-the-refill logic
  // already absorbs a short stall; this is for one that outlasts it).
  // login/PBKDF2 above only needs to happen once, so a retry here only
  // redoes the /api/sync fetch: a fresh TLS connection (the previous one's
  // HTTP framing is left desynced by a mid-body failure, so reusing it
  // isn't reliable), then bwSyncAndStream again. itemCount/folderCount/
  // orgCount and skipFolders/skipOrgs/skipCiphers are declared outside the
  // loop so they accumulate across attempts (see bwStreamSyncBody) - a
  // retry resumes past whatever was already sent+acked, rather than
  // re-sending it and creating duplicate records on the Palm.
  //
  // Item/folder/org acks retry in place instead (bwWriteFrameAcked, 5
  // attempts, no reconnect) and are never BW_ERR_SYNC_FETCH_FAILED, so
  // this loop only ever fires for the stream-stall/malformed-JSON failure
  // domain, where reconnecting to Vaultwarden and retrying still makes sense.
  uint16_t itemCount = 0, folderCount = 0, orgCount = 0;
  uint16_t skipFolders = 0, skipOrgs = 0, skipCiphers = 0;
  static const int kMaxSyncFetchAttempts = 3; // 1 initial + up to 2 retries
  bool ok = false;
  for (int attempt = 0; attempt < kMaxSyncFetchAttempts; attempt++) {
    if (attempt > 0) {
      tls.stop();
      delay(1000); // brief backoff - avoid hammering a struggling network/server immediately
      if (!tls.connect(host.c_str(), port) || !tls.verify(fpHex, nullptr)) {
        errCode = BW_ERR_CONNECT_FAIL; errMsg = "reconnect for sync retry failed";
        break;
      }
      if (usbDebug) {
        Serial.printf("BW: retrying /api/sync (attempt %d/%d) - already have %u folder(s), %u org(s), %u item(s)\r\n",
                      attempt + 1, kMaxSyncFetchAttempts, folderCount, orgCount, itemCount);
      }
    }
    ok = bwSyncAndStream(tls, host, accessToken, ctx.userEncKey, ctx.userMacKey,
                          itemCount, folderCount, orgCount, skipFolders, skipOrgs, skipCiphers,
                          errMsg, errCode, tSyncStart);
    if (ok || errCode != BW_ERR_SYNC_FETCH_FAILED) break;
  }
  tls.stop();
  memset(&ctx, 0, sizeof(ctx)); // wipe every derived key, success or failure

  if (!ok) {
    if (usbDebug) Serial.printf("BW: sync failed: %s\r\n", errMsg.c_str());
    sendErr(errCode, errMsg, newRememberToken);
    return;
  }

  BwWriter w(gFrameBody, sizeof(gFrameBody));
  w.u16(itemCount); w.u16(folderCount); w.u16(orgCount);
  w.str(newRememberToken); // empty = no change - see PROTOCOL.md's own SYNC_DONE note
  bwWriteFrame(BW_SYNC_DONE, gFrameBody, w.len);
  if (usbDebug) Serial.printf("BW: SYNC complete - %u item(s), %u folder(s), %u org(s)\r\n", itemCount, folderCount, orgCount);
}

// The default loop() task's stack (8KB) isn't sized for this: a
// recursive-descent JSON parse on top of an already deep TLS/HTTPClient/
// PBKDF2 call chain, against a multi-MB /api/sync response, can overflow
// it. Runs the entire sync handler on a dedicated FreeRTOS task with a
// much larger stack instead. This mode is already fully synchronous/modal
// (bitwardenRelayLoop won't service anything else mid-sync either way),
// so blocking the caller on a binary semaphore until the task finishes
// preserves the exact same external behavior as calling bwHandleSync()
// inline, just with headroom.
static SemaphoreHandle_t gSyncTaskDone = nullptr;
static const uint8_t *gSyncTaskBody = nullptr;
static size_t gSyncTaskBodyLen = 0;

static void bwHandleSyncTask(void *) {
  bwHandleSync(gSyncTaskBody, gSyncTaskBodyLen);
  xSemaphoreGive(gSyncTaskDone);
  vTaskDelete(NULL);
}

// body must point at storage that outlives the task (gFrameBody, the only
// real caller, is a static file-scope buffer - safe; its contents are
// fully consumed here before bitwardenRelayLoop() reads another frame into
// it, since the caller blocks until this task signals done).
static void bwHandleSyncBigStack(const uint8_t *body, size_t bodyLen) {
  if (!gSyncTaskDone) gSyncTaskDone = xSemaphoreCreateBinary();
  gSyncTaskBody = body;
  gSyncTaskBodyLen = bodyLen;
  TaskHandle_t h = nullptr;
  BaseType_t created = xTaskCreate(bwHandleSyncTask, "bwSync", 32768, nullptr, 1, &h);
  if (created != pdPASS) {
    // Couldn't even allocate the bigger stack (heap that low is its own
    // problem) - fall back to running inline rather than silently hanging
    // the Palm forever waiting for a response that will never come.
    if (usbDebug) Serial.println("BW: failed to create sync task (out of memory?) - running inline");
    bwHandleSync(body, bodyLen);
    return;
  }
  xSemaphoreTake(gSyncTaskDone, portMAX_DELAY);
}

// ============================================================================
// OLED header/status - same boxed-header-plus-plain-black-status shape as
// hotsync.cpp's own hsDrawHeader()/hsDrawStatus(). Status is intentionally
// just a one-line toggle between "Waiting..." and "Syncing..." - enough to
// show something's alive, nothing more.
// ============================================================================

static void bwDrawHeader() {
  display.fillRect(0, 0, 128, 16, SSD1306_WHITE);
  display.drawLine(1, 1, 126, 1, SSD1306_BLACK);
  display.drawLine(1, 14, 126, 14, SSD1306_BLACK);
  display.drawLine(1, 1, 1, 14, SSD1306_BLACK);
  display.drawLine(126, 1, 126, 14, SSD1306_BLACK);

  display.setCursor(4, 4);
  display.setTextColor(SSD1306_BLACK);
  // "Bitwarden" leaves room for the WiFi status icon in this same header.
  // Live baud (not the configured value) shown top-right, same as
  // hotsync.cpp's hsDrawHeader()/sasd.cpp's sasdDrawStatusLines().
  {
    String line = "Bitwarden";
    display.print(line);
    display.print(padLeft(liveBaudStr(PhysicalSerial.baudRate()), 20 - line.length()));
  }
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 17);
  display.display();
  drawWifiStatusIcon();  // WiFi status on every title bar
}

static void bwDrawStatus(const char *state) {
  display.fillRect(0, 17, 128, 47, SSD1306_BLACK);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 19);
  display.print(state);
  display.display();
}

// Reopens PhysicalSerial at BW_BAUD_IDLE and redraws the header so the
// title bar's live baud readout reflects it immediately - shared by the
// negotiation-timeout check and the post-sync cleanup in
// bitwardenRelayLoop, so there's exactly one place this sequence needs to
// be correct.
static void bwRevertToIdleBaud() {
  PhysicalSerial.begin(BW_BAUD_IDLE, WIRSA_SERIAL_FORMAT, RXD2, TXD2);
  bwDrawHeader();
}

// ============================================================================
// Mode entry/exit/loop - same shape as SASD (enterSasdMode/exitSasdMode/
// sasdLoop): entered from the main menu, passively services whatever the
// Palm sends until BACK or the +++ USB-serial escape exits it. See
// bitwarden_relay.h for why this can't be an always-pumped background mode
// the way FTP Server is (single shared physical UART).
// ============================================================================

void enterBitwardenRelayMode() {
  menuMode = MODE_BITWARDEN;
  setBinaryMode(true);

  SerialPrintLn("\r\nEntering BitWarden Relay Mode...");

  if (WiFi.status() != WL_CONNECTED) {
    // waitForWiFiConnection() shows its own "Waiting for WiFi..." / "WiFi
    // unavailable. Retry?" UI and retries for up to a minute - same
    // blocking-entry shape as FTP Server/SLIP Gateway, so all three
    // WiFi-requiring services behave the same way at entry. A false
    // return means the user backed out.
    if (!waitForWiFiConnection()) {
      SerialPrintLn("BitWarden Relay: WiFi not available - cancelled.");
      setBinaryMode(false);
      mainMenu(false);
      return;
    }
  }
  bwWifiReady = true;

  if (usbDebug) {
    UsbDebugPrint("");
    Serial.printf("BW: mode entered - WiFi connected\r\n");
  }

  // Start every session at the fixed idle rate rather than inheriting
  // whatever bauds[serialSpeed] the global setup() left PhysicalSerial at -
  // BW_HELLO negotiates up from here once the Palm connects. Same pattern
  // as sasd.cpp's own mode entry.
  PhysicalSerial.begin(BW_BAUD_IDLE, WIRSA_SERIAL_FORMAT, RXD2, TXD2);

  SerialPrintLn("BitWarden Relay Active - waiting for the Palm");
  SerialPrintLn("Press +++ or BACK button to exit");
  bwDrawHeader();
  // Serial-link key pair, generated once per boot (relay_proto.h).
  bwDrawStatus("Preparing...");
  RelayProto::secureBegin();
  bwDrawStatus("Waiting...");
}

void exitBitwardenRelayMode() {
  setBinaryMode(false);
  RelayProto::secureReset();
  // If BACK/+++ is pressed while a negotiated (non-idle) rate is still
  // active - before BW_BYE arrives or the negotiate-timeout fires - the
  // mode's own revert-to-idle tick check never gets a chance to run, since
  // leaving the mode stops bitwardenRelayLoop from being called at all.
  // Restore the configured interactive rate explicitly instead (same as
  // sasd.cpp's own exit path), so PhysicalSerial doesn't stay at whatever
  // BitWarden Relay last negotiated and desync the next mode that assumes
  // bauds[serialSpeed] on this port (FTP Server, SASD, HotSync).
  PhysicalSerial.begin(bauds[serialSpeed], WIRSA_SERIAL_FORMAT, RXD2, TXD2);
  SerialPrintLn("\r\nExiting BitWarden Relay Mode...");
  if (usbDebug) UsbDebugPrint("BW: mode exit");
}

void bitwardenRelayLoop() {
  readSwitches();
  if (BTNBK) {
    waitSwitches();
    exitBitwardenRelayMode();
    msgFlag = true;
    mainMenu(false);
    return;
  }
  waitSwitches();

  // +++ escape on USB Serial only - PhysicalSerial carries the binary
  // protocol and must not be interpreted as commands (same convention
  // sasd.cpp/slip_mode.cpp already use).
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
        exitBitwardenRelayMode();
        msgFlag = true; mainMenu(false);
        return;
      }
    } else {
      escapePos = 0;
    }
  }

  if (!bwWifiReady) {
    if (WiFi.status() == WL_CONNECTED) {
      bwWifiReady = true;
      if (usbDebug) Serial.println("BW: WiFi came up");
      SerialPrintLn("BitWarden Relay: WiFi connected - now servicing requests.");
      bwDrawStatus("Waiting...");
    } else {
      return; // nothing to service yet
    }
  }

  // If BW_HELLO negotiated a faster rate but the Palm never followed up
  // with BW_SYNC, don't stay stuck listening at a rate a future connection
  // attempt won't know to expect. Runs every tick regardless of whether
  // data is currently available.
  if (PhysicalSerial.baudRate() != BW_BAUD_IDLE &&
      gBwNegotiatedAt != 0 && (millis() - gBwNegotiatedAt) > BW_NEGOTIATE_TIMEOUT_MS) {
    if (usbDebug) Serial.println("BW: negotiated baud timed out waiting for sync request, reverting to idle rate");
    gBwNegotiatedAt = 0;
    bwRevertToIdleBaud();
    bwDrawStatus("Waiting...");
  }

  if (!PhysicalSerial.available()) return;

  uint8_t type; size_t bodyLen;
  if (!bwReadFrame(type, gFrameBody, sizeof(gFrameBody), bodyLen)) return;
  // Serial-link key exchange - always first on a new connection.
  if (RelayProto::handleKex(PhysicalSerial, type, gFrameBody, bodyLen)) return;

  switch (type) {
    case BW_HELLO:
      bwHandleHello(gFrameBody, bodyLen);
      break;
    case BW_TRUST:
      bwHandleTrust(gFrameBody, bodyLen);
      break;
    case BW_SYNC:
      bwDrawStatus("Syncing...");
      bwHandleSyncBigStack(gFrameBody, bodyLen);
      // Does not revert baud here. After bwHandleSyncBigStack sends its
      // own SYNC_DONE, the Palm's own wind-down (BwSyncRun's trailing
      // BW_BYE, sent best-effort before it closes its port) still goes out
      // at the negotiated rate - if WiRSa had already reverted to
      // BW_BAUD_IDLE by the time that BYE arrives, WiRSa would be
      // listening at the wrong rate for it, producing garbage right after
      // sync completion. Revert only happens on receiving that real
      // BW_BYE (below) or the negotiate-timeout safety net if the Palm
      // goes silent instead of sending BYE at all.
      //
      // gBwNegotiatedAt is refreshed here rather than left at its original
      // negotiation-time value: bwHandleSyncBigStack just blocked for the
      // entire sync (hundreds of items on a large vault), almost certainly
      // longer than BW_NEGOTIATE_TIMEOUT_MS on its own - without this
      // refresh, the very next tick's timeout check would see an already-
      // expired window and force a premature revert, racing the Palm's
      // trailing BYE.
      gBwNegotiatedAt = millis();
      bwDrawStatus("Waiting...");
      break;
    case BW_BYE:
      if (usbDebug) Serial.println("BW: BYE received");
      // The only place a session reverts to the idle baud - the Palm's
      // own BW_BYE arrives at whatever rate was negotiated, so switching
      // away only once it's been correctly received avoids garbling it.
      if (PhysicalSerial.baudRate() != BW_BAUD_IDLE) {
        gBwNegotiatedAt = 0;
        bwRevertToIdleBaud();
        bwDrawStatus("Waiting...");
      }
      break;
    case BW_KEX:
      // Phase 2 stub - HELLO_ACK already advertised capFlags bit0=0, so a
      // well-behaved Palm client won't send this against this build yet.
      if (usbDebug) Serial.println("BW: KEX received but not implemented in this build - ignored");
      break;
    default:
      if (usbDebug) Serial.printf("BW: unknown frame type 0x%02X ignored\r\n", type);
      break;
  }
}
