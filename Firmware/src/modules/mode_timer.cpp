// mode_timer.cpp - see mode_timer.h.

#include "mode_timer.h"
#include "settings.h"

void modeTimerStart(ModeExitTimer &t) {
  t.enteredAt = millis();
  t.endedAt = 0;
  t.sessionSeen = false;
}

void modeTimerSessionActive(ModeExitTimer &t) {
  t.sessionSeen = true;
  t.endedAt = 0;
}

void modeTimerSessionEnded(ModeExitTimer &t) {
  t.sessionSeen = true;
  t.endedAt = millis();
  if (t.endedAt == 0) t.endedAt = 1;   // 0 means "not ended"
}

bool modeTimerExpired(const ModeExitTimer &t) {
  unsigned long now = millis();
  if (!t.sessionSeen)
    return modeUnusedExitSec && now - t.enteredAt > (unsigned long) modeUnusedExitSec * 1000UL;
  return t.endedAt && modeAfterExitSec && now - t.endedAt > (unsigned long) modeAfterExitSec * 1000UL;
}
