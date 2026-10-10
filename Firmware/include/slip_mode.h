#ifndef SLIP_MODE_H
#define SLIP_MODE_H

#include <Arduino.h>
#include "slip.h"
#include "slip_nat.h"

// ============================================================================
// SLIP Mode States
// ============================================================================

enum SlipModeState {
    SLIP_MODE_IDLE,     // Not active
    SLIP_MODE_MENU,     // In SLIP menu
    SLIP_MODE_ACTIVE,   // SLIP gateway running
    SLIP_MODE_CONFIG,   // Configuring settings
    SLIP_MODE_ERROR     // Error state
};

// ============================================================================
// SLIP Configuration (saved to EEPROM)
// ============================================================================

struct SlipConfig {
    IPAddress slipIP;       // Gateway IP (ESP32 side)
    IPAddress clientIP;     // Client IP (vintage computer)
    IPAddress subnetMask;   // Subnet mask
    IPAddress dnsServer;    // DNS server
};

// ============================================================================
// SLIP Mode Context
// ============================================================================

struct SlipModeContext {
    SlipModeState state;
    SlipConfig config;
    bool configChanged;
    int previousMenuMode;  // Track previous mode for proper return
};

// ============================================================================
// EEPROM Addresses for SLIP Configuration
// ============================================================================

// Renumbered to sit immediately after globals.h's own ESC_CHAR_ADDRESS
// (103, 1 byte) - see that file's own comment on the compacted layout and
// the one-time defaultEEPROM() reset this renumbering requires.
#define SLIP_EEPROM_BASE        104
#define SLIP_IP_ADDRESS         104   // 4 bytes - Gateway IP
#define SLIP_CLIENT_IP_ADDRESS  108   // 4 bytes - Client IP
#define SLIP_SUBNET_ADDRESS     112   // 4 bytes - Subnet mask
#define SLIP_DNS_ADDRESS        116   // 4 bytes - DNS server
#define SLIP_EEPROM_END         120   // End of SLIP EEPROM area
// Note: Port forwards are stored in a shared location (PORTFWD_BASE in globals.h)

// ============================================================================
// Function Declarations - Mode Interface
// ============================================================================

// Menu function (called to enter/display SLIP menu)
void slipMenu(bool arrow);

// Main loop function (called from main loop when in MODE_SLIP)
void slipLoop();

// Enter SLIP gateway mode (start SLIP processing)
void enterSlipMode();

// Exit SLIP gateway mode (stop SLIP, return to menu)
void exitSlipMode();

// ============================================================================
// Function Declarations - Display
// ============================================================================

// Show SLIP status on display
void slipShowStatus();

// Show SLIP statistics
void slipShowStatistics();

// ============================================================================
// Function Declarations - Configuration
// ============================================================================

// Load SLIP settings from EEPROM
void loadSlipSettings();

// Save SLIP settings to EEPROM
void saveSlipSettings();

// Set default SLIP configuration
void slipDefaultConfig();

// Configure IP addresses
void slipConfigureIP();

// Configure port forwards
void slipConfigurePortForward();

// ============================================================================
// Function Declarations - AT Commands
// ============================================================================

// Handle SLIP-related AT commands
// Returns true if command was handled
bool handleSlipCommand(String& cmd, String& upCmd);

// ============================================================================
// Global Contexts (defined in slip_mode.cpp)
// ============================================================================

extern SlipContext slipCtx;
extern NatContext natCtx;
extern SlipModeContext slipModeCtx;

#endif // SLIP_MODE_H
