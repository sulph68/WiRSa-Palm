// mode_exit.h - the two ways every mode is left from the device itself, and the way back to the main menu:
//   - the BACK button;
//   - "+++" typed on the USB serial (never on the Palm port, which carries binary data).
// Used by the relay modes (relay_mode.h), SLIP Gateway and SASD, so the rule is in one place.
#pragma once

namespace ModeExit {

// Reads the buttons. True when BACK was pressed (it has been waited out, so the press does not leak into the menu). The other
// buttons (BTNUP, BTNDN, BTNEN) are left set for the mode to look at.
bool backPressed();

// True when "+++" has been typed on the USB serial, the three within one second of each other; "OK" is printed first. Other
// characters typed meanwhile are discarded.
bool escapeTyped();

// Back to the main menu with a full redraw (the modes draw their own screens, so the menu's usual partial redraw would leave them behind).
void toMenu();

}  // namespace ModeExit
