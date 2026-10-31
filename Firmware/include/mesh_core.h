// mesh_core.h - Mesh mode's data side, with no Arduino and no Bluetooth: the node list, the channel
// slots and the message store, fed by decoded FromRadio messages (mesh_pb.h). Host-tested
// (test_host/mesh_core_test.cpp). mesh_relay.cpp owns the Palm link and the Bluetooth session and
// calls this.
//
// Messages get a sequence number (seq, rising, never reused while WiRSa runs). The Palm remembers the
// highest seq it has and asks for what is newer; a cursor from before a WiRSa restart is higher than
// anything here and counts as "give me everything".
#ifndef MESH_CORE_H
#define MESH_CORE_H

#include "mesh_pb.h"

namespace MeshCore {

const int MAX_NODES = 500, MAX_CHANS = 8, TEXT_MAX = 233, MSG_CAP = 200;

// ST_SENT_NOACK: a direct message that only a relay confirmed (not the recipient). ST_KEYS: the keys are being exchanged, then it is sent again.
enum : uint8_t { ST_SENDING = 0, ST_SENT = 1, ST_ACKED = 2, ST_FAILED = 3, ST_SENT_NOACK = 4, ST_UNKNOWN = 5, ST_KEYS = 6 };
enum : uint8_t { F_OUT = 1, F_DM = 2 };
enum : uint8_t { ERR_WRITE = 240 };                 // the Bluetooth write to the node failed (not a node error)
// What a client can ask of another node (or tell the mesh) besides a text.
enum : uint8_t { REQ_INFO = 1, REQ_POSITION = 2, REQ_TELEMETRY = 3, REQ_TRACE = 4, REQ_SHARE_POS = 5, REQ_WAYPOINT = 6 };

struct Msg {
  uint32_t seq = 0, from = 0, to = 0, rxTime = 0, packetId = 0, localId = 0;
  uint8_t chan = 0, flags = 0, status = 0, len = 0;
  uint8_t err = 0;                  // why it failed: a Routing.Error from the node, or ERR_WRITE
  uint8_t tries = 0;                // 1 once it has been sent again after a key exchange
  char text[TEXT_MAX + 1] = "";
};

// Allocates the stores (PSRAM when there is some). False if even the small fallback failed. Safe to call again.
bool init();
// Forget what the node told us (nodes, channels, config-done) - a new Bluetooth connection starts over.
// Messages stay.
void resetSession();

// Feed one message the node sent. now = UTC seconds (0 = unknown), used when the node left rx_time out.
void onFromRadio(const MeshPb::FromRadio &fr, uint32_t now);

// ---- the node's side of the link ---------------------------------------------------------------
// The node's device / lora / bluetooth section as it sent it (MeshPb::CFG_*); false if not received yet.
bool config(uint8_t which, const uint8_t *&raw, size_t &len);
void setFavorite(uint32_t num, bool on);          // after the node took the change
// The node's clock now (UTC seconds), worked out from its own node record; 0 = it has no valid time.
uint32_t nodeClock();
// The MQTT module's config as the node sent it (MeshPb::readMqtt reads it); false if not received.
bool mqttConfig(const uint8_t *&raw, size_t &len);
// Drops the settings and channels held, before they are read afresh.
void forgetSettings();
bool configDone();
uint32_t myNum();
const char *firmware();
int nodeCount();
int namedNodes();                               // how many of them have a long name
const MeshPb::NodeInfo *node(int i);           // i = 0 is always this node once my_info has come
int findNode(uint32_t num);                     // -1 if unknown
int myBattery();                                // percent, -1 unknown
const MeshPb::Channel *channel(int slot);       // slot 0..7, NULL if the node has not said
uint32_t nodeUpdates();                         // bumps whenever a node entry changes

// ---- messages ---------------------------------------------------------------------------------
int msgCount();
const Msg *msgAt(int i);                        // oldest first
uint32_t version();                              // changes when a stored message changes state
uint32_t lastSeq();                             // 0 = nothing yet
void setSeqBase(uint32_t base);                 // the next message gets base + 1 (call before any message is stored)
// First stored message with seq > cursor (cursor > lastSeq() counts as 0); -1 if none.
int firstAfter(uint32_t cursor);
// Messages whose status changed after `cursor` is not tracked per message; the Palm asks for the
// outgoing ones it still holds as unfinished (see statusOf).
const Msg *findByLocalId(uint32_t localId);
uint32_t incomingSince(uint32_t cursor);        // how many messages newer than cursor

// Queues a text for sending: builds the ToRadio bytes in out (non-zero length on success) and stores
// the message (status SENDING). localId is the Palm's own id for it. Returns the packet id, 0 on failure
// (too long, no room).
uint32_t queueText(uint32_t to, uint8_t chan, const char *text, size_t len, uint32_t localId,
                   uint32_t now, uint8_t *out, size_t cap, size_t &outLen);
// Queues one of those requests the same way: the stored outgoing message reads like "[Asked for position]" and gets
// its status from the node's answers. The other node's reply is stored as an incoming message in the same chat.
// lat/lon (degrees x 1e7) and name are used by REQ_SHARE_POS and REQ_WAYPOINT. 0 on failure.
// ---- key exchange --------------------------------------------------------------------------------
// A direct message the node could not encrypt or the recipient could not read (Routing.Error 6, 35, 39) is not failed at once: it
// goes to ST_KEYS and one key job is posted. The worker sends a node info request (ours carries our key), waits for the answer
// (or a timeout) and sends the text again, once.
bool keyJob(uint32_t &to, uint8_t &chan);              // true once for each job posted
int keyState();                                         // the waiting text: its status, -1 if there is none (for the log)
bool keyReplied();                                      // the other node's node info has come in since the job began
// Builds the node info request for the job (nothing is stored). 0 on failure.
uint32_t keyRequest(uint32_t to, uint8_t chan, uint8_t *out, size_t cap, size_t &outLen);
// Builds the text again with a new packet id for the message in ST_KEYS; it goes back to ST_SENDING. 0 if there is none.
uint32_t keyResend(uint8_t *out, size_t cap, size_t &outLen, uint32_t &packetId);
void keyGiveUp();                                       // the job could not be carried out: the message fails with its first error

// Names: the sender of a received text that the node list has no name for is asked for its node info (the answer fills the list in).
// Builds one request, for the newest such sender not asked within againMs (nowMs is the caller's clock); false if there is none.
bool nameRequest(uint32_t nowMs, uint32_t againMs, uint32_t &to, uint8_t *out, size_t cap, size_t &outLen);

uint32_t queueRequest(uint8_t kind, uint32_t to, uint8_t chan, int32_t latE7, int32_t lonE7, const char *name,
                      uint32_t localId, uint32_t now, uint8_t *out, size_t cap, size_t &outLen);
// The Bluetooth write of a queued text went through / failed.
void textWritten(uint32_t packetId, bool ok);
// Messages still SENDING for longer than ackWaitSec are left as SENT (no ack heard).
void expire(uint32_t now, uint32_t ackWaitSec);

void clearMessages();                           // all of them
void resetSeen();                               // forget which packets were taken (another node is shown: it hears the same ones)
// Puts back an incoming text that was kept outside the store (SD file, RTC ring). False when it is already here
// (same sender and id, or same sender, time and text), so a replay never doubles a message.
bool importText(uint32_t from, uint32_t to, uint32_t rxTime, uint32_t packetId, uint8_t chan, uint8_t flags,
                const char *text, size_t len);
uint32_t clearOlderThan(uint32_t beforeUtc);    // returns how many went

// Random non-zero packet ids (seeded by the caller's entropy).
void seedIds(uint32_t seed);

}  // namespace MeshCore

#endif
