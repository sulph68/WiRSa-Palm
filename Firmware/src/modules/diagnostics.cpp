// Diagnostics: serial diagnostics and troubleshooting tools

#include "Arduino.h"
#include "diagnostics.h"
#include "globals.h"
#include "palm_ctl.h"   // menuKey()
#include "display_menu.h"
#include "serial_io.h"
#include "settings.h"
#include "network.h"
#include "battery.h"
#include "sd_format.h"
#include "sd_mount.h"
#include "sd_usage.h"
#include "vdisk.h"
#include "vdfs.h"
#include <WiFi.h>
#include <Adafruit_SSD1306.h>

extern WirsaDisplay display;
extern int menuMode;
extern int menuIdx;
extern int menuCnt;
extern int lastMenu;
extern byte serialSpeed;
extern const int bauds[];
extern HardwareSerial PhysicalSerial;
extern String build;
extern String ssid, password;
extern bool ftpServerRunning;   // main.cpp

extern bool BTNUP;
extern bool BTNDN;
extern bool BTNEN;
extern bool BTNBK;

// menu display array (defined in main.cpp)
extern String diagnosticsMenuDisp[];

DiagnosticsContext diagCtx;

void diagnosticsInit() {
  diagCtx.hexDumpEnabled = false;
}

// ---- Hex Dump Mode ----
// diagCtx.hexDumpEnabled has no writer left (setHexDumpMode was AT$HEX-only and went with the dead AT dispatcher), so it is permanently false and hexDumpByte's call sites (serial_io.cpp) are inert.
// Left in place rather than touch serial_io.cpp's RX/TX hot path in the same pass.

void hexDumpByte(char direction, uint8_t byte) {
  if (!diagCtx.hexDumpEnabled) return;

  // format: [T] 41 'A'  or  [R] 0D <CR>
  Serial.print("[");
  Serial.print(direction);
  Serial.print("] ");

  if (byte < 0x10) Serial.print("0");
  Serial.print(byte, HEX);
  Serial.print(" ");

  if (byte >= 0x20 && byte < 0x7F) {
    Serial.print("'");
    Serial.print((char)byte);
    Serial.println("'");
  } else if (byte == 0x0D) {
    Serial.println("<CR>");
  } else if (byte == 0x0A) {
    Serial.println("<LF>");
  } else if (byte == 0x1B) {
    Serial.println("<ESC>");
  } else if (byte == 0x08) {
    Serial.println("<BS>");
  } else if (byte == 0x09) {
    Serial.println("<TAB>");
  } else if (byte == 0x00) {
    Serial.println("<NUL>");
  } else if (byte == 0x7F) {
    Serial.println("<DEL>");
  } else {
    Serial.print("<0x");
    if (byte < 0x10) Serial.print("0");
    Serial.print(byte, HEX);
    Serial.println(">");
  }
}

// ---- Information screen (Utilities menu): a HotSync/BitWarden-style boxed header and a scrollable status area, reflowed into a line list sized for the OLED's status area. ----

#if defined(BATTERY_PIN)
#define INFO_MAX_LINES 15    // + the battery line
#else
#define INFO_MAX_LINES 14
#endif
#define INFO_VISIBLE_LINES 4  // status area is 47px tall / 10px per row

static String infoLines[INFO_MAX_LINES];
static int infoLineCount = 0;
static int infoScrollOffset = 0;

