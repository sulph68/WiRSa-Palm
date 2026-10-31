// Mesh Relay mode - the mode itself: how it starts, runs and ends, as the shared relay skeleton (relay_mode.h) sees it. See include/mesh_relay.h for
// what the rest of the firmware calls, include/mesh_internal.h for how the files divide the work, and docs/Mesh-Palm.md for the wire format.
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
#include "mesh_spool_sd.h"
#include <esp_random.h>

using namespace MeshRelay;

// What is Mesh's own in the skeleton (relay_mode.h): its state at entry, its worker and the texts on the way out, its own Exit Timer rules.
static void meshOnEnter() {
  if (!MeshBle::ready()) {
    // The core freed the Bluetooth memory at this boot. Turn it on for the next one and restart once.
    LOGI("Mesh: enabling Bluetooth - restarting once");
    SerialPrintLn("Mesh Relay: turning Bluetooth on - restarting WiRSa once.");
    MeshBle::enableAtNextBoot();
    display.fillRect(0, 17, 128, 47, SSD1306_BLACK);
    display.setCursor(0, 19); display.print("Bluetooth on"); display.setCursor(0, 31); display.print("Restarting..."); display.display();
    delay(1500);
    ESP.restart();
  }
  ensureWorker();
  stopWorker();                                   // a standalone check may be running: the mode takes over from a quiet worker
  gStandalone = false;
  loadPrefs();
  MeshCore::init();
  MeshCore::seedIds(esp_random());
  seqBootOnce();
  spoolRestore();
  seqEnsure();
  gLink = linkIdle();
  gLinkText[0] = 0;
  gLastAttempt = 0; gLastPalmFrame = 0; gNegotiatedAt = 0; gPalmSession = false; gExitAfterBye = false; gNextFetch = millis() + RelayTiming::ENTER_CHECK_HOLD_MS;   // a Palm usually says hello within seconds: its session takes the full link, so no background check first
  gBusyOp = 0; gScan.done = gPairRes.done = gConnRes.done = gCfgWriteRes.done = gCfgReadRes.done = false; gPinNeed = false; gSendHead = gSendTail = 0; gLinkPending = false; gWasUp = false;
  gWorkerOn = true;
  gStarted = true;
  ledActivity(LED_ACT_MODE);                      // an RGB LED: one green blink, the mode is up (it is dark in a session)
}

static void meshBeforeLeave() {
  if (gStarted && gPalmSession) {                // the mode ends while the app is connected: tell it, so it shows Disconnected
    writeFrame(M_BYE, nullptr, 0);
    PhysicalSerial.flush();
    delay(40);
  }
  stopWorker();
  spoolFlush();                                  // what the Palm has not confirmed goes to the card before the mode closes
  if (gRelayMode == 1 && gBgSlot < MeshSlots::MAX && gBgSlot != gActive) activateSlot(gBgSlot);   // the background node is home: its checks go on
  if (bgReady()) {                               // Background mode goes on without the mode: the worker keeps the schedule
    gStandalone = true;
    gPalmSession = false;
    gNextFetch = millis() + everyMs();
    gWorkerOn = true;
    LOGI("Mesh: background checks continue at the main menu (every %lu min)", everyMs() / 60000UL);
  }
  gStarted = false;
}

// The skeleton's two status texts; the rest of the time the mode draws its own status (node name and link).
static void meshStatusText(const char *text) {
  if (text && !strcmp(text, "Preparing...")) {
    display.fillRect(0, 17, 128, 47, SSD1306_BLACK);
    display.setCursor(0, 19); display.print(text); display.display();
  } else {
    drawStatus();
  }
}

static void meshOnFrame() {
  gLastPalmFrame = millis();
  if (gNegotiatedAt) gNegotiatedAt = millis();
}

// With a probe (M_PING): the "unused" Exit Timer until a session begins, then the silence probe; a node operation or a dialog open on the Palm is not silence.
static bool meshBusy() { return gBusyOp || (long) (gHoldUntil - millis()) > 0; }

static RelayMode::Spec meshMode = {
  "Mesh Relay", "Mesh", MODE_MESH, 'T', RelayTiming::BAUD_IDLE, M_MAX_PALM_TYPE, /*needsWifi*/ false, &gTimer, /*negotiateTimeoutMs*/ 0,
  /*wifiReady*/ nullptr, &gNegotiatedAt,
  drawHeader, meshStatusText, /*revertToIdle*/ nullptr, meshOnEnter, meshBeforeLeave, /*onLeave*/ nullptr, meshOnFrame, /*drainAtEntry*/ false,
  M_HELLO_ACK, PROTO_VERSION, "WiRSa-Mesh/3",
  M_PING, &gPalmSession, &gLastPalmFrame, meshBusy,
};

RelayMode::Spec &meshSpec() { return meshMode; }

void enterMeshRelayMode() { RelayMode::enter(meshMode); }
void exitMeshRelayMode() { RelayMode::leave(meshMode); }

void meshRelayLoop() {
  if (RelayMode::tick(meshMode) != RelayMode::Tick::Go) return;

  // The negotiated rate stays until the Palm says BYE (the app closed) or WiRSaCtl resets the mode. The Exit Timers and the silence probe
  // (a PING, then leaving if nothing answers) are the skeleton's: see the spec below.

  if (!PhysicalSerial.available()) { pumpNode(); return; }

  uint8_t type; size_t bodyLen;
  if (!RelayMode::readFrame(meshMode, type, gBody, sizeof gBody, bodyLen, RelayTiming::MESH_IDLE_TIMEOUT_MS, RelayTiming::MESH_BYTE_TIMEOUT_MS)) return;
  dispatch(type, gBody, bodyLen);
  runStashed();                                  // what the Palm sent while a fetch waited for its acknowledgements
  if (gExitAfterBye) {
    gExitAfterBye = false;
    RelayMode::leaveToMenu(meshMode);
    return;
  }
  // a long job (connect, pairing) must not count as silence from the Palm
  gLastPalmFrame = millis();
  if (gNegotiatedAt) gNegotiatedAt = millis();
}

#endif  // WIRSA_FEATURE_MESH
