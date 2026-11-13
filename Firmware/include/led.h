#ifndef LED_H
#define LED_H

// Status LEDs. All LED writes go through here.
//
// The plain LED (LED_PIN; WIRSA_LED_BRIGHTNESS percent, PWM below 100): the WiFi state and the SD flash, as it always was (ledSet, ledPlain).
//
// A colour LED (WIRSA_LED_COLOUR: three LEDs on WIRSA_LED_R_PIN / _G_PIN / _B_PIN with -DWIRSA_LED_DISCRETE, or a WS2812 on -DWIRSA_LED_NEOPIXEL=<gpio>), at WIRSA_LED_NEO_PCT percent:
//   BLUE    while the WiFi is up (connected, or the access point) or the BLE link is up, by the Wifi Pulse setting (settings.h wifiPulse): steady, or dark with a 100 ms blip every 15 s / 30 s / minute, or never a blip.
//           Whatever the setting: connecting = slow blink (30 s at most), came up = steady for 30 s, lost = three quick blinks. Flickers on WiFi or BLE traffic (a dip of the lit blue, a flash of the dark one)
//   GREEN   off; flickers on Palm serial port traffic (bytes in or out of the Palm UART)
//   RED     off; flickers on SD card traffic. Blinking 1 s on / 1 s off: the BOOT button has started HotSync
//   dark    all off in the deep sleep
// Every flicker lasts 30 ms and is followed by at least 30 ms without one, so heavy traffic reads as a steady fast flicker, not as a solid light.
// Nothing else is a state: HotSync, SASD, SLIP, CalDAV, BitWarden, Mesh show as the traffic they cause (SLIP: green and blue).
void ledInit();            // pin setup, LED off
void ledSet(bool on);      // the plain LED
bool ledIsOn();
#if defined(WIRSA_LED_NEOPIXEL) || defined(WIRSA_LED_DISCRETE)
#define WIRSA_LED_COLOUR 1
#endif
#if defined(WIRSA_LED_DISCRETE)
void ledPlain(bool on);    // the plain LED alone, beside the colour LEDs (the SD flash in sd_activity.cpp)
#endif
// Activity kinds for ledActivity(): safe from any task or interrupt-free context, costs a flag write. Does nothing on a plain LED.
enum { LED_ACT_SD = 1, LED_ACT_SERIAL = 2, LED_ACT_NET = 3 };   // red / green / blue
#if defined(WIRSA_LED_COLOUR)
void ledActivity(uint8_t kind);
void ledBle(bool up);          // the BLE link is up (blue steady, like the WiFi)
void ledHotSync(bool on);      // the BOOT button's HotSync blink: red on / off
void ledRefresh();
void ledDark();                // dark from now on, for the deep sleep
#else
static inline void ledActivity(uint8_t) {}   // compiled away: nothing changes on a board with a plain LED
static inline void ledBle(bool) {}
static inline void ledHotSync(bool) {}
static inline void ledRefresh() {}
#endif

#endif // LED_H
