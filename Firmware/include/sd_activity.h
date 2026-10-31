#ifndef SD_ACTIVITY_H
#define SD_ACTIVITY_H

// Flash the status LED for ~30 ms (inverse of its current state) to show SD
// card traffic. Non-blocking: a one-shot timer puts the LED back. Safe to call
// for every sector; calls during a flash only extend it.
void sdActivity();

#endif // SD_ACTIVITY_H
