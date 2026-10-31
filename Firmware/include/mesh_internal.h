// mesh_internal.h - what the files of Mesh Relay mode share. Not for the rest of the firmware: that sees only mesh_relay.h.
//
//   mesh_state.cpp       the shared state, the settings kept in NVS, the OLED
//   mesh_worker.cpp      the node, on its own task: connect, pairing, scan, reads, the key exchange
//   mesh_admin.cpp       changing the node's settings (admin messages)
//   mesh_palm.cpp        the frames the Palm sends, and the answers (the frame table)
//   mesh_store.cpp       texts kept outside the store: the SD card, the RTC ring, the seq marks
//   mesh_background.cpp  background checks at the main menu, deep sleep, the timer wake
//   mesh_relay.cpp       the mode itself: how it starts, runs and ends (relay_mode.h)
//
// Which task does what (the rule everything here follows): the WORKER task owns the Bluetooth link to the node and everything that
// waits on it; the LOOP task alone draws the display and writes to the Palm. They hand over through the flags and queues below
// (the worker's results are the OpResult / ScanResult structures, which the loop task picks up in serviceWorker); MeshCore (messages,
// nodes) is shared and guarded by CoreLock.
#pragma once
#include "mesh_relay.h"

#if WIRSA_FEATURE_MESH

#include "globals.h"
#include "relay_proto.h"
#include "relay_mode.h"
#include "mode_timer.h"
#include "mesh_core.h"
#include "mesh_ble.h"
#include "mesh_spool.h"
#include "mesh_slots.h"
#include <Preferences.h>

// Mesh's description for the skeleton (defined with the mode, in mesh_relay.cpp)
RelayMode::Spec &meshSpec();

namespace MeshRelay {

using RelayProto::Writer;
using RelayProto::Reader;

// ---- the wire: frame types (docs/protocol/Protocol-wire.md, section 9) -------------------------------------------------------------------------------
// The numbers follow section 6 of that document: the common frames at 0x01-0x06, Mesh's own in its block 0x40-0x6F, the Palm-sent ones first
// (0x40-0x52, the junk filter's bound M_MAX_PALM_TYPE is the highest) and then the ones WiRSa sends (0x53-0x60).
enum : uint8_t {
  M_HELLO = 0x01, M_HELLO_ACK = 0x02, M_BYE = 0x03, M_ACK = 0x04, M_SYNC_DONE = 0x05, M_ERR = 0x06,
  // Palm -> WiRSa (PING either way)
  M_STATE = 0x40, M_SCAN = 0x41, M_PAIR = 0x42, M_PIN = 0x43, M_UNPAIR = 0x44, M_CONNECT = 0x45, M_SYNC = 0x46,
  M_SEND = 0x47, M_REQ = 0x48, M_NODE_GET = 0x49, M_RELAY_GET = 0x4A, M_RELAY_SET = 0x4B, M_CLEAR = 0x4C, M_HIST = 0x4D,
  M_PING = 0x4E, M_HOLD = 0x4F, M_CFG_GET = 0x50, M_CFG_SET = 0x51, M_CFG_ACT = 0x52,
  // WiRSa -> Palm
  M_STATE_R = 0x53, M_SCAN_R = 0x54, M_PAIR_R = 0x55, M_PIN_NEED = 0x56, M_NODE = 0x57, M_NODES = 0x58, M_CHAN = 0x59,
  M_MSG = 0x5A, M_DELIV = 0x5B, M_SEND_R = 0x5C, M_RELAY_R = 0x5D, M_DONE = 0x5E, M_NEWS = 0x5F, M_CFG_R = 0x60,
  // Palm -> WiRSa, added with version 3 (the Palm-sent frames of the block end here)
  M_SLOTS_GET = 0x61, M_SLOT_SELECT = 0x62,
  // WiRSa -> Palm
  M_SLOTS_R = 0x63
};
constexpr uint8_t M_MAX_PALM_TYPE = M_SLOT_SELECT;
enum : uint8_t { E_BAD = 1, E_NOLINK = 2, E_NOROOM = 3, E_BUSY = 4 };
enum : uint8_t { L_NONE = 0, L_DOWN = 1, L_READY = 2, L_FAILED = 3, L_ARCHIVED = 4 };   // link to the node, as the Palm sees it

constexpr uint8_t PROTO_VERSION = 3;   // 2: the type numbers of Protocol-wire.md section 6; 3: several nodes (slots). The Palm app refuses a WiRSa that reports less
constexpr size_t BODY_MAX = 960;
constexpr uint16_t EVERY_MIN[5] = {1, 5, 15, 30, 60};

// The Palm's list positions <-> Meshtastic's numbers (the Palm lists are in node_setup order).
constexpr uint8_t REGION_CODE[14] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 15, 18};
constexpr uint8_t PRESET_CODE[8] = {0, 1, 7, 4, 3, 6, 5, 8};
constexpr uint8_t ROLE_CODE[9] = {0, 1, 2, 5, 6, 7, 8, 9, 12};
uint8_t indexOf(const uint8_t *tab, size_t n, uint32_t code);

