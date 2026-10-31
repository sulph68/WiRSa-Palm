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
void waitSwitches();
String padLeft(String s, int len);
String padRight(String s, int len);
String liveBaudStr(uint32_t baud);

// Relocated from modem.cpp (not modem-specific - general menu-loop/input
// helpers other modes depend on) during MODEM mode removal.
void led_on();
void mainLoop();
void baudLoop();
void orientationLoop();
void settingsLoop();
void printSDCardInfo();
#if WIRSA_FEATURE_SASD
void startSasdFromMenu();       // the D key: the disk check, then SASD mode (modes.cpp)
#endif
#if WIRSA_FEATURE_HOTSYNC
bool startHotsyncFromMenu();   // H key / BOOT button: SD check, then HotSync mode
#endif
void ftpServerLoop();

// Generic blocking character-entry primitive, generalized from the WiFi-join
// password entry mechanism (wifi_setup.cpp) for reuse by FTP/AP credential
// setup - see settingsLoop()'s FTP Setup/AP Setup handling. charset limits
// what can be entered (e.g. "0123456789." for SLIP Setup's addresses); the
// default, NULL, is every printable character, starting at 'A'.
bool charEntryPrompt(const char *title, char *buf, int maxLen, const char *charset = NULL);

#endif // DISPLAY_MENU_H
