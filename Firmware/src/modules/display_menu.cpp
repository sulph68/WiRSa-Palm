// Display rendering and menu navigation

#include "Arduino.h"
#include "display_menu.h"
#include "modes.h"
#include "globals.h"
#include "sd_mount.h"
#include "palm_ctl.h"   // menuKey()
#include "serial_io.h"
#include "network.h"
#include "led.h"
#include "settings.h"
#include "hotsync.h"
#include "sasd.h"
#include "vdisk.h"
#include "slip_mode.h"
#include "diagnostics.h"
#include "wifi_setup.h"
#include "ftp_server.h"
#include "bitwarden_relay.h"
#include "caldav_relay.h"
#include "mesh_relay.h"
#include <Adafruit_SSD1306.h>
#include <SD.h>
#include <WiFi.h>
#include <arduino-timer.h>

// dispOrientation_t (D_NORMAL/D_FLIPPED) is defined in main.cpp with no shared header; orientationLoop() needs a local copy of the enum for the names. The dispOrientation byte is declared extern below.
enum dispOrientation_t { D_NORMAL, D_FLIPPED };

// external globals
extern WirsaDisplay display;
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

#define BAUDS_COUNT 7

extern const unsigned char wifi_symbol[];

// Menu modes (MODE_MAIN, ...) and display modes (MENU_BOTH, MENU_NUM, MENU_DISP) are in globals.h

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

// Compact "X.Yk" for a LIVE baud (9600 -> "9.6k", 57600 -> "57.6k", 300 -> "0.3k"), for titles of modes that show PhysicalSerial.baudRate() (SASD, HotSync change their UART rate mid-session) rather
// than baudDisp[serialSpeed]. Always divide by 1000, one decimal, "k", instead of baudDisp[]'s mixed style ("9600" but "57.6k"): shorter and predictable.
String liveBaudStr(uint32_t baud) {
  return String(baud / 1000.0, 1) + "k";
}

void mainMenu(bool arrow) {
  menuMode=MODE_MAIN;
  ledRefresh();   // an RGB LED was dark in a Mesh session: its WiFi colour is back
  // The internal menu name stays "MAIN" (it drives the serial "-=-=- MAIN MENU -=-=-" banner); the header shows "Main Menu", freeing room for the WiFi status icon.
  showMenu("MAIN", mainMenuDisp, mainMenuCount, (arrow?MENU_DISP:MENU_BOTH), 0, false, "Main Menu");
  // showHeader() (called by showMenu() above) paints the WiFi status icon with every header redraw.
}

// true if the default menu item should be set
bool showHeader(String menu, int dispMode, bool showWiRSaPrefix, String displayTitle) {
  bool setDef = false;

  if (dispMode!=MENU_DISP) {
    SerialPrintLn();
    SerialPrintLn();
    SerialPrintLn("-=-=- " + menu + " MENU -=-=-");
  }

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
    // just clear the selection area
    display.fillRect(6, 17, 8, 46, SSD1306_BLACK);
  }

  display.fillRect(0, 0, 127, 16, SSD1306_WHITE); //fill in the yellow header area

  display.drawLine(1, 1, 126, 1, SSD1306_BLACK);  //add an outline to fill this
  display.drawLine(1, 14, 126, 14, SSD1306_BLACK); //somewhat annoyingly large area
  display.drawLine(1, 1, 1, 14, SSD1306_BLACK);
  display.drawLine(126, 1, 126, 14, SSD1306_BLACK);


  display.setCursor(4, 4);     // Start at top-left corner
  display.setTextColor(SSD1306_BLACK); // Draw white text
  // displayTitle (optional) shows a friendlier header than the menu's internal name (diagnosticsMenu() keeps "DIAG" for the serial banner but shows "Utilities") without touching menu, which
  // drives the serial "-=-=- X MENU -=-=-" banner and must stay the caller's real name.
  String titleText = displayTitle.length() ? displayTitle : menu;
  String line = showWiRSaPrefix ? ("WiRSa " + titleText) : titleText;
  display.print(line);
  // The rate the menu runs at now (menuBaud(), palm_ctl.h): 9600 after boot or the Palm's rate with the dual-rate menu, not the configured maximum. Shown in baudDisp[]'s style when it is a table rate.
  uint32_t now = menuBaud();
  String bps = liveBaudStr(now);
  for (int i = 0; i < 9; i++)
    if ((uint32_t) bauds[i] == now) bps = baudDisp[i];

  display.print(padLeft(bps,20-line.length()));
  display.setTextColor(SSD1306_WHITE); // Draw white text
  display.setCursor(0, 17);     // Start at top-left corner
  display.display();

  // Every showHeader() screen gets the WiFi status icon here, in the slot HotSync/SASD/SLIP/BitWarden's custom headers call directly.
  drawWifiStatusIcon();

  return setDef;
}

