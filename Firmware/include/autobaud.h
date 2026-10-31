#ifndef AUTOBAUD_H
#define AUTOBAUD_H

// Autobaud on the first character of the "SETBAUD" phrase (palm_ctl.cpp, WIRSA_FEATURE_AUTOBAUD).
// A Palm network login script (or terminal) sends text at its connection's fixed speed. WiRSa's menu runs at 9600 after a boot, so at any other speed it arrives as garbage. The Palm line's RX pin is
// watched by a pin interrupt that time-stamps the first eight edges after a quiet line. If they are the edges of the character 'S' (0x53) (start bit, data bits and stop bit make 8 edges at 0, 1, 3, 5, 6,
// 7, 8, 9 bit times), the bit time comes from a least-squares fit through them (a Palm's UART can put one edge half a bit off, as the sixth at 38400, so fits without one edge are tried too) and the
// speed is the standard rate nearest to it. A "BR<rate>" line, decoded by the UART itself (palm_ctl.cpp), then confirms the speed.
// The detector is plain C++ (host-tested, test_host/autobaud_test.cpp); the pin interrupt part exists only on the ESP32.

#include <stdint.h>

#define AUTOBAUD_EDGES 8

// e: AUTOBAUD_EDGES time stamps (CPU cycles, wrap-safe), cps: cycles per second, rates: the speeds to choose from. Returns the speed, or 0 if the edges are not those of an 'S' or fit no rate within 8%.
uint32_t autobaudDetect(const uint32_t e[AUTOBAUD_EDGES], uint32_t cps, const int *rates, uint8_t nRates);

#ifdef ARDUINO
// start / stop watching pin (idempotent)
void autobaudArm(int pin);
void autobaudDisarm();
// true once a burst of AUTOBAUD_EDGES edges was seen after >= 2 ms of quiet and the line has been quiet again for 3 ms (the whole phrase is over); the stamps are copied to e. A partial burst is discarded after 20 ms.
bool autobaudTake(uint32_t e[AUTOBAUD_EDGES]);
#endif

#endif
