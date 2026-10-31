// modes.h - the table of WiRSa's modes: one row for each, and the one place that knows which of them are built. Everything that used to
// repeat "which modes are there" reads it instead: the main menu (its entries, their letters and order), the key handling of the main
// menu, the main loop's dispatch on menuMode, WiRSaCtl (the mode letter it shows, which modes it may start, what it is told is built,
// what each mode needs before it starts) and the background WiFi reconnect (which modes are data transfers).
//
// Adding a mode: write its loop and enter functions, add a MODE_xxx id and a WIRSA_FEATURE_xxx switch to globals.h, and add ONE row to
// kModes in modes.cpp (with the `#if` that compiles it out). See docs/Relay-Modes.md for the relay modes' part.
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace Modes {

// What WiRSaCtl is told is built in (the flags word of STATUS / PING, see WiRSaControl.md). The two that are not modes (SCREENLESS and
// SPEED) are palm_ctl.cpp's.
enum : uint16_t {
  FLAG_FTP = 0x0002, FLAG_HOTSYNC = 0x0004, FLAG_SASD = 0x0008, FLAG_SLIP = 0x0010,
  FLAG_BITWARDEN = 0x0020, FLAG_CALDAV = 0x0040, FLAG_MESH = 0x0100,
};

struct Info {
  int id;                  // the value of menuMode while the mode runs (MODE_xxx)
  char key;                // its letter, on the main menu and for WiRSaCtl; 0 = not on the main menu
  const char *label;       // the main menu's text ("SAS[D] SDCard"); nullptr = not listed
  uint16_t flag;           // the bit WiRSaCtl is told when it is built (0 = none)
  bool built;              // compiled in (the WIRSA_FEATURE_xxx switch)
  bool ctlEnter;           // WiRSaCtl may start it (ENTER)
  bool needsWifi;          // ... and WiFi must be up first
  bool needsCard;          // ... and an SD card, unless the virtual card can stand in (HotSync, SASD)
  bool transfer;           // it moves data over the Palm line and WiFi: the background WiFi reconnect leaves WiFi alone meanwhile
  void (*loop)();          // runs every pass of loop() while menuMode == id; nullptr = nothing to run
  void (*enter)();         // starts it from the main menu; nullptr = not startable that way
};

const Info *all(size_t &count);
const Info *byId(int id);              // nullptr when no row has that id, or it is not built
const Info *byKey(char key);           // the letter in either case; nullptr when there is none, or it is not built
bool knownCtlLetter(char key);         // a letter WiRSaCtl can ask for, built or not (to tell "not a mode" from "not built")

// The main menu: its entries, in row order (only built rows with a label).
int menuEntries(const char **labels, int cap);

// A key (or a menu selection) at the main menu: starts the mode whose letter or position it is, echoing the letter first.
// False when it was neither.
bool startFromMenu(char key, int menuSel);

// The main loop's dispatch: runs the loop of the mode that menuMode names. False when there is none.
bool runLoop(int menuModeId);

// Whether the running mode is a data transfer (the background WiFi reconnect must not touch WiFi then).
bool isTransfer(int menuModeId);

// The letter WiRSaCtl shows for the running mode ('M' for the main menu and everything that is not a mode it can start).
char ctlLetter(int menuModeId);

// The flags of the modes that are built.
uint16_t builtFlags();

}  // namespace Modes
