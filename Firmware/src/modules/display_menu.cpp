// Display and Menu System Module
// Handles all display rendering and menu navigation

#include "Arduino.h"
#include "display_menu.h"
#include "globals.h"
#include "serial_io.h"
#include "network.h"
#include "settings.h"
#include "hotsync.h"
#include "sasd.h"
#include "slip_mode.h"
#include "diagnostics.h"
#include "wifi_setup.h"
#include "ftp_server.h"
#include "bitwarden_relay.h"
#include "caldav_relay.h"
#include <Adafruit_SSD1306.h>
#include <SD.h>
#include <WiFi.h>
#include <arduino-timer.h>

// dispOrientation_t (D_NORMAL/D_FLIPPED) is defined in main.cpp with no
// shared header - orientationLoop() (relocated here from modem.cpp) needs
// its own local copy of the enum to reference the symbolic names, same as
// modem.cpp itself used to carry locally. The underlying dispOrientation
// byte variable is separately declared extern below.
enum dispOrientation_t { D_NORMAL, D_FLIPPED };

// External global variables
extern Adafruit_SSD1306 display;
extern String baudDisp[];
extern String mainMenuDisp[];
extern String settingsMenuDisp[];
extern String orientationMenuDisp[];
extern int menuMode;
extern int menuIdx;
extern int menuCnt;
extern int lastMenu;
extern byte serialSpeed;
extern byte dispOrientation;
extern bool msgFlag;
extern int dispCharCnt;
extern int dispLineCnt;
extern bool ftpServerRunning;
extern const int bauds[];
extern bool usbDebug;
extern String build;
extern unsigned long ledTime;
extern bool quietMode;
extern bool verboseResults;
extern String resultCodes[];

#define BAUDS_COUNT 9

// WiFi and display graphics
extern const unsigned char wifi_symbol[];

// Menu mode constants and display modes are #defined in globals.h
// (MODE_MAIN, MODE_MODEM, etc. and MENU_BOTH, MENU_NUM, MENU_DISP)

String padLeft(String s, int len) {
  while (s.length()<len)
  {
    s = " " + s;
  }
  return s;
}

String padRight(String s, int len) {
  while (s.length()<len)
  {
    s = s + " ";
  }
  return s;
}

// Compact "X.Yk" thousands display for a LIVE baud rate (e.g. 9600 -> "9.6k",
// 57600 -> "57.6k", 300 -> "0.3k") - used by modes whose title needs the live
// PhysicalSerial.baudRate() (SASD, HotSync - both actively change their own
// UART rate mid-session) rather than the configured baudDisp[serialSpeed]
// string every ordinary menu's showHeader() shows. Deliberately uniform
// (always divide-by-1000, one decimal, always "k") rather than replicating
// baudDisp[]'s own inconsistent mixed style (plain "9600" but "57.6k") -
// shorter and predictable at every rate.
String liveBaudStr(uint32_t baud) {
  return String(baud / 1000.0, 1) + "k";
}

void mainMenu(bool arrow) {
  menuMode=MODE_MAIN;
  // Internal menu name stays "MAIN" (drives the serial "-=-=- MAIN MENU
  // -=-=-" banner) - header shows "Main Menu" instead of "WiRSa MAIN",
  // freeing header room for the WiFi status icon.
  showMenu("MAIN", mainMenuDisp, mainMenuCount, (arrow?MENU_DISP:MENU_BOTH), 0, false, "Main Menu");
  // showHeader() (called by showMenu() above) paints the WiFi status icon
  // itself as part of every header redraw - no extra call needed here.
}

