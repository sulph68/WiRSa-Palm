// relay_mode.h - the part every relay mode (BitWarden, CalDAV, Mesh) does the same way: entering, leaving, and the checks at the
// top of each pass of the loop. A mode describes itself once in a RelayMode::Spec and keeps only what is its own: the frames it
// understands and the work it does for them.
//
//   enter:  menuMode, binary mode, banner, wait for WiFi (if the mode needs it), idle baud, key pair, exit timer
//   tick:   BACK button, the "+++" escape on USB, WiRSaCtl's EXIT, the Exit Timers, WiFi coming up, the negotiated-baud watchdog
//   frame:  the next Palm frame, with the serial-link key exchange taken out (readFrame)
//   leave:  the mode's cleanup hook, key pair forgotten, menu baud back, banner
//
// How a mode uses it (see caldav_relay.cpp):
//   static RelayMode::Spec kSpec = { "CalDAV Relay", "CalDAV", MODE_CALDAV_RELAY, 'A', 9600, CDDV_MAX_PALM_TYPE, true, &timer, ... };
//   void enterXxxMode() { RelayMode::enter(kSpec); }
//   void xxxLoop()      { if (RelayMode::tick(kSpec) != RelayMode::Tick::Go) return; ... RelayMode::readFrame(...) ... }
#pragma once
#include <Arduino.h>
#include <stdint.h>
#include "mode_timer.h"
#include "hello_confirm.h"

namespace RelayMode {

struct Spec {
  const char *name;            // for the banners: "CalDAV Relay"
  const char *tag;             // for the log: "CalDAV"
  int modeId;                  // MODE_xxx
  char letter;                 // WiRSaCtl's letter for the mode
  uint32_t idleBaud;           // the rate every session starts at (HELLO negotiates up from it)
  uint8_t maxPalmType;          // the highest frame type the Palm sends (palmCtlModeHook tells WiRSaCtl's frames from the mode's)
  bool needsWifi;              // wait for WiFi at entry, and hold the loop until it is up
  ModeExitTimer *timer;        // the Exit Timers; nullptr = the mode keeps its own timing (no mode does now)
  uint32_t negotiateTimeoutMs; // a negotiated rate the Palm never uses is given up after this long (0 = no watchdog)

  // The mode's own state, which the skeleton reads and writes:
  bool *wifiReady;             // needsWifi: set when WiFi is there
  unsigned long *negotiatedAt; // millis() of the last HELLO or activity at the negotiated rate; 0 = at the idle rate

  // The mode's own work, called by the skeleton (any may be nullptr except drawHeader and drawStatus):
  void (*drawHeader)();                   // the title bar (live baud readout)
  void (*drawStatus)(const char *text);   // the status line ("Preparing...", "Waiting...")
  void (*revertToIdle)();                 // back to idleBaud, header redrawn (the mode's own cleanup first if it has some); needed with negotiateTimeoutMs
  void (*onEnter)();                      // a new session, counters at zero: after WiFi is up, before the idle baud and the banner
  void (*beforeLeave)();                  // the very first thing on the way out (the mode still owns the port: a BYE, a flush, stopping a task)
  void (*onLeave)();                      // cleanup that belongs to the mode, after binary mode is off and before the key pair is forgotten
  void (*onFrame)();                      // a frame of any kind has arrived (readFrame): the mode notes the Palm is there
  bool drainAtEntry;                      // throw away bytes already sitting on the serial port

  // The HELLO that negotiates the speed (handleHello):
  uint8_t helloAckType;                   // the frame type of the reply
  uint8_t helloVersion;                   // reply body: protocol version, ...
  const char *helloServer;                // ... capability byte and server name; nullptr = the short reply (the agreed rate only)

  // The silence probe, for a mode whose session stays open for hours (Mesh). Leave probeType 0 and the rest empty for the others, which end
  // on the Exit Timers alone. With a probe: before a session begins (*sessionActive false) the "unused" Exit Timer rules; during it, after
  // the After-session time without a frame from the Palm the skeleton sends probeType and leaves only if nothing arrives within the same
  // time again. A frame (any), the Palm's PING answers included, clears the probe.
  uint8_t probeType;                      // the probe's frame type (a PING); 0 = no probe
  bool *sessionActive;                    // the mode's "a Palm session is running" flag; handleHello sets it
  unsigned long *lastFrameAt;             // millis() of the last frame from the Palm (set by the mode's onFrame, or by it when it has long jobs)
  bool (*busy)();                         // work under way that is not silence (a node operation, a dialog open on the Palm): never probed
  bool probed;                            // the skeleton's own state: a probe is out
  unsigned long probeAt;                  // ... and when it went

  // The confirmed switch (handleHello): a Palm that offers it (HELLO_OFFER) gets the reply at the idle rate, and the port moves only when its
  // HELLO_CONFIRM arrives. Off (false) for a mode that does not use it: the HELLO is answered and the port moves at once, as before. Set at entry.
  bool helloConfirm;
};


// Enters the mode. False when it was cancelled (WiFi not available: the user is back at the main menu).
bool enter(Spec &m);

// Leaves the mode: beforeLeave, onLeave, key pair forgotten, the menu's baud back. Does not go to the main menu.
void leave(Spec &m);

// leave(), then the main menu: what BACK, "+++" and the Exit Timers do.
void leaveToMenu(Spec &m);

enum class Tick : uint8_t {
  Left,   // the mode was left (BACK, "+++", WiRSaCtl EXIT or an Exit Timer): the loop returns at once
  Wait,   // nothing to do this pass (WiFi not up yet)
  Go,     // carry on with the frames
};

// The checks at the top of every pass of the loop (see the list above).
Tick tick(Spec &m);

// The next frame of the mode: false when there is none, when it was a WiRSaCtl frame (left for the next pass) or when it was the
// serial-link key exchange. True with type and body filled in; the Exit Timer (if the mode has one) is told a frame of the mode's own
// protocol arrived, and onFrame is called for every frame, the key exchange included.
bool readFrame(Spec &m, uint8_t &type, uint8_t *body, size_t cap, size_t &len, uint32_t idleMs, uint32_t byteMs);

// One frame type of a mode and what handles it. A mode keeps a table of these instead of a switch.
using Handler = void (*)(const uint8_t *body, size_t len);
struct Frame {
  uint8_t type;
  const char *name;                       // for the verbose log ("HELLO")
  Handler fn;
};

// Calls the handler of `type` from the table (logging "<tag>: frame NAME, N bytes" at verbose level); a type that is not in it is logged
// as a warning and ignored. False for an unknown type.
bool dispatch(const Spec &m, const Frame *table, size_t count, uint8_t type, const uint8_t *body, size_t len);

// A HELLO from the Palm: body = the rate it offers (u32). Replies with the rate agreed (never above the configured speed), then moves the
// port to it and records the time in negotiatedAt. False when the body was too short (no reply). The mode's own session bookkeeping
// (the Exit Timer, counters) stays with the mode.
bool handleHello(Spec &m, const uint8_t *body, size_t len);

// The rate to agree on for a HELLO that offered `offered`: never above the configured speed, and the idle rate when that is 0.
uint32_t agreedBaud(uint32_t offered, uint32_t idleBaud);

}  // namespace RelayMode