// ---- the worker's commands and results -----------------------------------------------------------------------------------------
enum : uint8_t { W_CONNECT = 1, W_PAIR, W_DISCONNECT, W_FORGET, W_SCAN, W_SEND, W_SHUTDOWN, W_CFG_SET, W_ACT, W_CFG_READ };
struct Cmd { uint8_t op; uint8_t arg; uint16_t len; uint32_t id; uint32_t lid; uint8_t data[640]; };

// What the worker finished, for the loop task: done is set last. text is the reason when ok is false (or the answer's words).
struct OpResult {
  volatile bool done = false;
  bool ok = false;
  char text[48] = "";
};
struct ScanResult { volatile bool done = false; int n = 0; MeshBle::Hit hits[8]; };
struct SendRes { uint32_t localId, id; bool ok; uint8_t code; char text[32]; };

// ---- shared state ---------------------------------------------------------------------------------------------------------------
extern uint8_t gBody[BODY_MAX];
extern uint8_t gOut[BODY_MAX];
extern uint8_t gAckBody[BODY_MAX];            // a frame the Palm sends while the Palm is asked to acknowledge one of ours is read into this (stashFrame)
extern uint8_t gRadio[520];
extern Preferences gPrefs;

struct Paired { bool have = false; uint8_t addr[6] = {0}; uint8_t type = 0; char name[24] = ""; uint32_t pin = 0; };
extern Paired gPaired;                       // the active node's working copy (from its slot)
extern MeshSlots::Slot gSlots[MeshSlots::MAX];
extern uint8_t gActive, gBgSlot;             // the slot shown; the slot Background mode works for (MeshSlots::NONE: none)
extern uint8_t gRelayMode, gRelayEvery;      // 0 session (mode ends with the Palm app), 1 background; every: index into 1/5/15/30/60 min
extern uint8_t gSdFlush;                     // when the ring goes to the card: index into every check / 3 / 5 / 10 checks / when full (4)
extern bool gExitAfterBye;
extern ModeExitTimer gTimer;
extern unsigned long gNextFetch, gFetchStart, gLastFrameAt;
extern bool gConfigOnly;                     // the link was asked for the node's settings only (a background check), not its node list
extern uint8_t gLink;
extern char gLinkText[40];
extern bool gPalmSession;
extern uint32_t gSeenSeq, gSeenVer;
extern uint8_t gSeenLink;
extern int gSeenBatt;                        // the node's battery as the Palm last saw it (the node table fills after the Palm has connected)
extern unsigned long gLastNews;
extern unsigned long gLastAttempt, gLastPalmFrame, gLastPoll, gNegotiatedAt, gHoldUntil;   // gHoldUntil: the Palm has a dialog open
extern uint32_t gLastPollUtc;
extern bool gStarted;
extern unsigned long gRestartUntil;          // the node was told to restart: failing to reach it is expected until then
extern volatile uint8_t gCheckState;         // 0 none yet, 1 a check is running, 2 the last one has ended
extern bool gHeadless;
extern volatile bool gStandalone;            // background checks running with the mode closed (main menu); the worker owns the Bluetooth

// Texts kept on the SD card (/Meshtastic) while no Palm has them yet. gFlushedSeq: the highest store seq the card holds;
// gConfirmedSeq: what the Palm said it has saved. With "SD History" off the card is emptied once the Palm has everything.
extern bool gHistKeep, gRestored, gSdHas, gSpoolSaid;
extern uint32_t gFlushedSeq, gConfirmedSeq;
extern unsigned long gPendingSince, gSpoolRetryAt;

// Deep sleep between background checks: the ring of texts found while the store (PSRAM) is gone lives in fast RTC memory, which a deep
// sleep leaves alone (a reset or power loss clears it). A timer wake runs a check without the rest of setup().
constexpr size_t RING_BYTES = 4096;          // header included; adjustable
constexpr uint32_t BG_MAGIC = 0x4247534CUL;
extern uint8_t gRingChecks;                 // checks since the ring last went to the card (kept through deep sleep)
extern uint32_t gBgSleepMagic;               // set when a sleep armed the check timer: the next timer wake is a background check
extern MeshSpool::Ring gRing;
extern bool gSpiUp;