//returns true/false depending on if default menu item should be set
bool showHeader(String menu, int dispMode, bool showWiRSaPrefix, String displayTitle) {
  bool setDef = false;

  if (dispMode!=MENU_DISP) {
    SerialPrintLn();
    SerialPrintLn();
    SerialPrintLn("-=-=- " + menu + " MENU -=-=-");
  }

  //21 chars wide x 8 lines);
  if (lastMenu != menuMode || msgFlag)
  { //clear the whole display, reset the selction
    msgFlag=false;
    display.clearDisplay();
    menuIdx=0;
    setDef=true;
    lastMenu=menuMode;
    display.drawLine(0,0,0,63,SSD1306_WHITE);
    display.drawLine(127,0,127,63,SSD1306_WHITE);
    display.drawLine(0,63,127,63,SSD1306_WHITE);
    display.drawLine(0,16,127,16,SSD1306_WHITE);
  } else {
    //just clear the selection area
    display.fillRect(6, 17, 8, 46, SSD1306_BLACK);
  }

  //display.fillRect(0, 0, 127, 9, SSD1306_WHITE);
  display.fillRect(0, 0, 127, 16, SSD1306_WHITE); //fill in the yellow header area

  display.drawLine(1, 1, 126, 1, SSD1306_BLACK);  //add an outline to fill this
  display.drawLine(1, 14, 126, 14, SSD1306_BLACK); //somewhat annoyingly large area
  display.drawLine(1, 1, 1, 14, SSD1306_BLACK);
  display.drawLine(126, 1, 126, 14, SSD1306_BLACK);


  //display.drawLine(0,0,0,63,SSD1306_WHITE);
    //display.setFont(&FreeMono9pt7b);
  display.setCursor(4, 4);     // Start at top-left corner
  display.setTextColor(SSD1306_BLACK); // Draw white text
  // displayTitle (optional) lets a screen show a friendlier/shorter header
  // string than its own internal menu name (e.g. diagnosticsMenu() keeps its
  // "DIAG" menu/serial-banner name but shows "Utilities" here) without
  // touching menu, which still drives the serial "-=-=- X MENU -=-=-"
  // banner above and must stay whatever the caller's real menu name is.
  String titleText = displayTitle.length() ? displayTitle : menu;
  String line = showWiRSaPrefix ? ("WiRSa " + titleText) : titleText;
  display.print(line);
  String bps = baudDisp[serialSpeed];

  display.print(padLeft(bps,20-line.length()));
  display.setTextColor(SSD1306_WHITE); // Draw white text
  //display.setCursor(0, 13);     // Start at top-left corner
  display.setCursor(0, 17);     // Start at top-left corner
  //display.setFont();
  display.display();

  // Every screen that goes through showHeader() gets the WiFi status icon
  // here, in the same slot/geometry HotSync/SASD/SLIP/BitWarden's own
  // custom headers each call this directly for.
  drawWifiStatusIcon();

  return setDef;
}

void showMessage(String message) {
  //display.fillRect(1, 10, 125, 53, SSD1306_BLACK);
  //display.drawRect(15, 12, 97, 40, SSD1306_WHITE);
  //display.fillRect(17, 14, 93, 36, SSD1306_WHITE);
  display.fillRect(1, 17, 125, 44, SSD1306_BLACK);
  display.drawRect(15, 18, 97, 43, SSD1306_WHITE);
  display.fillRect(17, 20, 93, 39, SSD1306_WHITE);

  int x = 19;
  //int y = 16;
  int y = 21;
  int c= 0;
  display.setCursor(x,y);
  display.setTextColor(SSD1306_BLACK);

  for (int i = 0; i < message.length(); i++) {
    if (message[i] == '\n') {
      x = 19; y += 8; c = 0;
      display.setCursor(x, y);
    } else if (c == 15) {
      x = 19; y += 8; c = 0;
      display.setCursor(x, y);
      display.write(message[i]);
    } else {
      display.write(message[i]);
      c++;
    }
  }
  display.setTextColor(SSD1306_WHITE);
  display.display();
  msgFlag=true;
}

void showMenu(String menuName, String options[], int sz, int dispMode, int defaultSel, bool showWiRSaPrefix, String displayTitle) {
  if (showHeader(menuName, dispMode, showWiRSaPrefix, displayTitle))
    menuIdx = defaultSel;

  menuCnt = sz;

  int menuStart=0;
  int menuEnd=0;

  int maxItems=5; //can only show 5 menu items per page

  if (menuCnt<=maxItems)
    menuEnd=menuCnt;
  else {
    //clear out menu area
    display.fillRect(1, 17, 126, 46, SSD1306_BLACK);

    menuStart=menuIdx;
    if (menuIdx<maxItems)
      menuStart=0;
    else
      menuStart=menuIdx-(maxItems-1);

    menuEnd=menuStart+maxItems;
    if (menuEnd>menuCnt)
      menuEnd=menuCnt;
  }

  display.setCursor(1, 19);

  for (int i=menuStart;i<menuEnd;i++) {
    menuOption(i, options[i]);
  }

  if (dispMode!=MENU_DISP) {
    int c = 0;
    for (int i=0;i<menuCnt;i++) {
      if (dispMode==MENU_NUM)
      {
        if (c==2) {
          SerialPrintLn(" [" + String((char)(65+i)) + "] " + options[i]);
          c=0;
        }
        else {
          SerialPrint(" [" + String((char)(65+i)) + "] " + padRight(options[i], 15));
          c++;
        }
      }
      else if (options[i].indexOf('[') >= 0)
        // Already has an explicit "[X]" shortcut hint authored inline (e.g.
        // mainMenuDisp's "SAS[D] SDCard", where the real shortcut isn't the
        // first letter) - print as-is instead of auto-bracketing below.
        SerialPrintLn(" " + options[i]);
      else //MENU_BOTH
        SerialPrintLn(" [" + options[i].substring(0,1) + "]" + options[i].substring(1));
    }
  }

  if (menuStart>0) {
    display.setCursor(120, 19);
    display.write(0x1E);
  }

  if (menuEnd<menuCnt) {
    display.setCursor(120, 54);
    display.write(0x1F);
  }

  display.display();

  if (dispMode!=MENU_DISP) {
    SerialPrintLn();
    SerialPrint("> ");
  }
}