void showMessage(String message) {
  display.fillRect(1, 17, 125, 44, SSD1306_BLACK);
  display.drawRect(15, 18, 97, 43, SSD1306_WHITE);
  display.fillRect(17, 20, 93, 39, SSD1306_WHITE);

  int x = 19;
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
    // clear the menu area
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
        // Already has an explicit "[X]" shortcut hint (mainMenuDisp's "SAS[D] SDCard", where the key is not the first letter): print as is, without auto-bracketing below.
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
  // Some items embed an explicit "[X]" key hint for the serial menu (see showMenu); the OLED gets the string without the brackets, since it is navigated by buttons.
  item.replace("[", "");
  item.replace("]", "");
  display.println(item.substring(0,17));
}

void settingsMenu(bool arrow) {
  menuMode = MODE_SETTINGS;
  // showWiRSaPrefix=false: the header reads "CONFIG", not "WiRSa CONFIG", freeing room for the WiFi icon.
  showMenu("CONFIG", settingsMenuDisp, settingsMenuCount, (arrow?MENU_DISP:MENU_BOTH), 0, false);
}

void orientationMenu(bool arrow) {
  menuMode = MODE_ORIENTATION;
  showMenu("DISPLAY", orientationMenuDisp, 2, (arrow?MENU_DISP:MENU_BOTH), dispOrientation);
}

void baudMenu(bool arrow) {
  menuMode = MODE_SETBAUD;
#if WIRSA_FEATURE_PALMCTL && WIRSA_FEATURE_DUAL_BAUD
  showMenu("MAX BAUD RATE", baudDisp, BAUDS_COUNT, (arrow?MENU_DISP:MENU_NUM), serialSpeed);
#else
  showMenu("BAUD RATE", baudDisp, BAUDS_COUNT, (arrow?MENU_DISP:MENU_NUM), serialSpeed);
#endif
}

// "Start", "Start (AP Mode)", "Stop" and "Status" all begin with 'S', and showMenu()'s MENU_BOTH serial hint brackets the first letter of any item without an explicit "[X]", which would print
// three misleading "[S]..." hints. Same inline-bracket convention as "SAS[D] SDCard". The bracketed letter is lowercase for "Stop"/"Status" (naturally cased mid-word; an uppercase one left a stray capital, "StatUs",
// once menuOption() strips the brackets for the OLED); "Start"/"AP Mode" keep capitals. Case does not affect the key match (chr == 't' || 'T'), only looks.
String ftpServerMenuDisp[] = { "Main Menu", "[S]tart", "Start ([A]P Mode)", "S[t]op", "Stat[u]s" };

void ftpServerMenu(bool arrow) {
  menuMode = MODE_FTPSERVER;
  ftpServerMenuDisp[4] = String("Stat[u]s: ") + (ftpServerRunning ? "Started" : "Stopped");
  showMenu("FTP Server", ftpServerMenuDisp, 5, (arrow?MENU_DISP:MENU_BOTH), 0, false);
}

void readSwitches()
{ // These digital pins are pulled HIGH, pushed=LOW
  // restored from the original WiRSa.ino, for button debouncing
#if WIRSA_HAS_BUTTONS
  BTNUP = !digitalRead(SW1_PIN);
  BTNBK = !digitalRead(SW2_PIN);
  BTNEN = !digitalRead(SW3_PIN);
  BTNDN = !digitalRead(SW4_PIN);
#else
  // Screenless build: no button board, so the INPUT pins would float.
  BTNUP = BTNBK = BTNEN = BTNDN = false;
#endif
}

void waitSwitches()
{
  // wait for all switches to be released
  for (int i=0;i<10;i++) {
    while(BTNDN||BTNBK||BTNEN||BTNUP) {
      delay(10);
      readSwitches();
    }
  }
}

// Input loops for MODE_MAIN, Baud Rate, Orientation and the Config menu, plus led_on(), relocated from modem.cpp when MODEM mode was removed (none is modem-specific), next to their
// *Menu() counterparts.

