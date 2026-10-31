// bitwarden_login.cpp - prelogin and login (token request, two-factor provider choice). Part of the BitWarden Relay mode; include/bitwarden_internal.h says how the files divide the work.
#include "bitwarden_internal.h"

namespace BitWardenRelay {

// ---- Vaultwarden HTTP calls ----
// All three share one already-connected, pin-verified WiFiClientSecure (see bwHandleSync): HTTPClient's connect() reuses a client that is already connected(), so the whole SYNC is one TLS session.


bool bwPrelogin(WiFiClientSecure &tls, const String &host, const String &email, BwKdfParams &kdf, String &errOut) {
  HTTPClient http;
  if (!http.begin(tls, "https://" + host + "/identity/accounts/prelogin")) { errOut = "prelogin begin() failed"; return false; }
  http.addHeader("Content-Type", "application/json");
  JsonDocument reqDoc;
  reqDoc["email"] = email;
  String body; serializeJson(reqDoc, body);
  int code = http.POST(body);
  if (code < 0) { errOut = "prelogin connect failed (" + String(code) + ")"; http.end(); return false; }
  if (code != 200) { errOut = "prelogin HTTP " + String(code); http.end(); return false; }
  // tls directly, not http.getStream() (see bwSyncAndStream): getStream() can silently substitute an empty Stream when its connected() bookkeeping disagrees with reality. This response is tiny (KDF
  // params), so it was never at risk of the /api/sync NoMemory bug, but it shares the fragility; fixed for consistency, never seen failing.
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

// Examines a 400 body with TwoFactorProviders/TwoFactorProviders2 (2FA is enabled, or the code just submitted was wrong: Vaultwarden uses one shape for both, see Protocol-wire.md) and picks ONE
// provider to ask the Palm for (Authenticator app = 0, else Email = 1, else the first listed) plus a short label. v1 has no "pick your 2FA method" UI, so it always auto-picks.
void bwPickTwoFactorProvider(JsonDocument &doc, uint8_t &outProvider, String &outLabel) {
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
  outProvider = (uint8_t) (provider < 0 ? 0 : provider);  // defensive: the caller already checked TwoFactorProviders(2) is present
}

// twoFactorToken/twoFactorProvider are empty/0 on a fresh attempt (2FA never completed from this Palm, or the remembered token was rejected); bwHandleSync seeds them from the Palm's stored
// "remember this device" token (provider 5) before any interactive prompt and refills them from a TWO_FACTOR_SUBMIT on retry. outTwoFactorProvider/outTwoFactorLabel mean something only when this
// returns false with errCode == BW_ERR_TWO_FACTOR_REQUIRED. outNewTwoFactorToken is set only when a SUCCESSFUL response includes a TwoFactorToken; the caller reports it to the Palm to keep
// (SYNC_DONE's updatedTwoFactorToken, Protocol-wire.md). twoFactorRemember=1 is always sent (not only on an interactive code) so a login with a still-valid remembered token renews it, as every real Bitwarden client.
bool bwLogin(WiFiClientSecure &tls, const String &host, const String &email, const String &masterPasswordHashB64,
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
    // Vaultwarden's two_factor_remember is Option<i32>, not a bool: the literal "1" is required ("true" fails to parse, counts as absent, and no remember token is issued). camelCase field names are
    // fine: Vaultwarden matches them case-insensitively against its snake_case names.
    form += "&twoFactorToken=" + urlEncode(twoFactorToken) +
            "&twoFactorProvider=" + String(twoFactorProvider) +
            "&twoFactorRemember=1";
  }
  int code = http.POST(form);
  if (code < 0) { errCode = BW_ERR_CONNECT_FAIL; errOut = "token connect failed (" + String(code) + ")"; http.end(); return false; }

  if (code == 400) {
    // tls directly, not http.getStream() (see bwSyncAndStream).
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

  // tls directly, not http.getStream() (see bwSyncAndStream).
  JsonDocument doc;
  DeserializationError je = deserializeJson(doc, tls);
  http.end();
  if (je) { errCode = BW_ERR_AUTH_FAILED; errOut = "token JSON parse failed: " + String(je.c_str()); return false; }

  accessToken = doc["access_token"] | "";
  keyEncString = doc["Key"] | "";
  outNewTwoFactorToken = doc["TwoFactorToken"] | ""; // present only when twoFactorRemember=1 was honored
  // Key names only, never values (access_token, Key and refresh_token are live session secrets): a missing expected field can be diagnosed from the debug log without exposing session data.
  if (logOn(LOG_VERBOSE) && twoFactorToken.length()) {
    String keys;
    for (JsonPair kv : doc.as<JsonObject>()) { keys += ' '; keys += kv.key().c_str(); }
    logPrintf(LOG_VERBOSE, "BW: token response keys:%s", keys.c_str());
  }
  if (accessToken.length() == 0 || keyEncString.length() == 0) {
    errCode = BW_ERR_AUTH_FAILED; errOut = "token response missing access_token/Key";
    return false;
  }
  return true;
}

// A large vault's /api/sync (multiple MB) can exceed the usable heap. ArduinoJson's Filter bounds only which fields are kept per array element; it still holds the whole filtered ciphers array in one
// JsonDocument, so enough items run out of memory whatever the filter. Peak memory must instead be bounded by ONE cipher at a time, the "decrypt one item, send it, discard it" discipline used toward the
// Palm (bwWriteFrameAcked). ArduinoJson has no streaming-array API, so this is a small hand-written scanner: BwJsonScanner reads bytes off the live stream (tls directly, see bwSyncAndStream);
// bwScanJsonValue finds the start and end of one JSON value by tracking brace/bracket depth and string boundaries (escaped quotes included) without unescaping what is inside. Each captured raw
// value goes to deserializeJson() for a small single-item parse, reusing its unescaping and field extraction, once per cipher instead of once for the array.
// Reading one byte at a time off a multi-MB stream multiplies the per-call mbedTLS/lwIP overhead enough to plausibly trip a per-read idle timeout on a slow patch. Buffering cuts the real stream reads
// by up to 512x: a refill is one readBytes() for whatever is available (not a fixed wait), so it adds no latency, it only removes the per-byte overhead.

}  // namespace BitWardenRelay
