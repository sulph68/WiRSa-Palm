#ifndef SLIP_MODE_H
#define SLIP_MODE_H

#include <Arduino.h>
#include "slip.h"
#include "slip_nat.h"

// ---- SLIP mode states ----

enum SlipModeState {
    SLIP_MODE_IDLE,     // Not active
    SLIP_MODE_MENU,     // In SLIP menu
    SLIP_MODE_ACTIVE,   // SLIP gateway running
    SLIP_MODE_CONFIG,   // Configuring settings
    SLIP_MODE_ERROR     // Error state
};

// ---- SLIP configuration (saved to EEPROM) ----

struct SlipConfig {
    IPAddress slipIP;       // Gateway IP (ESP32 side)
    IPAddress clientIP;     // Client IP (vintage computer)
    IPAddress subnetMask;   // Subnet mask
    IPAddress dnsServer;    // DNS server
};

// ---- SLIP mode context ----

struct SlipModeContext {
    SlipModeState state;
    SlipConfig config;
    bool configChanged;
    int previousMenuMode;  // Track previous mode for proper return
};

// ---- EEPROM addresses for the SLIP configuration ----

// Renumbered to follow globals.h's ESC_CHAR_ADDRESS (103, 1 byte); see its comment on the compact layout and the one-time defaultEEPROM() reset the renumbering needs.
#define SLIP_IP_ADDRESS         104   // 4 bytes - Gateway IP
#define SLIP_CLIENT_IP_ADDRESS  108   // 4 bytes - Client IP
#define SLIP_SUBNET_ADDRESS     112   // 4 bytes - Subnet mask
#define SLIP_DNS_ADDRESS        116   // 4 bytes - DNS server

// ---- mode interface ----

// main loop (called from loop() in MODE_SLIP)
void slipLoop();

void enterSlipMode();

// exit SLIP gateway mode (the caller returns to the main menu)
void exitSlipMode();

// ---- display ----

void slipShowStatus();


// ---- configuration ----

void loadSlipSettings();

void saveSlipSettings();

// Config menu's "SLIP Setup": Gateway IP / Client IP / DNS Server prompts
void slipSetup();

// ---- global contexts (defined in slip_mode.cpp) ----

extern SlipContext slipCtx;
extern NatContext natCtx;
extern SlipModeContext slipModeCtx;

#endif // SLIP_MODE_H