static void infoBuildLines() {
  infoLineCount = 0;
  infoLines[infoLineCount++] = "FW: " + build;
  infoLines[infoLineCount++] = "Heap: " + String(ESP.getFreeHeap() / 1024) + "/" + String(ESP.getHeapSize() / 1024) + "KB";
  infoLines[infoLineCount++] = "CPU: " + String(ESP.getCpuFreqMHz()) + "MHz";
  infoLines[infoLineCount++] = "Chip: " + String(ESP.getChipModel());
  infoLines[infoLineCount++] = "SDK: " + String(ESP.getSdkVersion());

  unsigned long uptime = millis() / 1000;
  int days = uptime / 86400;
  int hours = (uptime % 86400) / 3600;
  int mins = (uptime % 3600) / 60;
  int secs = uptime % 60;
  String up = days > 0 ? (String(days) + "d " + String(hours) + "h")
                        : (String(hours) + "h " + String(mins) + "m " + String(secs) + "s");
  infoLines[infoLineCount++] = "Up: " + up;
#if defined(BATTERY_PIN)
  infoLines[infoLineCount++] = batteryInfoLine();
#endif

  if (WiFi.status() == WL_CONNECTED) {
    infoLines[infoLineCount++] = "WiFi: " + WiFi.SSID();
    infoLines[infoLineCount++] = "RSSI: " + String(WiFi.RSSI()) + "dBm";
    infoLines[infoLineCount++] = "IP: " + WiFi.localIP().toString();
    infoLines[infoLineCount++] = "DNS: " + WiFi.dnsIP().toString();
  } else {
    infoLines[infoLineCount++] = "WiFi: Not connected";
  }

  infoLines[infoLineCount++] = "Baud: " + String(bauds[serialSpeed]) +
    (menuBaud() != (uint32_t) bauds[serialSpeed]   // dual-rate menu swapped (palm_ctl.h)
       ? " now " + String(menuBaud()) : String(""));   // "Baud: 115200 now 9600" = 21 chars

  // The disks: which one SASD and HotSync use (Config's Disk setting), then each card's used/total in the units of
  // WiRSaControl's SD row. The SD card's used space is counted from its FAT (sd_usage.h): give it up to 2 s here.
  infoLines[infoLineCount++] = String("Disk: ") +
    (sasdDiskMode == SASD_DISK_SD ? "SD card" : sasdDiskMode == SASD_DISK_VIRTUAL ? "Virtual" : "Auto");
  bool sdUp = SD.cardType() != CARD_NONE ? sdAlive() : sdMount();
  if (sdUp) {
    // The count only runs at the main menu (palm_ctl.cpp), so step it here: once to pick up a change, then until done.
    sdUsageStep();
    for (unsigned long t0 = millis(); millis() - t0 < 2000 && sdUsageText().startsWith("?"); ) sdUsageStep();
    String u = sdUsageText();
    infoLines[infoLineCount++] = "SD: " + (u.length() ? u : String("present"));   // "SD: 24.016MB/921.0MB" = 20
  } else {
    infoLines[infoLineCount++] = "SD: Not present";
  }
  if (vdiskPresent()) {
    uint64_t used = 0, total = 0;
    bool ok = vdiskUsage(&used, &total);
    if (!vdfsMounted()) vdiskClose();   // HotSync or the FTP server may hold it open
    infoLines[infoLineCount++] = "V. SD: " + (ok ? sdHumanBytes(used, 1) + "/" + sdHumanBytes(total, 1)
                                                  : String("not formatted"));   // "V. SD: 515.0KB/11.9MB" = 21
  }

  // Defensive truncate: a line is ~21 chars before the default font wraps it onto the next row and breaks the fixed 10 px spacing (mainly long WiFi SSIDs).
  for (int i = 0; i < infoLineCount; i++) {
    if (infoLines[i].length() > 21) infoLines[i] = infoLines[i].substring(0, 21);
  }
}

static void infoDrawHeader() {
  display.fillRect(0, 0, 128, 16, SSD1306_WHITE);
  display.drawLine(1, 1, 126, 1, SSD1306_BLACK);
  display.drawLine(1, 14, 126, 14, SSD1306_BLACK);
  display.drawLine(1, 1, 1, 14, SSD1306_BLACK);
  display.drawLine(126, 1, 126, 14, SSD1306_BLACK);

  display.setCursor(4, 4);
  display.setTextColor(SSD1306_BLACK);
  // Live baud top right, as hotsync.cpp's hsDrawHeader() and bitwarden_relay.cpp's bwDrawHeader(): every title bar shows the active rate.
  {
    String line = "Information";
    display.print(line);
    display.print(padLeft(liveBaudStr(PhysicalSerial.baudRate()), 20 - line.length()));
  }
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 17);
  display.display();
  drawWifiStatusIcon();  // WiFi status on every title bar
}

static void infoDrawStatus() {
  display.fillRect(0, 17, 128, 47, SSD1306_BLACK);
  display.setTextColor(SSD1306_WHITE);

  int y = 19;
  for (int i = infoScrollOffset; i < infoLineCount && i < infoScrollOffset + INFO_VISIBLE_LINES; i++) {
    display.setCursor(0, y);
    display.print(infoLines[i]);
    y += 10;
  }

  // the up/down scroll glyphs showMenu() uses for a long list
  if (infoScrollOffset > 0) {
    display.setCursor(120, 19);
    display.write(0x1E);
  }
  if (infoScrollOffset + INFO_VISIBLE_LINES < infoLineCount) {
    display.setCursor(120, 54);
    display.write(0x1F);
  }

  display.display();
}

