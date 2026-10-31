// Battery monitor - boards that wire the battery through a resistor divider
// to an ADC pin (LilyGO T7-S3: GPIO2, 1/2 divider). A board opts in by
// defining BATTERY_PIN in its globals.h block; everywhere else this module
// compiles to nothing and batteryAvailable() is a constant false.

#ifndef BATTERY_H
#define BATTERY_H

#include <Arduino.h>

bool batteryAvailable();

// Battery voltage in millivolts (averaged, divider and calibration applied),
// or 0 when the board has no battery input or nothing is connected to it.
uint32_t batteryMilliVolts();

// Approximate state of charge, 0-100, from a resting-voltage LiPo curve.
// Under load or while charging it reads high/low - treat it as a guide.
int batteryPercent(uint32_t mv);

// One display line for the Information screen, e.g. "Batt: 3.92V 78%", or
// "Batt: none" when nothing is connected to the input.
String batteryInfoLine();

#endif // BATTERY_H
