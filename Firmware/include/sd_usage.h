#ifndef SD_USAGE_H
#define SD_USAGE_H

// SD card used/total for the menus (the WiRSaControl SD row), in the same short
// units as the SASD status screen. Used space comes from walking the FAT16/FAT32
// allocation table a few sectors per loop pass while the menus are idle (a
// full-card walk blocks for seconds), so it appears some time after the card
// is first seen and is redone after any SD traffic. Nothing is stored.

#include <Arduino.h>

String sdHumanBytes(uint64_t bytes, int decimals);   // "1.234GB", "14.8GB" - SASD's own shortening

void sdUsageActivity();        // SD traffic happened (sd_activity): used space may have changed
void sdUsageStep();            // call every loop pass while in the menus; does a little work at most
String sdUsageText();          // "1.234GB/14.8GB", "?/14.8GB" while counting, "" if unknown

#endif // SD_USAGE_H
