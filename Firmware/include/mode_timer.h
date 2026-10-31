#ifndef MODE_TIMER_H
#define MODE_TIMER_H

// When HotSync, CalDAV Relay and Bitwarden Relay go back to the main menu
// by themselves - the same two rules for all three, however the mode was
// started (button, menu key, WiRSaCtl, Screenless Mode):
//  - no session at all within modeUnusedExitSec of entering: leave
//  - a session ended (BYE, or the line went quiet) modeAfterExitSec ago and
//    no new one started: leave
// Both in seconds, 0 = never; Config > Exit Timers and WiRSaCtl Setup
// (settings.h). Each mode keeps one ModeExitTimer and reports to it.

#include <Arduino.h>

struct ModeExitTimer {
  unsigned long enteredAt;
  unsigned long endedAt;    // 0 = no session has ended since the last one started
  bool sessionSeen;
};

void modeTimerStart(ModeExitTimer &t);          // on entering the mode
void modeTimerSessionActive(ModeExitTimer &t);  // a frame of the mode's own protocol arrived
void modeTimerSessionEnded(ModeExitTimer &t);   // BYE, or the session went quiet
bool modeTimerExpired(const ModeExitTimer &t);  // time to go back to the main menu?

#endif // MODE_TIMER_H