// The worker
extern QueueHandle_t gCmdQ;
extern SemaphoreHandle_t gCoreMx;            // guards MeshCore (messages, nodes) and the link text below
extern volatile bool gWorkerOn, gShutdownDone;
extern volatile uint8_t gBusyOp;             // loop task: 1 scan, 2 pair, 3 connect, 4 settings write or action, 5 settings read under way

struct CoreLock {
  CoreLock() { xSemaphoreTake(gCoreMx, portMAX_DELAY); }
  ~CoreLock() { xSemaphoreGive(gCoreMx); }
};

// worker -> loop task
extern bool gLinkPending; extern uint8_t gLinkNew; extern char gLinkNewText[40];
extern ScanResult gScan;
extern OpResult gPairRes, gConnRes, gCfgReadRes, gCfgWriteRes;
extern uint8_t gKey[16]; extern size_t gKeyLen;          // worker: the node's admin session passkey
extern volatile bool gPinNeed, gPinGiven; extern volatile uint32_t gPinValue; extern bool gPinSent;
extern SendRes gSendRes[6]; extern volatile uint8_t gSendHead, gSendTail;
extern bool gPairing;                        // worker: a pairing is under way
extern bool gWasUp;                          // worker: the node link was up at the last look

// ---- mesh_state.cpp ---------------------------------------------------------------------------------------------------------------
uint32_t utcNow();
String latinName(const char *s);             // a name for the Palm: each emoji becomes the byte 0x10
unsigned long everyMs();
void loadPrefs();
void saveSlots();
void saveRelay();
uint8_t linkIdle();                          // the link while nothing is connected: down, none, or archived
bool bgReady();                              // Background mode is on for the active node, which is paired
uint8_t modeOfActive();                      // 1 when Background mode works for the active node
uint8_t activeEvery();                       // the active node's check interval and SD flush choice (Background mode uses the background node's)
uint8_t activeFlush();
void drawHeader();
void drawStatus();
void setLink(uint8_t l, const char *text);

// ---- mesh_slots.cpp ---------------------------------------------------------------------------------------------------------------
void syncPaired();                           // gPaired and the SD files follow the active slot
bool activateSlot(uint8_t s);                // texts of the node shown go to its folder, the store is emptied, the new node's folder is loaded
void learnNodeId();                          // the active node's id is known: the slot and its SD folder get it
bool unpairSlot(uint8_t s, bool keep);       // keep: the slot stays archived with its texts; else everything of it goes

// ---- mesh_worker.cpp --------------------------------------------------------------------------------------------------------------
void postLink(uint8_t l, const char *text);
bool sendToRadio(const uint8_t *buf, size_t n);
bool sendWantConfig(uint32_t nonce);
bool workerLinkUp(bool configOnly = false);
bool drainRadio(unsigned long maxMs = 0);
uint32_t askPalmForPin();
void post(const Cmd &c);
void ensureWorker();
void stopWorker();                           // stops the worker's Bluetooth work and switches the radio off (waits until it has)

// ---- mesh_admin.cpp ---------------------------------------------------------------------------------------------------------------
bool adminSend(uint32_t me, const uint8_t *adm, size_t al);
bool adminAck(const char *what, unsigned long ms);
bool cfgRead(char *why, size_t wcap);
bool cfgSet(const uint8_t *body, size_t len, char *why, size_t wcap);
bool cfgAct(uint8_t action, uint32_t utc, char *why, size_t wcap);

// ---- mesh_palm.cpp ----------------------------------------------------------------------------------------------------------------
void writeFrame(uint8_t type, const uint8_t *b, size_t n);
void sendErr(uint8_t code, const char *text);
void markSeen();
void pumpNode();                             // the loop task's look at what the worker finished, and the news for the Palm
void dispatch(uint8_t type, const uint8_t *body, size_t len);
// A frame the Palm sent while a fetch was waiting for the Palm's acknowledgements (the user pressed Send during one) is kept (stashFrame) and
// handled when the fetch is over, instead of being lost.
void runStashed();
void handleHist(const uint8_t *body, size_t len);

// ---- mesh_store.cpp ---------------------------------------------------------------------------------------------------------------
uint32_t spoolFloor();
int spoolPending();
bool spoolFlush();                           // writes the texts the card does not hold yet; true when there was nothing to do or it worked
void ringAdd(const MeshSpool::Rec &r);
bool ringFlushToSd();
void ringRestore();
void seqEnsure();
void seqBootOnce();
void spoolRestore();

}  // namespace MeshRelay

#endif  // WIRSA_FEATURE_MESH
