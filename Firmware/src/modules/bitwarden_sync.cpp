// bitwarden_sync.cpp - TRUST and SYNC: the login, fetch and send sequence, and the big-stack task. Part of the BitWarden Relay mode; include/bitwarden_internal.h says how the files divide the work.
#include "bitwarden_internal.h"

namespace BitWardenRelay {

// ---- Frame handlers ----

String bwUrlToHost(const String &url, uint16_t &port) {
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

// Discovery only: setInsecure() so the handshake completes whatever the trust, just to read the certificate the server presents. This connection is never reused (bwHandleSync has the pinning one).
void bwHandleTrust(const uint8_t *body, size_t bodyLen) {
  BwReader r(body, bodyLen);
  String url = r.str();
  if (!r.ok) { LOGW("BW: malformed TRUST frame"); return; }

  uint16_t port;
  String host = bwUrlToHost(url, port);

  WiFiClientSecure tls;
  tls.setInsecure();
  tls.setTimeout(8); // seconds (WiFiClientSecure::setTimeout takes seconds, not ms)
  // WiFiClientSecure::setTimeout() above is non-virtual and shadows Stream::setTimeout() (seconds, not ms): it sets only WiFiClientSecure's connect/handshake budget, never Stream::_timeout (stuck at
  // Arduino's 1000 ms), which readBytes()/readStringUntil() use. Set it through a Stream& so static dispatch reaches the base method (confirmed in the vendored ESP32 core, fixed the same way in caldav_relay.cpp).
  ((Stream &) tls).setTimeout(8000);
  tls.setHandshakeTimeout(8); // seconds - otherwise silently defaults to 120s

  BwWriter w(gFrameBody, sizeof(gFrameBody));
  if (!tls.connect(host.c_str(), port)) {
    LOGW("BW: TRUST connect failed to %s:%u\r\n", host.c_str(), port);
    w.u8(2); // connect_fail
    uint8_t zero[32] = {0}; w.bytesRaw(zero, 32);
    w.str(""); w.str("");
    bwWriteFrame(BW_TRUST_RESULT, gFrameBody, w.len);
    return;
  }

  uint8_t fp[32];
  bool haveFp = tls.getFingerprintSHA256(fp);
  tls.stop();

  if (logOn(LOG_VERBOSE)) {
    char hex[65];
    for (int i = 0; i < 32; i++) sprintf(hex + i * 2, "%02X", fp[i]);
    hex[64] = 0;
    logPrintf(LOG_VERBOSE, "BW: TRUST %s -> fingerprint %s\r\n", host.c_str(), haveFp ? hex : "(unavailable)");
  }

  w.u8(haveFp ? 0 : 3); // 0=ok, 3=tls_handshake_fail
  uint8_t zero[32] = {0};
  w.bytesRaw(haveFp ? fp : zero, 32);
  // Subject/issuer CN left empty: NetworkClientSecure has no stable public accessor for them across arduino-esp32 versions (not compile-verifiable here). The fingerprint is what TOFU pins; these would
  // only be a nicer display.
  w.str(""); w.str("");
  bwWriteFrame(BW_TRUST_RESULT, gFrameBody, w.len);
}

// A live status label for the Palm during the one long stretch (prelogin, PBKDF2, login and the api-sync fetch, all inside one bwHandleSync call), once a silent gap (see BW_SYNC_STATUS). Cosmetic:
// the Palm shows the text and does NOT shorten its receive-timeout override (unlike FOLDER/ORG/SYNC_ITEM), since a status ping does not mean data is flowing (Sync.c).
void bwSendSyncStatus(const char *text) {
  BwWriter w(gFrameBody, sizeof(gFrameBody));
  w.str(String(text));
  bwWriteFrame(BW_SYNC_STATUS, gFrameBody, w.len);
}

void bwHandleSync(const uint8_t *body, size_t bodyLen) {
  // The Palm's receive idle timeout (Serial.c bwIdleTimeoutTicks, 180 s) starts when it sends this SYNC, well before WiRSa's PBKDF2, login and fetch. These prints time that pipeline so a run near
  // the budget shows in the log.
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
  String rememberedTwoFactorToken = r.str();  // see Protocol-wire.md's SYNC/TWO_FACTOR_REQUIRED note
  if (!r.ok) {
    LOGW("BW: malformed SYNC frame");
    masterPassword = String();
    return;
  }

  // token: Vaultwarden invalidates the account's remember-device token as soon as a login succeeds, whatever happens next, so after a mid-sync failure the Palm's old token is dead and this is the only
  // copy of the new one. It is reported here (SYNC_DONE is never reached then) so a failed sync does not force a fresh interactive 2FA prompt. "Empty = no change", as SYNC_DONE's field (Protocol-wire.md).
  auto sendErr = [&](uint8_t code, const String &msg, const String &token = String()) {
    BwWriter w(gFrameBody, sizeof(gFrameBody));
    w.u8(code); w.str(msg); w.str(token);
    bwEmitFinal(BW_ERR, gFrameBody, w.len);
  };

  uint16_t port;
  String host = bwUrlToHost(url, port);

  WiFiClientSecure tls;
  tls.setInsecure(); // TOFU: accept whatever cert is presented during the handshake
  tls.setTimeout(45); // seconds - shared across prelogin/login/sync; sync's
                       // /api/sync is unbounded and PBKDF2 alone has taken over 90 s on this device, so 15 s was too tight (see bwSyncAndStream).
                       // BUG (confirmed in the vendored core, as in caldav_relay.cpp): WiFiClientSecure::setTimeout() above is non-virtual and shadows Stream::setTimeout() (seconds, not ms) and never sets Stream::_timeout,
                       // which stays at 1000 ms. BwJsonScanner::next() reads through s (a Stream&) via readBytes(), which uses THAT field, not the 45 s just set: the real per-refill wait was ~1 s (~3 s over
                       // kMaxRefillRetries). That was the root cause of "malformed cipher JSON - error=IncompleteInput" on large items (more refills, more chances to hit an ordinary sub-second WiFi/TLS gap): one oversized item
                       // failed identically on all 3 retries around ~17 KB. Set it through a Stream& so static dispatch reaches the base method.
  ((Stream &) tls).setTimeout(45000);
  tls.setHandshakeTimeout(45); // seconds - otherwise silently defaults to 120s

  if (!tls.connect(host.c_str(), port)) {
    LOGW("BW: SYNC connect failed to %s:%u\r\n", host.c_str(), port);
    sendErr(BW_ERR_CONNECT_FAIL, "connect failed");
    masterPassword = String();
    return;
  }

  char fpHex[65];
  for (int i = 0; i < 32; i++) sprintf(fpHex + i * 2, "%02X", pinnedFp[i]);
  fpHex[64] = 0;
  // Fingerprint-only check (domain_name = nullptr): TOFU trusts the exact certificate bytes the user accepted, not a CN/SAN match; many self-hosted Vaultwarden certs are self-signed with no
  // matching SAN and would fail a legitimately pinned reconnect.
  if (!tls.verify(fpHex, nullptr)) {
    LOGW("BW: SYNC fingerprint mismatch for %s\r\n", host.c_str());
    tls.stop();
    sendErr(BW_ERR_FINGERPRINT_MISMATCH, "certificate fingerprint does not match the pinned value");
    masterPassword = String();
    return;
  }

  BwKdfParams kdf;
  String errMsg;
  if (kdfTypeIn == 0) {
    if (!bwPrelogin(tls, host, email, kdf, errMsg)) {
      LOGW("BW: prelogin failed: %s\r\n", errMsg.c_str());
      tls.stop();
      sendErr(BW_ERR_MALFORMED_KDF, errMsg);
      masterPassword = String();
      return;
    }
  } else {
    kdf.type = kdfTypeIn; kdf.iterations = kdfIterationsIn; kdf.memoryKiB = kdfMemoryIn; kdf.parallelism = kdfParallelismIn;
  }

  if (kdf.type != 0) { // 0 = PBKDF2-SHA256; 1 = Argon2id, not yet supported
    LOGW("BW: account uses Argon2id - not supported by this relay build yet");
    tls.stop();
    sendErr(BW_ERR_MALFORMED_KDF, "this account uses Argon2id - not yet supported by this relay build");
    masterPassword = String();
    return;
  }

  LOGV("BW: deriving master key (PBKDF2-SHA256, %lu iterations)\r\n", (unsigned long) kdf.iterations);
  bwSendSyncStatus("Iterating...");

  BwCryptoContext ctx;
  bwDeriveMasterKey(masterPassword, email, kdf.iterations, ctx.masterKey);
  LOGV("BW: [t+%lums] PBKDF2 done\r\n", millis() - tSyncStart);
  bwSendSyncStatus("Fetching vault...");  // the Palm's own pre-SYNC label for this whole phase (Sync.c)
  String masterPasswordHash = bwDeriveMasterPasswordHash(ctx.masterKey, masterPassword);
  bwDeriveStretchedKeys(ctx.masterKey, ctx.stretchedEncKey, ctx.stretchedMacKey);
  masterPassword = String(); // raw password no longer needed - drop the RAM copy now

  // 2FA round trip (Protocol-wire.md TWO_FACTOR_REQUIRED/TWO_FACTOR_SUBMIT): bwLogin is stateless per call, so this loop retries it with the code the Palm just submitted, up to a bound, without
  // repeating the TLS connect, prelogin and PBKDF2 above (still valid; only the token exchange is retried).
  // If the Palm sent a remembered "trust this device" token, the first attempt uses it (provider 5, Bitwarden's Remember pseudo-provider): an account that already completed 2FA from this Palm, with
  // an unexpired token, succeeds with no prompt, as every real Bitwarden client. If it is rejected (expired or invalid), errCode is BW_ERR_TWO_FACTOR_REQUIRED as for a wrong interactive code and
  // falls into the normal TWO_FACTOR_REQUIRED prompt cycle; no special case, since "failed but within budget" is treated alike whatever the reason.
  String accessToken, keyEncString;
  uint8_t errCode = 0;
  String twoFactorToken = rememberedTwoFactorToken;
  uint8_t twoFactorProvider = rememberedTwoFactorToken.length() ? 5 : 0;
  String newRememberToken; // what to report back in SYNC_DONE - empty = no change
  bool loggedIn = false;
  static const int kMaxTwoFactorAttempts = 3;
  for (int attempt = 0; ; attempt++) {
    // Reconnect before every attempt, not only the first: this loop reuses the tls connection opened before prelogin, and a long gap (a high-iteration PBKDF2, or a human-timescale 2FA wait) can
    // make the server or a proxy close it, failing the next request at the transport level. Cheap against the rest of the flow, and the request always gets a live socket. The same reconnect and re-verify
    // as the sync-retry loop's recovery.
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
      // Logs unconditionally (not only when outTfToken is present), so a login that succeeds via provider 5 (the remembered token) also shows this branch is reached, telling "never runs" from "the server
      // legitimately returns no TwoFactorToken even on success".
      if (logOn(LOG_VERBOSE)) {
        logPrintf(LOG_VERBOSE, "BW: login succeeded (attempt %d, provider used=%u), TwoFactorToken %s",
                      attempt, twoFactorProvider, outTfToken.length() ? "present, length=" : "ABSENT");
        if (outTfToken.length()) logPrintf(LOG_VERBOSE, "%u", (unsigned) outTfToken.length());
        logPrintf(LOG_VERBOSE, "\r\n");
      }
      if (outTfToken.length()) newRememberToken = outTfToken;
      break;
    }
    if (errCode != BW_ERR_TWO_FACTOR_REQUIRED || attempt >= kMaxTwoFactorAttempts) break;

    LOGI("BW: two-factor required (provider %u: %s)\r\n", rxProvider, rxLabel.c_str());
    {
      BwWriter w(gFrameBody, sizeof(gFrameBody));
      w.u8(rxProvider);
      w.str(rxLabel);
      bwWriteFrame(BW_TWO_FACTOR_REQUIRED, gFrameBody, w.len);
    }

    uint8_t rxType; size_t rxLen;
    if (!bwReadFrame(rxType, gFrameBody, sizeof(gFrameBody), rxLen, RelayTiming::BW_TWO_FACTOR_TIMEOUT_MS)) {
      errCode = BW_ERR_AUTH_FAILED; errMsg = "timed out waiting for the two-factor code";
      break;
    }
    if (rxType == BW_BYE) {
      LOGI("BW: two-factor entry cancelled by Palm");
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
    LOGI("BW: retrying login with submitted two-factor code");
  }
  if (!loggedIn) {
    LOGW("BW: login failed: %s\r\n", errMsg.c_str());
    sendErr(errCode, errMsg);
    memset(&ctx, 0, sizeof(ctx));
    tls.stop();
    return;
  }
  masterPasswordHash = String();
  // errMsg/errCode may still hold BW_ERR_TWO_FACTOR_REQUIRED from the loop's first, expected-to-fail bwLogin attempt: loggedIn == true means it was resolved. Clear them so nothing downstream
  // (bwSyncAndStream reuses these variables) takes the stale value for a new error.
  errMsg = String();
  errCode = 0;

  String userKeyRaw;
  if (!bwDecryptEncString(keyEncString, ctx.stretchedEncKey, ctx.stretchedMacKey, userKeyRaw) || userKeyRaw.length() < 64) {
    LOGW("BW: failed to unwrap the account key (likely wrong master password)");
    sendErr(BW_ERR_DECRYPT_FAILED, "failed to unwrap the account key (likely wrong master password)", newRememberToken);
    memset(&ctx, 0, sizeof(ctx));
    tls.stop();
    return;
  }
  memcpy(ctx.userEncKey, userKeyRaw.c_str(), 32);
  memcpy(ctx.userMacKey, userKeyRaw.c_str() + 32, 32);
  userKeyRaw = String();

  LOGI("BW: [t+%lums] authenticated, fetching /api/sync\r\n", millis() - tSyncStart);

  // Retries here cover a Vaultwarden-side stream stall or malformed JSON that outlasts BwJsonScanner's own refill retry. Login and PBKDF2 happen once, so a retry only redoes the /api/sync fetch on a
  // fresh TLS connection (a mid-body failure leaves the old one's HTTP framing desynced), then bwSyncAndStream. itemCount/folderCount/orgCount and skipFolders/skipOrgs/skipCiphers are declared outside
  // the loop and accumulate across attempts (see bwStreamSyncBody): a retry resumes past what was already sent and acked, with no duplicate Palm records.
  // Item, folder and org acks retry in place (bwWriteFrameAcked, 5 attempts, no reconnect) and are never BW_ERR_SYNC_FETCH_FAILED, so this loop fires only for the stall and malformed-JSON failures,
  // where reconnecting to Vaultwarden still makes sense.
  uint16_t itemCount = 0, folderCount = 0, orgCount = 0;
  uint16_t skipFolders = 0, skipOrgs = 0, skipCiphers = 0;
  static const int kMaxSyncFetchAttempts = 3; // 1 initial + up to 2 retries
  bool ok = false;
  for (int attempt = 0; attempt < kMaxSyncFetchAttempts; attempt++) {
    if (attempt > 0) {
      tls.stop();
      delay(1000);  // a brief backoff, so a struggling network or server is not hammered
      if (!tls.connect(host.c_str(), port) || !tls.verify(fpHex, nullptr)) {
        errCode = BW_ERR_CONNECT_FAIL; errMsg = "reconnect for sync retry failed";
        break;
      }
      if (logOn(LOG_VERBOSE)) {
        logPrintf(LOG_VERBOSE, "BW: retrying /api/sync (attempt %d/%d) - already have %u folder(s), %u org(s), %u item(s)\r\n",
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
    gBwSessFail++;
    LOGW("BW: sync failed: %s\r\n", errMsg.c_str());
    sendErr(errCode, errMsg, newRememberToken);
    return;
  }

  BwWriter w(gFrameBody, sizeof(gFrameBody));
  w.u16(itemCount); w.u16(folderCount); w.u16(orgCount);
  w.str(newRememberToken);  // empty = no change (Protocol-wire.md SYNC_DONE)
  bwEmitFinal(BW_SYNC_DONE, gFrameBody, w.len);
  // Fetched and handed over; whether the Palm took it is known once the queue has drained.
  gBwProdOk = true;
  gBwProdItems = itemCount; gBwProdFolders = folderCount; gBwProdOrgs = orgCount;
  LOGI("BW: fetched %u item(s), %u folder(s), %u org(s) - sending to the Palm\r\n", itemCount, folderCount, orgCount);
}

// After the whole sync has gone to the Palm: log what arrived and add it to the session totals.
void bwReportDelivery() {
  if (!gBwProdOk) return;
  gBwProdOk = false;
  bool cut = false;
#if WIRSA_BW_QUEUE
  cut = gQ.cancel;
#endif
  unsigned lost = gBwSkippedItems;
  unsigned got = gBwProdItems > lost ? gBwProdItems - lost : 0;
  if (!cut && lost == 0) {
    gBwSessOk++;
    LOGI("BW: synced %u item(s), %u folder(s), %u org(s) to the Palm\r\n", gBwProdItems, gBwProdFolders, gBwProdOrgs);
  } else {
    gBwSessFail++;
    LOGW("BW: sync incomplete - %u of %u item(s) reached the Palm%s\r\n", got, gBwProdItems,
         cut ? " (the Palm stopped answering)" : "");
  }
  gBwSessItems += got;
}

// The default loop() task's stack (8 KB) is too small: a recursive-descent JSON parse on top of a deep TLS/HTTPClient/PBKDF2 chain, on a multi-MB /api/sync, can overflow it. The whole sync handler
// runs on a dedicated FreeRTOS task with a larger stack. The mode is already synchronous and modal (bitwardenRelayLoop services nothing else mid-sync), so blocking the caller on a binary semaphore
// until the task ends is the same externally as calling bwHandleSync() inline, with headroom.
static SemaphoreHandle_t gSyncTaskDone = nullptr;
static const uint8_t *gSyncTaskBody = nullptr;
static size_t gSyncTaskBodyLen = 0;

static const uint32_t BW_SYNC_STACK = 16384;   // measured peak 7.0 KB

void bwHandleSyncTask(void *) {
  bwHandleSync(gSyncTaskBody, gSyncTaskBodyLen);
  LOGI("BW: sync task stack used at most %u of %u bytes\r\n",
       (unsigned) (BW_SYNC_STACK - uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t)), (unsigned) BW_SYNC_STACK);
#if WIRSA_BW_QUEUE
  if (gQ.active) { gQ.done = true; xSemaphoreGive(gQ.avail); }   // wakes the consumer
#endif
  xSemaphoreGive(gSyncTaskDone);
  vTaskDelete(NULL);
}

// body must outlive the task: gFrameBody, the only caller's buffer, is a static file-scope buffer and is fully consumed here before bitwardenRelayLoop() reads another frame into it (the caller blocks until done).
void bwHandleSyncBigStack(const uint8_t *body, size_t bodyLen) {
  if (!gSyncTaskDone) gSyncTaskDone = xSemaphoreCreateBinary();
  gSyncTaskBody = body;
  gSyncTaskBodyLen = bodyLen;
  TaskHandle_t h = nullptr;
  gBwProdOk = false; gBwSkippedItems = 0;
#if WIRSA_BW_QUEUE
  gQ.active = psramFound();    // no PSRAM: the old synchronous path
  if (gQ.active) bwQReset();
  LOGV("BW: download-ahead queue %s\r\n", gQ.active ? "ON (PSRAM)" : "OFF (no PSRAM)");
#endif
  // Internal RAM when the stack is wanted (a task stack must be one internal block).
  LOGI("BW: sync start, internal heap free %u, largest block %u (stack needs %u); PSRAM free %u\r\n",
       (unsigned) heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
       (unsigned) heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL), (unsigned) BW_SYNC_STACK,
       (unsigned) ESP.getFreePsram());
  BaseType_t created = xTaskCreate(bwHandleSyncTask, "bwSync", BW_SYNC_STACK, nullptr, 1, &h);
  if (created != pdPASS) {
    // The bigger stack could not be allocated (a heap that low is its own problem): run inline rather than hang the Palm waiting for a response that never comes.
    LOGW("BW: failed to create sync task (out of memory?) - running inline\r\n");
#if WIRSA_BW_QUEUE
    gQ.active = false;       // one thread: nothing could drain a queue
#endif
    bwHandleSync(body, bodyLen);
    bwReportDelivery();
    return;
  }
#if WIRSA_BW_QUEUE
  if (gQ.active) bwQConsume();     // sends the queued frames while the task fetches
#endif
  xSemaphoreTake(gSyncTaskDone, portMAX_DELAY);
  bwReportDelivery();
}

}  // namespace BitWardenRelay
