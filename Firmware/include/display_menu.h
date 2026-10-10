#ifndef DISPLAY_MENU_H
#define DISPLAY_MENU_H

#include <Arduino.h>

// Display and menu system functions
void showMessage(String message);
// showWiRSaPrefix defaults to true (every existing menu keeps the "WiRSa "
// title prefix unchanged) - only the FTP Server/CONFIG/Utilities menus pass
// false, since they want a shorter title (see displayTitle below).
// displayTitle (optional) overrides the header's own printed text without
// touching menuName, which still drives the serial "-=-=- X MENU -=-=-"
// banner - used by diagnosticsMenu() to show "Utilities" while keeping its
// internal "DIAG" menu name.
void showMenu(String menuName, String options[], int sz, int dispMode, int defaultSel, bool showWiRSaPrefix = true, String displayTitle = "");
void menuOption(int idx, String item);
bool showHeader(String menu, int dispMode, bool showWiRSaPrefix = true, String displayTitle = "");
void mainMenu(bool arrow);
void settingsMenu(bool arrow);
void baudMenu(bool arrow);
void orientationMenu(bool arrow);
void ftpServerMenu(bool arrow);
void readSwitches();
void readBackSwitch();
void waitSwitches();
void waitBackSwitch();
String padLeft(String s, int len);
String padRight(String s, int len);
String liveBaudStr(uint32_t baud);

// Relocated from modem.cpp (not modem-specific - general menu-loop/input
// helpers other modes depend on) during MODEM mode removal.
String getLine();
String prompt(String msg, String dflt = "");
void led_on();
void mainLoop();
void baudLoop();
void orientationLoop();
void settingsLoop();
void printSDCardInfo();
void ftpServerLoop();
void sendResult(int resultCode);

// checkCancel()/clearInputBuffer() relocated here from file_transfer.cpp
// during its removal (XMODEM/YMODEM/ZMODEM file transfer, confirmed
// unreachable from the main menu) - both are still genuinely used by
// getLine()/prompt() above, which SLIP Gateway's config prompts depend on.
bool checkCancel();
void clearInputBuffer();

// Generic blocking character-entry primitive, generalized from the WiFi-join
// password entry mechanism (wifi_setup.cpp) for reuse by FTP/AP credential
// setup - see settingsLoop()'s FTP Setup/AP Setup handling.
bool charEntryPrompt(const char *title, char *buf, int maxLen);

#endif // DISPLAY_MENU_H
