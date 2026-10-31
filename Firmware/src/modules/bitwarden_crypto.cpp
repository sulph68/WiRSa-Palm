// bitwarden_crypto.cpp - the Bitwarden crypto: key derivation, AES-CBC, RSA-OAEP unwrapping. Part of the BitWarden Relay mode; include/bitwarden_internal.h says how the files divide the work.
#include "bitwarden_internal.h"

namespace BitWardenRelay {

// ---- Crypto primitives: mbedTLS, bundled with the ESP32 Arduino core; no vendored crypto code (small proven pieces rather than a large system from scratch) ----

void bwHmacSha256(const uint8_t *key, size_t keyLen, const uint8_t *msg, size_t msgLen, uint8_t out[32]) {
  const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  mbedtls_md_context_t ctx;
  mbedtls_md_init(&ctx);
  mbedtls_md_setup(&ctx, info, 1 /* hmac */);
  mbedtls_md_hmac_starts(&ctx, key, keyLen);
  mbedtls_md_hmac_update(&ctx, msg, msgLen);
  mbedtls_md_hmac_finish(&ctx, out);
  mbedtls_md_free(&ctx);
}

// PBKDF2-HMAC-SHA256, hand-rolled on mbedtls_md's HMAC context (not mbedtls_pkcs5_pbkdf2_hmac[_ext]) so it compiles unchanged whichever mbedTLS 2.x/3.x API a given arduino-esp32 core exposes: this
// sandbox cannot compile-test against real hardware, so the portable primitive wins over a faster but version-fragile call.
void bwPbkdf2Sha256(const uint8_t *pw, size_t pwLen, const uint8_t *salt, size_t saltLen,
                            uint32_t iterations, uint8_t out[32]) {
  const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  mbedtls_md_context_t ctx;
  mbedtls_md_init(&ctx);
  mbedtls_md_setup(&ctx, info, 1);

  // single 32-byte output (dkLen == hLen): RFC 8018 block index 1 only
  uint8_t saltBlock[256];
  size_t sLen = saltLen < (sizeof(saltBlock) - 4) ? saltLen : (sizeof(saltBlock) - 4);
  memcpy(saltBlock, salt, sLen);
  saltBlock[sLen+0] = 0; saltBlock[sLen+1] = 0; saltBlock[sLen+2] = 0; saltBlock[sLen+3] = 1;

  uint8_t u[32], t[32];
  mbedtls_md_hmac_starts(&ctx, pw, pwLen);
  mbedtls_md_hmac_update(&ctx, saltBlock, sLen + 4);
  mbedtls_md_hmac_finish(&ctx, u);
  if (pwLen <= 119) {
    // the remaining iterations: pbkdf2_fast.cpp (key pads hashed once, two SHA-256 blocks each)
    mbedtls_md_free(&ctx);
    uint32_t t0 = millis();
    pbkdf2Sha256Iterate(pw, pwLen, u, iterations, out);
    if (logOn(LOG_INFO) && iterations > 1000)
      logPrintf(LOG_INFO, "BW: PBKDF2 %lu iterations in %lu ms (%s)\r\n", (unsigned long) iterations,
                    (unsigned long) (millis() - t0), pbkdf2UsedHardware() ? "SHA hardware" : "software");
    return;
  }
  // A very long password: the generic (slow) loop.
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

// RFC 5869 HKDF-Expand only (PRK supplied, no Extract step), Bitwarden's construction: PRK = masterKey (32 bytes), info = "enc" or "mac" (ASCII), one 32-byte block T(1).
void bwHkdfExpandSha256(const uint8_t prk[32], const char *info, uint8_t out[32]) {
  size_t infoLen = strlen(info);
  uint8_t buf[64];
  memcpy(buf, info, infoLen);
  buf[infoLen] = 0x01;
  bwHmacSha256(prk, 32, buf, infoLen + 1, out);
}

bool bwBase64Decode(const String &in, uint8_t *out, size_t outCap, size_t &outLen) {
  size_t olen = 0;
  int rc = mbedtls_base64_decode(out, outCap, &olen, (const unsigned char *) in.c_str(), in.length());
  if (rc != 0) return false;
  outLen = olen;
  return true;
}

String bwBase64Encode(const uint8_t *in, size_t inLen) {
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

bool bwAesCbcDecrypt(const uint8_t key32[32], const uint8_t iv16[16],
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

// Bitwarden EncString: "{encType}.{b64 iv}|{b64 ct}|{b64 mac}". encType 2 = AES-256-CBC + HMAC-SHA256, the only type a personal vault uses and the only one decrypted here; anything else is a
// decrypt failure, not guessed at.
bool bwDecryptEncString(const String &encString, const uint8_t encKey[32], const uint8_t macKey[32], String &plainOut) {
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

// ---- organization key unwrapping (RSA-OAEP) ----
// Organization-owned items are encrypted under the organization's symmetric key, wrapped for this user by RSA, not the AES-CBC+HMAC EncString of personal items, so a personal-key decrypt of an org
// item fails the HMAC or gives garbage.
// The chain: profile.privateKey is the user's RSA private key, wrapped the normal way (type "2." EncString under the user's encKey/macKey; bwDecryptEncString handles it). Each
// profile.organizations[].key is that ORG's 64-byte symmetric key (encKey+macKey), RSA-OAEP-wrapped under the user's RSA PUBLIC key: type "3." (OAEP-SHA256) or "4." (OAEP-SHA1), Bitwarden's two
// RSA EncString variants. Unlike "2." there is no separate iv/mac: OAEP is one ciphertext, integrity-checked by its own padding.
bool bwRsaOaepUnwrap(mbedtls_pk_context &pk, const String &wrapped, uint8_t out[64]) {
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

// Parses profile.privateKey (already unwrapped to raw PKCS8 DER by bwDecryptEncString) into an mbedtls_pk_context reused for every org's key in one sync pass. The caller must mbedtls_pk_free(pk)
// (see bwStreamSyncBody).
bool bwParsePrivateKey(const uint8_t *der, size_t derLen, mbedtls_pk_context &pk) {
  // This mbedtls's mbedtls_pk_parse_key takes no rng args (the older, pre-blinding signature); an unencrypted DER key needs no password.
  return mbedtls_pk_parse_key(&pk, der, derLen, NULL, 0) == 0;
}

String urlEncode(const String &s) {
  String out; out.reserve(s.length() * 3);
  const char *hex = "0123456789ABCDEF";
  for (size_t i = 0; i < s.length(); i++) {
    uint8_t c = (uint8_t) s[i];
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') out += (char) c;
    else { out += '%'; out += hex[c >> 4]; out += hex[c & 0xF]; }
  }
  return out;
}

// ---- Master-password derivation (Bitwarden's construction) ----


void bwDeriveMasterKey(const String &password, const String &email, uint32_t iterations, uint8_t out[32]) {
  String emailLower = email; emailLower.toLowerCase();
  bwPbkdf2Sha256((const uint8_t *) password.c_str(), password.length(),
                 (const uint8_t *) emailLower.c_str(), emailLower.length(), iterations, out);
}

String bwDeriveMasterPasswordHash(const uint8_t masterKey[32], const String &password) {
  uint8_t hash[32];
  bwPbkdf2Sha256(masterKey, 32, (const uint8_t *) password.c_str(), password.length(), 1, hash);
  return bwBase64Encode(hash, 32);
}

void bwDeriveStretchedKeys(const uint8_t masterKey[32], uint8_t encKeyOut[32], uint8_t macKeyOut[32]) {
  bwHkdfExpandSha256(masterKey, "enc", encKeyOut);
  bwHkdfExpandSha256(masterKey, "mac", macKeyOut);
}

}  // namespace BitWardenRelay
