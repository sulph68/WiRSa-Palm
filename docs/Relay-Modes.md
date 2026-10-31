# Relay modes - how one is built, and how to add another

The wire they speak is specified in [protocol/Protocol-wire.md](protocol/Protocol-wire.md); this document describes the firmware skeleton they share.

BitWarden Relay, CalDAV Relay and Mesh Relay are "relay modes": a Palm app talks to WiRSa over the serial line with framed, encrypted
messages (`relay_proto.h`) and WiRSa does the network side. The parts every one of them needs live in two shared places; a mode
adds only its own frames and work.

## What is shared

- **`relay_mode.h` / `relay_mode.cpp`** - entering, leaving and the checks at the top of each pass of the loop. A mode describes itself
  once in a `RelayMode::Spec`:

  | field | meaning |
  |---|---|
  | `name`, `tag` | banner text ("CalDAV Relay") and log prefix ("CalDAV") |
  | `modeId`, `letter` | the `MODE_xxx` id and WiRSaCtl's letter for the mode |
  | `idleBaud`, `maxPalmType` | the rate every session starts at, and the highest frame type the Palm sends (WiRSaCtl's frames are told apart by it) |
  | `needsWifi`, `wifiReady` | wait for WiFi at entry, and hold the loop until it is up |
  | `timer` | the shared Exit Timers (`mode_timer.h`); every mode passes its own `ModeExitTimer`; `nullptr` is allowed for a mode with its own timing (none now; Mesh adds the silence probe on top of the shared timers) |
  | `negotiateTimeoutMs`, `negotiatedAt`, `revertToIdle` | give up a negotiated rate the Palm never uses |
  | `drawHeader`, `drawStatus` | the title bar and the status line |
  | `onEnter` | state at entry: counters at zero, anything the mode must set up |
  | `beforeLeave`, `onLeave` | cleanup on the way out: the first thing (the mode still owns the port), and after binary mode is off |
  | `onFrame` | any frame arrived: the mode notes the Palm is there |
  | `drainAtEntry` | throw away stale bytes already on the port |
  | `probeType`, `sessionActive`, `lastFrameAt`, `busy` | the silence probe (below); leave `probeType` 0 and the rest empty and the mode ends on the Exit Timers alone |
  | `helloAckType`, `helloVersion`, `helloServer` | the HELLO reply: its frame type, protocol version and server name (`WiRSa-DAV/2`, `WiRSa-BW/2`, `WiRSa-Mesh/3`) |

  `enter`, `leave`, `leaveToMenu`, `tick`, `readFrame`, `handleHello`, `dispatch` and `agreedBaud` do the rest. `tick` runs the BACK button, the `+++` escape on USB,
  WiRSaCtl's EXIT, the Exit Timers, the WiFi wait and the negotiated-rate watchdog, and says whether the mode was left (`Left`), has nothing to do (`Wait`)
  or should carry on (`Go`). `readFrame` returns the next frame of the mode's own protocol and filters out WiRSaCtl's frames and the serial-link
  key exchange. `handleHello` answers the Palm's HELLO the same way in every relay: it agrees the speed (never above the configured one), sends
  `version u8, capabilities u8, name str, rate u32`, and moves the port to the agreed rate.
- **The silence probe** (`Spec.probeType`), for a mode whose session stays open for hours (Mesh): before a session begins, the "unused" Exit Timer;
  during it, after the *After session* time without a frame from the Palm the skeleton sends the probe frame (a PING) and leaves only if nothing
  arrives within the same time again. Any frame clears it. `busy()` (a node operation, a dialog open on the Palm) counts as activity. BitWarden
  and CalDAV leave it off: their sessions are short and end with a BYE or the negotiated-speed watchdog, then the Exit Timers.
- **A frame table** (`RelayMode::Frame { type, name, fn }` and `RelayMode::dispatch`): each mode lists the frame types it understands and the
  handler of each (`void fn(const uint8_t *body, size_t len)`); `dispatch` logs `<tag>: frame NAME, N byte(s)` at verbose level and warns about a type
  that is not in the table. The tables are `kFrames` (Mesh), `kBwFrames` and `kCddvFrames`; a new frame is one line in the table and one handler.
- **`palm_text.h`** - UTF-8 to Palm Latin and back, with the emoji options. Every relay that sends names, notes or messages to the Palm uses it
  (never on secrets: passwords, keys, URIs).
- **`relay_proto.h`** - the framing, the encryption, `writeFrameAcked`.

## What a mode writes

```cpp
static ModeExitTimer timer;
static RelayMode::Spec mode = {
  "Foo Relay", "Foo", MODE_FOO, 'F', 9600, FOO_MAX_PALM_TYPE, /*needsWifi*/ true, &timer, 15000,
  &wifiReady, &negotiatedAt,
  drawHeader, drawStatus, revertToIdle, onEnter, /*beforeLeave*/ nullptr, onLeave, /*onFrame*/ nullptr, /*drainAtEntry*/ true,
  FOO_HELLO_ACK, /*helloVersion*/ 1, "WiRSa-Foo/1",
  /* probeType and the rest left out: no silence probe */
};

static const RelayMode::Frame kFrames[] = {
  { FOO_HELLO, "HELLO", onHello },       // onHello calls RelayMode::handleHello(mode, body, len)
  { FOO_SYNC,  "SYNC",  onSync },
  { FOO_BYE,   "BYE",   onBye },
};

void enterFooRelayMode() { RelayMode::enter(mode); }
void exitFooRelayMode()  { RelayMode::leave(mode); }

void fooRelayLoop() {
  if (RelayMode::tick(mode) != RelayMode::Tick::Go) return;
  uint8_t type; size_t len;
  if (!RelayMode::readFrame(mode, type, body, sizeof body, len, 60000 /* idle ms */, 3000 /* byte ms */)) return;
  RelayMode::dispatch(mode, kFrames, sizeof kFrames / sizeof kFrames[0], type, body, len);   // HELLO: RelayMode::handleHello(mode, body, len)
}
```

`caldav_relay.cpp` is the plainest example (WiFi, drain at entry, cleanup on exit; the rest of CalDAV is in `caldav_*.cpp`, divided as `include/caldav_internal.h` lists), `bitwarden_relay.cpp` the plainest without cleanup (the rest of BitWarden is in `bitwarden_*.cpp`, divided as `include/bitwarden_internal.h` lists), and
`mesh_relay.cpp` the one that bends the skeleton (no WiFi, its own Exit Timer rules, a BYE and a flush on the way out, Background mode
carrying on after the mode closes).

## Where a new mode is wired in

1. `globals.h`: `MODE_FOO` (the value of `menuMode` while it runs) and the `WIRSA_FEATURE_FOO` switch.
2. `modes.cpp`: **one row** in `kModes` (and one `IF_FOO(x)` macro, which turns the mode's functions into `nullptr` when the feature is off). The
   row holds its letter, main-menu label, WiRSaCtl flag, whether WiRSaCtl may start it, whether it needs WiFi or an SD card first, whether it is a
   data transfer, its loop and its enter function. The main menu's entries, the menu key handling, the main loop's dispatch on `menuMode`,
   WiRSaCtl's mode letter, built flags and ENTER checks, and the background WiFi reconnect all read this table (`modes.h`).
3. The WiRSaCtl flag bit, if it is a new mode WiRSaCtl should know about: `Modes::FLAG_FOO` in `modes.h`, and the Palm side (WiRSaCtl's mode list).
4. The Palm side: its app, its notes under `apps/`, and the line in the internal reference (`WiRSa-Palm.md`).

Timeouts, delays and speeds of the relay modes are in `relay_timing.h` (`RelayTiming::`), each with what it is for: a value two modes share has
one name, a value that is one mode's own has its prefix. Add a new mode's there, not as a local constant.

## Testing a mode

Nothing in the skeleton can run on the host (it uses the UART and the display), so a change to it is checked on the board, in each mode:
enter from the main menu and from WiRSaCtl; a real session; BACK where the board has the button, and `+++` over USB (`printf 'A' > /dev/ttyACM0`, then
three `+`, about 0.2 s apart); WiRSaCtl's EXIT; the "unused" and "after session" Exit Timers; another mode afterwards, to see the port's rate was put back.
Text conversion and the frame builders that do not touch hardware have host tests in `test_host/`.