void showInformationScreen() {
  infoBuildLines();
  infoScrollOffset = 0;

  SerialPrintLn("\r\n===== System Information =====");
  for (int i = 0; i < infoLineCount; i++) SerialPrintLn(infoLines[i]);
  SerialPrintLn("==============================");
  SerialPrintLn("Press BACK or ESC to exit, DOWN/UP to scroll");

  infoDrawHeader();
  infoDrawStatus();

  while (true) {
    readSwitches();
    if (BTNBK) {
      waitSwitches();
      break;
    }
    else if (BTNDN) {
      waitSwitches();
      if (infoScrollOffset + INFO_VISIBLE_LINES < infoLineCount) {
        infoScrollOffset++;
        infoDrawStatus();
      }
    }
    else if (BTNUP) {
      waitSwitches();
      if (infoScrollOffset > 0) {
        infoScrollOffset--;
        infoDrawStatus();
      }
    }

    if (SerialAvailable() > 0) {
      char c = SerialRead();
      if (c == 0x1B) break;  // ESC to exit
    }
    delay(20);
  }

  // menuMode never left MODE_DIAGNOSTICS while this screen was up (it draws with infoDrawHeader()/infoDrawStatus(), bypassing showMenu()), so showHeader()'s lastMenu != menuMode check would not
  // clear the display on the way back, and the screen's text and arrows would stay as garbage. Force it, as sasd.cpp and slip_mode.cpp do for their custom full-screen views.
  lastMenu = -1;
  diagnosticsMenu(false);  // BACK returns to Utilities
}

// ---- Format Card (Utilities): erase the SD card or the virtual card ----

// A key from the Palm line or the console (burst-guarded, as Factory Reset's confirmation), or 0 after 15 s.
static char formatKey() {
  unsigned long t0 = millis();
  while (millis() - t0 < 15000) {
    char k = 0;
    if (menuKey(k)) { SerialPrint(k); return k; }
    delay(10);
  }
  return 0;
}

// Buttons (OLED builds): wait for one of ENTER/UP/DOWN/BACK and return 'E', 'U', 'D' or 'B'.
static char formatButton() {
  waitSwitches();
  while (true) {
    readSwitches();
    if (BTNEN) { waitSwitches(); return 'E'; }
    if (BTNUP) { waitSwitches(); return 'U'; }
    if (BTNDN) { waitSwitches(); return 'D'; }
    if (BTNBK) { waitSwitches(); return 'B'; }
    delay(50);
  }
}

static void formatDone(const String &line, const String &screen) {
  SerialPrintLn(line);
  showMessage(screen);
  delay(2500);
}

// SD card only. The virtual card is never formatted from here (or from WiRSaControl): it holds the files a blank Palm
// is set up from (HotSync installs, the SASD set), and with no copy elsewhere they could only come back over FTP.
static void formatCardScreen(bool byButton) {
  if (ftpServerRunning) {   // the FTP server holds the card
    formatDone("\r\nCannot format: the FTP server is running. Stop it first.", "Cannot format.\nStop FTP first");
    return;
  }
  if (byButton) {
    showMessage("ERASE SD CARD?\n\nENTER: Confirm\nBACK: Cancel");
    if (formatButton() != 'E') { formatDone("\r\nFormat cancelled.", "Format\ncancelled"); return; }
  } else {
    SerialPrintLn("\r\nErase EVERYTHING on the SD card? Press Y to confirm, any other key to cancel.");
    showMessage("ERASE SD CARD?\n\nY: Confirm\nOther key: Cancel");
    char y = formatKey();
    if (y != 'Y' && y != 'y') { formatDone("\r\nFormat cancelled.", "Format\ncancelled"); return; }
  }

  String err;
  SerialPrintLn("\r\nFormatting the SD card...");
  showMessage("PLEASE WAIT\n\nFormatting the\nSD card");
  uint8_t fat = 0;
  if (!sdFormatCard(&fat, err)) { formatDone("SD card not formatted: " + err, "Format failed\n\n" + err); return; }
  SerialPrintLn("SD card formatted (FAT" + String(fat) + ").");

  // A new card gets the SASD install set from the virtual card, so a Palm can install the driver from it.
  SerialPrintLn("Copying the SASD install set from the virtual card...");
  showMessage("PLEASE WAIT\n\nCopying the\nSASD files");
  uint8_t copied = 0, total = 0;
  if (!sdCopySasdFromVirtual(&copied, &total, err))
    formatDone("SASD files not copied: " + err, "Formatted, FAT" + String(fat) + "\n\nSASD copy failed");
  else if (total == 0)
    formatDone("The virtual card has no /PALM/PROGRAMS/SASD folder: nothing copied.",
               "Formatted, FAT" + String(fat) + "\n\nNo SASD files\nto copy");
  else
    formatDone("Copied " + String(copied) + " of " + String(total) + " SASD files to /PALM/PROGRAMS/SASD.",
               "Formatted, FAT" + String(fat) + "\n\nSASD files:\n" + String(copied) + " of " + String(total) + " copied");
}