void menuOption(int idx, String item) {
  display.print(" ");
  if (menuIdx==idx)
    display.write(16);
  else
    display.print(" ");
  display.print(" ");
  // Some menu items (e.g. mainMenuDisp's "SAS[D] SDCard") embed an explicit
  // "[X]" shortcut-key hint for the serial menu's benefit (see showMenu's
  // own handling of this) - the OLED gets the same underlying string but
  // without the bracket clutter, since it's navigated by buttons not
  // keystrokes.
  item.replace("[", "");
  item.replace("]", "");
  display.println(item.substring(0,17));
}

void settingsMenu(bool arrow) {
  menuMode = MODE_SETTINGS;
  // showWiRSaPrefix=false: header reads just "CONFIG" instead of "WiRSa
  // CONFIG", freeing up room for the WiFi status icon in the header.
  showMenu("CONFIG", settingsMenuDisp, settingsMenuCount, (arrow?MENU_DISP:MENU_BOTH), 0, false);
}

void orientationMenu(bool arrow) {
  menuMode = MODE_ORIENTATION;
  showMenu("DISPLAY", orientationMenuDisp, 2, (arrow?MENU_DISP:MENU_BOTH), dispOrientation);
}

void baudMenu(bool arrow) {
  menuMode = MODE_SETBAUD;
  showMenu("BAUD RATE", baudDisp, 9, (arrow?MENU_DISP:MENU_NUM), serialSpeed);
}

// "Start"/"Start (AP Mode)"/"Stop"/"Status" all share the first letter 'S' -
// showMenu()'s MENU_BOTH serial hint auto-brackets the literal first letter
// of any item without an explicit "[X]" already embedded, which would
// otherwise print three misleading "[S]..." hints for keys that don't
// actually do that action. Same inline-bracket convention mainMenuDisp's own
// "SAS[D] SDCard" already uses for exactly this reason. The bracketed letter
// itself is lowercase for "Stop"/"Status" (matches how those letters are
// naturally cased mid-word - an earlier uppercase choice looked fine in the
// bracketed serial view but left a stray capital, e.g. "StatUs", once
// menuOption() strips the brackets for the OLED). "Start"/"AP Mode" keep
// their capitals since those ARE naturally uppercase there (sentence-start
// and the "AP" acronym respectively) - case doesn't affect the key match
// either way (chr=='t'||chr=='T', etc.), purely cosmetic.
String ftpServerMenuDisp[] = { "Main Menu", "[S]tart", "Start ([A]P Mode)", "S[t]op", "Stat[u]s" };

void ftpServerMenu(bool arrow) {
  menuMode = MODE_FTPSERVER;
  ftpServerMenuDisp[4] = String("Stat[u]s: ") + (ftpServerRunning ? "Started" : "Stopped");
  showMenu("FTP Server", ftpServerMenuDisp, 5, (arrow?MENU_DISP:MENU_BOTH), 0, false);
}

void readSwitches()
{ // These digital pins are pulled HIGH, pushed=LOW
  // Restored from original WiRSa.ino for proper button debouncing
  BTNUP = !digitalRead(SW1_PIN);
  BTNBK = !digitalRead(SW2_PIN);
  BTNEN = !digitalRead(SW3_PIN);
  BTNDN = !digitalRead(SW4_PIN);
}

void readBackSwitch()
{ // Only read BACK button when in active connection (modem mode online)
  // Prevents interference with connection. All other buttons are disabled.
  BTNDN = LOW;
  BTNBK = !digitalRead(SW2_PIN);
  BTNEN = LOW;
  BTNUP = LOW;
}

void waitSwitches()
{
  //wait for all switches to be released
  for (int i=0;i<10;i++) {
    while(BTNDN||BTNBK||BTNEN||BTNUP) {
      delay(10);
      readSwitches();
    }
  }
}

void waitBackSwitch()
{
  //wait for all switches to be released
  for (int i=0;i<10;i++) {
    while(BTNDN||BTNBK||BTNEN||BTNUP) {
      delay(10);
      readBackSwitch();
    }
  }
}

// ============================================================================
// Relocated from modem.cpp during MODEM mode removal - none of the below are
// modem-specific. They're the general input-handling loops for MODE_MAIN,
// Baud Rate, Orientation, and the Config menu itself, plus getLine()/prompt()/
// led_on() which slip_mode.cpp also depends on via extern declarations.
// Consolidated here alongside their existing *Menu() counterparts, which
// already lived in this file. (fileLoop/protocolLoop/listFilesLoop - the
// Utilities/file-transfer loops this comment used to also cover - were
// removed along with the rest of file_transfer.cpp/.h, confirmed
// unreachable from the main menu.)
// ============================================================================

