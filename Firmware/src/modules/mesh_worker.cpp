// mesh_worker.cpp - Mesh Relay mode: the node, on its own task. Everything that talks to the Meshtastic node (connect, pairing, scan, reads,
// writes, the key exchange) runs on a worker task, so the Palm line is never held up by Bluetooth. The loop task alone draws the display and
// writes to the Palm; the worker hands its results over through the flags in mesh_internal.h and the loop task picks them up (serviceWorker).
#include "mesh_internal.h"

#if WIRSA_FEATURE_MESH

#include "serial_io.h"
#include "display_menu.h"
#include "network.h"
#include "palm_ctl.h"
#include "log.h"
#include "led.h"
#include "settings.h"
#include "relay_timing.h"
#include "palm_text.h"
#include <esp_random.h>

namespace MeshRelay {

void postLink(uint8_t l, const char *text) {
  CoreLock lk;
  gLinkNew = l; snprintf(gLinkNewText, sizeof gLinkNewText, "%s", text); gLinkPending = true;
}

bool sendToRadio(const uint8_t *buf, size_t n) { return MeshBle::write(buf, n); }


// The node drops a ToRadio write that is byte-for-byte the one before it ("Drop duplicate ToRadio packet", seen in its own log), and
// the config-only request is always the same four bytes: the second read of the settings on a link, and any resend, was thrown away
// and the read ended "settings missing". A heartbeat in front makes every request a different packet.
bool sendWantConfig(uint32_t nonce) {
  uint8_t hb[8], b[16];
  size_t hn = MeshPb::buildHeartbeat(hb, sizeof hb);
  if (hn) { MeshBle::write(hb, hn); vTaskDelay(pdMS_TO_TICKS(60)); }
  size_t n = MeshPb::buildWantConfig(b, sizeof b, nonce);
  return n && MeshBle::write(b, n);
}

// Senders of texts the node list has no name for are asked for their node info, one at a time and not many per link.
static unsigned long gNameNext = 0;
static int gNameCount = 0;

static void nameTick(unsigned long now) {
  if ((long) (now - gNameNext) < 0 || gNameCount >= RelayTiming::NAME_ASK_MAX) return;
  gNameNext = now + RelayTiming::NAME_ASK_MS;
  uint8_t pkt[256]; size_t n = 0; uint32_t to = 0; bool job;
  { CoreLock lk; job = MeshCore::nameRequest(now, RelayTiming::NAME_ASK_AGAIN_MS, to, pkt, sizeof pkt, n); }
  if (!job) return;
  gNameCount++;
  LOGI("Mesh: %08lx has no name - asking for its node info", (unsigned long) to);
  sendToRadio(pkt, n);
}

// Connect to the paired node and ask for its configuration and node list.
bool workerLinkUp(bool configOnly) {
  if (!gPaired.have) return false;
  gLastAttempt = millis();
  LOGI("Mesh: connecting to %s", gPaired.name);
  if (!MeshBle::connect(gPaired.addr, gPaired.type, gPaired.pin)) {
    if ((long) (millis() - gRestartUntil) < 0) postLink(L_DOWN, "The node is restarting - try again in a minute");
    else postLink(L_FAILED, MeshBle::lastError());
    return false;
  }
  { CoreLock lk; MeshCore::resetSession(); }
  if (!sendWantConfig(configOnly ? MeshPb::NONCE_CONFIG_ONLY : MeshPb::NONCE_ALL)) { MeshBle::disconnect(); postLink(L_FAILED, "Could not talk to the node"); return false; }
  gWasUp = true;
  gNameCount = 0; gNameNext = millis() + RelayTiming::NAME_ASK_FIRST_MS;
  gConfigOnly = configOnly;
  gLastFrameAt = millis();
  postLink(L_READY, "Linked");
  return true;
}

// Drain what the node has queued. Returns false if the link broke. A waiting command is not kept waiting.
bool drainRadio(unsigned long maxMs) {
  unsigned long t0 = millis();
  for (int guard = 0; guard < 200; guard++) {
    if (uxQueueMessagesWaiting(gCmdQ)) return true;
    if (maxMs && millis() - t0 > maxMs) return true;                  // a long list from the node is taken in slices: the caller keeps its own clock
    size_t n;
    if (!MeshBle::read(gRadio, sizeof gRadio, n)) return false;
    if (n == 0) return true;
    gLastFrameAt = millis();
    MeshPb::FromRadio fr;
    if (!MeshPb::parseFromRadio(gRadio, n, fr)) { LOGW("Mesh: unreadable FromRadio (%u bytes)", (unsigned) n); continue; }
    if (fr.kind == MeshPb::K_QUEUE)
      LOGV("Mesh: node queue: result %d, %lu free, packet %lu", (int) fr.queue.res, (unsigned long) fr.queue.free, (unsigned long) fr.queue.packetId);
    else if (fr.kind == MeshPb::K_PACKET && fr.packet.decoded && fr.packet.port == MeshPb::PORT_ROUTING)
      LOGI("Mesh: routing answer from %08lx for packet %lu: error %u", (unsigned long) fr.packet.from, (unsigned long) fr.packet.requestId, (unsigned) fr.packet.routeError);
    else if (fr.kind == MeshPb::K_PACKET && fr.packet.decoded && fr.packet.port == MeshPb::PORT_TEXT)
      LOGI("Mesh: text from %08lx on channel %u, %u bytes", (unsigned long) fr.packet.from, (unsigned) fr.packet.channel, (unsigned) fr.packet.payloadLen);
    CoreLock lk;
    MeshCore::onFromRadio(fr, utcNow());
    if (fr.kind == MeshPb::K_CONFIG_DONE)
      LOGI("Mesh: the node sent its configuration: %d nodes, %d with a name", MeshCore::nodeCount(), MeshCore::namedNodes());
  }
  return true;
}

// The node asked for its PIN (it showed one on its screen). Runs on the Bluetooth host task while the worker waits
// for the pairing: hand the request to the loop task (which asks the Palm) and sleep until the answer is in.
uint32_t askPalmForPin() {
  if (!gPairing) return 0;
  LOGI("Mesh: the node wants a PIN - asking the Palm");
  gPinGiven = false; gPinValue = 0; gPinNeed = true;
  unsigned long until = millis() + RelayTiming::PIN_WAIT_MS;
  while (!gPinGiven && (long) (millis() - until) < 0) vTaskDelay(pdMS_TO_TICKS(20));
  gPinNeed = false;
  if (!gPinGiven) { LOGW("Mesh: no PIN from the Palm"); return 0; }
  LOGI("Mesh: PIN received from the Palm");
  return gPinValue;
}

void runCmd(Cmd &c) {

  switch (c.op) {
    case W_CONNECT: {
      if (c.arg == 2 && MeshBle::connected()) {                 // the Palm's node list Sync: the node sends its list again, and it is waited for
        uint32_t last; { CoreLock lk; last = MeshCore::nodeUpdates(); }
        if (!sendWantConfig(MeshPb::NONCE_ALL)) { MeshBle::disconnect(); gWasUp = false; postLink(L_DOWN, "Link lost"); gConnRes.done = true; break; }
        unsigned long t0 = millis(), quiet = millis();
        while (millis() - t0 < RelayTiming::NODE_LIST_MAX_MS && millis() - quiet < RelayTiming::NODE_LIST_QUIET_MS) {                // the node sends a long list slowly: until it goes quiet, at most 40 s
          if (!drainRadio(RelayTiming::NODE_LIST_SLICE_MS)) { MeshBle::disconnect(); gWasUp = false; postLink(L_DOWN, "Link lost"); break; }
          uint32_t u; { CoreLock lk; u = MeshCore::nodeUpdates(); }
          if (u != last) { last = u; quiet = millis(); }
          vTaskDelay(pdMS_TO_TICKS(300));
        }
        gConnRes.done = true;
        break;
      }
      if (c.arg == 1 && MeshBle::connected()) {                 // "sync now": the link is up, just take what the node holds
        if (!drainRadio()) { MeshBle::disconnect(); gWasUp = false; postLink(L_DOWN, "Link lost"); }
        gConnRes.done = true;
        break;
      }
      MeshBle::disconnect();
      if (workerLinkUp()) drainRadio();
      gConnRes.done = true;
      break;
    }
    case W_PAIR: {
      MeshBle::disconnect();
      gPairing = true;
      MeshBle::setPinProvider(askPalmForPin);
      bool ok = workerLinkUp();
      gPairing = false;
      MeshBle::setPinProvider(nullptr);
      snprintf(gPairRes.text, sizeof gPairRes.text, "%s", ok ? "Paired" : MeshBle::lastError());
      if (ok) drainRadio();
      gPairRes.ok = ok;
      gPairRes.done = true;
      break;
    }
    case W_DISCONNECT:
      MeshBle::disconnect();
      gWasUp = false;
      postLink(linkIdle(), "");
      break;
    case W_FORGET:                                              // arg 1: a node that is not the one linked: only its bond goes
      if (!c.arg) { MeshBle::disconnect(); gWasUp = false; }
      MeshBle::forget(c.data, c.data[6]);
      if (!c.arg) postLink(linkIdle(), "");
      break;
    case W_SCAN: {
      if (MeshBle::connected()) { MeshBle::disconnect(); gWasUp = false; postLink(L_DOWN, "Scanning"); }   // one radio
      gScan.n = MeshBle::scan(gScan.hits, 8, c.arg);
      gLastAttempt = millis();
      gScan.done = true;
      break;
    }
    case W_SEND: {
      bool ok = MeshBle::connected() && sendToRadio(c.data, c.len);
      LOGI("Mesh: message %lu %s", (unsigned long) c.id, ok ? "written to the node" : "NOT written (no link or the write failed)");
      { CoreLock lk; MeshCore::textWritten(c.id, ok); }
      SendRes &r = gSendRes[gSendHead % 6];
      r.localId = c.lid; r.id = c.id; r.ok = ok; r.code = MeshCore::ERR_WRITE;
      snprintf(r.text, sizeof r.text, "%s", ok ? "" : "The node did not take it");
      gSendHead = gSendHead + 1;
      break;
    }
    case W_CFG_READ:
      gCfgReadRes.ok = cfgRead(gCfgReadRes.text, sizeof gCfgReadRes.text);
      gCfgReadRes.done = true;
      break;
    case W_CFG_SET:
      gCfgWriteRes.ok = cfgSet(c.data, c.len, gCfgWriteRes.text, sizeof gCfgWriteRes.text);
      if (gCfgWriteRes.ok) gCfgWriteRes.text[0] = 0;
      gCfgWriteRes.done = true;
      break;
    case W_ACT: {
      uint32_t utc = ((uint32_t) c.data[0] << 24) | ((uint32_t) c.data[1] << 16) | ((uint32_t) c.data[2] << 8) | c.data[3];
      gCfgWriteRes.ok = cfgAct(c.arg, utc, gCfgWriteRes.text, sizeof gCfgWriteRes.text);
      if (gCfgWriteRes.ok) gCfgWriteRes.text[0] = 0;
      gCfgWriteRes.done = true;
      break;
    }
    case W_SHUTDOWN:
      MeshBle::shutdown();
      gWasUp = false;
      gShutdownDone = true;
      break;
  }
}

// What the worker does when no command is waiting: keep the link, read what the node has, or - in Background mode
// with no Palm app open - reach the node once every X minutes, take what it holds and hang up again (a node that
// cannot be found just skips the round).
// A direct message lacked a key (the node had none for the recipient, or the recipient none for us): ask the recipient for its node
// info - ours goes along with the request - then send the text again once its answer is in, or after a wait.
static bool gKeyAct = false;
static unsigned long gKeyDue = 0;

static void keyExchangeTick(unsigned long now) {
  uint8_t pkt[256]; size_t n = 0;
  if (!gKeyAct) {
    uint32_t to; uint8_t chan; bool job;
    { CoreLock lk; job = MeshCore::keyJob(to, chan); if (job) { uint32_t id = MeshCore::keyRequest(to, chan, pkt, sizeof pkt, n); if (!id) { MeshCore::keyGiveUp(); job = false; } } }
    if (!job) return;
    LOGI("Mesh: key exchange with %08lx: asking for its node info", (unsigned long) to);
    if (!sendToRadio(pkt, n)) { CoreLock lk; MeshCore::keyGiveUp(); return; }
    gKeyAct = true; gKeyDue = now + RelayTiming::KEY_WAIT_MS;
    return;
  }
  bool replied; { CoreLock lk; replied = MeshCore::keyReplied(); }
  if (!replied && (long) (now - gKeyDue) < 0) return;
  uint32_t id = 0, idOut = 0;
  int st;
  { CoreLock lk; st = MeshCore::keyState(); id = MeshCore::keyResend(pkt, sizeof pkt, n, idOut); }
  gKeyAct = false;
  if (!id) { LOGI("Mesh: key exchange %s, but the text is not waiting any more (status %d)", replied ? "answered" : "timed out", st); return; }
  bool ok = sendToRadio(pkt, n);
  LOGI("Mesh: key exchange %s: message %lu sent again, %s", replied ? "answered" : "timed out", (unsigned long) id, ok ? "written to the node" : "NOT written");
  { CoreLock lk; MeshCore::textWritten(id, ok); }
}

void workerTick() {
  // Standalone (the mode is closed): checks happen at the main menu only. Another mode may need the radio and
  // the memory, so the Bluetooth is let go while one runs and the next check waits for the main menu.
  if (gStandalone && menuMode != MODE_MAIN) {
    MeshBle::shutdown();                          // does nothing when the stack is already off
    gWasUp = false;
    return;
  }
  bool bg = bgReady() && !gPalmSession;                       // only the background node has checks
  unsigned long now = millis();
  if (MeshBle::connected()) {
    keyExchangeTick(now);
    if (!bg && !gKeyAct) nameTick(now);
    if (MeshBle::takeNotify() || now - gLastPoll > 3000) {
      gLastPoll = now;
      if (!drainRadio()) { MeshBle::disconnect(); gWasUp = false; postLink(L_DOWN, "Link lost"); return; }
    }
    if (!bg && gConfigOnly) {                                 // a Palm session began on a background check's link: start over with the node list
      MeshBle::disconnect();
      gWasUp = false;
      gConfigOnly = false;
      gLastAttempt = 0;
      return;
    }
    // The node has told its settings and has nothing more to say: that is the whole check.
    if (bg && ((MeshCore::configDone() && now - gLastFrameAt > RelayTiming::CHECK_QUIET_MS) || now - gFetchStart > RelayTiming::CHECK_MAX_MS)) {
      MeshBle::disconnect();
      gWasUp = false;
      postLink(L_DOWN, "Waiting for the next check");
      gNextFetch = now + everyMs();
      if (gStandalone) MeshBle::shutdown();                   // no mode open: give the Bluetooth memory back between checks
      gCheckState = 2;
      LOGI("Mesh: background check done (%lu ms)", now - gFetchStart);
    }
    return;
  }
  if (gWasUp) { gWasUp = false; postLink(L_DOWN, "Link lost"); }
  if (!gPaired.have) return;
  if (bg) {
    if ((long) (now - gNextFetch) >= 0) {
      gFetchStart = now;
      gCheckState = 1;
      LOGI("Mesh: background check");
      if (workerLinkUp(true)) drainRadio();
      else {
        if (!(gStandalone && menuMode != MODE_MAIN)) gNextFetch = millis() + everyMs();   // cut by another mode opening: stays due, retried at the main menu
        gCheckState = 2;
        if (gStandalone) MeshBle::shutdown();
      }
    }
    return;
  }
  if (now - gLastAttempt > RelayTiming::RECONNECT_MS) {
    if (workerLinkUp()) drainRadio();
  }
}

void workerTask(void *) {
  for (;;) {
    Cmd c;
    if (xQueueReceive(gCmdQ, &c, pdMS_TO_TICKS(MeshBle::connected() ? 100 : 300)) == pdTRUE) runCmd(c);
    else if (gWorkerOn) workerTick();
  }
}

void post(const Cmd &c) { xQueueSend(gCmdQ, &c, pdMS_TO_TICKS(100)); }

static TaskHandle_t gWorker = nullptr;           // the worker task

void ensureWorker() {
  if (gCmdQ) return;
  gCmdQ = xQueueCreate(6, sizeof(Cmd));
  gCoreMx = xSemaphoreCreateMutex();
  xTaskCreatePinnedToCore(workerTask, "meshble", 8192, nullptr, 1, &gWorker, 0);
}

// Stop the worker's Bluetooth work and switch the radio off (waits until it has).
void stopWorker() {
  gWorkerOn = false;
  if (!gCmdQ) return;
  xQueueReset(gCmdQ);
  MeshBle::abort();
  gShutdownDone = false;
  Cmd c{}; c.op = W_SHUTDOWN;
  post(c);
  for (int i = 0; i < 1500 && !gShutdownDone; i++) vTaskDelay(pdMS_TO_TICKS(10));
}

}  // namespace MeshRelay

#endif  // WIRSA_FEATURE_MESH
