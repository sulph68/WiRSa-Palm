// mode_exit.cpp - see mode_exit.h.
#include "mode_exit.h"
#include "globals.h"
#include "serial_io.h"
#include "display_menu.h"
#include <Arduino.h>

namespace ModeExit {

bool backPressed() {
  readSwitches();
  if (!BTNBK) return false;
  waitSwitches();
  return true;
}

bool escapeTyped() {
  static char escapeBuffer[4] = {0};
  static int escapePos = 0;
  static unsigned long lastEscapeTime = 0;
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '+') {
      if (millis() - lastEscapeTime > 1000) escapePos = 0;
      escapeBuffer[escapePos++] = c;
      lastEscapeTime = millis();
      if (escapePos >= 3) {
        SerialPrintLn("\r\nOK");
        return true;
      }
    } else {
      escapePos = 0;
    }
  }
  return false;
}

void toMenu() {
  msgFlag = true;
  mainMenu(false);
}

}  // namespace ModeExit