// Relocated from modem.cpp - genuinely shared with SASD and SLIP Gateway
// (both send AT-style OK/ERROR result codes over serial), not modem-only
// despite living there originally. Simplified: modem.cpp's own R_CONNECT/
// R_NOCARRIER special-casing (which needed connectTimeString(), a real
// telnet-session concept) is dropped since no surviving caller ever passes
// those codes - both SASD and SLIP Gateway only ever call sendResult(R_OK_STAT).
void sendResult(int resultCode) {
  if (quietMode) return; // ATQ1-style suppression
  SerialPrint("\r\n");
  if (verboseResults == 0) {
    SerialPrintLn(resultCode);
    return;
  }
  SerialPrint(String(resultCodes[resultCode]));
  SerialPrint("\r\n");
}

// Relocated from file_transfer.cpp during its removal (XMODEM/YMODEM/ZMODEM
// file transfer, confirmed unreachable from the main menu) - both are still
// genuinely used by getLine()/prompt() below, which SLIP Gateway's config
// prompts (slip_mode.cpp) depend on.
bool checkCancel() {
  readBackSwitch();
  bool cncl = BTNBK;
  waitBackSwitch();
  return cncl;
}

void clearInputBuffer()
{
  while (SerialAvailable() > 0)
  {
    char discard = SerialRead();
  }
}

String getLine()
{
  String line="";
  char c;
  while (true)
  {
    if (checkCancel())
      return "";
    if (SerialAvailable() > 0)
    {
      c = SerialRead();
      if (c==13)
        return line;
      else if (c==8 || c==127) {
        if (line.length()>0) {
          SerialPrint(c);
          line = line.substring(0, line.length()-1);
        }
      }
      else
      {
        SerialPrint(c);
        line += c;
      }
    }
  }
}

// Prompt user for input with optional default value
String prompt(String msg, String dflt)
{
  SerialPrintLn("");
  SerialPrintLn("");
  SerialPrint(msg);
  if (dflt!="")
    SerialPrint("[" + dflt + "] ");
  showMessage("{SERIAL}\n"+msg);
  String resp = getLine();
  if (resp=="")
    resp = dflt;
  clearInputBuffer();
  return resp;
}

// Turn on the LED and store the time
void led_on()
{
  digitalWrite(LED_PIN, !digitalRead(LED_PIN));
  ledTime = millis();
}

void printSDCardInfo() {
  Serial.print("Card type: ");
  uint8_t cardType = SD.cardType();
  if (cardType == CARD_MMC)
      Serial.println("MMC");
  else if (cardType == CARD_SD)
      Serial.println("SDSC");
  else if (cardType == CARD_SDHC)
      Serial.println("SDHC");
  else
      Serial.println("UNKNOWN");

  Serial.print("Card size: ");
  Serial.print(SD.cardSize() / (1024 * 1024));
  Serial.println(" MB");

  Serial.print("Total space: ");
  Serial.print(SD.totalBytes() / (1024 * 1024));
  Serial.println(" MB");

  Serial.print("Used space: ");
  Serial.print(SD.usedBytes() / (1024 * 1024));
  Serial.println(" MB");
}

