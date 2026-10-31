// Mount the SD card: the normal speed first, and if that fails once more at a slow clock. A card
// that is marginal at 20 MHz (long wires, a worn contact) often still mounts at 4 MHz, and a
// working slow mount is better than "no card". Logs on a change only, so a repeated probe is quiet.
// The card is mounted at /SDCard and the virtual card at /ESP32 (vdfs.h), so the FTP server's root
// (ftp_rootfs.h) can show both as folders; everything else uses the SD object and never sees the name.
#pragma once
#include "globals.h"

// Is the mounted card still answering? SD.cardType() is the type found at mount and stays after the card is pulled,
// so read the first sector: about a millisecond. A card that does not answer is unmounted, and the next sdMount()
// starts afresh (also when it is put back).
inline bool sdAlive() {
  if (SD.cardType() == CARD_NONE) return false;
  static uint8_t sector[512];
  if (SD.readRAW(sector, 0)) return true;
  SD.end();
  return false;
}

inline bool sdMount() {
  static int8_t last = -1;
  bool ok = SD.begin(SD_CS, SPI, WIRSA_SD_HZ, "/SDCard");
  bool slow = false;
  if (!ok) {
    SD.end();
    delay(20);
    ok = slow = SD.begin(SD_CS, SPI, 4000000, "/SDCard");
  }
  if ((int8_t) ok != last) {
    last = (int8_t) ok;
    if (ok && slow) LOGW("SD: card mounted only at a reduced clock (4 MHz) - check the wiring\r\n");
    else if (ok) LOGI("SD: card mounted\r\n");
    else LOGW("SD: no card (or it did not answer at 20 MHz or 4 MHz)\r\n");
  }
  return ok;
}
