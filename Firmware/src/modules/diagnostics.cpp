// Diagnostics Module
// Serial diagnostics and troubleshooting tools for WiRSa

#include "Arduino.h"
#include "diagnostics.h"
#include "globals.h"
#include "display_menu.h"
#include "serial_io.h"
#include "settings.h"
#include "network.h"
#include <WiFi.h>
#include <Adafruit_SSD1306.h>

// External globals
extern Adafruit_SSD1306 display;
extern int menuMode;
extern int menuIdx;
extern int menuCnt;
extern int lastMenu;
extern byte serialSpeed;
extern const int bauds[];
extern HardwareSerial PhysicalSerial;
extern String build;

// Button state
extern bool BTNUP;
extern bool BTNDN;
extern bool BTNEN;
extern bool BTNBK;

// Menu display array (defined in main.cpp)
extern String diagnosticsMenuDisp[];

// Diagnostics context
DiagnosticsContext diagCtx;

void diagnosticsInit() {
  diagCtx.hexDumpEnabled = false;
}

// ============================================================================
// Hex Dump Mode
// ============================================================================
//
// diagCtx.hexDumpEnabled has no remaining writer (setHexDumpMode, its only
// one, was AT$HEX-only and removed with the rest of the dead AT-command
// dispatcher below) - permanently false, so hexDumpByte's real call sites
// (serial_io.cpp) are now unconditionally inert. Left in place rather than
// also touching serial_io.cpp's RX/TX hot path in this same pass.

void hexDumpByte(char direction, uint8_t byte) {
  if (!diagCtx.hexDumpEnabled) return;

  // Format: [T] 41 'A'  or  [R] 0D <CR>
  Serial.print("[");
  Serial.print(direction);
  Serial.print("] ");

  if (byte < 0x10) Serial.print("0");
  Serial.print(byte, HEX);
  Serial.print(" ");

  // Show ASCII or control char name
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

// ============================================================================
// Information Screen (Utilities menu) - HotSync/BitWarden-style boxed
// header + scrollable status area, reflowed into a scrollable line list
// sized for the OLED's status area.
// ============================================================================

#define INFO_MAX_LINES 11
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

  if (WiFi.status() == WL_CONNECTED) {
    infoLines[infoLineCount++] = "WiFi: " + WiFi.SSID();
    infoLines[infoLineCount++] = "RSSI: " + String(WiFi.RSSI()) + "dBm";
    infoLines[infoLineCount++] = "IP: " + WiFi.localIP().toString();
    infoLines[infoLineCount++] = "DNS: " + WiFi.dnsIP().toString();
  } else {
    infoLines[infoLineCount++] = "WiFi: Not connected";
  }

  infoLines[infoLineCount++] = "Baud: " + String(bauds[serialSpeed]);

  // Defensive truncate - one line is ~21 chars wide before the default font
  // would wrap it onto the next display row and throw off the fixed 10px
  // row spacing below (mainly a concern for long WiFi SSIDs).
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
  // Live baud top-right, same as hotsync.cpp's hsDrawHeader()/bitwarden_
  // relay.cpp's bwDrawHeader() - every title bar shows the active baud rate.
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

  // Same up/down scroll-indicator glyphs showMenu() uses for a long list.
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

  // menuMode never left MODE_DIAGNOSTICS while this screen was up (it draws
  // directly via infoDrawHeader()/infoDrawStatus(), bypassing showMenu()),
  // so showHeader()'s own lastMenu!=menuMode check wouldn't trigger a full
  // clearDisplay() on the way back in - it'd only repaint the small
  // selection-arrow strip, leaving this screen's scrollable text/arrows
  // behind as garbage. Force it, same idiom sasd.cpp/slip_mode.cpp already
  // use for their own custom full-screen status views.
  lastMenu = -1;
  diagnosticsMenu(false);  // BACK returns to Utilities
}

// ============================================================================
// Diagnostics Menu and Loop
// ============================================================================

void diagnosticsMenu(bool arrow) {
  menuMode = MODE_DIAGNOSTICS;
  // Internal menu name stays "DIAG" (drives the serial "-=-=- DIAG MENU
  // -=-=-" banner) - header shows "Utilities" instead to match the
  // main-menu item that leads here, and to free up room for the WiFi
  // status icon.
  showMenu("DIAG", diagnosticsMenuDisp, 3, (arrow ? MENU_DISP : MENU_BOTH), 0, false, "Utilities");
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

  // Serial input
  int serAvl = SerialAvailable();
  char chr = 0;
  if (serAvl > 0) {
    chr = SerialRead();
    SerialPrint(chr);
  }

  // Handle selection
  if (serAvl > 0 || menuSel > -1) {
    if (chr == 'M' || chr == 'm' || menuSel == 0) {
      // Main Menu - matches the 'M'/'m' convention every other menu that
      // returns straight to mainMenu() uses (settingsMenuDisp/
      // ftpServerMenuDisp/slipMenuDisp), instead of the 'B'/'b' this menu
      // used to share with menus that only go back one level to a
      // submenu.
      mainMenu(false);
    }
    else if (chr == 'R' || chr == 'r' || menuSel == 1) {
      // Retry WiFi - manual kick for maintainWiFi()'s background reconnect,
      // which gives up retrying after one grace window and needs an
      // explicit restart rather than a reboot.
      retryWiFi();
      SerialPrintLn("\r\nWiFi: retrying connection to saved SSID...");
      showMessage("WiFi\n\nRetrying...");
      delay(1000);
      diagnosticsMenu(false);
    }
    else if (chr == 'I' || chr == 'i' || menuSel == 2) {
      // Information - own scrollable screen, handles its own BACK-to-
      // Utilities exit internally (see showInformationScreen()).
      showInformationScreen();
    }
    else if (serAvl > 0) {
      diagnosticsMenu(false);
    }
  }
}
