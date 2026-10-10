// Diagnostics Module
// Serial diagnostics and troubleshooting tools for WiRSa

#ifndef DIAGNOSTICS_H
#define DIAGNOSTICS_H

#include <Arduino.h>

// Diagnostic state context - was a 7-field struct (loopback counters,
// connection/byte counters) whose only writer was the AT-command dispatcher
// below, itself confirmed to have zero real callers anywhere (the AT-
// command parser it served was already removed elsewhere - see this file's
// own real-hardware comment history). hexDumpEnabled is the one field with
// a real, live reader left (hexDumpByte, called from serial_io.cpp) - it's
// permanently false now (no remaining writer), making those calls inert,
// but kept rather than also touching serial_io.cpp's RX/TX hot path.
struct DiagnosticsContext {
  bool hexDumpEnabled;       // Hex dump mode active - permanently false, see above
};

extern DiagnosticsContext diagCtx;

// Initialization
void diagnosticsInit();

// Menu and navigation
void diagnosticsMenu(bool arrow);
void diagnosticsLoop();

// Hex Dump Mode
void hexDumpByte(char direction, uint8_t byte);  // 'T' for TX, 'R' for RX - inert, see DiagnosticsContext's own comment

// Utilities menu's own "Information" screen - HotSync/BitWarden-style
// boxed header + scrollable status area, Palm-button-navigable (DOWN
// scrolls, BACK returns to Utilities) since it overflows one screen.
void showInformationScreen();

#endif // DIAGNOSTICS_H
