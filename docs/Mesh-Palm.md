# Mesh Relay (Meshtastic) - Palm link

WiRSa's Mesh Relay mode links the Palm app `meshtastic.prc` (palmos_sdk `apps/meshtastic_client`) to a
Meshtastic node over Bluetooth LE. The Palm cannot do BLE or the node's protobuf protocol, so WiRSa holds
the connection, keeps the node list and the messages, and serves the Palm over the serial line.

Build switch: `WIRSA_FEATURE_MESH` (globals.h, default 1). Set it to 0 (build flag `-DWIRSA_FEATURE_MESH=0`)
to leave out the mode, its menu entry and the Bluetooth code. Main menu entry `Mesh[T]astic`, mode letter `T`
(WiRSaCtl ENTER). Needs the NimBLE-Arduino library when on.

Files (all in `Firmware/src/modules/`, shared declarations in `include/mesh_internal.h`, which also says which task does what): `mesh_relay.cpp` (the
mode itself: how it starts, runs and ends, as the relay skeleton sees it), `mesh_palm.cpp` (the frames the Palm sends and their answers, the frame
table), `mesh_worker.cpp` (the node on its own task: connect, pairing, scan, reads, the key exchange), `mesh_admin.cpp` (changing and reading the
node's settings), `mesh_store.cpp` (texts outside the store: the SD card, the RTC ring, the seq marks), `mesh_background.cpp` (background checks at the
main menu, deep sleep, the timer wake), `mesh_state.cpp` (the shared state, settings in NVS, the OLED), `mesh_slots.cpp` (the table of nodes: showing another one, learning a node's number, unpairing; the table itself is `include/mesh_slots.h`), `mesh_ble.cpp` (NimBLE client),
`mesh_core.cpp` (nodes, channels, messages), `mesh_pb.cpp` (the protobuf messages, by hand), `mesh_spool.cpp` (record format and the RTC ring),
`mesh_spool_sd.cpp` (the SD files), `deep_sleep.cpp` (sleep and wake). Text for the Palm (UTF-8 to Palm Latin, emoji, and back) is `palm_text.cpp`,
shared with the other relays. Entering, leaving and the top of the loop come from `relay_mode.cpp`, shared with the other relays
(`docs/Relay-Modes.md`); the timeouts are in `relay_timing.h`. Host tests: `test_host/mesh_pb_test.cpp`, `test_host/mesh_core_test.cpp`,
`test_host/mesh_spool_test.cpp`, `test_host/mesh_slots_test.cpp`, `test_host/palm_text_test.cpp`.

## The wire

The line framing, the secured link, the session rules and **every frame with its body** are defined in one place: [protocol/Protocol-wire.md](protocol/Protocol-wire.md)
(sections 2 to 7 for the common layer, section 9 for the Mesh frames). This document describes what Mesh Relay does with them. The mode starts at 9600 and `HELLO`
negotiates up. WiRSaCtl's `ENTER` with letter `T` starts the mode; its PING and STATUS work while the mode runs.

### Message status (outgoing)

The status values are defined in [protocol/Protocol-wire.md](protocol/Protocol-wire.md), 9.5; this table adds the mark the Palm draws for each.

| status | meaning | the Palm's mark |
|---|---|---|
| 0 | sending: queued on WiRSa or being written to the node | `...` (character 0x18, bold) |
| 1 | sent: the node has it, no acknowledgement yet | `?` (bold) |
| 2 | delivered: for a direct message the recipient's own ack, for a channel message any ack (reached the mesh) | tick (Symbol font 0x16) |
| 3 | failed (err says why) | `!` (bold) |
| 4 | relayed: a direct message only a relay acknowledged (the answer came from a node that is not the recipient). The recipient's ack makes it 2 later, and a later relay ack never undoes a 2 | `~` (bold) |
| 5 | unknown (never set by WiRSa; shown like 1) | `?` |
| 6 | exchanging keys: a direct message that failed with 6, 35 or 39 waits for the key exchange and is sent once more | character 0x14, normal font |

An incoming message has no mark. The Palm lists status 0, 1, 4 and 6 messages in SYNC so their progress comes back as DELIV.

### Key exchange

A direct message that fails with Routing.Error 39 (the node has no public key for the recipient), 35 (the recipient has none
for us) or 6 (the recipient could not read it) is not failed at once, once per message: it goes to status 6 and one key
job is posted (`MeshCore::failMsg`). The worker (`keyExchangeTick`) sends a node info request to the recipient (nothing is stored; ours
goes along, so both sides get the other's key), waits for its node info or 20 s, and sends the text again with a new packet id. A
second failure is final. The Palm also asks quietly (REQ kind 1, localId 0) when a chat is opened with a node whose key is not known, once per
node while the app runs. The node record carries the key flag (`NodeInfo.hasKey`, from `User.public_key`, kept across updates that
leave it out).

**Names and keys of other nodes.** A node that WiRSa has no name for is asked for it with the same node info request (`MeshCore::nameRequest`, `nameTick` in the worker): 20 s after the link is up, then one node
at a time 25 s apart, at most 8 per link, and the same node not again for 6 hours (`NAME_ASK_*` in `relay_timing.h`). It does not run during a background check or while a key exchange is under way; the answer arrives as an ordinary node info. The log line `Mesh: the node sent its configuration: N nodes, M with a name` after each full node list shows how many names the node itself had.

## Sync

`seq` rises with every stored message and never goes back, also across restarts and sleeps: NVS holds a mark that no
seq has reached (`wmesh/seq`); a boot starts at the mark and moves it 64 up, and the main loop moves it again when a long
awake period comes within 16 of it (the first ever mark is 1000000, above anything an older firmware handed out). Without
that, texts restored after a restart would get small numbers again and hide behind a Palm cursor that lies inside that range. The Palm keeps the highest seq
it has and sends it as the cursor; WiRSa sends what is newer. A cursor higher than WiRSa's last seq (WiRSa
restarted) means "everything": the Palm keeps what it holds, asks again from 0 and skips the messages it already has (the same sender and packet id; an older stored copy without an id, same sender, minute and text). The Palm sends SYNC without waiting
for the answer and acknowledges the NODE, CHAN, MSG and DELIV frames as they arrive; the channel list comes from the store at once
and is asked for again until the count stops changing. Unfinished sends (status 0, 1, 4, 6) are listed in SYNC by their localId and come
back as DELIV frames with the status now. `rxUtc` is the node's receive time; when the node left it out WiRSa
uses its own clock, and 0 means "unknown - use the time the Palm received it".

## Node list

WiRSa holds up to 500 nodes (`MeshCore::MAX_NODES`, in PSRAM; 64 if that much memory is not there); a busy mesh peaks near 350. The Palm keeps only a
compact entry for each (number, long name to 23 bytes, short name to 5, last heard, favourite and key flags, about 40 bytes) and asks
for the rest when a node's card is opened (NODE_GET). SYNC with the node bit sends this node in full (NODE) and then the rest sorted in NODES
frames of up to 6, so 350 nodes are about 60 frames. A Palm table that fills in that order needs hardly any sorting. A node marked as a favourite on the node itself comes with the favourite flag; the Palm's favourite check box sends CFG_ACT 2 / 3
(`set_favorite_node` / `remove_favorite_node`, the node number as an unsigned value), and WiRSa marks the node in its store only after the node has acknowledged it (`Mesh: node ... is now a favourite on the node`). A refresh of
the whole list from the node itself is CONNECT with keep = 2: the node sends its list slowly over Bluetooth, so the worker reads it in
1 s slices (`drainRadio(maxMs)`) and answers when it has been quiet for 3 s or after 40 s. A single read of up to 200 packets
once took longer than the Palm's wait and the Palm dropped the link.

## Keeping the link alive

- **The probe and PING** are defined in [protocol/Protocol-wire.md](protocol/Protocol-wire.md), 5.4, and the probe is the shared skeleton's (`RelayMode::tick`, `docs/Relay-Modes.md`): Mesh switches it on,
  BitWarden and CalDAV leave it off. The Palm sends its own PING only when no fetch is under way, because a frame sent in the middle of a fetch would be taken for a missing ACK.
  A Palm that is blocked cannot send, so it also answers a PING while waiting for a reply or in the middle of a fetch.
- **HOLD:** the Palm's alerts and keyboards stop it answering, so it asks for 120 s of grace first.
- **BYE from WiRSa** when the mode ends with the Palm connected; the Palm closes its side and shows Disconnected. The Palm's own BYE ends Session mode (and
  in Background mode the checks go on at the main menu).