void mainLoop()
{
    int menuSel=-1;
    readSwitches();
    if (BTNUP) {  //UP
        menuIdx--;
        if (menuIdx<0)
          menuIdx=menuCnt-1;
        mainMenu(true);
    }
    else if (BTNDN) { //DOWN
      menuIdx++;
      if (menuIdx>(menuCnt-1))
        menuIdx=0;
      mainMenu(true);
    }
    else if (BTNEN) { //ENTER
      menuSel = menuIdx;
    }
    else if (BTNBK) { //BACK
    }
    waitSwitches();

    int serAvl = SerialAvailable();
    char chr;
    if (serAvl>0)
      chr = SerialRead();

    if (serAvl>0 || menuSel>-1)
    {
      // Each branch (both the char shortcut AND the menuSel index check) is
      // wrapped in the SAME WIRSA_FEATURE_* guard as its menu entry
      // (buildMainMenu(), main.cpp) - a disabled feature's keyboard
      // shortcut has to disappear along with its menu entry, not just be
      // hidden from view, or it'd still be reachable by typing the letter
      // over the USB serial console. mmIdx* (not a literal position) is
      // what actually still works once some OTHER feature ahead of this one
      // in the list is compiled out and every later index shifts down.
#if WIRSA_FEATURE_FTPSERVER
      if (chr=='F'||chr=='f'||menuSel==mmIdxFtp) //FTP Server
      {
        SerialPrint(chr); //echo it back if it was a valid entry
        ftpServerMenu(false);
      }
      else
#endif
#if WIRSA_FEATURE_HOTSYNC
      if (chr=='H'||chr=='h'||menuSel==mmIdxHotsync)   // HotSync Install (repurposed slot - see globals.h)
      {
        SerialPrint(chr); //echo it back if it was a valid entry
        SerialPrintLn("");
        SerialPrintLn("Initializing SD card...");
        if (!SD.begin(SD_CS, SPI, 20000000)) {
          SerialPrintLn("H) Initialization failed! Please check that SD card is inserted and formatted as FAT16 or FAT32.");
          showMessage("\nPLEASE INSERT\n   SD CARD");
          return;
        }
        SerialPrintLn("Initialization Complete.");
        printSDCardInfo();
        enterHotsyncMode();
      }
      else
#endif
#if WIRSA_FEATURE_SASD
      if (chr=='D'||chr=='d'||menuSel==mmIdxSasd) // SASD (serial-attached SD)
      {
        SerialPrint(chr); //echo it back if it was a valid entry
        SerialPrintLn("");
        SerialPrintLn("Initializing SD card...");
        if (!SD.begin(SD_CS, SPI, 20000000)) {
          SerialPrintLn("D) Initialization failed! Please check that SD card is inserted and formatted as FAT16 or FAT32.");
          showMessage("\nPLEASE INSERT\n   SD CARD");
          return;
        }
        SerialPrintLn("Initialization Complete.");
        printSDCardInfo();
        enterSasdMode();
      }
      else
#endif
#if WIRSA_FEATURE_SLIP
      if (chr=='S'||chr=='s'||menuSel==mmIdxSlip) // SLIP Gateway
      {
        SerialPrint(chr); //echo it back if it was a valid entry
        // Starts the gateway immediately instead of showing the SLIP
        // submenu first - slipActiveLoop()'s own BACK handler drops back to
        // slipMenu(false) (the SLIP submenu, not all the way to Main).
        enterSlipMode();
      }
      else
#endif
#if WIRSA_FEATURE_BITWARDEN
      if (chr=='B'||chr=='b'||menuSel==mmIdxBitwarden) // BitWarden Relay
      {
        SerialPrint(chr); //echo it back if it was a valid entry
        enterBitwardenRelayMode();
      }
      else
#endif
#if WIRSA_FEATURE_CALDAV
      // 'A' not 'C'/'D' - both already taken (Config, SASD); mainMenuDisp's
      // own label below carries the "[A]" hint the same way "SAS[D] SDCard"
      // already does for a non-first-letter hotkey.
      if (chr=='A'||chr=='a'||menuSel==mmIdxCalDav) // CalDAV Relay
      {
        SerialPrint(chr); //echo it back if it was a valid entry
        enterCalDavRelayMode();
      }
      else
#endif
#if WIRSA_FEATURE_UTILITIES
      if (chr=='U'||chr=='u'||menuSel==mmIdxUtilities) // Utilities
      {
        SerialPrint(chr); //echo it back if it was a valid entry
        diagnosticsInit();
        diagnosticsMenu(false);
      }
      else
#endif
      if (chr=='C'||chr=='c'||menuSel==mmIdxConfig) // Config - always built, never gated
      {
        SerialPrint(chr); //echo it back if it was a valid entry
        settingsMenu(false);
      }
      else if (chr=='\r'||chr=='\n'||chr=='?')
      {
        SerialPrint(chr); //echo it back if it was a valid entry
        mainMenu(false);
      }
      else if (serAvl>0)
      { //redisplay the menu
      }
    }
}

// Baud rate selection loop
void baudLoop()
{
  int menuSel=-1;
  readSwitches();
  if (BTNUP) {  //UP
      menuIdx--;
      if (menuIdx<0)
        menuIdx=menuCnt-1;
      baudMenu(true);
  }
  else if (BTNDN) { //DOWN
    menuIdx++;
    if (menuIdx>(menuCnt-1))
      menuIdx=0;
    baudMenu(true);
  }
  else if (BTNEN) { //ENTER
    menuSel = menuIdx;
  }
  else if (BTNBK) { //BACK
    settingsMenu(false);
  }
  waitSwitches();

  int serAvl = SerialAvailable();
  char chr;
  if (serAvl>0) {
    chr = SerialRead();
    SerialPrint(chr);
  }

  if (menuSel>-1)
  {
    serialSpeed = menuIdx;
    writeSettings();
    PhysicalSerial.end();
    delay(200);
    PhysicalSerial.begin(bauds[serialSpeed], WIRSA_SERIAL_FORMAT);
    settingsMenu(false);
  } else if (serAvl>0) {
    //between A-I                                or a-i
    if (chr>=65 && chr <= 65+BAUDS_COUNT-1 || chr>=97 && chr <= 97+BAUDS_COUNT-1)
    {
      if (chr>=97)
        chr -= 32; //convert to uppercase
      serialSpeed = chr-65;
      writeSettings();
      PhysicalSerial.end();
      delay(200);
      PhysicalSerial.begin(bauds[serialSpeed], WIRSA_SERIAL_FORMAT);
      settingsMenu(false);
    } else
      baudMenu(false);
  }
}