// ---- Diagnostics menu and loop ----

void diagnosticsMenu(bool arrow) {
  menuMode = MODE_DIAGNOSTICS;
  // The internal menu name stays "DIAG" (it drives the serial "-=-=- DIAG MENU -=-=-" banner); the header shows "Utilities" to match the main-menu item that leads here and to free room for the WiFi icon.
  showMenu("DIAG", diagnosticsMenuDisp, 4, (arrow ? MENU_DISP : MENU_BOTH), 0, false, "Utilities");
}

void diagnosticsLoop() {
  int menuSel = -1;
  readSwitches();

  if (BTNUP) {
    menuIdx--;
    if (menuIdx < 0) menuIdx = menuCnt - 1;
    diagnosticsMenu(true);
  }
  else if (BTNDN) {
    menuIdx++;
    if (menuIdx > (menuCnt - 1)) menuIdx = 0;
    diagnosticsMenu(true);
  }
  else if (BTNEN) {
    menuSel = menuIdx;
  }
  else if (BTNBK) {
    mainMenu(false);
    return;
  }
  waitSwitches();

  char chr = 0;
  int serAvl = menuKey(chr);   // burst-guarded Palm-line key, or a control frame (palm_ctl.h)
  if (serAvl > 0) {
    SerialPrint(chr);
  }

  if (serAvl > 0 || menuSel > -1) {
    if (chr == 'M' || chr == 'm' || menuSel == 0) {
      // Main Menu: the 'M'/'m' convention of the other menus that return straight to mainMenu() (settingsMenuDisp, ftpServerMenuDisp), not the 'B'/'b' this menu used to share with menus that go back one level.
      mainMenu(false);
    }
    else if (chr == 'R' || chr == 'r' || menuSel == 1) {
      // Retry WiFi: a manual kick for maintainWiFi()'s background reconnect, which gives up after one grace window and needs an explicit restart, not a reboot.
      // USB only: what is saved, and does the radio see that SSID at all
      if (ssid == "" || password == "") {
        Serial.printf("\r\nWiFi: NOTHING TO RETRY - saved SSID %s, password %s\r\n",
                      ssid == "" ? "is empty" : "set", password == "" ? "is empty" : "set");
      } else {
        Serial.printf("\r\nWiFi: saved SSID \"%s\" (%u chars), password %u chars - scanning...\r\n",
                      ssid.c_str(), (unsigned) ssid.length(), (unsigned) password.length());
        int n = WiFi.scanNetworks();
        bool seen = false;
        for (int i = 0; i < n; i++) {
          if (WiFi.SSID(i) == ssid) {
            seen = true;
            Serial.printf("WiFi: seen on channel %d, RSSI %d dBm, auth type %d\r\n",
                          (int) WiFi.channel(i), (int) WiFi.RSSI(i), (int) WiFi.encryptionType(i));
          }
        }
        if (!seen) Serial.printf("WiFi: SSID NOT seen in %d networks found (2.4 GHz only; check spelling/case)\r\n", n);
        WiFi.scanDelete();
      }
      retryWiFi();
      SerialPrintLn("\r\nWiFi: retrying connection to saved SSID...");
      showMessage("WiFi\n\nRetrying...");
      delay(1000);
      diagnosticsMenu(false);
    }
    else if (chr == 'I' || chr == 'i' || menuSel == 2) {
      // Information: its own scrollable screen, which handles its BACK to Utilities itself (see showInformationScreen()).
      showInformationScreen();
    }
    else if (chr == 'F' || chr == 'f' || menuSel == 3) {
      formatCardScreen(menuSel == 3);
      lastMenu = -1;   // the messages drew over the menu: redraw it in full
      diagnosticsMenu(false);
    }
    else if (serAvl > 0) {
      diagnosticsMenu(false);
    }
  }
}
