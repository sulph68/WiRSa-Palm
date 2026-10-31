// relay_timing.h - the timeouts, delays and speeds of the relay modes (BitWarden, CalDAV, Mesh) in one place, each with what it
// is for. They were spread through the three modes, some of them twice (BitWarden and CalDAV carried the same four numbers under
// different names). A value that two modes share has one name here; a value that is one mode's own has that mode's prefix.
//
// Units: _MS milliseconds, _S seconds. Changing one is a behaviour change: the reason for each is the comment beside it.
#pragma once
#include <stdint.h>

namespace RelayTiming {

// ---- The Palm line ----------------------------------------------------------------------------------------------------------
// Every session starts at this rate (the Palm's apps open at the same one); HELLO negotiates up from it. WiRSaCtl's frames are
// accepted at this rate only. SASD has its own initial rate of the same value (sasd.cpp).
constexpr uint32_t BAUD_IDLE = 9600;

// ---- BitWarden and CalDAV (long request / reply exchanges that end with a BYE) ------------------------------------------------
constexpr uint32_t BYTE_TIMEOUT_MS = 3000;          // the longest gap between two bytes of one frame
constexpr uint32_t FRAME_IDLE_TIMEOUT_MS = 60000;   // how long to wait for the first byte of the next frame
constexpr uint32_t NEGOTIATE_TIMEOUT_MS = 15000;    // a rate the Palm negotiated and never used (no sync request) is given up after this
constexpr uint32_t ITEM_ACK_TIMEOUT_MS = 10000;     // one try of an item frame: the Palm's ACK is awaited this long, a retry's sleep included (the Palm's record writes can be slow)
constexpr uint32_t ACK_RETRY_DELAY_MS = 500;        // sleep before each retry of an acknowledged frame; it counts against that try's ack timeout
constexpr int BW_ACK_ATTEMPTS = 5;                  // BitWarden's tries for an acknowledged frame (the Palm's own SendItemAckWithRetry matches it)
constexpr int CDDV_ACK_ATTEMPTS = 3;                // CalDAV's tries; a frame still unacknowledged is deferred (relay_queue.h)
constexpr uint32_t BW_TWO_FACTOR_TIMEOUT_MS = 180000;   // BitWarden: a person types the 2FA code on the Palm (matches the Palm's own 180 s)
constexpr uint32_t CDDV_OAUTH_TIMEOUT_MS = 300000;      // CalDAV: the Google sign-in listener stays open this long

// ---- Mesh: frames on the Palm line --------------------------------------------------------------------------------------------
constexpr uint32_t MESH_IDLE_TIMEOUT_MS = 50;       // waiting for a frame is a short poll: the loop also serves the node
constexpr uint32_t MESH_BYTE_TIMEOUT_MS = 500;      // the longest gap between two bytes of one frame
constexpr int MESH_ACK_ATTEMPTS = 5;                // tries for an acknowledged frame (NODE, CHAN, MSG, DELIV)
constexpr uint32_t MESH_ACK_TIMEOUT_MS = 3000;      // ... one try's wait for the ACK, a retry's sleep included (the Palm applies a frame, then acknowledges)
constexpr uint32_t NEWS_REPEAT_MS = 3000;           // NEWS is repeated this often until the Palm has fetched it
constexpr uint32_t ENTER_CHECK_HOLD_MS = 8000;      // after entering the mode, a background check waits so a Palm that says hello takes the full link
constexpr uint32_t PIN_WAIT_MS = 40000;             // the Palm has this long to answer a node's PIN request

// ---- Mesh: the node over Bluetooth --------------------------------------------------------------------------------------------
constexpr uint32_t RECONNECT_MS = 20000;            // between attempts to reach the node when the link is down
constexpr uint32_t BLE_CONNECT_TIMEOUT_S = 10;      // one connect attempt
constexpr uint32_t BLE_PAIRING_WATCHDOG_MS = 45000; // a pairing that has not finished is dropped
constexpr uint32_t ADMIN_ACK_MS = 3000;             // each step of a settings write waits this long for the node's answer
constexpr uint32_t SETTINGS_READ_MAX_MS = 40000;    // reading the node's settings: give up after this
constexpr uint32_t SETTINGS_RESEND_AFTER_MS = 3000; // ... nothing at all by now: ask once more
constexpr uint32_t SETTINGS_QUIET_MS = 8000;        // ... the node has gone quiet: that is all it has
constexpr uint32_t NODE_LIST_MAX_MS = 40000;        // refreshing the whole node list: at most this long
constexpr uint32_t NODE_LIST_QUIET_MS = 3000;       // ... or when it has been quiet this long
constexpr uint32_t NODE_LIST_SLICE_MS = 1000;       // ... read in slices of this length, so the loop keeps its own clock
constexpr uint32_t NODE_RESTART_MS = 120000;        // after the node is told to restart: failing to reach it is expected for this long
constexpr uint32_t NODE_RESTART_MQTT_MS = 150000;   // ... after MQTT settings (it reboots about 7 s later and takes longer to come back)
constexpr uint32_t NAME_ASK_FIRST_MS = 20000;       // a node without a name: asked for it this long after the link is up (the node list has come in by then) ...
constexpr uint32_t NAME_ASK_MS = 25000;             // ... then one at a time, this far apart ...
constexpr int NAME_ASK_MAX = 8;                     // ... at most this many for one link
constexpr uint32_t NAME_ASK_AGAIN_MS = 6UL * 3600000UL;   // the same node is not asked again before this
constexpr uint32_t KEY_WAIT_MS = 20000;             // the key exchange: wait this long for the recipient's node info, then send again

// ---- Mesh: background checks and the store ------------------------------------------------------------------------------------
constexpr uint32_t CHECK_QUIET_MS = 2000;           // a check ends when the node has been quiet this long after its end marker ...
constexpr uint32_t CHECK_MAX_MS = 25000;            // ... or after this long at the most
constexpr uint32_t BOOT_CHECK_DELAY_MS = 60000;     // after a boot the first check waits (a reboot is not a due check)
constexpr uint32_t HEADLESS_CHECK_MAX_MS = 45000;   // a timer wake's check: at most this long awake
constexpr uint32_t SPOOL_AGE_MS = 10UL * 60000UL;   // texts waiting for the SD card are written when the oldest has waited this long ...
constexpr int SPOOL_BATCH = 8;                      // ... or when this many wait
constexpr uint32_t SPOOL_RETRY_MS = 60000;          // no card or a write error: not again before this
constexpr uint32_t SPOOL_POLL_MS = 5000;            // how often the main loop looks at the spool

}  // namespace RelayTiming
