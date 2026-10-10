// Firmware Version Module
// Over-The-Air (OTA) update support (httpUpdate.h flash-over-HTTP) has been
// removed entirely - it was already fully stubbed out/commented-out dead
// code with zero live callers before this cleanup. Only the startup banner
// remains.

#include "Arduino.h"
#include "firmware.h"
#include "serial_io.h"

void firmwareCheck() {
  SerialPrintLn("\nThis WiRSa runs a custom firmware optimized for PalmOS."); yield();
}
