// bitwarden_vault.cpp - the vault: a streaming JSON scan of /sync, one cipher at a time, decrypted and handed on. Part of the BitWarden Relay mode; include/bitwarden_internal.h says how the files divide the work.
#include "bitwarden_internal.h"

namespace BitWardenRelay {

// Captures the exact source text of one JSON value (object, array, string, number, true, false, null) from the next non-whitespace byte into out, verbatim and unescaped.
// maxLen is a ceiling: an item that exceeds it should fail loudly, not be truncated (which would corrupt what is parsed from out next).
bool bwScanJsonValue(BwJsonScanner &sc, String &out, size_t maxLen) {
  out = String();
  int c = sc.nextNonWs();
  if (c < 0) {
    LOGW("BW: bwScanJsonValue: stream ended before any value started");
    return false;
  }

  if (c == '"') {
    out += (char) c;
    bool escape = false;
    for (;;) {
      int d = sc.next();
      if (d < 0) {
        LOGW("BW: bwScanJsonValue: stream ended mid-string (%u bytes captured)\r\n", (unsigned) out.length());
        return false;
      }
      out += (char) d;
      if (out.length() > maxLen) {
        LOGW("BW: bwScanJsonValue: string exceeded %u-byte cap\r\n", (unsigned) maxLen);
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
    // Reserve a starting size: growing byte by byte makes a full cipher object (routinely 1-2 KB) reallocate many times on a heap already fragmented by hundreds of per-item
    // allocations. A capacity hint only; it cannot fix low memory, but cuts the realloc count.
    out.reserve(512);
    out += open;
    int depth = 1;
    bool inString = false, escape = false;
    for (;;) {
      int d = sc.next();
      if (d < 0) {
        LOGW("BW: bwScanJsonValue: stream ended mid-value (%u bytes captured, depth=%d)\r\n", (unsigned) out.length(), depth);
        return false;
      }
      out += (char) d;
      if (out.length() > maxLen) {
        LOGW("BW: bwScanJsonValue: value exceeded %u-byte cap (depth=%d) - raising kMaxSyncItemJson may be needed\r\n", (unsigned) maxLen, depth);
        return false;
      }
      // A value can reach kMaxSyncItemJson (256 KB): give the WiFi/lwIP tasks a slice periodically, not only once per item.
      if ((out.length() & 0x3FF) == 0) yield();
      if (inString) {
        if (escape) { escape = false; continue; }
        if (d == '\\') { escape = true; continue; }
        if (d == '"') inString = false;
        continue;
      }
      if (d == '"') { inString = true; continue; }
      // Only the outer bracket TYPE is counted: a differently typed bracket nested inside is balanced by the time its own close arrives, so ignoring it nets zero.
      if (d == open) depth++;
      else if (d == close) { depth--; if (depth == 0) return true; }
    }
  }

  // A bare literal (number, true, false, null) runs to a delimiter, which is pushed back: the caller expects ',', '}' or ']' next.
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
      LOGW("BW: bwScanJsonValue: bare literal exceeded %u-byte cap\r\n", (unsigned) maxLen);
      return false;
    }
  }
}

// One item's raw JSON (attachments, fields[], passwordHistory[], a long notes field, captured verbatim before re-parsing) can be large. Peak memory is one item at a time
// (bwStreamSyncBody), so 256 KB is only a ceiling against a corrupt response.
static const size_t kMaxSyncItemJson = 262144;

// Iterates a JSON array at the scanner's position (just after the array's ':'), calling fn(rawElementText) per element. False if fn returns false (e.g. a lost Palm ack) or the array is malformed or truncated.
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

// An org-owned item carries much more metadata than a personal one (collections, attachments, fields[], passwordHistory[], permissions): deserializeJson() can run out of heap
// on a full DOM of one. This filter keeps each item's peak memory to the ~30 fields used.
void bwBuildCipherFilter(JsonDocument &filter) {
  filter["deletedDate"] = true;
  filter["archivedDate"] = true;
  filter["type"] = true;
  filter["id"] = true;
  filter["folderId"] = true;
  filter["organizationId"] = true;
  filter["key"] = true;
  filter["name"] = true;
  // "notes" is not in this filter (unlike the other kept fields): see bwFindCappedStringField. It has no natural size bound and can blow the memory budget alone in an outlier
  // item; it is extracted separately, capped, after this smaller parse succeeds.
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

// "notes" needs another approach: a Filter includes or excludes a whole value and cannot cap its length. This finds "fieldName":"..." by substring search (safe only because it
// is used for a known top-level, non-nested name like "notes") and captures the raw, still JSON-escaped value up to maxLen. False (out unset) if the field is absent or its raw value is
// over maxLen: the caller skips that item's notes rather than risk another NoMemory on a value far past the Palm's bwMaxField(512) cap.
bool bwFindCappedStringField(const String &json, const char *fieldName, String &out, size_t maxLen) {
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
  // Wrap as a standalone JSON string literal and let ArduinoJson unescape it (correct, and the parse is rare and small).
  JsonDocument tmp;
  String literal = "\"" + raw + "\"";
  if (deserializeJson(tmp, literal)) return false;
  out = tmp.as<String>();
  return true;
}

// One organization's symmetric key, RSA-unwrapped from profile.organizations[].key (see bwRsaOaepUnwrap). valid is false if unwrapping failed (missing or malformed key,
// private key parse failure): that org's items then fall back to the personal key, which the Palm shows as unreadable or blank, not a sync error.

// Processes one cipher against a small per-item JsonDocument (one array element's raw text, filtered by bwBuildCipherFilter) rather than a JsonArray in one whole-response
// document. The caller (bwStreamSyncBody) builds filter once and reuses it.
bool bwProcessOneCipher(const String &itemJson, JsonDocument &filter,
                                const uint8_t userEncKey[32], const uint8_t userMacKey[32],
                                const String gFolderIds[], int folderCount,
                                const String gOrgIds[], int orgCount,
                                const BwOrgKey orgKeys[],
                                uint16_t &itemCount, String &errOut, uint8_t &errCode) {
  JsonDocument cdoc;
  DeserializationError je = deserializeJson(cdoc, itemJson, DeserializationOption::Filter(filter));
  if (je) {
    if (logOn(LOG_WARN)) {
      // Logging head and tail tells a filtered-parse failure (the tail ends in a clean '}') from bwScanJsonValue's brace tracking stopping in the wrong place (it does not).
      size_t n = itemJson.length();
      String head = itemJson.substring(0, n < 100 ? n : 100);
      String tail = itemJson.substring(n < 100 ? 0 : n - 100);
      logPrintf(LOG_WARN, "BW: malformed cipher JSON - error=%s, captured %u bytes\r\nBW:   head: %s\r\nBW:   tail: %s\r\n",
                     je.c_str(), (unsigned) n, head.c_str(), tail.c_str());
    }
    errCode = BW_ERR_SYNC_FETCH_FAILED; errOut = "malformed cipher JSON"; return false;
  }
  JsonObject c = cdoc.as<JsonObject>();

  if (!c["deletedDate"].isNull()) return true; // skip trashed items - not a failure
  // Bitwarden's per-item Archive (distinct from Trash): a non-null archivedDate means archived. Unlike a trashed item it is still forwarded, flagged, for the Palm's "Archived Items" filter (MainForm.c gArchiveFilter).
  uint8_t archived = c["archivedDate"].isNull() ? 0 : 1;

  // itemType goes to the Palm as a byte, and the Palm's switch(itemType) decides which rStr fields the tail has, but the tail's actual shape is decided here by which of
  // login/card/identity is present, not by c["type"]. Trusting c["type"] risked a mismatch (type missing or wrong while c["card"] exists) and the Palm would read the wrong number of
  // fields, desyncing every later read. Deriving itemType from the same presence check as the tail guarantees they agree (c["type"] only for note/unknown, which has no sub-object).
  uint8_t itemType = (uint8_t) (c["type"] | 1);
  if (!c["identity"].isNull()) itemType = 4;
  else if (!c["card"].isNull()) itemType = 3;
  else if (!c["login"].isNull()) itemType = 1;
  String guid = c["id"] | "";
  // Logged before the send, not only on ack, so the item the Palm chokes on is identifiable when no ack comes: itemType and which of login/card/identity is present.
  if (logOn(LOG_VERBOSE)) {
    logPrintf(LOG_VERBOSE, "BW: about to send item id=%s itemType=%u has(login=%d,card=%d,identity=%d)\r\n",
                  guid.c_str(), itemType,
                  (int) !c["login"].isNull(), (int) !c["card"].isNull(), (int) !c["identity"].isNull());
  }
  String folderId = c["folderId"].isNull() ? String() : c["folderId"].as<String>();
  uint16_t folderIndex = 0xFFFF;
  if (folderId.length()) {
    for (int i = 0; i < folderCount; i++) if (gFolderIds[i] == folderId) { folderIndex = (uint16_t) i; break; }
  }

  // An org-owned item's organizationId maps to ORG's index as folderId maps to FOLDER's: an item is personal (folderIndex meaningful) or org-owned (orgIndex), never both.
  String orgId = c["organizationId"].isNull() ? String() : c["organizationId"].as<String>();
  uint16_t orgIndex = 0xFFFF;
  if (orgId.length()) {
    for (int i = 0; i < orgCount; i++) if (gOrgIds[i] == orgId) { orgIndex = (uint16_t) i; break; }
    // Logs only items that claim an organizationId. orgIndex == 0xFFFF means it matched none of the orgCount IDs from profile.organizations: look at how gOrgIds was filled, not the Palm's filter.
    if (logOn(LOG_VERBOSE)) {
      logPrintf(LOG_VERBOSE, "BW: item orgId=%s -> orgIndex=%d (orgCount known=%d)\r\n",
                    orgId.c_str(), (int) orgIndex, orgCount);
    }
  }

  // Base key: the org's key if org-owned and unwrapped via RSA (bwRsaOaepUnwrap), else the personal key (an org item under the personal key fails its HMAC and yields nothing).
  const uint8_t *baseEncKey = userEncKey, *baseMacKey = userMacKey;
  if (orgIndex != 0xFFFF && orgKeys[orgIndex].valid) {
    baseEncKey = orgKeys[orgIndex].encKey;
    baseMacKey = orgKeys[orgIndex].macKey;
  }

  // A per-item wrapped key (cipher.key) is rare in a personal vault but real for some org-shared items. Unwrap it (AES-256-CBC+HMAC EncString, as everything) with the BASE key
  // above (an org item's own key is wrapped under the ORG's key) and use it instead of the base key for this item.
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
    // 8192 raw encrypted bytes is generous against the Palm's bwMaxField(512) cap (AES-CBC and base64 overhead make encrypted longer than plain, never near this). An outlier this
    // large is skipped for this item rather than risk another NoMemory for the whole sync.
    String notesEnc;
    if (bwFindCappedStringField(itemJson, "notes", notesEnc, 8192)) {
      bwDecryptEncString(notesEnc, itemEncKey, itemMacKey, notes);
    } else if (logOn(LOG_WARN) && itemJson.indexOf("\"notes\":\"") >= 0) {
      logPrintf(LOG_WARN, "BW: an item's notes are over 8192 encrypted bytes (about 6 KB of text) or malformed - that item's notes are left empty (the Palm keeps only 512 bytes of notes anyway)");
    }
    // The Palm's BwStoreAddItem silently drops a secure note with an empty name and empty notes (deliberate, for blank notes); this logs that case just before it happens.
    if (logOn(LOG_VERBOSE) && itemType == 2 && name.length() == 0 && notes.length() == 0) {
      logPrintf(LOG_VERBOSE, "BW: secure note id=%s has EMPTY name AND notes after extraction - "
                    "Palm will silently drop this item. raw notes field present in JSON: %s\r\n",
                    guid.c_str(), itemJson.indexOf("\"notes\":\"") >= 0 ? "yes" : "no");
    }
  }

  // Decrypts obj[key] into out if present, else leaves it empty: the repeated shape of Card's and Identity's fields (dedicated wire slots per field, see Protocol-wire.md SYNC_ITEM).
  auto field = [&](JsonObject &obj, const char *key, String &out) {
    if (!obj[key].isNull()) bwDecryptEncString(obj[key].as<String>(), itemEncKey, itemMacKey, out);
  };

  BwWriter w(gFrameBody, sizeof(gFrameBody));
  w.str(guid);
  w.u8(itemType);
  w.u16(folderIndex);
  w.u16(orgIndex);
  w.u8(archived);
  w.str(PalmText::fromUtf8(name));
  w.str(PalmText::fromUtf8(notes));

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
  // itemType 2 (secure note): nothing extra, the notes above cover it

  // Timing (USB log only): how long WiRSa spent producing this item since the previous ack ("prep": read, parse, decrypt, build) against waiting for the Palm.
  static uint32_t sLastAckAt = 0;
  uint32_t tSend = millis();
  {
    bool palmSaidBye = false;
    if (!bwStreamSend(BW_SYNC_ITEM, gFrameBody, w.len, &palmSaidBye, name.c_str())) {
      // Always terminal, not retried by bwHandleSync's reconnect-and-refetch loop: bwWriteFrameAcked already retried this frame in place up to 5 times, so this means the Palm quit
      // (palmSaidBye) or 5 real attempts failed, and the Palm has almost certainly closed its port, so a whole-fetch retry is pointless.
      errCode = BW_ERR_CONNECT_FAIL;
      errOut = palmSaidBye ? "Palm ended the session" : "Palm lost mid-sync (item ack) after 5 attempts";
      return false;
    }
  }
  itemCount++;
  if (logOn(LOG_VERBOSE)) {
    uint32_t tAck = millis();
    logPrintf(LOG_VERBOSE, "BW: item #%u '%s' type=%u notes=%u chars sent\r\n",
                   itemCount, name.c_str(), (unsigned) itemType, (unsigned) notes.length());
    if (sLastAckAt) logPrintf(LOG_VERBOSE, "BW: timing prep=%lums ack-wait=%lums\r\n",
                                   (unsigned long) (tSend - sLastAckAt), (unsigned long) (tAck - tSend));
#if WIRSA_BW_QUEUE
    if (gQ.active) logPrintf(LOG_VERBOSE, "BW: queued depth=%u bytes=%u\r\n", gQ.depth, (unsigned) gQ.bytes);
#endif
    sLastAckAt = tAck;
  }
  yield();  // a big vault can take a while: keep WiFi and the watchdog serviced between items
  return true;
}

// The ciphers array when a server lists it before "folders" and "profile" (newer Vaultwarden lists the keys alphabetically): the text is held in PSRAM, as it arrives, and
// walked once the others are in. Grows by half each time, so a vault of a few MB is a handful of reallocations, not one per byte.
static const size_t kMaxHeldCiphers = 6UL * 1024 * 1024;

struct HeldJson {
  char *p = nullptr;
  size_t n = 0, cap = 0;
  ~HeldJson() { release(); }
  void release() { if (p) { heap_caps_free(p); p = nullptr; } n = cap = 0; }
  bool put(char c) {
    if (n == cap) {
      size_t nc = cap ? cap + cap / 2 : 128 * 1024;
      if (nc > kMaxHeldCiphers) nc = kMaxHeldCiphers;
      if (nc <= cap) return false;                                                   // the cap
      char *q = (char *) heap_caps_realloc(p, nc, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
      if (!q) return false;                                                          // no PSRAM left (p is still valid and freed by release())
      p = q; cap = nc;
    }
    p[n++] = c;
    return true;
  }
};

// Copies the array at the next non-whitespace byte into h, verbatim. False if it is not an array, ends early, or does not fit.
static bool bwHoldJsonArray(BwJsonScanner &sc, HeldJson &h) {
  int c = sc.nextNonWs();
  if (c != '[') return false;
  int depth = 0;
  bool inStr = false, esc = false;
  unsigned long n = 0;
  for (;;) {
    if (!h.put((char) c)) return false;
    if (inStr) {
      if (esc) esc = false;
      else if (c == '\\') esc = true;
      else if (c == '"') inStr = false;
    } else if (c == '"') inStr = true;
    else if (c == '[' || c == '{') depth++;
    else if (c == ']' || c == '}') { if (--depth == 0) return true; }
    if ((++n & 0x3FFF) == 0) yield();                                                // a few MB: keep WiFi and the watchdog serviced
    c = sc.next();
    if (c < 0) return false;
  }
}

// The held text as a Stream, for the same scanner the live response goes through.
struct HeldStream : public Stream {
  const char *p; size_t n, i = 0;
  HeldStream(const char *text, size_t len) : p(text), n(len) {}
  int available() override { return (int) (n - i); }
  int read() override { return i < n ? (uint8_t) p[i++] : -1; }
  int peek() override { return i < n ? (uint8_t) p[i] : -1; }
  size_t readBytes(char *buf, size_t len) override { size_t k = n - i < len ? n - i : len; memcpy(buf, p + i, k); i += k; return k; }
  size_t write(uint8_t) override { return 0; }
};

static const int kMaxSyncFolders = 64;
static const int kMaxSyncOrgs = 16;

// One forward pass over the top-level /api/sync object. folders and profile.organizations must be seen (and their id-to-index maps built) before ciphers, which reference them
// by GUID, and a live stream cannot seek back. The official SyncResponseModel's order has profile and folders before ciphers and that is streamed as it arrives. A server that lists
// ciphers first (newer Vaultwarden, alphabetical keys) has the array held in PSRAM (HeldJson, at most kMaxHeldCiphers) and walked after the others, so no wrong folderIndex/orgIndex
// is ever emitted; if folders or profile never come, or the array does not fit, the sync fails with a clear error.
// skipFolders/skipOrgs/skipCiphers: how many elements of each array were already forwarded and acked by the Palm in a PRIOR attempt of this same sync (see bwHandleSync's retry loop);
// 0/0/0 on the first. itemCount/folderCount/orgCount belong to the CALLER and are not reset here: they keep accumulating across retries as the totals reported in SYNC_DONE and as the stable
// send-index of each folder/org (a retry must give the same folders/orgs the same indices, since already-sent ciphers reference them).
bool bwStreamSyncBody(Stream &s, const uint8_t userEncKey[32], const uint8_t userMacKey[32],
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
  String keysSeen;   // the top-level keys met so far, for the message when "ciphers" comes before the ones it needs
  BwJsonScanner sc(s);
  HeldJson held;
  bool heldCiphers = false;

  // The ciphers array, from the live response or from the held text: each cipher is decrypted and handed on.
  auto doCiphers = [&](BwJsonScanner &cs) -> bool {
    LOGV("BW: [t+%lums] streaming ciphers...\r\n", millis() - tSyncStart);
    JsonDocument cipherFilter;
    bwBuildCipherFilter(cipherFilter);
    // elemIndex/skipCiphers: the same retry-resume shape as folders and orgs, but it skips the whole bwProcessOneCipher call, not only the send: decrypting a cipher is real work,
    // unlike a folder or org name. Correct while the retried /api/sync returns the same items in the same order, which holds in the seconds-long window of a retry.
    // skipCiphers is advanced per item, right after each is confirmed; once after the array would leave it stale on a mid-array ack failure, and a retry would re-decrypt and re-send confirmed items.
    int elemIndex = 0;
    bool arrOk = bwStreamArray(cs, [&](String &itemJson) -> bool {
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
    return true;
  };

  // errOut/errCode are shared with bwHandleSync's login retry loop (bwLogin legitimately sets errCode = BW_ERR_TWO_FACTOR_REQUIRED on its first, expected-to-fail attempt):
  // reset them so the "if (errCode == 0) generic message" checks below do not mistake a stale value for one set here.
  errCode = 0;
  errOut = String();

  {
    int first = sc.nextNonWs();
    if (first != '{') {
      // Logs the first byte seen and the elapsed time, to tell a real stall (first == -1: BwJsonScanner's read timed out after the headers; the server has not started the body)
      // from wrong bytes (leading garbage or BOM, an HTML error page despite HTTP 200, undechunked framing, an empty body).
      if (logOn(LOG_VERBOSE)) {
        logPrintf(LOG_VERBOSE, "BW: sync response first byte: %d ('%c') - expected '{' (0x7B) - t+%lums since sync start\r\n",
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
    if (keysSeen.length() < 120) { if (keysSeen.length()) keysSeen += ','; keysSeen += key.substring(0, 24); }

    if (key == "folders") {
      // elemIndex: the position within THIS attempt's array walk, reset on every call (folderCount persists across retries as the stable send-index). Folders before skipFolders were
      // sent and acked in a prior attempt; they are still parsed and decrypted (so gFolderIds is complete for cipher folderIndex lookups) but not re-sent, so a retry creates no duplicate records.
      // skipFolders is advanced here per folder, as each is confirmed (acked or already skipped); advancing once after the array would leave it stale on a mid-array ack failure and a retry
      // would re-send acked folders.
      int elemIndex = 0;
      // folderAckFailed tells an ack failure (bwWriteFrameAcked already retried 5x in place, so it is terminal) from a malformed-JSON element (genuinely retryable; see BW_SYNC_ITEM's
      // comment on why the outer loop no longer retries ack failures).
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
            w.str(PalmText::fromUtf8(name));
            if (!bwStreamSend(BW_FOLDER, gFrameBody, w.len, &folderPalmSaidBye)) { folderAckFailed = true; return false; }
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
          // Organizations are not end-to-end encrypted in Bitwarden (the server manages membership and metadata): relay the name as /api/sync returns it, no decrypt.
          // elemIndex/alreadySent: the same retry-resume shape as "folders", including advancing skipOrgs per item (see that comment for the duplicate-record bug).
          int elemIndex = 0;
          // orgAckFailed: as folderAckFailed above, tracked apart from a malformed-JSON element.
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
                w.str(PalmText::fromUtf8(name));
                if (!bwStreamSend(BW_ORG, gFrameBody, w.len, &orgPalmSaidBye)) { orgAckFailed = true; return false; }
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
          // Captured raw (still JSON-quoted) and unwrapped once the whole profile object is parsed: organizations may come before OR after this key and both are needed. Quotes are stripped before decrypting.
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
      // Unwrap every org's key now that profile.privateKey and profile.organizations are both known (either may come first). A failure (no privateKey, a parse failure, one org's key
      // not unwrapping) leaves that org's gOrgKeys[i].valid false, and bwProcessOneCipher falls back to the user's key rather than failing the sync over one org.
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
              LOGW("BW: org[%d] key unwrap %s\r\n", i, gOrgKeys[i].valid ? "OK" : "FAILED");
            }
          } else if (logOn(LOG_WARN)) {
            logPrintf(LOG_WARN, "BW: profile.privateKey failed to parse as an RSA key - org items will fall back to the personal key");
          }
          mbedtls_pk_free(&pk);
        } else if (logOn(LOG_WARN)) {
          logPrintf(LOG_WARN, "BW: profile.privateKey failed to decrypt - org items will fall back to the personal key");
        }
      }
      privateKeyEnc = String();	/* private key material - don't linger in RAM longer than needed */
    } else if (key == "ciphers") {
      if (!sawFolders || !sawProfile) {
        // Listed before the keys it needs: hold the array and walk it after them (below the loop).
        LOGI("BW: [t+%lums] 'ciphers' comes before 'folders'/'profile' - holding it in PSRAM until they are in\r\n", millis() - tSyncStart);
        if (!bwHoldJsonArray(sc, held)) {
          errCode = BW_ERR_SYNC_FETCH_FAILED;
          errOut = "the ciphers could not be held (" + String((unsigned) held.n) + " bytes read, PSRAM free " + String((unsigned) ESP.getFreePsram()) + ")";
          return false;
        }
        LOGI("BW: [t+%lums] held %u bytes of ciphers (PSRAM free %u)\r\n", millis() - tSyncStart, (unsigned) held.n, (unsigned) ESP.getFreePsram());
        heldCiphers = true;
      } else if (!doCiphers(sc)) {
        return false;
      }
    } else {
      // An unknown or uninteresting top-level field (collections, sends, policies, domains, object, ...): skip its value without materialising it.
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

  if (heldCiphers) {
    if (!sawFolders || !sawProfile) {
      errCode = BW_ERR_SYNC_FETCH_FAILED;
      errOut = "sync response lacks 'folders'/'profile' (keys seen: " + keysSeen + ")";
      return false;
    }
    HeldStream hs(held.p, held.n);
    BwJsonScanner hsc(hs);
    if (!doCiphers(hsc)) return false;
    held.release();
  }

  return true;
}

bool bwSyncAndStream(WiFiClientSecure &tls, const String &host, const String &accessToken,
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
  // /api/sync is unbounded (a real account measured 2.2 MB), and PBKDF2 alone has taken over 90 s on this device profile, so a short read timeout is its own failure mode.
  // 45 s for both the HTTPClient and the socket timeout (two timers, both need raising).
  http.setTimeout(45000);
  int code = http.GET();
  if (code < 0) { errCode = BW_ERR_CONNECT_FAIL; errOut = "sync connect failed (" + String(code) + ")"; http.end(); return false; }
  if (code != 200) { errCode = BW_ERR_SYNC_FETCH_FAILED; errOut = "sync HTTP " + String(code); http.end(); return false; }
  LOGV("BW: [t+%lums] /api/sync HTTP 200, Content-Length=%d, connected=%d, streaming item-by-item\r\n",
                               millis() - tSyncStart, http.getSize(), (int) http.connected());

  // Parses straight off tls (the WiFiClientSecure this whole bwHandleSync call owns), not http.getStream(), which can return an empty stream on a live, connected() socket.
  bool ok = bwStreamSyncBody(tls, userEncKey, userMacKey, itemCount, folderCount, orgCount,
                              skipFolders, skipOrgs, skipCiphers, errOut, errCode, tSyncStart);
  http.end();
  if (!ok && errOut.length() == 0) { errCode = BW_ERR_SYNC_FETCH_FAILED; errOut = "sync failed"; }
  return ok;
}

}  // namespace BitWardenRelay
