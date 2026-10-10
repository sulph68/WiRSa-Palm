// FTP Server module
// Background FTP server (MultiFTPServer, vendored in lib/MultiFTPServer)
// serving the SD card, replacing the old web config server and MODEM mode.

#include "ftp_server.h"
#include "globals.h"
#include "serial_io.h"
#include "display_menu.h"
#include "network.h"
#include <WiFi.h>
#include <SD.h>

// STORAGE_TYPE/FTP_MAX_SESSIONS/FTP_SERVER_DEBUG are configured directly in
// the vendored library's own FtpServerKey.h (the config header it ships
// specifically for this purpose), NOT as a #define in this file. This file
// (ftp_server.cpp) and the library's own implementation file
// (lib/MultiFTPServer/FtpServer.cpp, which has its own independent
// #include <FtpServer.h> and is compiled as a SEPARATE translation unit)
// each do their own preprocessing pass - a #define written only here would
// be invisible to that one, silently leaving FtpServer.cpp's actual
// compiled member functions (openDir(), doList(), etc.) using the
// library's un-overridden ESP32 default (STORAGE_FFAT, the internal flash
// partition, not the SD card) regardless of what this file's own view of
// the class claimed - this was the real root cause of the FTP client
// seeing no SD card contents, and was also a latent ODR-violation risk
// (two translation units compiling different STORAGE_TYPE-dependent member
// layouts for the same class is undefined behavior). Editing
// FtpServerKey.h directly guarantees both translation units see the same
// value, since they both include that identical physical file.
#include <FtpServer.h>

// ftpUser/ftpPass/apSsid/apPass/ftpServerRunning are defined in main.cpp
// (alongside ssid/password) and declared extern in ftp_server.h.

static FtpServer ftpSrv;
static bool ftpServerApMode = false;

void ftpServerStart(bool apMode) {
  // SASD does raw-sector I/O bypassing the FAT filesystem entirely; running
  // the FTP server (normal FAT writes) at the same time risks real card
  // corruption - see enterSasdMode()'s own ftpServerRunning check, which is
  // the check that actually matters. This one is effectively unreachable
  // under the current single-active-mode-loop dispatch (ftpServerStart() is
  // only ever called from ftpServerLoop(), which itself only ever runs when
  // menuMode==MODE_FTPSERVER already - there's no path to reach this
  // function while SASD's own loop, and thus MODE_PLAYBACK, is active).
  // Kept as a harmless defensive belt-and-braces check in case that
  // invariant ever changes (e.g. SASD ever becomes a background service).
  if (menuMode == MODE_PLAYBACK) {
    SerialPrintLn("Cannot start FTP server: SASD mode is active.");
    showMessage("Cannot start.\nStop SASD first");
    delay(1500);
    return;
  }
  if (ftpServerRunning) return;

  if (!SD.begin(SD_CS, SPI, 20000000)) {
    SerialPrintLn("FTP server: no SD card - please insert one and try again.");
    showMessage("\nPLEASE INSERT\n   SD CARD");
    delay(1500);
    return;
  }
  // Confirm what the SD library itself actually sees BEFORE the FTP server
  // starts (and before control returns to the FTP Server menu on serial) -
  // if this shows a wrong/zero card size, the FTP-side "no files visible"
  // symptom is an SD-mount problem, not an FTP-library problem.
  SerialPrintLn("FTP server: SD card info before starting -");
  printSDCardInfo();

  if (apMode) {
    if (apSsid.length() == 0) {
      SerialPrintLn("FTP server (AP mode): AP Setup required first (Config menu).");
      showMessage("AP Setup required\nfirst (Config menu)");
      delay(1500);
      return;
    }
    if (apPass.length() > 0 && apPass.length() < 8) {
      // ESP32 WiFi.softAP() silently falls back to an OPEN network for any
      // password under 8 chars (WPA2 minimum) - warn explicitly rather than
      // starting an unencrypted AP with no indication anything changed.
      SerialPrintLn("FTP server (AP mode): configured password is under 8 characters - starting OPEN (unencrypted).");
      showMessage("AP password <8 chars\nStarting OPEN network");
      delay(2000);
    }
    SerialPrint("FTP server: starting AP \""); SerialPrint(apSsid); SerialPrintLn("\"");
    showMessage("Starting AP\n" + apSsid);
    bool apOk = (apPass.length() >= 8) ? WiFi.softAP(apSsid.c_str(), apPass.c_str())
                                        : WiFi.softAP(apSsid.c_str());
    if (!apOk) {
      SerialPrintLn("FTP server (AP mode): WiFi.softAP() failed to start.");
      showMessage("AP start failed");
      delay(1500);
      return;
    }
    ftpServerApMode = true;
    mdns.begin("WiRSa"); // wirsa.local resolves in AP mode too
    mdns.addService("ftp", "tcp", 21);
  } else {
    if (WiFi.status() != WL_CONNECTED) {
      // waitForWiFiConnection() shows its own "Waiting for WiFi..." /
      // "WiFi unavailable. Retry?" UI and retries for up to a minute -
      // a false return means the user backed out.
      if (!waitForWiFiConnection()) {
        SerialPrintLn("FTP server: WiFi not available - cancelled.");
        return;
      }
    }
    ftpServerApMode = false;
  }

  if (ftpUser.length() > 0) {
    ftpSrv.begin(ftpUser.c_str(), ftpPass.c_str());
  } else {
    ftpSrv.begin(); // anonymous
  }
  ftpServerRunning = true;
  SerialPrintLn(ftpServerApMode ? "FTP server started (AP mode)." : "FTP server started.");
}

void ftpServerStop() {
  if (!ftpServerRunning) return;
  ftpSrv.end();
  ftpServerRunning = false;
  SerialPrintLn("FTP server stopped.");

  if (ftpServerApMode) {
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_STA);
    ftpServerApMode = false;
  }
}

void ftpServerPump() {
  if (ftpServerRunning) ftpSrv.handleFTP();
}
