#ifndef HOTSYNC_H
#define HOTSYNC_H

#include <Arduino.h>

// ---- HotSync Install mode ----
// Wire-compatible (not a code port) with the embedded HotSync host client of dmitry.gr's SASD project (hotsync.c, prc.c, slk.c, frag.c). WiRSa plays the desktop cradle: it waits for a Palm's HotSync
// WAKE, negotiates a session over the real DLP/PADP/SLP protocol, pushes every .PRC/.PDB in two folders of the selected disk (the SD card or the virtual card, per the Disk setting) to the device, and
// ends the sync, with no desktop computer. Use: docs/README-Palm.md. Design: apps/WiRSa-Palm.md.
// It reuses the MODE_FILE slot (globals.h), once "File Transfer" (XMODEM/YMODEM/ZMODEM), as SASD reused MODE_PLAYBACK. File Transfer's code was unreachable and is removed; only zmCRC16Update() (now
// main.cpp/globals.h) survives, for sasd.cpp and hotsync.cpp's slk-layer CRC.

// menu loop (called from main.cpp's loop() in MODE_FILE)
void hotsyncLoop();

// Enter HotSync mode: entering the menu item is the on/off, as every mode; nothing to configure.
void enterHotsyncMode();

// Choose and mount the disk HotSync installs from (the Disk setting) and create its install folders. Call before enterHotsyncMode(); false when there is no disk.
bool hotsyncPrepareDisk();

// Leave HotSync if it is still waiting for the Palm (BOOT button).
void hotsyncCancelWait();

#endif // HOTSYNC_H
