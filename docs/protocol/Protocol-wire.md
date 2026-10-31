# The Palm line - wire protocol of WiRSa's Palm-facing modes

This is the one place that defines what crosses the serial line between a Palm (or any other client) and WiRSa: how bytes are framed, how the link is
secured, how a session runs, and every frame of every protocol with its body. The firmware is the source: each fact here was read from the code,
and section 14 says which code. The Palm apps (`apps/meshtastic_client`, `apps/caldav_sync`, `apps/bitwarden`, `apps/WiRSaControl`) implement to this
document; their own documents describe the apps and point here for the wire.

**One place to update:** a change to a frame is made in the firmware, in the Palm code, and here, together. Where this document and the code
disagree, the code is right and this document has a bug.

| Protocol | Section | Palm side | Firmware |
|---|---|---|---|
| Mesh Relay (Meshtastic) | [9](#9-mesh-relay) | `apps/meshtastic_client` | `mesh_*.cpp` |
| CalDAV / CardDAV Relay | [10](#10-caldav--carddav-relay) | `apps/caldav_sync` | `caldav_*.cpp` |
| BitWarden Relay | [11](#11-bitwarden-relay) | `apps/bitwarden` | `bitwarden_*.cpp` |
| Palm control (WiRSaCtl) | [12](#12-palm-control-wirsactl) | `apps/WiRSaControl`, `apps/lib/wirsactl.c` | `palm_ctl.cpp` |

HotSync, SASD and SLIP speak Palm's own protocols, not this one. FTP is not on the Palm line.

## 1. Transport

- One serial line (the Palm's cradle port), 8N1 (`WIRSA_SERIAL_FORMAT`, `globals.h`). Palm side: the Serial Manager (`SrmOpen` on `serPortCradlePort`,
  `relayproto.c`). WiRSa side: `PhysicalSerial`. One mode owns the line at a time.
- **Every session starts at 9600 baud** (`RelayTiming::BAUD_IDLE`, Palm `rpBaudIdle`). `HELLO` negotiates a faster rate (5.1).
- A mode is started from WiRSa's main menu or by WiRSaCtl's `ENTER_MODE` (section 12); the Palm app then opens the line and begins at section 4.

## 2. Frame

```
+------+---------------+--------+----------+
| type | bodyLen (u16) |  body  | checksum |
| 1 B  | 2 B, big-end. | n B    | 1 B      |
+------+---------------+--------+----------+
```

- `checksum` = XOR of `type`, both `bodyLen` bytes and every `body` byte (`relay_proto.cpp readFrameRest`; Palm `RpRecvFrame`). A frame with a wrong checksum is
  dropped (WiRSa logs a warning); the sender finds out from the missing reply.
- A frame whose `bodyLen` is over the receiver's buffer is read through to its checksum and thrown away, so the stream stays in step.
- Body limits of the receivers: Mesh 960 bytes (`BODY_MAX`), CalDAV 3192 (`CDDV_MAX_BODY`; the Palm's `cddvMaxFrameBody` must match), BitWarden 4096
  (`BW_MAX_BODY`), control link 1024.
- Byte timeout (the longest gap inside one frame): WiRSa 3 s on BitWarden and CalDAV, 0.5 s on Mesh, 0.1 s on the control link; the Palm waits 3 s
  (`rpByteTimeoutTicks`). The wait for the first byte of a frame: WiRSa 60 s on BitWarden and CalDAV, a 50 ms poll on Mesh; the Palm's default is 180 s.
- The control link puts one byte, `0x16`, in front of every frame, in both directions (section 7).

## 3. Data types

All multi-byte integers are big-endian in both directions.

| Type | Wire form |
|---|---|
| `u8`, `u16`, `u32` | 1, 2, 4 bytes. `i8` is a `u8` read as two's complement |
| `str` | `u16 length` + that many bytes, no terminator; empty = length 0. Text is **Palm Latin** (cp1252 with the Palm's differences), not UTF-8: WiRSa converts both ways (`palm_text.cpp`). Emoji are dropped from contacts, calendar entries and vault items; in Mesh names each one becomes the single byte `0x10` (a symbol in the Palm's font) |
| `addr[6]`, `bytes[n]` | raw bytes; `n` is fixed by the frame |

- WiRSa's `Writer::str` cuts a string that does not fit the buffer to what fits and writes a matching length, so it cannot put the receiver out of step.
- A reader that runs past the end of a body has failed (`Reader.ok` is false): the frame is ignored or answered with the protocol's error frame, never half applied.
- Fields added to a frame later are **appended**, never put in the middle. A reader of an older shape stops before them (the control link's settings, Mesh `SYNC`),
  and a newer reader takes them only if the body is long enough.

Palm helpers: `apps/lib/relaywire.c`; WiRSa: `RelayProto::Writer` and `Reader`.

## 4. The secured link

Every frame after the key exchange is encrypted; there is no plaintext fallback. An old app or firmware without it gets no answer.

Right after the port opens (9600 baud, before `HELLO`) the Palm runs a key exchange (`relaysec.c RpSecure`; WiRSa `relay_proto.cpp handleKex`):

| Type | Name | Direction | Body |
|---|---|---|---|
| `0x70` | KEX_REQ | Palm -> WiRSa | `reqId u32` (random). Any session WiRSa had is dropped. A body that is not 4 bytes is ignored without a reply |
| `0x71` | KEX_PUB | WiRSa -> Palm | `reqId u32, modLen u16 (=128), modulus[128], r3[128], wRand[16]`. RSA-1024, e = 3. `r3` = R^3 mod modulus with R = 2^1024, so the Palm can encrypt with three Montgomery multiplications |
| `0x72` | KEX_KEY | Palm -> WiRSa | `reqId u32, RSA-PKCS#1-v1.5(secret[16])` (128 bytes). A wrong `reqId`, no exchange pending, a wrong length or a failed decrypt gets KEX_FAIL |
| `0x73` | KEX_OK | WiRSa -> Palm | sent **encrypted**; plaintext body `"RPOK"` |
| `0x74` | KEX_FAIL | WiRSa -> Palm | `reqId u32`. Also the answer to a KEX_REQ when WiRSa has no key |

- Session key = AES-128-ECB(key = `secret`, block = `wRand`): fresh for every connection.
- WiRSa makes its RSA key pair in RAM when a relay mode is first entered after a boot (a second or two, `secureBegin`), or at the first KEX_REQ if that failed.
  Nothing is stored; the Palm pins nothing.
- The Palm accepts a modulus of 64 to 128 bytes (even length) and a KEX_PUB of exactly `6 + 2*modLen + 16` bytes. It skips up to 8 frames carrying another `reqId`
  (left over from an abandoned attempt) and waits 20 s for KEX_OK. It makes up to 3 attempts, flushing the line between them. A WiRSa that never answers ends it
  at once, except when the Palm knows WiRSa is there (WiRSaCtl just started the mode): then silence is retried.
- Every other frame is encrypted: wire type = `type | 0x80`; wire body = `seq u32` + AES-128-CTR(body); wire `bodyLen` = body + 4. Counter block (16 bytes):
  byte 0 = direction (`'P'` 0x50 Palm -> WiRSa, `'W'` 0x57 WiRSa -> Palm), bytes 4-7 = `seq` big-endian, bytes 14-15 = the number of the 16-byte block within the
  frame (0, 1, 2 ...), the rest 0. Each sender starts `seq` at 0 after the exchange and adds 1 per frame (a resent frame too). The checksum covers the bytes as sent.
  Body limits apply to the decrypted body.
- Strict on both sides: a plaintext frame is accepted only if it is a key-exchange type, and an encrypted one only once the link is secured. An encrypted frame with
  a body shorter than 4 bytes is dropped. The one exception is the control link (section 7).

## 5. A session

### 5.1 HELLO and the rate

```
Palm  -> HELLO      baud u32                         the rate the Palm would like (its own setting)
WiRSa -> HELLO_ACK  version u8, caps u8, server str, baud u32
```

`HELLO` is `0x01` and `HELLO_ACK` `0x02` in Mesh, CalDAV and BitWarden (`RelayMode::handleHello`, `relay_mode.cpp`).

- WiRSa caps the offer at its configured maximum (Setup > Baud, `bauds[serialSpeed]`); an offer of 0 means 9600. `baud` in the reply is the rate both sides use next.
- `version` is 2 in BitWarden and CalDAV and 3 in Mesh (the numbers of section 6; Mesh 3 adds several nodes). `caps` is 0 (kept for later). `server` is `"WiRSa-Mesh/3"`, `"WiRSa-DAV/2"` or `"WiRSa-BW/2"`.
  The three Palm apps need 2 and stop with a message on less (an older CalDAV WiRSa sent the rate alone, which counts as less).
- The reply is sent at the **old** rate and flushed; only then does WiRSa switch its port. The Palm switches after it has read the reply.
- A `HELLO` with a short body is ignored and gets no reply (an old Palm build).
- BitWarden and CalDAV: a rate that was negotiated and then never used (no request within 15 s) is given up and WiRSa goes back to 9600.

### 5.2 Acknowledged frames

A frame that carries data the receiver must keep is **acknowledged** with an empty-body frame, and the next one is not sent before it arrives
(`RelayProto::writeFrameAcked`). The ack is the common `ACK 0x04` in all three.

- Before each try WiRSa throws away stale bytes on the line (a late ack of an abandoned try looks exactly like the ack of the retry), sends, and waits. With no ack in
  time it sends the **same** frame again after 0.5 s.
- Tries and waits: BitWarden 5 tries x 10 s, CalDAV 3 x 10 s (the Palm's record writes can be slow), Mesh 5 x 3 s.
- The CalDAV Palm retries its `ACK` send up to 40 times at 500 ms (`SyncEngine.c`).
- The BitWarden Palm retries its own ack send and its wait for the next frame up to 5 times each (`Sync.c`). Its first wait of a sync is the full 180 s (WiRSa derives the key and signs in
  first); after that it waits 12 s per attempt (`gBwIdleOverrideTicks`), so its patience of about 60 s tracks WiRSa's.
- If the peer sends `BYE` instead of the ack, it has ended the session: the sender stops at once.
- A frame that is neither the ack nor `BYE` while an ack is awaited is the peer's own message, sent at the same moment. Mesh keeps one such frame and handles it after
  the exchange (`stashFrame`, `runStashed`); the others count it as a failed try.

### 5.3 BYE

`BYE` (`0x03`, empty body) ends a session. In all three protocols the Palm sends it when its app closes. Beyond that:

- **Mesh:** WiRSa sends it when the mode ends while the Palm is connected (an Exit Timer, WiRSaCtl `EXIT_MODE`), so the Palm can show that it was disconnected. It does not send
  one in answer to the Palm's BYE: it puts its port back at 9600 and leaves the mode.
- **CalDAV:** WiRSa sends it after `SYNC_DONE`.
- **BitWarden:** WiRSa never sends it. The Palm's BYE also cancels a pending two-factor wait.

The next session starts again at section 1.

### 5.4 Are you still there? (Mesh)

Only Mesh keeps a session open for hours, so only Mesh has the probe (`RelayMode::tick`, `probeType` = Mesh `PING 0x4E`). While a session is active and the Palm sends nothing
for the **After session** time (default 15 s, section 5.5), WiRSa sends `PING`; if no frame at all arrives within the same time again it leaves the mode.
Any frame clears the probe. It is not sent while a node operation is under way or a dialog is open on the Palm (`HOLD`, 9.1). The Palm also sends `PING` itself every 5 s
while connected and idle; WiRSa takes it as presence and does not answer.

### 5.5 Exit timers

HotSync, CalDAV and BitWarden share two rules (`mode_timer.h`): no session within the **unused** time of entering (default 45 s), or none starting within the **after
session** time of the last one ending (default 15 s), and WiRSa returns to the main menu by itself; 0 = never. Mesh uses the same two times with the probe in 5.4. SLIP
and SASD have their own silence timeouts (1, 2, 3 or 5 minutes, and 3, 5, 10 or 15). All are WiRSa settings (section 12, `CFG_SET`): a client must not assume the values.

### 5.6 Junk filter

While a relay mode runs, a byte at a frame boundary that cannot start one of the mode's own frames is dropped before the frame reader sees it (`palmCtlModeHook`). It lets
through the types `1` up to the mode's highest (`RelayMode::Spec.maxPalmType`: Mesh `M_MAX_PALM_TYPE` = `0x52`, BitWarden `BW_MAX_PALM_TYPE` = `0x32`, CalDAV `CDDV_MAX_PALM_TYPE` = `0x21`), the same
with bit `0x80`, the lead byte `0x16`, and `0x70`-`0x74`. A new protocol must therefore keep every frame type the Palm sends at or below that value.

## 6. Type allocation

A frame type is 7 bits: the `0x80` bit marks an encrypted frame (section 4), so `0x80`-`0xFF` are the encrypted forms of `0x00`-`0x7F` and are never types. This section is the allocation
every relay, present or future, follows. **Status: in use.** All three relays use it (the line at the end of 6.3 says which version); `check_wire.py` checks sections 9 to 12 against the code.

### 6.1 Allocation of the type space

| Types | Use | Used | Spare |
|---|---|---|---|
| `0x00` | never a type | | |
| `0x01`-`0x06` | **Common frames**: `HELLO` `0x01`, `HELLO_ACK` `0x02`, `BYE` `0x03`, `ACK` `0x04`, `SYNC_DONE` `0x05`, `ERR` `0x06` | 6 | |
| `0x07`-`0x15` | Common, reserved for future common frames | | 15 |
| `0x16` | the control link's lead byte: **never a relay type** (section 7) | | |
| `0x17`-`0x1F` | Common, reserved for future common frames | | 9 |
| `0x20`-`0x6F` | **Relay window** (80 types), see 6.2 | see 6.3 | see 6.3 |
| `0x70`-`0x7F` | **Reserved.** `0x70`-`0x74` are the key exchange (section 4); both sides treat the whole `0x70`-`0x7F` as key-exchange types (`isKex`, `RP_IS_KEX`) and send them in plaintext, so a relay never uses any of them | 5 | 11 |
| `0x80`-`0xFF` | the encrypted forms of `0x00`-`0x7F`; never types | | |
| after a `0x16` lead byte | **WiRSaCtl**, its own namespace (section 12): requests `0x40`-`0x4E`, `ERR` `0x5F`, replies `0x60`-`0x6E` (request + `0x20`). The lead byte tells it from a relay frame (`palmCtlModeHook`), so it may overlap the window | 31 | |

### 6.2 Rules

1. **Common frames.** Every relay uses the six common frames for exactly those roles, with the bodies of sections 5 and 13, and never gives these numbers another meaning. Free common numbers are for future
   common frames, never for one relay's own use.
2. **The window.** Numbers inside `0x20`-`0x6F` are **per relay**: only one mode runs at a time, so a new relay may use any number there, including one an existing relay uses.
3. **Outside the window.** A relay that needs a number outside `0x20`-`0x6F` must take one that no other relay or protocol uses.
4. **Palm-sent frames first.** A relay lists the frames the Palm sends at the start of its block. The mode's Palm-sent bound (`RelayMode::Spec.maxPalmType`) is the highest of them: the junk filter (5.6)
   drops every other byte at a frame boundary, so a Palm-sent frame above the bound is lost and the Palm just times out. The bound is checked by `check_wire.py`.
5. **Never `0x16` from the Palm.** It is the control link's lead byte and is taken as one.
6. **No other restriction.** `0xBE`, HotSync's and SASD's own magic, is read only inside those two modes, so it limits no relay type.

### 6.3 What the existing relays use inside the window

| Types | Relay | Frames of its own | Spare in its block |
|---|---|---|---|
| `0x20`-`0x2F` | CalDAV / CardDAV | 7: `SYNC_REQUEST`, `EVENT_ITEM`, `CONTACT_ITEM`, `GROUP_ITEM`, `OAUTH_START`, `OAUTH_INFO`, `OAUTH_TOKEN` | 9 |
| `0x30`-`0x3F` | BitWarden | 9: `TRUST`, `TRUST_RESULT`, `SYNC`, `SYNC_ITEM`, `FOLDER`, `ORG`, `TWO_FACTOR_REQUIRED`, `TWO_FACTOR_SUBMIT`, `SYNC_STATUS` | 7 |
| `0x40`-`0x6F` | Mesh | 35 (state, scan, pair, nodes, sync, messages, relay settings, node settings, hold and the rest) | 15 |

These blocks are packed side by side only for ease of implementation. Rule 2 still lets a new relay reuse any of these numbers.

**Moved to this allocation:** all three relays (protocol version 2; sections 9, 10 and 11 have their numbers; Mesh is at version 3). The control link is unchanged.

## 7. The control link

WiRSaCtl talks to WiRSa while WiRSa is at its main menu, or in a mode that is idle or waiting for its Palm app (`palm_ctl.cpp`). The frame is the one in section 2 with
`0x16` (`CTL_LEAD`) in front, in both directions (`setControlLink`); menu text that WiRSa prints on the same line is skipped by the Palm until a `0x16` (`gRpLeadByte`).

- After a `0x16`, a second `0x16` is taken as the real start (the first was a stray byte). A type outside `0x40`-`0x4F` and `0x70`-`0x74` (ignoring bit `0x80`) restarts the search at once.
- Plaintext frames are sent and accepted while the link is not secured (`gRpAllowPlain` on the Palm); once a key exchange has secured it, frames are encrypted as in section 4.
- At the main menu, only `WIFI_SET` and `SECRETS_SET` may (and must) arrive encrypted; every other command must be plain, and a plain command drops any session. An encrypted
  command is one session: the session is cleared after its reply.
- The single-letter menu keys on the Palm line have a burst guard: a key counts only if the line was quiet for 100 ms before it and nothing but CR or LF follows within the next
  100 ms. A `0x16` skips the guard.
- In a relay mode (CalDAV, BitWarden, Mesh) and while HotSync waits, only `PING`, `STATUS`, `EXIT_MODE` and `ENTER_MODE` of the running mode's own letter are answered; anything else gets `ERR`
  busy. In a relay mode `EXIT_MODE` is accepted only at the idle rate (within 2 % of 9600), that is, between sessions; otherwise `ERR` busy ("A session is still running").

## 8. Timing, in one place

Values are in `Firmware/include/relay_timing.h`, with the reason for each.

| What | Value |
|---|---|
| Start rate of every session | 9600 baud |
| Byte timeout (WiRSa) | 3 s BitWarden and CalDAV, 0.5 s Mesh, 0.1 s control link |
| Wait for the next frame's first byte (WiRSa) | 60 s BitWarden and CalDAV, 50 ms poll Mesh |
| Ack wait x tries | 10 s x 5 BitWarden, 10 s x 3 CalDAV, 3 s x 5 Mesh; 0.5 s between tries |
| Negotiated rate never used (BitWarden, CalDAV) | back to 9600 after 15 s |
| BitWarden two-factor code wait | 180 s (3 attempts) |
| CalDAV Google sign-in listener | 300 s |
| Mesh PIN wait | 40 s |
| Mesh `PING` from the Palm | every 5 s while connected and idle |
| Mesh `NEWS` repeat | every 3 s until the Palm has synced |
| Key exchange: wait for KEX_OK / attempts | 20 s / 3 |
| Exit timers, unused / after session | 45 s / 15 s (settable) |

## 9. Mesh Relay

WiRSa holds a Bluetooth LE link to a Meshtastic node, keeps the node list, channels and messages, and serves the Palm over this line. Mode letter `T`. Files: `mesh_palm.cpp`
(the frames), `mesh_admin.cpp` (node settings), `mesh_worker.cpp` (the node link), `mesh_relay.cpp` (the mode). Frame types are `MeshRelay::M_*` in `mesh_internal.h`; the numbers follow section 6 (protocol version 3). What is not wire
(background checks, deep sleep, power, log lines) is in `Mesh-Palm.md`.

### 9.1 Frames

`P -> W` Palm to WiRSa, `W -> P` WiRSa to Palm. "Acked" means the Palm answers the frame with `ACK` (5.2).

| Type | Name | Dir | Body | What it does |
|---|---|---|---|---|
| `0x01` | HELLO | P -> W | `baud u32` | Opens the session and offers a rate (5.1) |
| `0x02` | HELLO_ACK | W -> P | `version u8, caps u8, name str, baud u32` | Names the protocol and gives the agreed rate |
| `0x40` | STATE | P -> W | - | Asks for the state; answered with STATE_R |
| `0x53` | STATE_R | W -> P | `link u8, battery i8, myNum u32, firmware str, nodeName str, nodes u16, messages u16, lastSeq u32, relayMode u8, every u8, utc u32, lastPollUtc u32, linkText str, slot u8` | The link, the node shown (its identity, name, slot) and counts, WiRSa's clock. `relayMode` is 1 when Background mode works for this node. Also the answer to CONNECT |
| `0x41` | SCAN | P -> W | `seconds u8` | Looks for Meshtastic nodes over Bluetooth, dropping the node link meanwhile. 0 or missing becomes 5, over 15 becomes 15. ERR busy if WiRSa is busy with the node |
| `0x54` | SCAN_R | W -> P | `n u8, n x (addr[6], addrType u8, rssi i8, name str)` | The nodes found (8 at most) |
| `0x42` | PAIR | P -> W | `addr[6], addrType u8, pin u32 (unused, send 0), name str` | Connects to a node and keeps it in a slot of its own (the one it had before if it is known, else a free one; ERR code 3 when four nodes are kept). The node becomes the one shown. Nodes paired before stay |
| `0x56` | PIN_NEED | W -> P | - | During PAIR, the node asks for its PIN |
| `0x43` | PIN | P -> W | `pin u32` (0 = cancelled) | The PIN the user typed. WiRSa waits 40 s for it |
| `0x55` | PAIR_R | W -> P | `ok u8, text str, slot u8, nodeId u32` | Whether pairing worked, and why not. `slot` is the node shown now; `nodeId` its node number, 0 until the node has said it (STATE_R carries it later) |
| `0x44` | UNPAIR | P -> W | `slot u8, keep u8` (none: the node shown, `keep` 0) | Removes the node's bond. `keep` 1: the slot stays as an archive (name, texts); 0: its texts and SD folder go too and the slot is free. Answered with DONE (count 0), or ERR "No such node" |
| `0x61` | SLOTS_GET | P -> W | - | Asks for the kept nodes; answered with SLOTS_R |
| `0x63` | SLOTS_R | W -> P | `n u8, n x (slot u8, state u8, nodeId u32, flags u8, name str)` | The kept nodes. `state` 1 paired, 2 archived. `flags` bit 0: the node shown, bit 1: the node Background mode works for |
| `0x62` | SLOT_SELECT | P -> W | `slot u8` | Shows another node: the texts of the one shown are saved, the store and the link become the new node's. Answered with DONE (0), or ERR "No such node" |
| `0x45` | CONNECT | P -> W | `keep u8` (optional) | `0` or none: drop the link and connect again. `1`: "sync now", keep the link if it is up and take what the node holds. `2`: the node sends its node list again and WiRSa waits for it (40 s at most, or until it has been quiet for 3 s). Answered with STATE_R |
| `0x46` | SYNC | P -> W | `cursor u32, nPending u8, localId u32 x nPending (24 at most), what u8` | Fetches what is new. `what`: bit 0 = the node list, bit 1 = the channels. Answered with the stream in 9.2 |
| `0x57` | NODE | W -> P | 9.3 | This node (node 0) in a SYNC stream, acked; also the answer to NODE_GET, not acked |
| `0x58` | NODES | W -> P | `n u8 (1-6), n x (num u32, long str, short str, lastHeard u32, flags u8)` | The other nodes in short form, favourites first, then by name; `long` is cut to 23 bytes, `short` to 5. Acked. `flags`: 1 favourite, 2 public key known |
| `0x59` | CHAN | W -> P | `index u8, role u8, name str` | One channel in use (unused slots are skipped). Acked |
| `0x5A` | MSG | W -> P | `seq u32, channel u8, flags u8, status u8, from u32, to u32, rxUtc u32, id u32, text str, err u8` | One message. `flags`: 1 sent by this node, 2 direct. `id` is the Palm's `localId` for a message it sent, the packet's own id for a received one (it tells a resend from a second identical text). `rxUtc` 0 = unknown. Acked |
| `0x5B` | DELIV | W -> P | `localId u32, status u8, err u8` | The status of a message the Palm listed as unfinished in its SYNC. Only inside a SYNC stream. Acked |
| `0x04` | ACK | P -> W | - | The Palm took a NODE, NODES, CHAN, MSG or DELIV frame |
| `0x05` | SYNC_DONE | W -> P | `lastSeq u32, messagesSent u16, nodes u16` | The stream is complete |
| `0x47` | SEND | P -> W | `to u32, channel u8, localId u32, text str` | Sends a text (233 bytes at most); `to` `0xFFFFFFFF` = broadcast. Answered with SEND_R; the delivery follows in a later SYNC (DELIV, MSG) |
| `0x5C` | SEND_R | W -> P | `localId u32, ok u8, packetId u32, text str, code u8` | Whether the text was handed to the node; on failure the reason and `code` (9.5) |
| `0x48` | REQ | P -> W | `to u32, channel u8, localId u32, kind u8, lat i32, lon i32, name str` | A request or share other than text. `kind`: 1 node info, 2 position request, 3 telemetry request, 4 traceroute, 5 share position, 6 waypoint; `lat`, `lon` are degrees x 1e7 (kinds 5 and 6), `name` is for a waypoint. With kind 1, `localId` 0 and a direct `to` it is the quiet key request: nothing is stored and nothing is answered. Otherwise answered like SEND |
| `0x49` | NODE_GET | P -> W | `num u32` | Asks for one node in full; answered with NODE, or ERR "Unknown node" |
| `0x4A` | RELAY_GET | P -> W | - | Asks for the relay setting |
| `0x5D` | RELAY_R | W -> P | `mode u8, every u8, link u8, nodeName str, waiting u16 (always 0), stored u16, lastPollUtc u32, battery i8, sdFlush u8` | The relay setting and status |
| `0x4B` | RELAY_SET | P -> W | `mode u8 (0 session, 1 background), every u8 (0-4 = 1, 5, 15, 30, 60 minutes), sdFlush u8 (optional; 0-4 = ring to the SD card every check, 3, 5, 10 checks, only when full)` | Sets how the mode runs, how often it checks and when the sleep checks' ring is written to the card; answered with DONE (0), or ERR for a bad value |
| `0x4C` | CLEAR | P -> W | `olderThanUtc u32` (0 = all) | Deletes stored messages; answered with DONE (how many went) |
| `0x5E` | DONE | W -> P | `count u16` (for HIST one byte: 1 = the SD copy was emptied) | UNPAIR, SLOT_SELECT, CFG_SET, CFG_ACT, RELAY_SET, CLEAR or HIST finished |
| `0x4D` | HIST | P -> W | `keep u8, deliveredSeq u32` | After each fetch: whether SD History is on, and the highest `seq` the Palm has saved, so the card's copy can be trimmed. Answered with DONE (1 byte). Sent only when no fetch is running |
| `0x5F` | NEWS | W -> P | `link u8, lastSeq u32` | Something is new (a message, a delivery, a link change, the node's battery); repeated every 3 s until the Palm syncs. The Palm never has to ask |
| `0x4E` | PING | either | - | "I am still here" (from the Palm) or "are you still there?" (from WiRSa, 5.4). Any frame answers it |
| `0x4F` | HOLD | P -> W | `seconds u8` | A dialog is open on the Palm: do not take it for gone for that long. Sent only when no fetch is running |
| `0x50` | CFG_GET | P -> W | - | Reads the node's settings afresh (up to 40 s; nothing else is served meanwhile); answered with CFG_R |
| `0x60` | CFG_R | W -> P | 9.4 | The node's settings, or why they are not available |
| `0x51` | CFG_SET | P -> W | 9.4 | Changes the node's settings; answered with DONE (0), or ERR |
| `0x52` | CFG_ACT | P -> W | `action u8, value u32` | `0` set the node's clock (value = UTC seconds), `1` reboot it, `2` make node `value` a favourite, `3` not. Answered with DONE (0), or ERR |
| `0x03` | BYE | either | - | Ends the session (5.3) |
| `0x06` | ERR | W -> P | `code u8, text str` | A request could not be done: 1 bad request, 2 no link, 3 no room, 4 busy |

A Palm frame whose type is not in this table is logged and ignored.

### 9.2 What a SYNC does

1. WiRSa sends, each acked and in this order: if `what` bit 0 is set, `NODE` (this node) and then `NODES` frames; if bit 1, one `CHAN` for each channel in use; then one `MSG` for each message
   with a `seq` above `cursor`, oldest first; then one `DELIV` for each `localId` the Palm listed that WiRSa knows. It ends with `SYNC_DONE` (not acked).
2. If the Palm stops acknowledging, the stream stops.
3. A frame the Palm sends in the middle of the stream (the user pressed Send) is kept and handled after it.

### 9.3 NODE body

`num u32, id str, long str, short str, hw u8, role u8, snr i8, hasSnr u8, lastHeard u32, hops i8 (-1 unknown), battery i8 (percent, 101 = powered, -1 unknown), uptime u32, chanUtil u16,
airUtil u16 (both x10 %), hasPos u8, lat i32, lon i32 (degrees x 1e7), isSelf u8, favourite u8, hasKey u8`.

### 9.4 CFG_R and CFG_SET

`CFG_R`: `ok u8`. If 0: `text str` (why). If 1:
`owner long str, owner short str, region u8, preset u8, hop u8 (1-7), txPower u8 (0-30), role u8, btMode u8, btPin u32, nChannels u8, n x (index u8, role u8, name str, key str (base64)), nodeClock u32,
mqttPresent u8 [, mqttFlags u8, address str, username str, password str, root str], nMqtt u8, nMqtt x (index u8, flags u8)`.
`region`, `preset` and `role` are positions in the Palm's lists, not Meshtastic's numbers (`REGION_CODE`, `PRESET_CODE`, `ROLE_CODE` in `mesh_internal.h` map them). `mqttFlags`: 1 enabled, 2 encrypted,
4 JSON, 8 TLS, 16 proxy, 32 map report. The last list gives each channel's MQTT flags: 1 uplink, 2 downlink.

`CFG_SET`: `mask u16`, then the parts the mask names, in this order; parts not named are left alone:

| Bit | Part |
|---|---|
| 1 | `long str, short str` (owner) |
| 2 | `region u8, preset u8, hop u8, txPower u8` |
| 4 | `role u8` |
| 8 | `btMode u8, btPin u32` |
| 16 | `n u8 (8 at most), n x (index u8, role u8, name str, key str [, flags u8 if bit 64 is set])` |
| 32 | `mqttFlags u8, address str, username str, password str, root str` |
| 64 | each channel in bit 16 carries its MQTT flags (1 uplink, 2 downlink) |

### 9.5 Status, flags and errors of a message

- `status`: 0 sending, 1 sent, 2 delivered, 3 failed, 4 relayed only (a direct message that only a relay confirmed), 5 unknown, 6 exchanging keys (`ST_*` in `mesh_core.h`).
- A direct message counts as delivered only when the routing ack comes from the recipient. A direct message that fails with error 6, 35 or 39 is given one key exchange (a node info request, a 20 s wait,
  one resend) and fails for good only if that fails too.
- `err` and `code` of a failed message: 1 to 39 are the node's own Routing.Error numbers (for example 5 max retransmit, 9 duty cycle, 35 and 39 no public key, 38 rate limit);
  240 WiRSa could not write to the node, 241 not linked to the node, 243 too long or no room. 242 ("the Palm is not connected to WiRSa") is made by the Palm. The texts the Palm shows for each are in its own document.
- `link` (STATE_R, RELAY_R, NEWS): 0 no node paired, 1 down, 2 ready, 3 failed, 4 archived (the node shown is kept for its texts only).
- **Several nodes (version 3).** Up to four nodes are kept, paired or archived. One is shown: the store, the link and every frame above are its. Each has its own folder on the SD card, `/Meshtastic/<node id as 8 hex digits>/`. Background mode works for one node; closing the Palm app returns the node shown to that one. The numbers `0x61`-`0x63` come after the WiRSa-sent frames of the block, so the highest Palm-sent type is `0x62`.

## 10. CalDAV / CardDAV Relay

WiRSa fetches calendars (CalDAV, iCloud, an ICS feed, Google) and address books (iCloud Contacts, CardDAV) and hands them to the Palm, one acknowledged item at a time. Mode letter `A`. Frame types are `CDDV_*` in
`caldav_internal.h` and in the Palm's `apps/caldav_sync/Serial.h`; they must match exactly.

| Type | Name | Dir | Body | What it does |
|---|---|---|---|---|
| `0x01` | HELLO | P -> W | `baud u32` | Opens the session (5.1) |
| `0x02` | HELLO_ACK | W -> P | `version u8, caps u8, server str, baud u32` | Names the protocol and gives the agreed rate |
| `0x03` | BYE | either | - | The Palm: its app closed. WiRSa: sent after SYNC_DONE |
| `0x04` | ACK | P -> W | - | The Palm stored the item |
| `0x05` | SYNC_DONE | W -> P | - | Everything was sent |
| `0x06` | ERR | W -> P | `code u8, message str` | The fetch failed. `code`: 1 bad request, 2 failed; `message` is for the user. A server type left out of the build gets "... sync is not enabled in this WiRSa build" |
| `0x20` | SYNC_REQUEST | P -> W | `serverType u8, server str, path str, uuid str, user str, secret str, pastWindow u8, futureWindow u8, utcNow u32, googleClientId str, googleClientSecret str` | Fetch one calendar or address book. `serverType`: 0 CalDAV, 1 iCloud, 2 Google, 3 ICS feed, 4 iCloud Contacts, 5 CardDAV. `pastWindow` 0-5 = none, 1 week, 2 weeks, 1 month, 2 months, 3 months (0, 7, 14, 30, 60, 90 days); `futureWindow` 0-4 = 2 weeks, 1 month, 3 months, 6 months, 1 year (14, 30, 90, 180, 365 days); a value out of range becomes 0. `utcNow` is the Palm's UTC in **Unix** seconds (a value before 2020 makes WiRSa use its own clock). A short body is answered with ERR "Malformed SYNC_REQUEST", an unknown type with "Unknown server type" |
| `0x21` | OAUTH_START | P -> W | - | Starts the one-time Google sign-in (a listener on WiRSa opens for 300 s) |
| `0x22` | EVENT_ITEM | W -> P | `uid str, title str, location str, utcStart u32, utcEnd u32, allDay u8, description str, isRepeating u8` | One calendar event. Times are UTC in **Palm** seconds (since 1904-01-01; WiRSa adds 2082844800 to Unix time). For `allDay` = 1 they are the event's start and end **dates** at 00:00 with no time zone, as the source gives them (an iCalendar `VALUE=DATE`, Google's `date`), and the Palm keeps them as dates; `utcStart` is then a misnomer. Acked |
| `0x23` | CONTACT_ITEM | W -> P | `uid str, displayName str, sortName str, nDates u8 (8 at most), nDates x (year u16, month u8, day u8, kind u8, name str), nCategories u8 (16 at most), nCategories x str, text str` | One contact. `year` 0 = unknown; date `kind` 0 birthday, 1 engagement, 2 remembrance, 3 wedding, 4 work, 5 other (then `name` is the date's own name; Address35's anniversary labels are `kind` - 1). `text` is the card trimmed for the Palm, cut at a line break if it would not fit the frame. Acked |
| `0x24` | GROUP_ITEM | W -> P | `uid str, name str, n u8, n x memberUid str` | One contact group; a big group comes as several frames with the same `uid`. Acked |
| `0x25` | OAUTH_INFO | W -> P | `ip str, port u16, pin str` | Where, and with which PIN, the user's computer hands the sign-in over |
| `0x26` | OAUTH_TOKEN | W -> P | `refreshToken str` | The Google refresh token, for the Palm to keep |

Order of a sync: `SYNC_REQUEST`, then the items (each acked), then `SYNC_DONE` or `ERR`. The sync window comes from the Palm's clock (`utcNow`), so WiRSa does not depend on its own NTP. A Sync All sends one `SYNC_REQUEST` per slot on the same session.

**Google sign-in** (`OAUTH_START`, `OAUTH_INFO`, `OAUTH_TOKEN`) is a separate exchange, run at 9600 baud with no `HELLO` (a handful of short strings, not a sync). The Palm sends `OAUTH_START`; WiRSa opens its listener and answers
`OAUTH_INFO` (its IP, a port, a short-lived PIN); it sends no error for this exchange, and a build without Google sign-in does not handle the frame at all (it is logged as unknown and the Palm waits in vain, so the Palm treats silence as failure); the user runs the helper script on their computer, which posts the token to that listener; WiRSa sends `OAUTH_TOKEN` and closes the listener.
The listener also closes after 300 s. The Palm sends `BYE` when it is done, whether or not it got a token (`OAuthRelay.c`).
Items may be produced ahead of the Palm's acks (the download-ahead queue, `relay_queue.h`); on the wire it is still one frame, one ack.

## 11. BitWarden Relay

WiRSa signs in to a Bitwarden server, downloads the vault, decrypts each item and hands it to the Palm. The master password crosses the line only inside the secured link (section 4). Mode letter `B`.
Frame types are `BW_*` in `bitwarden_internal.h`; the Palm's are in `apps/bitwarden`. The numbers follow section 6 (protocol version 2).

| Type | Name | Dir | Body | What it does |
|---|---|---|---|---|
| `0x01` | HELLO | P -> W | `baud u32` | Opens the session (5.1) |
| `0x02` | HELLO_ACK | W -> P | `version u8, caps u8, server str, baud u32` | Names the protocol and gives the agreed rate |
| `0x03` | BYE | P -> W | - | The Palm's app closed (also cancels a pending two-factor wait). WiRSa never sends one |
| `0x04` | ACK | P -> W | - | The Palm stored the item |
| `0x05` | SYNC_DONE | W -> P | `items u16, folders u16, orgs u16, rememberToken str` | Everything was sent. A non-empty `rememberToken` replaces the Palm's stored one |
| `0x06` | ERR | W -> P | `code u8, message str, rememberToken str` | The sync failed. `code`: 1 DNS, 2 connect, 3 TLS, 4 fingerprint mismatch, 5 sign-in failed, 6 two-factor required, 7 sync fetch failed, 8 decrypt failed, 9 malformed KDF settings |
| `0x30` | TRUST | P -> W | `serverUrl str` | Reads the server's TLS certificate (without validating it) so the user can accept it |
| `0x31` | SYNC | P -> W | `serverUrl str, pinnedFingerprint[32], email str, masterPassword str, kdfType u8, kdfIterations u32, kdfMemoryKiB u32, kdfParallelism u8, rememberedTwoFactorToken str` | Syncs the vault. The Palm always sends `kdfType` 0 (ask the server for the account's key-derivation settings) and zeros for the three fields after it. WiRSa supports PBKDF2-SHA256 only: an account on Argon2id, or a non-zero `kdfType`, gets ERR code 9. `rememberedTwoFactorToken`: see the notes below |
| `0x32` | TWO_FACTOR_SUBMIT | P -> W | `code str` | The code the user typed, verbatim. WiRSa waits 180 s; a `BYE` here cancels the sync quietly. Any other frame, or no frame, ends it with ERR code 5. A wrong code brings another TWO_FACTOR_REQUIRED, up to 3 times |
| `0x33` | TRUST_RESULT | W -> P | `status u8, sha256[32], subjectCN str, issuerCN str` | `status` 0 ok, 2 connect failed, 3 TLS handshake failed; the fingerprint is zeros unless 0. The two names are always empty in this build |
| `0x34` | SYNC_ITEM | W -> P | `guid str, itemType u8, folderIndex u16, orgIndex u16, archived u8, name str, notes str` + the part for the type | One vault item, decrypted. `itemType`: 1 login, 2 secure note, 3 card, 4 identity. Index `0xFFFF` = none. Acked. The part: login `username, password, uri (the first only), totp` (4 str); secure note nothing; card `cardholderName, brand, number, expMonth, expYear, code` (6 str); identity `title, firstName, middleName, lastName, username, company, ssn, passportNumber, licenseNumber, email, phone, address1, address2, address3, city, state, postalCode, country` (18 str) |
| `0x35` | FOLDER | W -> P | `index u16, name str` | One folder. Acked |
| `0x36` | ORG | W -> P | `index u16, name str` | One organization. Acked |
| `0x37` | TWO_FACTOR_REQUIRED | W -> P | `providerType u8, label str` | The account needs a code. WiRSa picks one provider (0 authenticator app, else 1 email, else the first the server lists); the number is Bitwarden's own |
| `0x38` | SYNC_STATUS | W -> P | `text str` | A line of progress text for the Palm to show |

Order of a sync: `SYNC`; `SYNC_STATUS` lines; if the account needs it, `TWO_FACTOR_REQUIRED` and `TWO_FACTOR_SUBMIT`; `FOLDER` and `ORG` frames and `SYNC_ITEM` frames, all acked; `SYNC_DONE`, or
`ERR` at any point. Notes over 8192 encrypted bytes are sent empty.

Notes (`bitwarden_sync.cpp`):

- **Remember this device.** If `rememberedTwoFactorToken` is not empty, the first sign-in uses it (as Bitwarden's "remember" provider, number 5), so an account that has completed two-factor from this Palm needs no prompt.
  `TWO_FACTOR_REQUIRED` is sent only if that is rejected or there was no token. Every sign-in asks the server to remember the device, and a token that comes back is reported to the Palm in `rememberToken`
  of `SYNC_DONE`, or of a `ERR` that happens **after** a successful sign-in (codes 7 and 8: the server renews the token at every sign-in, so the old one is already dead and this is the only copy of the new
  one). Errors before or at sign-in carry an empty `rememberToken`. Empty always means "no change".
- **Fetch retries.** If the download stalls or the JSON is malformed, WiRSa reconnects and fetches again, up to 3 tries in all, and carries on after what it has already sent and had acknowledged: the Palm
  never gets a record twice.
- **Items.** `guid` is the real Bitwarden item id. `folderIndex` and `orgIndex` are numbers WiRSa gives to the `FOLDER` and `ORG` frames of this sync (0, 1, 2 ...), not Bitwarden's ids; a folder or
  organization frame always comes before an item that uses it. Organization names are relayed as the server returns them. An item in the trash is not sent; an archived one is, with `archived` = 1. `itemType` is derived from
  which of login, card or identity the item actually holds, so the part that follows always has the shape `itemType` says. The master password exists in WiRSa's RAM only for the one request and is never logged.
- **Trust.** The Palm stores the fingerprint the user accepted (from `TRUST_RESULT`) and sends it in every `SYNC`; WiRSa accepts the server only if its certificate has exactly that SHA-256 fingerprint (not a name match), else ERR code 4.

## 12. Palm control (WiRSaCtl)

The commands of WiRSaCtl and of the apps that use its link (`apps/lib/wirsactl.c`). Every frame carries the lead byte `0x16` (section 7). A request is `0x40`-`0x4F`; the reply has the request's type + `0x20`; `ERR` (`0x5F`) can
answer any request. `ERR` body: `code u8, text str`; codes 1 no WiFi, 2 no SD card, 3 mode not built, 4 not a mode, 5 busy, 6 bad request, 7 failed.

Names: the firmware calls the four longer ones `CTL_SECRETS`, `CTL_ENTER`, `CTL_EXIT` and `CTL_FACTORY`, the Palm's `wirsactl.h` `wctlTypeSecrets`, `wctlTypeEnter`, `wctlTypeExit`, `wctlTypeFactory`; the numbers are the same.

| Type | Name | Request body | Reply body | What it does |
|---|---|---|---|---|
| `0x40` | PING | `nonce u32` | `nonce u32, proto u8 (1), firmware str, flags u16, mode u8, menuBaud u32` | Is WiRSa there; which mode (`'M'` for the main menu, else the mode's letter); where the menu listens. `flags`: 0x0001 screenless, 0x0002 FTP, 0x0004 HotSync, 0x0008 SASD, 0x0010 SLIP, 0x0020 BitWarden, 0x0040 CalDAV, 0x0080 SPEED supported, 0x0100 Mesh (the modes that are built) |
| `0x41` | STATUS | - | `mode u8, wifi u8, ssid str, rssi i8, ip str, sd u8, ftp u8, ftpClients u8 (always 0), uptime u32 (s), batteryPresent u8, batteryMv u16, batteryPercent u8, sdText str, sasdDisk u8` | `wifi`: 0 not set up, 1 connecting, 2 connected, 3 gave up. `sd`: 0 unknown, 1 present, 2 none, 3 the virtual card is the SASD disk. The fields after `uptime` were appended later |
| `0x42` | WIFI_SCAN | - | `n u8 (16 at most), n x (ssid str, rssi i8, open u8)` | Lists the WiFi networks; takes a few seconds |
| `0x43` | WIFI_SET | **encrypted** `ssid str, password str` | **encrypted** `ok u8` | Tests the network and saves it if it connects (SSID 1-32 bytes, password up to 63) |
| `0x44` | CFG_GET | - | see below | Reads WiRSa's settings |
| `0x45` | CFG_SET | see below | `ok u8` | Writes them in one frame; a changed baud applies after the reply |
| `0x46` | SECRETS_SET | **encrypted** `ftpPassword str, apPassword str` | **encrypted** `ok u8` | Sets the FTP and access-point passwords (empty = unchanged) |
| `0x47` | FTP | `start u8` | `ok u8` | Starts or stops the FTP server |
| `0x48` | ENTER_MODE | `letter u8` (a flags byte after it is ignored) | `ok u8` | Starts a mode by its letter: `H` HotSync, `D` SASD, `S` SLIP, `B` BitWarden, `A` CalDAV, `T` Mesh. ERR for no WiFi, no SD card, not built, not a mode. The mode then leaves on the Exit Timers |
| `0x49` | EXIT_MODE | `"EXIT"` | `ok u8` | Leaves the running mode (7). At the main menu it only answers |
| `0x4A` | REBOOT | - | `ok u8` | Replies, then restarts |
| `0x4B` | FACTORY_RESET | `"RESET"` (5 bytes) | `ok u8` | Replies, resets all settings, restarts |
| `0x4C` | SPEED | `baud u32` | `ok u8, baud u32` | Sets the rate the menu uses: the fastest of 9600, 14400, 19200, 28800, 38400, 57600, 115200 not above the request. The reply goes out at the old rate |
| `0x4D` | LOG | `op u8 (0 read, 1 clear), offset u32` | `total u32, offset u32, n u16, n bytes` | A page (480 bytes at most) of WiRSa's session log, oldest first. Main menu only |
| `0x4E` | SYSINFO | `op u8 (unused), offset u32` | `total u32, offset u32, n u16, n bytes` | WiRSa's system page as text, same page shape as LOG. Main menu only |
| `0x5F` | ERR | - | `code u8, text str` | A request could not be done |

Notes (`palm_ctl.cpp`):

- **ENTER_MODE preconditions**, checked before the mode starts: `S`, `B` and `A` need WiFi connected (ERR 1); `H` and `D` need a disk, judged from the result of the last STATUS probe only (a fresh probe before the reply lost the reply on hardware):
  known to be missing gives ERR 2, never probed gives no refusal and the main menu's own check still applies. The disk follows the `sasdDisk` setting: the SD card, the internal virtual card, or auto (the SD card if one answers, else the virtual card);
  with the virtual card as the disk they are not refused for a missing SD card. `T` needs neither. A letter that is not built gives ERR 3; `F`, `U`, `C` give ERR 4. WiRSa sends `ok` at the menu rate, flushes, then calls the same function
  the main menu calls.
- **STATUS and the SD card.** At the main menu STATUS reads the card's first sector before it answers, because the card type found at mount is kept even after the card is pulled; a pulled card shows as none (2) on the next STATUS. Inside a mode the card is not touched.
  After a STATUS answered from the menus WiRSa mounts the card if nothing has, so the next STATUS can say whether there is one.
- **Passwords** are never returned: `CFG_GET` has only the two `passSet` bits.

`CFG_GET` reply: `baud u8, logLevel u8, orientation u8, ftpUser str, apSsid str, slipIp[4], clientIp[4], subnet[4], dns[4], passSet u8`, then, appended later (an app stops reading where its shape ends):
`unusedExitSec u8, afterExitSec u8, slipTimeoutMin u8, sasdDisk u8, deepSleepMin u8, sasdTimeoutMin u8`. `baud` is the Palm link's rate number: 4 = 9600, 9 = 14400, 5 = 19200, 10 = 28800, 6 = 38400, 7 = 57600, 8 = 115200
(a request with 0-3 becomes 9600). `logLevel`: 0 off, 1 info, 2 warn, 3 verbose. `orientation`: 0 normal, 1 flipped. `passSet`: bit 0 FTP password set, bit 1 AP password set.

`CFG_SET` request: `baud u8, logLevel u8, orientation u8, ftpUser str, apSsid str`, 16 IP bytes (the same four addresses; `passSet` is not sent), then the same appended tail, each field present only if the body goes on.
Valid values: `slipTimeoutMin` 1, 2, 3 or 5; `sasdDisk` 0 auto, 1 SD card, 2 virtual card; `deepSleepMin` 0, 1, 3, 5 or 15; `sasdTimeoutMin` 3, 5, 10 or 15 (an invalid value is ignored). A body shorter than the fixed part gets ERR bad request.

## 13. Writing a client, and writing a relay

### 13.1 A client

1. Open the line at 9600. 2. Run the key exchange (4). 3. Send `HELLO` with the rate you want, read `HELLO_ACK`, switch to the rate in it (5.1). 4. Speak the protocol (9-11); acknowledge what needs it (5.2).
5. Send `BYE` and close. For WiRSaCtl, send `0x16`-framed commands (7, 12) instead.

The Palm apps share `apps/lib/relayproto.c` (frames), `relaywire.c` (fields) and `relaysec.c` (the key exchange); a client on another platform needs those three things and this document.
`Firmware/test_host/` holds host tests for WiRSa's own end of the formats.

### 13.2 A relay: the rules

"Today" says where the existing relays do not yet follow a rule; the migration in 6.3 closes those gaps.

| # | Topic | Rule | Today |
|---|---|---|---|
| 1 | **Frame types** | Use the common frames of 6.1 for their roles. Put your own frames in a free block of the window (6.2), the Palm-sent ones first. Set the mode's Palm-sent bound to the highest of them. | None: all three use the common frames. |
| 2 | **Request shapes** | The Palm starts every exchange. Two patterns only. **R**: one request, one reply (or `ERR`). **S**: one request, then items each acknowledged (5.2), then `SYNC_DONE` or `ERR`. WiRSa sends a frame on its own only in a session that stays open for hours, and the protocol lists each such frame (Mesh: `NEWS`, `PING`). | CalDAV and BitWarden are S. Mesh is R and S, and has the unsolicited frames. WiRSaCtl is R. |
| 3 | **Errors** | `ERR` is `code u8, text str`, then any appended fields. Code `0` is never used. The code values are the relay's own and are listed in its section. `text` is for the user and may be shown as it is. | All four have this shape (BitWarden appends `rememberToken`). |
| 4 | **Acks, limits, timeouts** | The ack rule of 5.2 is fixed for every relay. Pick one profile (below); a different value needs its reason written in the relay's section. | Mesh follows *interactive*, CalDAV and BitWarden follow *bulk*. |
| 5 | **Time and text** | Text is a `str` in Palm Latin (section 3); characters the Palm cannot show are dropped, unless the relay names an exception. Time is Unix seconds UTC in a `u32`, `0` = unknown. | Mesh and CalDAV's `utcNow` follow it. Exceptions: CalDAV event times are Palm seconds (section 10); Mesh names turn an emoji into `0x10`. |
| 6 | **Compatibility** | New fields are appended, never inserted (section 3). `version` in `HELLO_ACK` is raised by any change an older peer cannot read; a Palm that needs a higher `version` than WiRSa reports stops and tells the user to update WiRSa. `caps` bits are reserved. | All three report `version` 2 and their Palm apps refuse less. |
| 7 | **Checklist** | See 13.3. | `check_wire.py` checks names and numbers only. |

**Profiles** (values from section 8, `relay_timing.h`):

| | Interactive | Bulk |
|---|---|---|
| Used by | Mesh | CalDAV, BitWarden |
| Receive body limit | 960 | 3192 (CalDAV), 4096 (BitWarden) |
| Byte timeout | 0.5 s | 3 s |
| Wait for a frame's first byte | 50 ms poll (the loop also serves other work) | 60 s |
| Ack wait x tries | 3 s x 5 | 10 s x 3 (CalDAV), 10 s x 5 (BitWarden) |
| Between tries | 0.5 s | 0.5 s |

Choose *interactive* when WiRSa must keep serving something else while it waits (Mesh's Bluetooth link); choose *bulk* when the Palm's own slow operations (database writes) set the pace. The Palm's receive buffer sets the body limit and
the two ends must agree on it.

### 13.3 A relay: the checklist

1. Choose R or S for each exchange and list the frames in a table in the format of sections 9 to 12 (type, name, direction, body, what it does).
2. Take your block from 6.1 and 6.2, Palm-sent frames first; add the block to 6.3.
3. Firmware: an enum of the frame types in `<relay>_internal.h`, a `RelayMode::Spec` (the common frame types, `maxPalmType`, the HELLO reply) and a frame table; acknowledged frames go through `RelayProto::writeFrameAcked`.
4. Palm: the same constants, using `relayproto.c`, `relaywire.c` and `relaysec.c`; the app checks `version` (rule 6).
5. Add the protocol to `check_wire.py` and run it; it must pass for both sides.
6. Build all combinations of the feature flags (a relay is behind its own flag), then test on the hardware.

## 14. Where each fact comes from

| Section | Read from |
|---|---|
| 2, 3, 4 | `relay_proto.h/.cpp`; Palm `apps/lib/relayproto.c`, `relaysec.c`, `relaywire.c` |
| 5 | `relay_mode.h/.cpp` (`handleHello`, `tick`), `relay_timing.h`, `mode_timer.h`, `settings.h`, `palm_ctl.cpp palmCtlModeHook` |
| 6 | `relay_proto.h` (`isKex`), Palm `relaysec.h` (`RP_IS_KEX`), `palm_ctl.cpp palmCtlModeHook`; the allocation itself is the design (6.1 to 6.3); all three relays follow it in the code |
| 8, 13.2 | `relay_timing.h` |
| 9 | `mesh_palm.cpp` (every handler and `kFrames`), `mesh_admin.cpp cfgSet/cfgAct`, `mesh_worker.cpp`, `mesh_core.h`, `mesh_pb.h`, `mesh_internal.h` |
| 10 | `caldav_sync.cpp`, `caldav_relay.cpp`, `caldav_google.cpp`; Palm `apps/caldav_sync/SyncEngine.c`, `Serial.h` |
| 11 | `bitwarden_sync.cpp`, `bitwarden_vault.cpp`, `bitwarden_login.cpp`, `bitwarden_queue.cpp` |
| 12 | `palm_ctl.cpp`, `network.cpp wifiStateCode`, `modes.h`, `settings.cpp` |

`Firmware/tools/check_wire.py [palm apps dir]` compares the type numbers and names in the frame tables of sections 9 to 12 with the firmware headers and the Palm sources and fails on any difference. Run it whenever a frame changes.
