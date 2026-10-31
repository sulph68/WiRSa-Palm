// mesh_background.cpp - Mesh Relay mode: the background checks at the main menu (the mode closed), the spool poll, deep sleep with a timer wake.
// What the rest of the firmware calls is declared in mesh_relay.h.
#include "mesh_internal.h"

#if WIRSA_FEATURE_MESH

#include "serial_io.h"
#include "display_menu.h"
#include "network.h"
#include "palm_ctl.h"
#include "log.h"
#include "settings.h"
#include "relay_timing.h"
#include "mesh_spool_sd.h"
#include "deep_sleep.h"
#include <esp_random.h>
#include <esp_sleep.h>
#include <esp_system.h>

using namespace MeshRelay;

void meshSpoolPoll() {
  static unsigned long last = 0;
  if (millis() - last < RelayTiming::SPOOL_POLL_MS) return;
  last = millis();
  if (!gStandalone && !gStarted) return;
  seqEnsure();
  learnNodeId();
  if (gPalmSession) return;                                   // served from the store; written at the end
  if (menuMode != MODE_MAIN && menuMode != MODE_MESH) return;
  if ((long) (millis() - gSpoolRetryAt) < 0) return;
  int n = spoolPending();
  if (!n) { gPendingSince = 0; return; }
  if (!gPendingSince) gPendingSince = millis();
  if (n >= RelayTiming::SPOOL_BATCH || millis() - gPendingSince >= RelayTiming::SPOOL_AGE_MS) spoolFlush();
}

// At boot: Background mode with a paired node starts its checks without the mode being opened.
void meshBgBoot() {
  loadPrefs();
  if (!bgReady() || !MeshBle::ready()) return;
  ensureWorker();
  if (!MeshCore::init()) return;
  MeshCore::seedIds(esp_random());
  seqBootOnce();
  spoolRestore();
  seqEnsure();
  gLinkPending = false; gWasUp = false; gPalmSession = false;
  gNextFetch = millis() + RelayTiming::BOOT_CHECK_DELAY_MS;               // a reboot is not a due check: normal use comes first, the first check waits a minute
  gStandalone = true;
  gWorkerOn = true;
  LOGI("Mesh: background checks running at the main menu (every %lu min)", everyMs() / 60000UL);
}

bool meshBgActive() { return gStandalone; }

// No check is running and the Bluetooth is off: the board may sleep until the next one.
bool meshBgIdle() { return gStandalone && gCheckState != 1 && !MeshBle::active(); }
bool meshBgSleepArmed() { return gBgSleepMagic == BG_MAGIC; }

// Before a deep sleep with background checks on (the idle time has passed): the texts the card does not hold yet go to it,
// or into the ring when there is no card, and the check timer is armed. Returns the wait in microseconds.
uint64_t meshBeforeSleep() {
  if (!gStandalone) return 0;
  if (!spoolFlush()) {
    CoreLock lk;
    uint32_t fl = spoolFloor();
    for (int i = 0; i < MeshCore::msgCount(); i++) {
      const MeshCore::Msg *m = MeshCore::msgAt(i);
      if ((m->flags & MeshCore::F_OUT) || m->seq <= fl) continue;
      MeshSpool::Rec r;
      r.from = m->from; r.to = m->to; r.rxTime = m->rxTime; r.packetId = m->packetId;
      r.chan = m->chan; r.flags = m->flags; r.len = m->len;
      memcpy(r.text, m->text, m->len); r.text[m->len] = 0;
      ringAdd(r);
    }
  }
  gBgSleepMagic = BG_MAGIC;
  long remain = (long) (gNextFetch - millis());
  if (remain < 5000) remain = 5000;
  LOGI("Mesh: sleeping until the next background check in %ld s", remain / 1000);
  return (uint64_t) remain * 1000ULL;
}

// setup(), right after deepSleepBoot(): a timer wake that was armed for a background check runs it and goes back to sleep
// without the rest of setup() - 80 MHz, no WiFi, no display. Returns false when this boot is something else (the normal
// setup then goes on, at full speed). A wake by the Palm line is such a boot: everything on.
bool meshHeadlessWake() {
  if (esp_reset_reason() != ESP_RST_DEEPSLEEP || esp_sleep_get_wakeup_cause() != ESP_SLEEP_WAKEUP_TIMER || gBgSleepMagic != BG_MAGIC)
    return false;
  setCpuFrequencyMhz(80);                                     // the radio needs 80 MHz; nothing here needs more
  Serial.begin(115200);                                       // the log; the rest of setup() would do this later
#if defined(ARDUINO_USB_CDC_ON_BOOT) && ARDUINO_USB_CDC_ON_BOOT
  Serial.setTxTimeoutMs(50);                                  // native USB (HWCDC) only
#endif
  loadPrefs();
  if (!bgReady() || !MeshBle::ready() || !MeshCore::init()) {
    setCpuFrequencyMhz(240);
    return false;                                             // the settings changed meanwhile: a normal boot
  }
  gHeadless = true;
  gRing.attach();
  LOGI("Mesh: timer wake - background check (80 MHz, no WiFi)");
  ensureWorker();
  gStandalone = true; gPalmSession = false; gNextFetch = 0; gCheckState = 0; gLinkPending = false; gWasUp = false;
  gWorkerOn = true;
  unsigned long t0 = millis();
  while (gCheckState != 2 && millis() - t0 < RelayTiming::HEADLESS_CHECK_MAX_MS) delay(50);
  stopWorker();                                               // lets the worker finish and puts the Bluetooth away
  int added = 0;
  {
    CoreLock lk;
    for (int i = 0; i < MeshCore::msgCount(); i++) {
      const MeshCore::Msg *m = MeshCore::msgAt(i);
      if (m->flags & MeshCore::F_OUT) continue;
      MeshSpool::Rec r;
      r.from = m->from; r.to = m->to; r.rxTime = m->rxTime; r.packetId = m->packetId;
      r.chan = m->chan; r.flags = m->flags; r.len = m->len;
      memcpy(r.text, m->text, m->len); r.text[m->len] = 0;
      ringAdd(r);
      added++;
    }
  }
  static const uint8_t FLUSH_CHECKS[] = { 1, 3, 5, 10 };      // gSdFlush 0..3; 4 = only when the ring is full
  if (gRingChecks < 255) gRingChecks++;
  if (gSdFlush < 4 && gRing.count() && gRingChecks >= FLUSH_CHECKS[gSdFlush] && !ringFlushToSd())
    LOGW("Mesh: SD card not available - the ring keeps %u text(s)", (unsigned) gRing.count());
  unsigned long ms = everyMs();
  LOGI("Mesh: check over (%lu ms awake): %d new text(s); the ring now holds %u text(s), %u of %u bytes free; sleeping %lu s",
       millis(), added, (unsigned) gRing.count(), (unsigned) gRing.freeBytes(), (unsigned) gRing.capacity(), ms / 1000);
  gBgSleepMagic = BG_MAGIC;
  Serial.flush();
  delay(300);                                                 // lets the USB host read the last lines before the port goes
  deepSleepEnter((uint64_t) ms * 1000ULL);
  return true;
}

// Main loop, every pass: a mode other than the main menu opened while a check holds the node - cut the link now so
// the radio is free for it (the worker lets go of the stack on its next tick; the check stays due).
void meshBgPoll() {
  static unsigned long last = 0;
  if (!gStandalone || menuMode == MODE_MAIN || millis() - last < 100) return;
  last = millis();
  MeshBle::abort();
}

#endif  // WIRSA_FEATURE_MESH
