#pragma once
// Formatting the SD card from WiRSa (Utilities > Format SD Card, and WiRSaControl's FORMAT_SD), and preparing a newly formatted
// SD card with the SASD install set copied from the virtual card.
//
// Callers run these from the menus only, with the FTP server stopped (it holds both disks): nothing else may have the
// SD card or the virtual card open. The virtual card is never formatted on request: a blank Palm is set up from it. Each call blocks until it is done: seconds for a FAT16 card, longer for a big FAT32 one.
#include <Arduino.h>

// Erases the SD card: a new partition table and one FAT volume, FAT16 for cards up to 2 GB, FAT32 above (as
// docs/user_guides/WiRSa.md says). The card is mounted again afterwards. On success *fatBits is 16 or 32; on failure
// err says why ("No SD card", "Format failed (14)", ...).
bool sdFormatCard(uint8_t *fatBits, String &err);

// Copies every file in the virtual card's /PALM/PROGRAMS/SASD to the same folder on the SD card (made if missing),
// reading each one back to check it. *copied = files written and checked, *total = files found on the virtual card
// (0 if it has no SASD set). False only when a disk could not be opened (err says which); a file that fails is
// counted in total but not in copied.
bool sdCopySasdFromVirtual(uint8_t *copied, uint8_t *total, String &err);
