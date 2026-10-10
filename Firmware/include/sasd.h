#ifndef SASD_H
#define SASD_H

#include <Arduino.h>

// ============================================================================
// SASD - Serial-Attached SD mode
// ============================================================================
// Wire-compatible (not code-ported) with dmitry.gr's real SASD project
// (~/palmos_sdk/Sample_Source/serialSD/). WiRSa plays the "device" role (the
// SD-card-holding end that the reference firmware/lpc81x.c or firmware/
// lpc802.c plays) so the Palm OS SASD.prc slot driver - unmodified, already
// installed - can mount this device's SD card as a native VFS volume over
// the same serial cable SLIP/File Transfer already use. Every VFS-using app
// already built in this project (Address35's SD browsing, CryptoNote's
// card-file features) gains SD access through this automatically, with no
// app-side changes.
//
// This mode is fully passive: all commands are initiated BY the Palm's own
// slot driver; WiRSa only ever replies. Mutually exclusive with SLIP/PPP -
// occupies the repurposed MODE_PLAYBACK slot (see globals.h) that used to
// be "Text Playback".
// ============================================================================

// Menu loop function (called from main.cpp's loop() when in MODE_PLAYBACK)
void sasdLoop();

// Enter/exit SASD mode - entering the menu item IS the on/off mechanism,
// same as every other mode; no separate config step (nothing to configure).
void enterSasdMode();
void exitSasdMode();

// Show SASD status on the OLED
void sasdShowStatus();

#endif // SASD_H
