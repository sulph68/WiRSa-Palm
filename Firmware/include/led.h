#ifndef LED_H
#define LED_H

// Status LED with adjustable brightness. WIRSA_LED_BRIGHTNESS (percent, set in
// globals.h) below 100 drives the pin with PWM; at 100 it is a plain digital
// output. With -DWIRSA_LED_NEOPIXEL=<gpio> the LED is a WS2812 on that pin (colour WIRSA_LED_RGB, default blue = WiFi connected, WIRSA_LED_NOWIFI_RGB, default red = WiFi down, at WIRSA_LED_NEO_PCT percent, default 1).
// All LED writes go through here so the brightness applies everywhere.
void ledInit();            // pin setup, LED off
void ledSet(bool on);
bool ledIsOn();
// An RGB LED (WIRSA_LED_NEOPIXEL): ledSet(true) shows WIRSA_LED_RGB (blue, WiFi on), ledSet(false) shows WIRSA_LED_NOWIFI_RGB (red, WiFi off).
// In a Mesh session it is dark whatever the WiFi does: the green activity pulses are the only light there.
// ledActivity(kind): a short pulse over whatever the LED shows, then back. Safe from any task. Does nothing on a plain LED.
//   LED_ACT_SD     a dark flash of ~30 ms: SD card traffic (sd_activity.cpp)
//   LED_ACT_FRAME  a green flicker of ~40 ms: a frame to or from the Mesh node
//   LED_ACT_TEXT   a green blink of ~150 ms: a text sent or received, or an acknowledgement
//   LED_ACT_MODE   a green blink of ~400 ms: the Mesh mode has started
// ledRefresh(): draw the state again (the mode changed). ledDark(): dark from now on, for the deep sleep.
enum { LED_ACT_SD = 1, LED_ACT_FRAME = 2, LED_ACT_TEXT = 3, LED_ACT_MODE = 4 };
#if defined(WIRSA_LED_NEOPIXEL)
void ledActivity(uint8_t kind);
void ledRefresh();
void ledDark();
#else
static inline void ledActivity(uint8_t) {}   // compiled away: nothing changes on a board with a plain LED
static inline void ledRefresh() {}
#endif

#endif // LED_H
