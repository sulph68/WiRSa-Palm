#ifndef HOTSYNC_H
#define HOTSYNC_H

#include <Arduino.h>

// ============================================================================
// HotSync Install mode
// ============================================================================
// Wire-compatible (not code-ported) with dmitry.gr's real SASD project's own
// embedded HotSync host client (~/Downloads/sasdSource/SASD_release/software/
// serialSD/firmware/hotsync.c + prc.c + slk.c + frag.c). WiRSa plays the
// desktop-cradle role: it waits for a Palm's HotSync WAKE packet, negotiates
// a session over the real DLP/PADP/SLP wire protocol, then pushes every
// .PRC/.PDB file it finds in two SD-card folders onto the device, and ends
// the sync - no desktop computer involved. Full design/rationale in
// ~/WiRSa/HOTSYNC_PLAN.md.
//
// Repurposes the MODE_FILE slot (see globals.h) that used to be "File
// Transfer" (XMODEM/YMODEM/ZMODEM over modem.cpp/file_transfer.cpp) - same
// "repurpose an existing mode slot" approach already used once for SASD
// (MODE_PLAYBACK). File Transfer's own code (fileMenu/fileLoop/protocolMenu/
// listFilesMenu in modem.cpp/display_menu.cpp, plus all of file_transfer.cpp
// itself) was confirmed unreachable from the main menu once this was wired
// in, and has since been removed outright - only zmCRC16Update() (now in
// main.cpp/globals.h) survived, since sasd.cpp/hotsync.cpp both still use
// it for their own shared slk-layer CRC.
// ============================================================================

// Menu loop function (called from main.cpp's loop() when in MODE_FILE)
void hotsyncLoop();

// Enter HotSync mode - entering the menu item IS the on/off mechanism, same
// as every other mode; no separate config step (nothing to configure).
void enterHotsyncMode();

#endif // HOTSYNC_H
