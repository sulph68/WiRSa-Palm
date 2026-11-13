// boot_button.cpp - BOOT button (GPIO0) as a HotSync button. See
// boot_button.h and WIRSA_FEATURE_BOOT_HOTSYNC in globals.h.

#include "globals.h"
#include "led.h"

#if WIRSA_FEATURE_BOOT_HOTSYNC && WIRSA_FEATURE_HOTSYNC

#include "boot_button.h"
#include "hotsync.h"
#include "display_menu.h"
#include <WiFi.h>

namespace {

const unsigned long PRESS_MIN_MS = 50;     // shorter = contact bounce
const unsigned long PRESS_MAX_MS = 3000;   // longer = not a deliberate tap
const unsigned long BLINK_SHORT_MS = 1000;   // half a period: 1 s on, 1 s off
const unsigned long BLINK_LONG_MS = 2000;

bool gDown = false;
unsigned long gDownAt = 0;

// Non-blocking LED blinker: gBlinkSteps on/off half-periods left. The LED
// is the WiFi indicator otherwise (network.cpp, LOW = on), so it is put
// back to that state when the pattern ends.
int gBlinkSteps = 0;
unsigned long gBlinkStepMs = 0;
unsigned long gBlinkNextAt = 0;

void blinkStart(int blinks, unsigned long stepMs) {
  gBlinkSteps = blinks * 2;
  gBlinkStepMs = stepMs;
  gBlinkNextAt = millis();
}

void blinkTick() {
  if (gBlinkSteps <= 0 || (long) (millis() - gBlinkNextAt) < 0) return;
  gBlinkSteps--;
  gBlinkNextAt = millis() + gBlinkStepMs;
  if (gBlinkSteps > 0)
    { ledSet(gBlinkSteps & 1); ledHotSync(gBlinkSteps & 1); }   // odd = on: the plain LED and the red LED
  else {
    ledSet(WiFi.status() == WL_CONNECTED);
    ledHotSync(false);
  }
}

// true once per short press, on release
bool pressed() {
  bool now = digitalRead(SWITCH_PIN) == LOW;
  if (now && !gDown) {
    gDown = true;
    gDownAt = millis();
    return false;
  }
  if (!now && gDown) {
    gDown = false;
    unsigned long held = millis() - gDownAt;
    return held >= PRESS_MIN_MS && held <= PRESS_MAX_MS;
  }
  return false;
}

bool inMenus() {
  switch (menuMode) {
    case MODE_MAIN: case MODE_SETTINGS: case MODE_SETBAUD: case MODE_ORIENTATION:
    case MODE_WIFI_SETUP: case MODE_WIFI_PASSWORD: case MODE_DIAGNOSTICS: case MODE_FTPSERVER:
      return true;
    default:
      return false;
  }
}

}  // namespace

void bootButtonPoll() {
  blinkTick();
  if (!pressed()) return;

  if (menuMode == MODE_FILE) {               // HotSync, maybe still waiting
    hotsyncCancelWait();
    if (menuMode != MODE_FILE) blinkStart(3, BLINK_SHORT_MS);
    return;
  }
  if (!inMenus()) return;                    // SASD, SLIP, CalDAV, Bitwarden

  if (menuMode != MODE_MAIN) {
    msgFlag = true;
    mainMenu(false);
  }
  if (startHotsyncFromMenu())
    blinkStart(3, BLINK_SHORT_MS);
  else
    blinkStart(1, BLINK_LONG_MS);            // no SD card
}

#endif
