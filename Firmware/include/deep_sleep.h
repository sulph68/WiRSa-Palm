#pragma once
// Deep sleep: with a delay set (settings.h deepSleepMin: 1, 3, 5 or 15 minutes), the board sleeps when
// the main menu has had no traffic from the Palm (or the USB console) for that long and nothing needs
// it awake (no FTP server, no mounted card, no mode running). A byte on the Palm line wakes it:
// GPIO3 is wired to GPIO44 (the Palm RX), and a start bit pulls it low. A wake is a full restart
// (setup() runs again); the session log in RTC memory survives it. Only the T7-S3 build, which has the wire.

#include <Arduino.h>
#include "globals.h"

#if defined(WIRSA_DEEP_SLEEP)
void deepSleepBoot();   // setup(): after logInit(); logs a wake and checks that the fast RTC memory was kept
void deepSleepPoll();   // loop(): before palmCtlPoll()
// Arms the Palm-line wake (and a timer, 0 = none) and sleeps: no return. Used by the sleep after the idle time and by the
// Mesh relay's timer wake, which checks the node and sleeps again without the rest of setup().
void deepSleepEnter(uint64_t timerUs);
#else
static inline void deepSleepBoot() {}
static inline void deepSleepPoll() {}
static inline void deepSleepEnter(uint64_t) {}
#endif