- **Heartbeat before `want_config`:** the node drops a ToRadio write that is byte for byte the one before it ("Drop duplicate ToRadio packet"), and the config-only request is always the
  same four bytes, so a second read of the settings on a link was thrown away and ended "settings missing". Every `want_config` is
  now preceded by a heartbeat packet.

## MQTT and the channel form

- **MQTT page** (Settings on the Palm): enabled, address, username, password, root topic, encryption, JSON, TLS, proxy to client, map reporting. It is written with the
  Meshtastic `set_module_config` (ModuleConfig.mqtt = 1, MQTTConfig fields 1 to 10 and `map_report_settings` 11, defaults omitted, the rest copied back
  from what the node sent). **A node turns its Bluetooth off the moment it receives an MQTT module config**, even inside an edit transaction, and
  reboots about 7 s later, so it is sent alone (CFG_SET with only the MQTT bit, not inside begin_edit / commit); the
  Palm then says the node is restarting and the link comes back by itself. Other parts are sent in a transaction with an acknowledgement for each step.
- **Admin packets** carry want_ack, want_response, priority 70 and the PKI flag, with no hop limit, as the official client's.
- **Channels:** an empty primary name is allowed; an empty key is stored as the default key `AQ==` (a single byte `01`). Each channel has MQTT uplink / downlink check boxes
  (ChannelSettings 5 and 6).

