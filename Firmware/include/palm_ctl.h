#ifndef PALM_CTL_H
#define PALM_CTL_H

// Palm control link: WiRSaCtl (palmos_sdk apps/WiRSaControl/) drives WiRSa over the Palm line with framed commands, 0x16 + a RelayProto frame. Wire: docs/protocol/Protocol-wire.md (sections 7
// and 12). Built when WIRSA_FEATURE_PALMCTL.

#include <Arduino.h>
#include "globals.h"
#include "serial_io.h"

#if WIRSA_FEATURE_PALMCTL

// Menu key reader for every single-letter menu loop (main menu, Config and its submenus, Utilities, FTP Server, WiFi Setup). Returns 1 with the key in chr, or 0. USB console keys pass straight
// through. Palm-line keys pass the burst guard: a key counts only if the line was quiet for 100 ms before it and nothing but CR/LF follows within 100 ms; anything else (a burst, e.g. a stray SASD
// packet) is drained. A 0x16 starts a control frame, handled here, and returns 0. A mode the Palm asked for (ENTER_MODE) comes back as its menu letter once the main menu is active, so the main menu
// starts it as if typed.
int menuKey(char &chr);

// Called at the top of the CalDAV, BitWarden and Mesh loops and HotSync's waiting state with that mode's letter. Handles a control frame if one is next on the line (PING, STATUS, EXIT). True when the
// Palm asked to leave the mode (EXIT, accepted only if exitAllowed): the caller then exits as its BACK button does. maxRelayType (relay modes): the highest Palm->WiRSa frame type of the mode; bytes that
// cannot start a frame are dropped first. 0 = no filter (HotSync's scanner resyncs itself).
bool palmCtlModeHook(char mode, bool exitAllowed, uint8_t maxRelayType);

// The rate the menu (and SLIP Gateway, which starts from it) uses now. With WIRSA_FEATURE_DUAL_BAUD: 9600 after boot, then what SPEED or the second receiver set for this power-on; else the configured
// baud. Every place that reopens the Palm line for the menu uses this, and PING reports it.
uint32_t menuBaud();
// Bytes waiting on the Palm line (either receiver): used by the deep sleep idle timer.
bool palmCtlRxPending();
// The configured baud (the maximum for the modes' negotiation) changed (Config, CFG_SET): the menu keeps its session rate. The caller restarts PhysicalSerial at menuBaud().
void menuBaudReset();
// Once per loop() pass: the second receiver runs only in the menus.
void palmCtlPoll();

#else

inline int menuKey(char &chr) {
  if (SerialAvailable() > 0) { chr = (char) SerialRead(); return 1; }
  return 0;
}
inline bool palmCtlModeHook(char, bool, uint8_t) { return false; }
inline uint32_t menuBaud() { return (uint32_t) bauds[serialSpeed]; }
inline void menuBaudReset() {}
inline void palmCtlPoll() {}

#endif

#endif // PALM_CTL_H
