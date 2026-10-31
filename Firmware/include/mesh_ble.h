// mesh_ble.h - WiRSa as a Bluetooth LE client of a Meshtastic node (NimBLE-Arduino). Built with
// WIRSA_FEATURE_MESH. Blocking calls run on the loop task; a connect or scan holds it for seconds.
#ifndef MESH_BLE_H
#define MESH_BLE_H

#include <Arduino.h>
#include "globals.h"

#if WIRSA_FEATURE_MESH

namespace MeshBle {

struct Hit { uint8_t addr[6]; uint8_t type; int8_t rssi; char name[24]; };

// The Arduino core frees the Bluetooth controller's memory at boot unless btInUse() says it is wanted, and
// NimBLE aborts if that memory is gone. btInUse() (mesh_ble.cpp) says yes only when the NVS flag wmesh/bt
// is set, so a WiRSa that never uses Mesh keeps that RAM free. enableAtNextBoot() sets the flag; the
// caller then restarts once. ready() is true when this boot has the memory.
bool ready();
bool active();   // the Bluetooth stack is running (WiFi must keep modem sleep meanwhile)
void enableAtNextBoot();

// Starts the BLE stack (once). Returns false if it could not.
bool begin();
// Disconnects and stops the BLE stack (the radio is off again). begin() starts it again.
void shutdown();
// Looks for nodes advertising the Meshtastic service for `seconds`. Fills hits (up to max); returns how many.
int scan(Hit *hits, int max, uint8_t seconds);
// Called (on the Bluetooth task) when the node asks for its PIN: returns the six digits, 0 if there is none.
// Set by the Mesh mode while a pairing is in progress; the Palm is asked then, because a random PIN exists
// only once the node has started pairing.
typedef uint32_t (*PinProvider)();
void setPinProvider(PinProvider p);

// Connects to a node, pairs if it asks (pin = the six digits the node shows, or the fixed PIN), finds the
// Meshtastic characteristics and subscribes to new-data notifications. pin 0 = no PIN known (a bonded node
// does not ask). Returns true when the link is ready. lastError() says why not.
bool connect(const uint8_t addr[6], uint8_t type, uint32_t pin);
void disconnect();
// Cuts a connect or pairing short from another task (the wait in it ends at once).
void abort();
bool connected();
const char *lastError();
// Forget the pairing with this node (the bond), so a new PIN is asked next time.
void forget(const uint8_t addr[6], uint8_t type);

// ToRadio write (one protobuf message). False on failure or no link.
bool write(const uint8_t *buf, size_t len);
// One FromRadio message if the node has one: true with len > 0; true with len 0 = queue empty; false = link problem.
bool read(uint8_t *buf, size_t cap, size_t &len);
// True once since the last call if the node announced new data (fromNum notify).
bool takeNotify();

}  // namespace MeshBle

#endif
#endif