// LED on, and remember when
void led_on()
{
  ledSet(!ledIsOn());
  ledTime = millis();
}

#if WIRSA_FEATURE_HOTSYNC
// HotSync Install from the main menu: the H key and the BOOT button (boot_button.cpp) start it here. Needs the SD card; false (and "PLEASE INSERT SD CARD") without one.
bool startHotsyncFromMenu()
{
  SerialPrintLn("");
  SerialPrintLn("Initializing the disk...");
  if (!hotsyncPrepareDisk()) {
    SerialPrintLn("H) Initialization failed! Please check that SD card is inserted and formatted as FAT16 or FAT32 (or set Disk to the virtual card).");
    Serial.printf("SD: CS %d SCK %d MOSI %d MISO %d, %lu Hz - check wiring/3.3V; build with -DCORE_DEBUG_LEVEL=1 to see the SD driver's reason\r\n",
                  (int) SD_CS, (int) SPI_SCK, (int) SPI_MOSI, (int) SPI_MISO, (unsigned long) WIRSA_SD_HZ);
    showMessage("\nPLEASE INSERT\n   SD CARD");
    return false;
  }
  SerialPrintLn("Initialization Complete.");
  enterHotsyncMode();
  return true;
}
#endif

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

#if WIRSA_FEATURE_SASD
// The D key (and the menu entry): the SASD disk setting (WiRSaCtl Setup) decides what is needed: the virtual card needs no SD card, auto falls back to it when none answers, the SD card setting insists on one.
void startSasdFromMenu()
{
  SerialPrintLn("");
  bool haveSd = false;
  if (sasdDiskMode != SASD_DISK_VIRTUAL) {
    SerialPrintLn("Initializing SD card...");
    haveSd = sdMount();
    if (!haveSd && (sasdDiskMode == SASD_DISK_SD || !vdiskPresent())) {
      SerialPrintLn("D) Initialization failed! Please check that SD card is inserted and formatted as FAT16 or FAT32.");
      showMessage("\nPLEASE INSERT\n   SD CARD");
      return;
    }
  }
  if (haveSd) {
    SerialPrintLn("Initialization Complete.");
    printSDCardInfo();
  }
  enterSasdMode();
}
#endif

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

    char chr = 0;
    int serAvl = menuKey(chr);   // burst-guarded Palm-line key, or a control frame (palm_ctl.h)

    if (serAvl>0 || menuSel>-1)
    {
      // The main menu's keys and entries come from the mode table (modes.h): a mode that is not built has no row, so its letter over the USB console does nothing either.
      if (Modes::startFromMenu(chr, menuSel)) {
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

  char chr = 0;
  int serAvl = menuKey(chr);   // burst-guarded Palm-line key, or a control frame (palm_ctl.h)
  if (serAvl>0) {
    SerialPrint(chr);
  }

  if (menuSel>-1)
  {
    serialSpeed = menuIdx;
    writeSettings();
    menuBaudReset();   // the menu's session rate, within the new maximum (palm_ctl.h)
    PhysicalSerial.end();
    delay(200);
    PhysicalSerial.begin(menuBaud(), WIRSA_SERIAL_FORMAT, RXD2, TXD2);   // the pins: without them UART2 takes its defaults, GPIO 19/20 on the S3 = the USB lines
    settingsMenu(false);
  } else if (serAvl>0) {
    // between A and E, or a and e
    if (chr>=65 && chr <= 65+BAUDS_COUNT-1 || chr>=97 && chr <= 97+BAUDS_COUNT-1)
    {
      if (chr>=97)
        chr -= 32; //convert to uppercase
      serialSpeed = chr-65;
      writeSettings();
      menuBaudReset();
      PhysicalSerial.end();
      delay(200);
      PhysicalSerial.begin(menuBaud(), WIRSA_SERIAL_FORMAT, RXD2, TXD2);   // the pins: without them UART2 takes its defaults, GPIO 19/20 on the S3 = the USB lines
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

  char chr = 0;
  int serAvl = menuKey(chr);   // burst-guarded Palm-line key, or a control frame (palm_ctl.h)
  if (serAvl>0) {
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

// A blocking character-entry primitive, generalised from wifi_setup.cpp's wifiPasswordMenu()/wifiPasswordLoop() (cycle, commit, typed serial) so the FTP username/password and AP SSID/password
// are entered like WiFi credentials, by OLED and buttons or typed serial. True if the user accepted (long-press EN, or Enter over serial), false if they backed out (BK on an empty buffer).
// buf is caller-owned, at least maxLen+1 bytes, pre-filled with the current value (may be empty).
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
  // A plain cursor only. The OLED-cycle "next char" preview used to be appended here (currentChar + "_"), which looked like a stray letter for anyone typing over serial (currentChar defaults to 'A'
  // and the serial path never touches it: a bogus trailing "A"). It has its own labelled spot on the instructions line below.
  display.print("_");
  display.setCursor(3, 49);
  display.print("UD:(");
  display.print(currentChar);
  display.print(") EN:add BK:del");
  display.setCursor(3, 57);
  display.print("Hold EN/BK: OK/Cancel");
  display.display();
}

bool charEntryPrompt(const char *title, char *buf, int maxLen, const char *charset) {
  // charset == NULL: the full printable set, cycling from 'A' (FTP/AP setup). Otherwise only those characters can be cycled to or typed, and cycling starts at the first.
  const char *set = charset ? charset : CHARENTRY_SET;
  const int setLen = charset ? (int) strlen(charset) : CHARENTRY_SET_LEN;
  int pos = strlen(buf);
  char currentChar = charset ? charset[0] : 'A';
  unsigned long enDownSince = 0;
  unsigned long bkDownSince = 0;
  bool accepted = false;

  // announce the field over serial too (the OLED showed the prompt, a serial-only user got nothing)
  SerialPrintLn("");
  SerialPrintLn(title);
  SerialPrint("> ");
  SerialPrint(buf);

  charEntryDraw(title, buf, currentChar);

  while (true) {
    // This blocking loop never returns to loop(), so ftpServerPump() would not run while a person cycles characters: pump explicitly so the FTP server keeps serving while they edit its credentials.
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
        // short press: commit the current char
        if (pos < maxLen) {
          buf[pos++] = currentChar;
          buf[pos] = '\0';
          charEntryDraw(title, buf, currentChar);
        }
      }
      enDownSince = 0;
    }

    // Long-press BK cancels at once whatever the buffer holds. Otherwise editing a pre-filled field (an existing FTP username) would need backspacing through the whole value before a tap cancels,
    // since a short BK deletes one char. The WiFi password entry this came from always starts empty, so never needed it.
    if (BTNBK) {
      if (bkDownSince == 0) bkDownSince = millis();
      if (millis() - bkDownSince > 800) {
        accepted = false;
        break;
      }
    } else {
      if (bkDownSince != 0 && millis() - bkDownSince <= 800) {
        // short press: backspace one char, or cancel if empty
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
      const char *p = strchr(set, currentChar);
      int idx = p ? (int)(p - set) : 0;
      idx = (idx - 1 + setLen) % setLen;
      currentChar = set[idx];
      charEntryDraw(title, buf, currentChar);
      delay(150);
    } else if (BTNDN) {
      const char *p = strchr(set, currentChar);
      int idx = p ? (int)(p - set) : 0;
      idx = (idx + 1) % setLen;
      currentChar = set[idx];
      charEntryDraw(title, buf, currentChar);
      delay(150);
    }

    // typed-string entry over either serial port, as WiFi password entry
    if (SerialAvailable() > 0) {
      char c = SerialRead();
      if (c == 13) { // Enter accepts
        accepted = true;
        break;
      } else if (c == 0x1B || c == 0x16) {
        // ESC cancels (the only way out without a BACK button). 0x16 starts a Palm control frame (palm_ctl.cpp): cancel instead of typing its bytes into the field (the menu's burst guard drains the rest).
        break;
      } else if (c == 8 || c == 127) {
        if (pos > 0) { buf[--pos] = '\0'; SerialPrint(c); }
      } else if (c >= 32 && c < 127 && (!charset || strchr(charset, c))) {
        if (pos < maxLen) { buf[pos++] = c; buf[pos] = '\0'; SerialPrint(c); }
      }
      charEntryDraw(title, buf, currentChar);
    }
  }

  waitSwitches();
  return accepted;
}

// One Exit Timers value (mode_timer.h), seconds 0-255, 0 = never.
static void editExitTimer(const char *title, uint8_t &value)
{
  char buf[4];
  snprintf(buf, sizeof(buf), "%u", (unsigned) value);
  if (charEntryPrompt(title, buf, 3, "0123456789")) {
    long v = atol(buf);
    if (v > 255) v = 255;
    value = (uint8_t) v;
  }
}

// Factory Reset always asks first. Buttons: ENTER confirms, BACK cancels. Typed (F): the next key must be Y, read through menuKey() so a burst of stray bytes cannot confirm. No answer in 15 s cancels.
static bool confirmFactoryReset(bool byButton)
{
  if (byButton) {
    showMessage("FACTORY RESET?\n\nENTER: Confirm\nBACK: Cancel");
    waitSwitches();
    while (true) {
      readSwitches();
      if (BTNEN) { waitSwitches(); return true; }
      if (BTNBK) { waitSwitches(); return false; }
      delay(50);
    }
  }
  SerialPrintLn("\r\nFactory reset? Press Y to confirm, any other key to cancel.");
  showMessage("FACTORY RESET?\n\nY: Confirm\nOther key: Cancel");
  unsigned long t0 = millis();
  while (millis() - t0 < 15000) {
    char k = 0;
    if (menuKey(k)) {
      SerialPrint(k);
      return k == 'Y' || k == 'y';
    }
    delay(10);
  }
  return false;
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

  char chr = 0;
  int serAvl = menuKey(chr);   // burst-guarded Palm-line key, or a control frame (palm_ctl.h)
  if (serAvl>0) {
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
    else if (chr=='P'||chr=='p'||menuSel==smIdxApSetup)  // AP Setup (P for "aP"), used only by FTP Server's AP-mode fallback, gated with it
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
#if WIRSA_FEATURE_SLIP
    else if (chr=='S'||chr=='s'||menuSel==smIdxSlipSetup) // SLIP Setup - gateway/client/DNS addresses
    {
      slipSetup();
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
      // off, warn, info, verbose, off
      logSetLevel(gLogLevel >= LOG_VERBOSE ? LOG_OFF : gLogLevel + 1);
      writeSettings();
      const char *lv = gLogLevel == LOG_OFF ? "OFF" : gLogLevel == LOG_WARN ? "WARN" : gLogLevel == LOG_INFO ? "INFO" : "VERBOSE";
      SerialPrintLn(String("USB Debug: ") + lv);
      showMessage(String("USB Debug\n\n") + lv);
      delay(1500);  // Pause so the message can be read on the display
      settingsMenu(false);
    }
    else if (chr=='E'||chr=='e'||menuSel==smIdxExitTimers)  // Exit Timers: when HotSync/CalDAV/Bitwarden go back to the main menu
    {
      editExitTimer("Unused mode exit\n(seconds, 0=never)", modeUnusedExitSec);
      editExitTimer("After session exit\n(seconds, 0=never)", modeAfterExitSec);
      {
        uint8_t m = slipTimeoutMin;
        editExitTimer("SLIP timeout (min)\n1, 2, 3 or 5", m);
        if (slipTimeoutValid(m)) slipTimeoutMin = m;
      }
      {
        uint8_t m = sasdTimeoutMin;
        editExitTimer("SASD timeout (min)\n3, 5, 10 or 15", m);
        if (sasdTimeoutValid(m)) sasdTimeoutMin = m;
      }
      writeSettings();
      SerialPrintLn("\r\nExit timers: unused " + String(modeUnusedExitSec) + " s, after session " + String(modeAfterExitSec) + " s, SLIP " + String(slipTimeoutMin) + " min");
      settingsMenu(false);
    }
    else if (chr=='F'||chr=='f'||menuSel==smIdxFactoryReset)
    {
      if (confirmFactoryReset(menuSel == smIdxFactoryReset)) {
        SerialPrintLn("** PLEASE WAIT: FACTORY RESET **");
        showMessage("***************\n* PLEASE WAIT *\n*FACTORY RESET*\n***************");
        defaultEEPROM();
        ESP.restart();
      }
      SerialPrintLn("\r\nFactory reset cancelled.");
      msgFlag = true;
      settingsMenu(false);
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

  char chr = 0;
  int serAvl = menuKey(chr);   // burst-guarded Palm-line key, or a control frame (palm_ctl.h)
  if (serAvl>0) {
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
      // Force the full-clear redraw: menuMode does not change (already MODE_FTPSERVER), so showHeader()'s lastMenu-unchanged fast path would skip clearing before the Status text is redrawn, and
      // menuOption() never clears its row, so an old and new string of different pixel width would leave stale glyph fragments.
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