// Orientation selection loop
void orientationLoop()
{
  int menuSel=-1;
  readSwitches();
  if (BTNUP) {  //UP
      menuIdx--;
      if (menuIdx<0)
        menuIdx=menuCnt-1;
      orientationMenu(true);
  }
  else if (BTNDN) { //DOWN
    menuIdx++;
    if (menuIdx>(menuCnt-1))
      menuIdx=0;
    orientationMenu(true);
  }
  else if (BTNEN) { //ENTER
    menuSel = menuIdx;
  }
  else if (BTNBK) { //BACK
    settingsMenu(false);
  }
  waitSwitches();

  int serAvl = SerialAvailable();
  char chr;
  if (serAvl>0) {
    chr = SerialRead();
    SerialPrint(chr);
  }

  if (serAvl>0 || menuSel>-1)
  {
    if (chr=='N'||chr=='n')
      menuSel=0;
    else if (chr=='F'||chr=='f')
      menuSel=1;

    if (menuSel>-1)
    {
      dispOrientation = menuSel;
      writeSettings();
      if (dispOrientation==D_NORMAL)
        display.setRotation(0);
      else
        display.setRotation(2);
      settingsMenu(false);
    }
    else if (serAvl>0)
    {
      orientationMenu(false);
    }
  }
}

// Generic blocking character-entry primitive, generalized from
// wifi_setup.cpp's wifiPasswordMenu()/wifiPasswordLoop() mechanism (same
// cycle/commit/typed-serial idea) so FTP username/password and AP
// SSID/password can be entered the same way as WiFi join credentials, via
// either the OLED+buttons or typed serial. Returns true if the user accepted
// (long-press EN, or Enter over serial), false if they backed out (BK on an
// empty buffer). buf must be a caller-owned buffer of at least maxLen+1
// bytes, pre-populated with the current value (may be empty).
static const char CHARENTRY_SET[] = " !\"#$%&'()*+,-./0123456789:;<=>?@ABCDEFGHIJKLMNOPQRSTUVWXYZ[\\]^_`abcdefghijklmnopqrstuvwxyz{|}~";
static const int CHARENTRY_SET_LEN = sizeof(CHARENTRY_SET) - 1;

static void charEntryDraw(const char *title, char *buf, char currentChar) {
  display.fillRect(1, 17, 125, 46, SSD1306_BLACK);
  display.setCursor(3, 19);
  display.setTextColor(SSD1306_WHITE);
  display.print(title);
  display.drawLine(1, 28, 126, 28, SSD1306_WHITE);
  display.setCursor(3, 31);
  String disp = String(buf);
  int startPos = 0;
  if (disp.length() > 16) {
    startPos = disp.length() - 16;
    display.print("<");
  }
  display.print(disp.substring(startPos));
  // Plain cursor only - the OLED-cycle "next char" preview used to be
  // appended right here (currentChar + "_"), which looked like a stray
  // leftover letter stuck on screen for anyone typing over serial instead
  // (currentChar defaults to 'A' and is never touched by the serial-typing
  // path, so it always showed as a bogus trailing "A" nobody typed). Moved
  // to its own labeled spot on the instructions line below instead, where
  // it can't be mistaken for entered text.
  display.print("_");
  display.setCursor(3, 49);
  display.print("UD:(");
  display.print(currentChar);
  display.print(") EN:add BK:del");
  display.setCursor(3, 57);
  display.print("Hold EN/BK: OK/Cancel");
  display.display();
}

