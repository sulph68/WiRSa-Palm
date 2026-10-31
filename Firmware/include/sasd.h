#ifndef SASD_H
#define SASD_H

#include <Arduino.h>

// ---- SASD - Serial-Attached SD mode ----
// Wire-compatible (not a code port) with dmitry.gr's SASD project (palmos_sdk Sample_Source/serialSD/). WiRSa plays the "device" role (the SD-holding end that the reference firmware/lpc81x.c or lpc802.c
// plays), so the Palm OS SASD.prc slot driver, unmodified, mounts this device's SD card as a native VFS volume over the cable SLIP also uses. Every VFS-using app here (Address35's SD browsing,
// CryptoNote's card files) gains SD access with no app changes.
// Fully passive: the Palm's slot driver initiates every command and WiRSa only replies. Mutually exclusive with SLIP/PPP; it uses the repurposed MODE_PLAYBACK slot (globals.h), once "Text Playback".

// menu loop (called from main.cpp's loop() in MODE_PLAYBACK)
void sasdLoop();

// Enter/exit SASD mode: entering the menu item is the on/off, as every mode; nothing to configure.
void enterSasdMode();
void exitSasdMode();

void sasdShowStatus();

#endif // SASD_H
