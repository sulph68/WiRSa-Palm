// mesh_admin.cpp - Mesh Relay mode: changing the node's settings (admin messages), and reading them afresh.
#include "mesh_internal.h"

#if WIRSA_FEATURE_MESH

#include "serial_io.h"
#include "display_menu.h"
#include "network.h"
#include "palm_ctl.h"
#include "log.h"
#include "settings.h"
#include "relay_timing.h"
#include "palm_text.h"
#include <mbedtls/base64.h>
#include <esp_random.h>

namespace MeshRelay {

// ---- changing the node's settings (admin messages) ------------------------------------------------

uint8_t indexOf(const uint8_t *tab, size_t n, uint32_t code) {
  for (size_t i = 0; i < n; i++) if (tab[i] == code) return (uint8_t) i;
  return 0;
}

// Reads the node's frames for a while (the admin answers come that way); texts and the rest go to the store.
bool pollAdmin(unsigned long ms, bool wantKey) {
  unsigned long until = millis() + ms;
  while ((long) (millis() - until) < 0) {
    size_t n;
    if (!MeshBle::read(gRadio, sizeof gRadio, n)) return false;
    if (n == 0) { vTaskDelay(pdMS_TO_TICKS(40)); continue; }
    MeshPb::FromRadio fr;
    if (!MeshPb::parseFromRadio(gRadio, n, fr)) continue;
    if (fr.kind == MeshPb::K_PACKET && fr.packet.decoded && fr.packet.port == MeshPb::PORT_ADMIN) {
      if (fr.packet.payload && MeshPb::adminPasskey(fr.packet.payload, fr.packet.payloadLen, gKey, gKeyLen) && wantKey) return true;
      continue;
    }
    CoreLock lk;
    MeshCore::onFromRadio(fr, utcNow());
  }
  return false;
}

uint32_t gAdminId = 0;                             // the packet id of the last admin message sent

bool adminSend(uint32_t me, const uint8_t *adm, size_t al) {
  uint8_t tr[420];
  gAdminId = esp_random() | 1;
  size_t n = MeshPb::buildAdmin(tr, sizeof tr, me, gAdminId, adm, al);
  if (!n || !MeshBle::write(tr, n)) return false;
  vTaskDelay(pdMS_TO_TICKS(150));                  // the node handles one admin message at a time
  return true;
}

// The node's routing answer to the last admin message (what a client waits for before sending the next: an edit
// transaction that is not open yet makes the node save and restart at the very next setting). Returns true on an
// acknowledgement; frames that are not it go to the store as usual.
bool adminAck(const char *what, unsigned long ms) {
  unsigned long start = millis(), until = start + ms;
  while ((long) (millis() - until) < 0) {
    size_t n;
    if (!MeshBle::read(gRadio, sizeof gRadio, n)) { LOGW("Mesh: %s: the link went while waiting for the answer", what); return false; }
    if (n == 0) { vTaskDelay(pdMS_TO_TICKS(30)); continue; }
    MeshPb::FromRadio fr;
    if (!MeshPb::parseFromRadio(gRadio, n, fr)) continue;
    if (fr.kind == MeshPb::K_PACKET && fr.packet.decoded && fr.packet.port == MeshPb::PORT_ROUTING && fr.packet.requestId == gAdminId) {
      LOGV("Mesh: %s: the node answered after %lu ms (error %u)", what, millis() - start, (unsigned) fr.packet.routeError);
      return fr.packet.routeError == 0 || fr.packet.routeError == 0xFF;
    }
    if (fr.kind == MeshPb::K_PACKET && fr.packet.decoded && fr.packet.port == MeshPb::PORT_ADMIN) continue;   // an admin answer: nothing for the store
    CoreLock lk;
    MeshCore::onFromRadio(fr, utcNow());
  }
  LOGW("Mesh: %s: no answer from the node in %lu ms", what, ms);
  return false;
}

bool getSessionKey(uint32_t me) {
  uint8_t adm[8];
  size_t al = MeshPb::adminGetConfig(adm, sizeof adm, 8);
  gKeyLen = 0;
  if (!al || !adminSend(me, adm, al)) return false;
  pollAdmin(3000, true);                            // no key is fine on a node that does not ask for one
  return true;
}

// Asks the node for its settings again (config only) and waits for the end marker, so what the Palm shows is what
// the node has now. The answers go into the store as they come.
bool cfgRead(char *why, size_t wcap) {
  if (!MeshBle::connected()) { snprintf(why, wcap, "Not linked to the node"); return false; }
  { CoreLock lk; MeshCore::forgetSettings(); }
  if (!sendWantConfig(MeshPb::NONCE_CONFIG_ONLY)) { snprintf(why, wcap, "The node did not answer"); return false; }
  // The node may still be streaming its first dump (the whole node list) when this arrives, and every frame is a
  // Bluetooth round trip: wait while frames keep coming (up to 40 s), give up only after a silence.
  unsigned long start = millis(), last = start;
  unsigned frames = 0;
  bool resent = false;
  while ((long) (millis() - start) < (long) RelayTiming::SETTINGS_READ_MAX_MS) {
    size_t len;
    if (!MeshBle::read(gRadio, sizeof gRadio, len)) { snprintf(why, wcap, "Link lost"); return false; }
    if (len == 0) {
      if (millis() - last > RelayTiming::SETTINGS_RESEND_AFTER_MS && !resent && frames == 0) {        // nothing at all: ask once more
        resent = true;
        sendWantConfig(MeshPb::NONCE_CONFIG_ONLY);
        last = millis();
      }
      if (millis() - last > RelayTiming::SETTINGS_QUIET_MS) break;
      vTaskDelay(pdMS_TO_TICKS(40));
      continue;
    }
    last = millis();
    frames++;
    MeshPb::FromRadio fr;
    if (!MeshPb::parseFromRadio(gRadio, len, fr)) continue;
    { CoreLock lk; MeshCore::onFromRadio(fr, utcNow()); }
    if (fr.kind == MeshPb::K_CONFIG_DONE && fr.configId == MeshPb::NONCE_CONFIG_ONLY) return true;
  }
  const uint8_t *raw; size_t rl;
  bool have;
  { CoreLock lk; have = MeshCore::config(MeshPb::CFG_DEVICE, raw, rl) && MeshCore::config(MeshPb::CFG_LORA, raw, rl) && MeshCore::config(MeshPb::CFG_BLUETOOTH, raw, rl); }
  LOGW("Mesh: settings read ended after %lu ms, %u frames, %s", millis() - start, frames, have ? "settings present" : "settings missing");
  if (have) return true;                                              // everything the Setup pages need arrived
  snprintf(why, wcap, "The node did not send its settings");
  return false;
}

// One admin message of a settings write: sent, then (ackMs > 0) the node's answer is waited for before the next one. False, with `why` filled,
// when it could not be sent (no message built, or the write to the node failed). `al` is the length the builder returned.
static bool adminStep(const char *what, uint32_t me, const uint8_t *adm, size_t al, unsigned long ackMs, char *why, size_t wcap) {
  if (!al || !adminSend(me, adm, al)) {
    LOGW("Mesh: settings write failed at %s (%u bytes, link %s)", what, (unsigned) al, MeshBle::connected() ? "up" : "down");
    snprintf(why, wcap, "The node did not take it");
    return false;
  }
  if (ackMs) adminAck(what, ackMs);
  return true;
}

bool cfgSet(const uint8_t *body, size_t len, char *why, size_t wcap) {
  Reader r(body, len);
  uint16_t mask = r.u16();
  String lng, sht; uint8_t reg = 0, pre = 0, hop = 3, tx = 0, role = 0, btm = 0; uint32_t pin = 0;
  if (mask & 1) { lng = r.str(); sht = r.str(); }
  if (mask & 2) { reg = r.u8(); pre = r.u8(); hop = r.u8(); tx = r.u8(); }
  if (mask & 4) role = r.u8();
  if (mask & 8) { btm = r.u8(); pin = r.u32(); }
  struct Ch { uint8_t idx, role, flags; String name, key; } ch[8];
  uint8_t nCh = 0;
  if (mask & 16) { nCh = r.u8(); if (nCh > 8) nCh = 8; for (uint8_t i = 0; i < nCh; i++) { ch[i].idx = r.u8(); ch[i].role = r.u8(); ch[i].name = r.str(); ch[i].key = r.str(); ch[i].flags = (mask & 64) ? r.u8() : 0xFF; } }   // bit 64: each channel carries its MQTT flags
  MeshPb::MqttVals mq;
  if (mask & 32) {                                             // MQTT: flags (1 enabled, 2 encrypted, 4 JSON, 8 TLS, 16 proxy, 32 map report), address, user, password, root
    uint8_t fl = r.u8();
    mq.enabled = fl & 1; mq.encrypt = fl & 2; mq.json = fl & 4; mq.tls = fl & 8; mq.proxy = fl & 16; mq.mapRep = fl & 32;
    String a = r.str(), u = r.str(), p = r.str(), t = r.str();
    size_t k;
    k = PalmText::toUtf8(a.c_str(), a.length(), mq.address, sizeof mq.address - 1); mq.address[k] = 0;
    k = PalmText::toUtf8(u.c_str(), u.length(), mq.username, sizeof mq.username - 1); mq.username[k] = 0;
    k = PalmText::toUtf8(p.c_str(), p.length(), mq.password, sizeof mq.password - 1); mq.password[k] = 0;
    k = PalmText::toUtf8(t.c_str(), t.length(), mq.root, sizeof mq.root - 1); mq.root[k] = 0;
  }
  if (!r.ok) { snprintf(why, wcap, "Bad settings request"); return false; }
  if (!MeshBle::connected()) { snprintf(why, wcap, "Not linked to the node"); return false; }

  uint32_t me; uint8_t own[sizeof(MeshPb::NodeInfo::userRaw)]; size_t ownLen = 0;
  uint8_t cfgLora[96], cfgDev[96], cfgBt[96]; size_t lLen = 0, dLen = 0, bLen = 0;
  {
    CoreLock lk;
    me = MeshCore::myNum();
    const MeshPb::NodeInfo *n0 = MeshCore::node(0);
    if (n0 && n0->userRawLen) { memcpy(own, n0->userRaw, n0->userRawLen); ownLen = n0->userRawLen; }
    const uint8_t *raw; size_t rl;
    if (MeshCore::config(MeshPb::CFG_LORA, raw, rl)) { memcpy(cfgLora, raw, rl); lLen = rl; }
    if (MeshCore::config(MeshPb::CFG_DEVICE, raw, rl)) { memcpy(cfgDev, raw, rl); dLen = rl; }
    if (MeshCore::config(MeshPb::CFG_BLUETOOTH, raw, rl)) { memcpy(cfgBt, raw, rl); bLen = rl; }
  }
  if (!me || ((mask & 1) && !ownLen) || ((mask & 2) && !lLen) || ((mask & 4) && !dLen) || ((mask & 8) && !bLen)) {
    snprintf(why, wcap, "The node's settings have not arrived yet");
    return false;
  }
  const char *err = "The node did not take it";
  getSessionKey(me);
  LOGV("Mesh: settings write: mask 0x%02X, session key %u bytes", (unsigned) mask, (unsigned) gKeyLen);
  if (mask & 32) {
    // The node turns its Bluetooth off the moment it takes an MQTT config ("Disable bluetooth until reboot", seen in its own log),
    // also inside an edit transaction, so a commit could never follow. Sent alone, outside a transaction, it saves and restarts
    // at once, which is what the other Meshtastic clients do. The link is gone after it: that is the node restarting, not a failure.
    if (mask & 31) { snprintf(why, wcap, "MQTT is saved on its own: it restarts the node"); return false; }
    uint8_t adm2[400], tmp2[320], mraw[272]; size_t mlen = 0;
    { CoreLock lk; const uint8_t *raw; size_t rl; if (MeshCore::mqttConfig(raw, rl) && rl <= sizeof mraw) { memcpy(mraw, raw, rl); mlen = rl; } }
    size_t tl = MeshPb::patchMqtt(mraw, mlen, mq, tmp2, sizeof tmp2);
    size_t al2 = tl ? MeshPb::adminSetModuleConfig(adm2, sizeof adm2, 1, tmp2, tl, gKey, gKeyLen) : 0;
    if (!al2 || !adminSend(me, adm2, al2)) { LOGW("Mesh: MQTT write failed (%u bytes, link %s)", (unsigned) al2, MeshBle::connected() ? "up" : "down"); snprintf(why, wcap, "%s", err); return false; }
    postLink(L_DOWN, "Node restarting");
    gRestartUntil = millis() + RelayTiming::NODE_RESTART_MQTT_MS;
    LOGI("Mesh: MQTT settings sent on their own; the node saves them and restarts");
    return true;
  }
  uint8_t adm[400], tmp[320];
  size_t al;
  al = MeshPb::adminFlag(adm, sizeof adm, 64, gKey, gKeyLen);                 // begin_edit_settings
  if (!adminStep("begin_edit_settings", me, adm, al, RelayTiming::ADMIN_ACK_MS, why, wcap)) return false;
  if (mask & 1) {
    char u1[48], u2[16];
    size_t a = PalmText::toUtf8(lng.c_str(), lng.length(), u1, sizeof u1 - 1); u1[a] = 0;
    size_t b = PalmText::toUtf8(sht.c_str(), sht.length(), u2, sizeof u2 - 1); u2[b] = 0;
    size_t tl = MeshPb::patchOwner(own, ownLen, u1, u2, tmp, sizeof tmp);
    al = tl ? MeshPb::adminSetOwner(adm, sizeof adm, tmp, tl, gKey, gKeyLen) : 0;
    if (!adminStep("owner", me, adm, al, RelayTiming::ADMIN_ACK_MS, why, wcap)) return false;
  }
  if (mask & 2) {
    int32_t txp = tx;
    size_t tl = MeshPb::patchLora(cfgLora, lLen, PRESET_CODE[pre < 8 ? pre : 0], REGION_CODE[reg < 14 ? reg : 0], hop, txp, tmp, sizeof tmp);
    al = tl ? MeshPb::adminSetConfig(adm, sizeof adm, MeshPb::CFG_LORA, tmp, tl, gKey, gKeyLen) : 0;
    if (!adminStep("LoRa", me, adm, al, RelayTiming::ADMIN_ACK_MS, why, wcap)) return false;
  }
  if (mask & 4) {
    size_t tl = MeshPb::patchDevice(cfgDev, dLen, ROLE_CODE[role < 9 ? role : 0], tmp, sizeof tmp);
    al = tl ? MeshPb::adminSetConfig(adm, sizeof adm, MeshPb::CFG_DEVICE, tmp, tl, gKey, gKeyLen) : 0;
    if (!adminStep("device role", me, adm, al, RelayTiming::ADMIN_ACK_MS, why, wcap)) return false;
  }
  if (mask & 8) {
    size_t tl = MeshPb::patchBluetooth(cfgBt, bLen, btm, pin, tmp, sizeof tmp);
    al = tl ? MeshPb::adminSetConfig(adm, sizeof adm, MeshPb::CFG_BLUETOOTH, tmp, tl, gKey, gKeyLen) : 0;
    if (!adminStep("Bluetooth", me, adm, al, RelayTiming::ADMIN_ACK_MS, why, wcap)) return false;
  }
  for (uint8_t i = 0; i < nCh; i++) {
    char nm[16]; size_t a = PalmText::toUtf8(ch[i].name.c_str(), ch[i].name.length(), nm, sizeof nm - 1); nm[a] = 0;
    uint8_t psk[40]; size_t pl = 0;
    if (!ch[i].key.length()) { psk[0] = 1; pl = 1; }                // an empty key means the default one, "AQ=="
    else {
      if (mbedtls_base64_decode(psk, sizeof psk, &pl, (const unsigned char *) ch[i].key.c_str(), ch[i].key.length()) != 0 ||
          (pl != 1 && pl != 16 && pl != 32)) { snprintf(why, wcap, "Channel %u: the key is not valid", (unsigned) ch[i].idx); return false; }
    }
    uint8_t sraw[96]; size_t slen = 0; bool up = false, down = false;
    {
      CoreLock lk;
      const MeshPb::Channel *cur = MeshCore::channel(ch[i].idx);
      if (cur) { memcpy(sraw, cur->raw, cur->rawLen); slen = cur->rawLen; up = cur->uplink; down = cur->downlink; }
    }
    if (ch[i].flags != 0xFF) { up = ch[i].flags & 1; down = ch[i].flags & 2; }
    size_t tl = MeshPb::patchChannelSettings(sraw, slen, psk, pl, nm, up, down, tmp, sizeof tmp);   // what the node sent stays (id, position precision...)
    al = tl ? MeshPb::adminSetChannelRaw(adm, sizeof adm, ch[i].idx, ch[i].role, tmp, tl, gKey, gKeyLen) : 0;
    if (!adminStep("channel", me, adm, al, RelayTiming::ADMIN_ACK_MS, why, wcap)) return false;
  }
  al = MeshPb::adminFlag(adm, sizeof adm, 65, gKey, gKeyLen);                 // commit_edit_settings (the node restarts)
  if (!adminStep("commit_edit_settings", me, adm, al, 0, why, wcap)) {
    if ((mask & 32) && !MeshBle::connected()) {                // the node let go after the MQTT part: it restarted on its own
      gRestartUntil = millis() + RelayTiming::NODE_RESTART_MS;
      snprintf(why, wcap, "The node restarted on the MQTT change - check Settings in a minute");
    }
    return false;
  }
  postLink(L_DOWN, "Node restarting");
  gRestartUntil = millis() + RelayTiming::NODE_RESTART_MS;                         // it can take a minute or two to answer again
  LOGI("Mesh: settings sent (mask 0x%02X, %u channel(s)); the node restarts", (unsigned) mask, (unsigned) nCh);
  return true;
}

bool cfgAct(uint8_t action, uint32_t utc, char *why, size_t wcap) {
  if (!MeshBle::connected()) { snprintf(why, wcap, "Not linked to the node"); return false; }
  uint32_t me; { CoreLock lk; me = MeshCore::myNum(); }
  if (!me) { snprintf(why, wcap, "The node has not said who it is yet"); return false; }
  getSessionKey(me);
  uint8_t adm[40];
  // 0 set the clock, 1 reboot, 2 / 3 make the node `utc` (here: its number) a favourite / not
  size_t al = action == 0 ? MeshPb::adminTime(adm, sizeof adm, utc, gKey, gKeyLen)
            : action >= 2 ? MeshPb::adminInt(adm, sizeof adm, action == 2 ? 39 : 40, utc, gKey, gKeyLen)
                          : MeshPb::adminInt(adm, sizeof adm, 97, 2, gKey, gKeyLen);
  if (!al || !adminSend(me, adm, al)) { snprintf(why, wcap, "The node did not take it"); return false; }
  if (action == 1) postLink(L_DOWN, "Node restarting");
  if (action >= 2) {
    // Marked in the store only once the node has taken it, so the Palm never shows a mark the node does not have.
    if (!adminAck(action == 2 ? "favourite" : "remove favourite", RelayTiming::ADMIN_ACK_MS)) { snprintf(why, wcap, "The node did not take it"); return false; }
    LOGI("Mesh: node %08x %s a favourite on the node", (unsigned) utc, action == 2 ? "is now" : "is no longer");
    CoreLock lk2; MeshCore::setFavorite(utc, action == 2);
  }
  return true;
}

}  // namespace MeshRelay

#endif  // WIRSA_FEATURE_MESH