bool charEntryPrompt(const char *title, char *buf, int maxLen) {
  int pos = strlen(buf);
  char currentChar = 'A';
  unsigned long enDownSince = 0;
  unsigned long bkDownSince = 0;
  bool accepted = false;

  // Serial had no announcement at all before this - the OLED showed the
  // prompt/title, but a serial-only user got no indication of what they
  // were about to type.
  SerialPrintLn("");
  SerialPrintLn(title);
  SerialPrint("> ");
  SerialPrint(buf);

  charEntryDraw(title, buf, currentChar);

  while (true) {
    // This is a self-contained blocking loop (same family as this
    // codebase's existing getLine()/prompt() helpers) that never returns
    // control to loop(), so ftpServerPump() would otherwise never run for
    // however long a human takes to cycle through characters here - likely
    // much longer than any existing serial-typed prompt() use, since this
    // one also supports slow OLED button-cycling. Pump explicitly so the
    // background FTP server keeps serving while the user edits its own
    // credentials (a realistic thing to do while it's already running).
    ftpServerPump();
    readSwitches();

    if (BTNEN) {
      if (enDownSince == 0) enDownSince = millis();
      if (millis() - enDownSince > 800) {
        accepted = true;
        break;
      }
    } else {
      if (enDownSince != 0 && millis() - enDownSince <= 800) {
        // short press: commit current char
        if (pos < maxLen) {
          buf[pos++] = currentChar;
          buf[pos] = '\0';
          charEntryDraw(title, buf, currentChar);
        }
      }
      enDownSince = 0;
    }

    // Long-press BK cancels immediately regardless of buffer content -
    // without this, editing a pre-populated field (e.g. an existing saved
    // FTP username) would require backspacing through the whole existing
    // value one character at a time before a quick tap finally cancels,
    // since a short BK only deletes one char at a time. The original
    // WiFi-join password entry this was generalized from never hit this
    // case (always starts from an empty buffer), so it never needed this.
    if (BTNBK) {
      if (bkDownSince == 0) bkDownSince = millis();
      if (millis() - bkDownSince > 800) {
        accepted = false;
        break;
      }
    } else {
      if (bkDownSince != 0 && millis() - bkDownSince <= 800) {
        // short press: backspace one char, or cancel if already empty
        if (pos > 0) {
          buf[--pos] = '\0';
          charEntryDraw(title, buf, currentChar);
        } else {
          accepted = false;
          break;
        }
      }
      bkDownSince = 0;
    }

    if (BTNUP) {
      const char *p = strchr(CHARENTRY_SET, currentChar);
      int idx = p ? (int)(p - CHARENTRY_SET) : 0;
      idx = (idx - 1 + CHARENTRY_SET_LEN) % CHARENTRY_SET_LEN;
      currentChar = CHARENTRY_SET[idx];
      charEntryDraw(title, buf, currentChar);
      delay(150);
    } else if (BTNDN) {
      const char *p = strchr(CHARENTRY_SET, currentChar);
      int idx = p ? (int)(p - CHARENTRY_SET) : 0;
      idx = (idx + 1) % CHARENTRY_SET_LEN;
      currentChar = CHARENTRY_SET[idx];
      charEntryDraw(title, buf, currentChar);
      delay(150);
    }

    // Typed-string entry over either serial port, same as WiFi password entry
    if (SerialAvailable() > 0) {
      char c = SerialRead();
      if (c == 13) { // Enter accepts
        accepted = true;
        break;
      } else if (c == 8 || c == 127) {
        if (pos > 0) { buf[--pos] = '\0'; SerialPrint(c); }
      } else if (c >= 32 && c < 127) {
        if (pos < maxLen) { buf[pos++] = c; buf[pos] = '\0'; SerialPrint(c); }
      }
      charEntryDraw(title, buf, currentChar);
    }
  }

  waitSwitches();
  return accepted;
}