## Talking to the node

- **Connect scan first:** a Bluetooth connect that times out ended in a crash (a null call on core 0), so the node is first looked for with a fixed 2 s scan
  (`nodeVisible()`; the scan is never stopped by hand) and the connect is only tried when it is seen. A node that is restarting says so ("try again in a minute").

## Pairing

The Palm shows the scan list (SCAN) and sends PAIR for the chosen node. WiRSa connects and starts the
Bluetooth pairing. What happens next depends on the node's Bluetooth mode, and all three work:

- **Random PIN:** the node generates a PIN only now and shows it on its screen. WiRSa sends PIN_NEED, the Palm
  shows its keypad (title "PIN shown on the node"), and the answer comes back as PIN.
- **Fixed PIN:** the same exchange; the user types the fixed PIN (123456 unless it was changed).
- **No PIN:** the node cannot ask for one. WiRSa first asks for an authenticated pairing; if that fails before
  any PIN was requested it tries again without that requirement ("just works"). No PIN_NEED is sent.

Then PAIR_R says how it went (with the slot and the node's number once known). PAIR_R comes only after the node's configuration has been read, which takes up to a minute, so the Palm shows a
waiting text meanwhile. The bond is kept by the Bluetooth stack in flash; the PIN is never stored. A node that still has our bond from before asks for no PIN. The
paired node is kept in the slot table in flash (NVS namespace `wmesh`), and WiRSa reconnects by itself whenever the mode runs. A pairing that does not finish is dropped after 45 s.

## Several nodes

WiRSa keeps up to four nodes (`MeshSlots::MAX`, `include/mesh_slots.h`): each is *paired* (linked to when shown, bond kept) or *archived* (unpaired but its texts kept). A slot holds the node's address, name,
number (learned from the node once its configuration has arrived, `learnNodeId`), its Background check interval and its SD flush choice (stored as index + 1; 0 = not set), and the table is one NVS blob (`slots`, with `active` and `bgslot`).

- **Shown node:** one node is shown at a time (`gActive`). The Palm picks it with SLOT_SELECT (SLOTS_GET / SLOTS_R list the slots; flags bit 0 shown, bit 1 background). Showing another node
  writes the texts of the old one to its SD folder, drops the link, empties the store, the sequence-number dedup (`resetSeen`) and the Palm bookkeeping, restores the new node's texts from its SD folder and
  leaves the link idle; the Palm reconnects it (CONNECT). Sequence numbers are one rising counter shared by all nodes; each Palm database keeps its own cursor. The Palm connecting to a node it picked
  while offline sends SLOT_SELECT right after HELLO.
- **Background node:** at most one node runs Background mode (`gBgSlot`). Background checks happen only for that node and only while no Palm session holds the Bluetooth link (`bgReady()`); a Palm
  session on another node is a Session-mode node and the checks wait. Leaving Mesh mode returns to the Background node. Setting Background on the shown node moves it from the node that had it.
  A new pair always starts in Session mode (interval 5 min and flush "when full" are kept for when Background is chosen later). RELAY_GET / RELAY_SET and the interval and SD flush choices refer to the shown node.
- **Unpair:** UNPAIR carries the slot and a keep flag. *Keep* forgets the bond and makes the slot an archive (no link; its texts stay in `/Meshtastic/<node id>/`). *Delete* also removes the node's SD folder and frees the slot. Both log
  one line (`Mesh: unpaired <id> (slot n), kept as an archive` / `deleted`). If the shown node goes, the next paired node (Background node first) is shown, or none.
- **Node number unknown:** until the node has said who it is, nothing is read from or written to the SD card for it; its texts wait in RAM.

## Modes and settings

## Modes and settings

The node's own settings (owner, LoRa, role, Bluetooth, channels, node clock, reboot) are read and written with
Meshtastic admin messages (CFG_GET / CFG_SET / CFG_ACT). RELAY_SET chooses, for the shown node, **session** (0) or **background** (1) and,
for background, how often WiRSa checks the node (every 1, 5, 15, 30 or 60 minutes) and **SD Flush**, when the RTC ring of a sleeping WiRSa goes to the card: after every check, after 3, 5 or 10 checks, or only when the ring is full
(`FLUSH_CHECKS` in `mesh_background.cpp`). Both are kept per node.

In both modes the Palm app's BYE ends Mesh mode (and the Exit Timers end it when no Palm turns up or a Palm goes quiet),
and WiRSa is back at the main menu. In **background** mode the checks then go on at the main menu (below); in session
mode there are none.

## Background checks

With a node set to background (only one is), WiRSa reaches that node by itself: the worker task keeps the schedule
(`gNextFetch`), and it starts at boot (`meshBgBoot()`), so it needs no Palm and no open mode.

- **A check:** connect and secure (about 2 to 3 s), ask the node for its settings only (`want_config` with the
  config-only nonce: the node list is not needed to receive texts), read until the node has sent its end marker and then
  been quiet for 2 s (25 s at most), hang up and shut the Bluetooth stack down. About 12 s on the test node. The wait
  until the next check is counted from the end of one: with "1 minute" the cycle is about 72 s.
- **Only at the main menu.** While another mode is open (SLIP, FTP, HotSync, SASD...) the Bluetooth is let go, a running
  check is cut at once (`meshBgPoll()`), and the check stays due: it runs when the main menu is back. A check cut because a
  mode opened is not counted as a failed connect. A connect that fails normally (node out of range) waits for the next
  interval.
- **A Palm session** drops a background link and reconnects with the full request (node list and all); entering the mode
  also holds the first background check back 8 s, so a Palm that says hello within that time takes the full link directly. **After a boot** the first check waits 60 s (`meshBgBoot()`): a reboot is not a due check, and normal use comes first. A deep-sleep timer wake is the scheduled check and runs at once.
- **WiFi and Bluetooth** share the radio: while the Bluetooth stack is up the WiFi keeps modem sleep (otherwise the
  coexistence code aborts: `Should enable WiFi modem sleep when both WiFi and Bluetooth are enabled`).
- **NEWS** also goes out when the node's own battery value changes: after a restart the node table fills only after the
  Palm has connected, and the Palm would otherwise keep a missing battery reading.

## Deep sleep between checks

`deepSleepMin` (WiRSaControl Setup "Sleep:": Off, 1, 3, 5, 15 minutes) puts the board to sleep after that long without
Palm traffic at the main menu. It was built for the Palm line wake (GPIO3, wired to the Palm RX on GPIO44, wakes on a start
bit); with background checks on, a timer is armed too.

- **Idle time:** counted from the last real activity (Palm bytes, USB console, a mode, FTP). A background check is not
  activity, so the clock runs across the checks, but a running check holds the sleep until it has ended.
- **Before sleeping:** the texts the SD card does not hold yet are written to it, or into the RTC ring when there is no
  card; the WiFi is switched off; the timer is set to the time until the next check (at least 5 s).
- **Timer wake:** `meshHeadlessWake()` runs first in `setup()` and does not return. CPU 80 MHz (the radio needs 80 MHz;
  nothing here needs more), no WiFi, no display, no menu: it runs one background check, puts the texts found into the ring and
  sleeps again for the interval. About 10 s awake and 28 mA asleep. The LED stays off, which is how a timer wake differs
  from a full wake. In a live session an RGB status LED shows Mesh activity in green (see `README-Palm.md`, "Status LED"); the background checks never light it. The first and last lines of its log may be lost: the USB port needs about 2 s to come back after a wake.
- **Palm line wake:** a normal boot at full speed with everything on: the ring and the SD files are restored into the
  store, what the card does not hold is written to it, a background check runs at once, and the board stays awake for the
  whole Sleep time. A wake by line noise does the same (it sleeps again after the Sleep time).
- **During the ~10 s of a timer wake the board does not listen to the Palm line.** A Palm that talks then gets no answer
  and has to try again (WiRSaCtl's Find and Refresh do).
- **Sleep Off:** the board stays awake and the checks go on without it.
- **Lost:** the PSRAM store and the node table in any sleep, and the ring on a reset (reset button, crash, watchdog,
  reflash) or power loss: fast RTC memory survives a deep sleep only. The SD card survives everything.

## Texts outside the store

The store is 200 messages in PSRAM (and the node table): a deep sleep or a reset empties it. So texts also live elsewhere
until a Palm has them. All of them are plain text, with no encryption.

- **SD card, `/Meshtastic/<node number, 8 hex digits, lower case>/`:** one folder per node (`MeshSlots::dirName`). `mesh.dat`, rolled at 1 MB to `mesh.old` (1 to 2 MB are kept). A record is
  `A5 5A, len, from4 to4 rxTime4 packetId4 chan1 flags1 len1 text, crc8`; a reader skips damage and finds the next marker.
  Written in batches while no Palm is connected (8 texts waiting, or the oldest has waited 10 minutes), when the mode
  exits, before a sleep and at a full wake. No card or a write error: the texts stay in memory, one log line says so
  (`Mesh: SD card not available`) and it tries again every minute; a card pulled while mounted is noticed (`sdAlive()`
  reads the first sector, because `SD.cardType()` keeps the type found at mount) and mounted afresh when it is back. The card
  is used only at the main menu and in Mesh mode (SASD owns it otherwise).
- **RTC ring:** 4 KB of fast RTC memory (`RING_BYTES` in `mesh_relay.cpp`, adjustable; the records are 19 bytes plus the
  text, about 45 typical texts). Filled by the timer-wake checks of the Background node. It goes to the SD card as the **SD Flush** choice says (every check, 3, 5 or 10 checks) and in any case when full. No card: the
  oldest records go and are counted (`Mesh: N text(s) were lost while asleep ...`).
- **Restore:** once per boot, an empty store takes the newest 64 KB of the SD files back (oldest first, so the seq order
  is the time order), then the ring. A text already there (same sender and id, or same sender, time and text) is not taken
  twice.
- **HIST and SD History:** after each fetch the Palm sends HIST with its SD History setting and the highest seq it has
  saved. With History **off** (the default) the SD files are emptied once everything on them is covered by that seq
  (`the Palm has everything - SD card emptied`) and the card is a spool of texts no Palm has yet; with it **on** the card
  keeps everything (a history, up to 2 MB). WiRSa remembers the setting (`wmesh/hist`). CLEAR 0 (Device menu "clear WiRSa's
  store") empties the card too.

## Power

Measured at 5.1 V on the USB input, with a battery connected, SD card in:

| state | current | power |
|---|---|---|
| WiRSa awake at the main menu, WiFi on, checks waiting (before the sleep work) | 178 mA | 0.91 W |
| awake, idle, no radio, 240 / 160 / 80 / 40 MHz | 67 / 61 / 57 / 48 mA | 0.34 / 0.31 / 0.29 / 0.24 W |
| Bluetooth listening all the time, 240 / 80 MHz | 134 / 123 mA | 0.68 / 0.63 W |
| light sleep | 31 mA | 0.16 W |
| deep sleep | 28 mA | 0.14 W |

The board itself accounts for about 28 mA at 5 V (a bare sketch that only sleeps reads the same), so light and deep
sleep differ by 3 mA, the CPU speed matters little, and the WiFi is the large cost of staying awake. A check costs about
95 mA over the floor for about 12 s: hourly checks add about 0.3 mA on average, 5-minute checks about 4 mA, 1-minute
checks about 16 mA. The battery-only floor was not measured. The measurement sketch is `Firmware/tools/power_test/` and the
USB log recorder `Firmware/tools/wirsa_log.py`.

## Log lines

The USB log (Config > Log level) has three levels for this mode. **Warn:** something failed or was refused (a write that did not go, no answer from the node, an unreadable frame, the SD card missing,
texts lost while asleep, an unknown Palm frame). **Info:** what happened that a user would want to see: the mode entered and left, the Exit Timers, the speed agreed, connecting, `Mesh BLE: ready`, texts received and sent, a message
written to the node, a routing answer (who acknowledged, error code), the key exchange, settings sent, a background check, the sleep and wake lines, SD writes and restores. **Verbose:** the
detail of how it got there: the node's queue answers, each step's acknowledgement time and the settings write mask, the Bluetooth connect and pairing steps, and
each frame the Palm sends (`Mesh: frame SYNC, 12 byte(s)`, from the shared frame table, `docs/Relay-Modes.md`).

## Exit

The Exit Timers and the probe are described in [protocol/Protocol-wire.md](protocol/Protocol-wire.md), 5.4 and 5.5: a quiet Palm is sent PING and then the mode returns to the main menu, and so
does a Palm session that never begins. In Mesh a running node operation counts as activity. Palm app and usage notes: `apps/meshtastic_client.md` in the palmos_sdk repository.

## Why a message failed (err / code)

With status 3 (failed) the message carries a reason code; the codes and the key exchange that a failure with 6, 35 or 39 goes through first are defined in
[protocol/Protocol-wire.md](protocol/Protocol-wire.md), 9.5. The Palm shows each reason as **Error**, **Meaning** and
**What to do**, with the titles and meanings of the Meshtastic Android app's "Delivery Errors" (Data.c `ErrTitle`, `ErrMeaning`, `ErrAction`):

| code | title | meaning |
|---|---|---|
| 1 | No route | No path to the destination. The recipient may be offline or out of range. |
| 2, 3, 5 | Failed to deliver to mesh | Retries exhausted. This covers a relay refusing (NAK), a timeout, or running out of retransmits. |
| 4 | No radio interface | No radio available to send. |
| 6 | Channel/key mismatch | The channel or PSK doesn't match. For a direct message, the other node may also lack your public key. |
| 7 | Message is too large to send | The message exceeds the maximum payload size. |
| 8 | No app response | The app or plugin didn't respond. |
| 9 | Duty cycle limit | The regional airtime limit was reached. |
| 32 | Invalid request | The request was malformed. |
| 33 | Not authorized | The destination refused the request. |
| 34 | Could not send encrypted message | Encryption failed. |
| 35 | Recipient needs your key | Direct message encryption couldn't complete because the other node lacks your public key. |
| 36 / 37 | Admin session expired / Admin key not authorized | A remote-admin session timed out / the target doesn't accept your admin key. |
| 38 | Rate limited | The mesh is throttling you. |
| 39 | Recipient key unavailable | You don't have the recipient's public key. |
| 240 to 243 | Not written to the node / Not linked to the node / Not connected to WiRSa / Message too long | WiRSa's and the Palm's own. |
