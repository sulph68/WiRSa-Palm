#ifndef BOOT_BUTTON_H
#define BOOT_BUTTON_H

// BOOT button (GPIO0 = SWITCH_PIN) as a HotSync button - see
// WIRSA_FEATURE_BOOT_HOTSYNC in globals.h. Lets a fresh Palm (no WiRSaCtl
// yet) reach HotSync on a screenless WiRSa: press BOOT, then HotSync.

// Called once per loop() pass: one pin read, never blocks. A short press
// (released after 50 ms - 3 s) in any menu screen starts HotSync (same path
// as the H key: SD check, then HotSync mode); another press while HotSync is
// still waiting for the Palm leaves it. Ignored in every other mode. The
// status LED confirms: 3 slow blinks (1 s on, 1 s off) = HotSync started /
// left, one 2 s blink = not started (no SD card).
void bootButtonPoll();

#endif // BOOT_BUTTON_H
