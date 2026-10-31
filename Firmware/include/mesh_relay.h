#ifndef MESH_RELAY_H
#define MESH_RELAY_H

// Mesh Relay mode - WiRSa is the Palm's Bluetooth link to a Meshtastic node. The Palm cannot do BLE or
// the node's protobuf protocol, so WiRSa holds the connection, keeps the node list and the messages
// (mesh_core.h), and serves the Palm client (apps/meshtastic_client) over the serial line with the same
// framing and link encryption as the other relay modes. Wire format: docs/protocol/Protocol-wire.md (section 9).
//
// Same shape as the BitWarden and CalDAV relays: entered from the main menu or by WiRSaCtl's ENTER, then
// it services the Palm line until BACK, +++ or EXIT. Unlike those, the mode also works between Palm
// sessions: it keeps the node connected and collects messages. Build switch: WIRSA_FEATURE_MESH.

#include "globals.h"

#if WIRSA_FEATURE_MESH

void enterMeshRelayMode();
void exitMeshRelayMode();
void meshRelayLoop();
void meshBgBoot();      // at boot: Background mode checks without the mode open
void meshSpoolPoll();   // main loop: texts waiting for the SD card
void meshBgPoll();      // main loop: cut a check when another mode opens
bool meshBgActive();
bool meshBgIdle();             // background checks are scheduled but none is running: the board may sleep
bool meshBgSleepArmed();       // the last sleep armed the check timer
uint64_t meshBeforeSleep();    // before that sleep: texts to the card or the ring; the timer in microseconds
bool meshHeadlessWake();       // setup(): a timer wake that only checks the node; sleeps again (no return) when it ran    // those checks are scheduled (no deep sleep yet)

#endif
#endif