// Settings (Config) menu loop
void settingsLoop()
{
  int menuSel=-1;
  readSwitches();
  if (BTNUP) {  //UP
      menuIdx--;
      if (menuIdx<0)
        menuIdx=menuCnt-1;
      settingsMenu(true);
  } else if (BTNDN) { //DOWN
    menuIdx++;
    if (menuIdx>(menuCnt-1))
      menuIdx=0;
    settingsMenu(true);
  } else if (BTNEN) { //ENTER
    menuSel = menuIdx;
  } else if (BTNBK) { //BACK
    mainMenu(false);
  }
  waitSwitches();

  int serAvl = SerialAvailable();
  char chr;
  if (serAvl>0) {
    chr = SerialRead();
    SerialPrint(chr);
  }

  if (serAvl>0 || menuSel>-1)
  {
    if (chr=='M'||chr=='m'||menuSel==smIdxMain) //main menu
    {
      mainMenu(false);
    }
    else if (chr=='W'||chr=='w'||menuSel==smIdxWifiSetup) // WiFi Setup
    {
      wifiSetupInit();
      wifiSetupLoop();
    }
#if WIRSA_FEATURE_FTPSERVER
    else if (chr=='T'||chr=='t'||menuSel==smIdxFtpSetup) // FTP Setup (T since F is taken by Factory Reset)
    {
      char userBuf[FTP_USER_MAX+1];
      char passBuf[FTP_PASS_MAX+1];
      strncpy(userBuf, ftpUser.c_str(), sizeof(userBuf)-1); userBuf[sizeof(userBuf)-1] = '\0';
      strncpy(passBuf, ftpPass.c_str(), sizeof(passBuf)-1); passBuf[sizeof(passBuf)-1] = '\0';
      if (charEntryPrompt("FTP Username\n(blank=anonymous)", userBuf, FTP_USER_MAX)) {
        if (charEntryPrompt("FTP Password", passBuf, FTP_PASS_MAX)) {
          ftpUser = String(userBuf);
          ftpPass = String(passBuf);
          writeSettings();
        }
      }
      settingsMenu(false);
    }
    else if (chr=='P'||chr=='p'||menuSel==smIdxApSetup) // AP Setup (P for "aP") - only used by FTP Server's own AP-mode fallback, gated with it
    {
      char ssidBuf[AP_SSID_MAX+1];
      char passBuf[AP_PASS_MAX+1];
      strncpy(ssidBuf, apSsid.c_str(), sizeof(ssidBuf)-1); ssidBuf[sizeof(ssidBuf)-1] = '\0';
      strncpy(passBuf, apPass.c_str(), sizeof(passBuf)-1); passBuf[sizeof(passBuf)-1] = '\0';
      if (charEntryPrompt("AP SSID", ssidBuf, AP_SSID_MAX)) {
        if (charEntryPrompt("AP Password", passBuf, AP_PASS_MAX)) {
          apSsid = String(ssidBuf);
          apPass = String(passBuf);
          writeSettings();
        }
      }
      settingsMenu(false);
    }
#endif
    else if (chr=='B'||chr=='b'||menuSel==smIdxBaud)
    {
      baudMenu(false);
    }
    else if (chr=='O'||chr=='o'||menuSel==smIdxOrientation)
    {
      orientationMenu(false);
    }
    else if (chr=='U'||chr=='u'||menuSel==smIdxUsbDebug)
    {
      usbDebug = !usbDebug;
      writeSettings();
      SerialPrintLn(usbDebug ? "USB Debug: ENABLED" : "USB Debug: DISABLED");
      showMessage(usbDebug ? "USB Debug\n\nENABLED" : "USB Debug\n\nDISABLED");
      delay(1500);  // Pause so the message can be read on the display
      settingsMenu(false);
    }
    else if (chr=='F'||chr=='f'||menuSel==smIdxFactoryReset)
    {
       SerialPrintLn("** PLEASE WAIT: FACTORY RESET **");
       showMessage("***************\n* PLEASE WAIT *\n*FACTORY RESET*\n***************");
       defaultEEPROM();
       ESP.restart();
    }
    else if (chr=='R'||chr=='r'||menuSel==smIdxReboot)
    {
      SerialPrintLn("** PLEASE WAIT: REBOOTING **");
      showMessage("***************\n* PLEASE WAIT *\n*  REBOOTING  *\n***************");
      ESP.restart();
    }
    else if (chr=='A'||chr=='a'||menuSel==smIdxAbout)
    {
      SerialPrintLn("\n** WiRSa BUILD for Palm **");
      showMessage("***************\n* WiRSa BUILD *\n*  for Palm   *\n***************");
    }
    else if (serAvl>0)
    {
      settingsMenu(false);
    }
  }
}

// FTP Server submenu loop (Start / Start (AP Mode) / Stop / Status)
void ftpServerLoop()
{
  int menuSel=-1;
  readSwitches();
  if (BTNUP) {  //UP
      menuIdx--;
      if (menuIdx<0)
        menuIdx=menuCnt-1;
      ftpServerMenu(true);
  }
  else if (BTNDN) { //DOWN
    menuIdx++;
    if (menuIdx>(menuCnt-1))
      menuIdx=0;
    ftpServerMenu(true);
  }
  else if (BTNEN) { //ENTER
    menuSel = menuIdx;
  }
  else if (BTNBK) { //BACK
    mainMenu(false);
    return;
  }
  waitSwitches();

  int serAvl = SerialAvailable();
  char chr;
  if (serAvl>0) {
    chr = SerialRead();
    SerialPrint(chr);
  }

  if (serAvl>0 || menuSel>-1)
  {
    if (chr=='M'||chr=='m'||menuSel==0) //main menu
    {
      mainMenu(false);
    }
    else if (chr=='S'||chr=='s'||menuSel==1) // Start
    {
      ftpServerStart(false);
      // Force the full-clear redraw path: menuMode doesn't change (we're
      // already MODE_FTPSERVER), so showHeader()'s own lastMenu-unchanged
      // fast path would otherwise skip clearing before the Status line's
      // text is redrawn - menuOption() never clears its own row (relies on
      // the caller having done so), so an old and new string of different
      // pixel width would leave stale glyph fragments behind.
      msgFlag = true;
      ftpServerMenu(false);
    }
    else if (chr=='A'||chr=='a'||menuSel==2) // Start (AP Mode)
    {
      ftpServerStart(true);
      msgFlag = true;
      ftpServerMenu(false);
    }
    else if (chr=='T'||chr=='t'||menuSel==3) // Stop
    {
      ftpServerStop();
      msgFlag = true;
      ftpServerMenu(false);
    }
    else if (chr=='U'||chr=='u'||menuSel==4) // Status (informational only, no action)
    {
      ftpServerMenu(false);
    }
    else if (serAvl>0)
    {
      ftpServerMenu(false);
    }
  }
}
