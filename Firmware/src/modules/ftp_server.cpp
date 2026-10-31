// FTP Server: a background FTP server (MultiFTPServer, vendored in lib/MultiFTPServer) for the SD card, replacing the old web config server and MODEM mode.

#include "ftp_server.h"
#include "globals.h"
#include "sd_mount.h"
#include "serial_io.h"
#include "display_menu.h"
#include "network.h"
#include <WiFi.h>
#include <SD.h>
#include "sd_mount.h"
#include "vdisk.h"
#include "vdfs.h"

// STORAGE_TYPE, FTP_MAX_SESSIONS and FTP_SERVER_DEBUG are set in the vendored library's FtpServerKey.h (the config header it ships for this), NOT with a #define in this file. This file and the
// library's FtpServer.cpp (its own #include <FtpServer.h>, a SEPARATE translation unit) are preprocessed separately: a #define only here is invisible to that one and FtpServer.cpp's compiled members
// (openDir(), doList(), ...) silently keep the library's ESP32 default (STORAGE_FFAT, the internal flash partition, not the SD card). That was the real root cause of the FTP client seeing no SD
// contents, and an ODR-violation risk (two units compiling different STORAGE_TYPE-dependent layouts of one class). Editing FtpServerKey.h gives both units the same value, as both include that file.
#include <FtpServer.h>

// ftpUser, ftpPass, apSsid, apPass and ftpServerRunning are defined in main.cpp (with ssid/password) and declared extern in ftp_server.h.

static FtpServer ftpSrv;
static bool ftpServerApMode = false;
static bool ftpVirtualMounted = false;   // this server holds the virtual card (vdfs.h)

// Log lines from the library's callbacks; totals for the summary when the server stops.
static uint32_t ftpSessRecv = 0, ftpSessSent = 0, ftpSessFail = 0;
static uint64_t ftpSessBytes = 0;
static bool ftpLastUpload = false;

static void ftpOnConnect(FtpOperation op, uint32_t, uint32_t) {
  if (op == FTP_CONNECT) LOGI("FTP: client connected");
  else if (op == FTP_DISCONNECT) LOGI("FTP: client disconnected");
}

static void ftpOnTransfer(FtpTransferOperation op, const char *name, uint32_t size) {
  switch (op) {
    case FTP_UPLOAD_START:   ftpLastUpload = true;  break;
    case FTP_DOWNLOAD_START: ftpLastUpload = false; break;
    case FTP_TRANSFER_STOP:
      if (ftpLastUpload) ftpSessRecv++; else ftpSessSent++;
      ftpSessBytes += size;
      LOGI("FTP: %s %s (%lu bytes)\r\n", ftpLastUpload ? "received" : "sent", name, (unsigned long) size);
      break;
    case FTP_TRANSFER_ERROR:
      ftpSessFail++;
      LOGW("FTP: transfer of %s failed after %lu bytes\r\n", name, (unsigned long) size);
      break;
    default: break;
  }
}

void ftpServerStart(bool apMode) {
  // SASD does raw sector I/O around the FAT, and FTP writes through it, so both at once risks corrupting the card; enterSasdMode()'s ftpServerRunning check is the one that matters. This one is
  // effectively unreachable (ftpServerStart() is called only from ftpServerLoop(), which runs only in MODE_FTPSERVER, so SASD's loop and MODE_PLAYBACK cannot be active). Kept as a defensive check in
  // case that invariant changes (e.g. SASD becoming a background service).
  if (menuMode == MODE_PLAYBACK) {
    SerialPrintLn("Cannot start FTP server: SASD mode is active.");
    showMessage("Cannot start.\nStop SASD first");
    delay(1500);
    return;
  }
  if (ftpServerRunning) return;

  // The root shows "SDCard" when a card mounts and "ESP32" (the virtual card, set up as in any other mode: formatted if blank) whenever it can; only when neither is there does the server not start.
  bool haveSd = sdMount();
  ftpVirtualMounted = vdiskPresent() && vdfsMount();
  if (!haveSd && !ftpVirtualMounted) {
    SerialPrintLn("FTP server: no SD card and no virtual card - please insert a card and try again.");
    showMessage("\nPLEASE INSERT\n   SD CARD");
    delay(1500);
    return;
  }
  LOGI("FTP: serving%s%s\r\n", haveSd ? " SDCard" : "", ftpVirtualMounted ? " ESP32" : "");
  if (haveSd) {
    // Confirm what the SD library sees BEFORE the server starts (and before control returns to the FTP Server menu on serial): a wrong or zero card size here means the FTP-side "no files visible"
    // symptom is an SD-mount problem, not an FTP-library problem.
    SerialPrintLn("FTP server: SD card info before starting -");
    printSDCardInfo();
  }

  if (apMode) {
    if (apSsid.length() == 0) {
      SerialPrintLn("FTP server (AP mode): AP Setup required first (Config menu).");
      showMessage("AP Setup required\nfirst (Config menu)");
      delay(1500);
      { if (ftpVirtualMounted) { vdfsUnmount(); ftpVirtualMounted = false; } return; }
    }
    if (apPass.length() > 0 && apPass.length() < 8) {
      // ESP32 WiFi.softAP() silently falls back to an OPEN network for a password under 8 chars (the WPA2 minimum): warn, rather than start an unencrypted AP with no sign anything changed.
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
      { if (ftpVirtualMounted) { vdfsUnmount(); ftpVirtualMounted = false; } return; }
    }
    ftpServerApMode = true;
    mdns.begin("WiRSa"); // wirsa.local resolves in AP mode too
    mdns.addService("ftp", "tcp", 21);
  } else {
    if (WiFi.status() != WL_CONNECTED) {
      // waitForWiFiConnection() shows its own "Waiting for WiFi..." / "WiFi unavailable. Retry?" UI and retries for up to a minute; false means the user backed out.
      if (!waitForWiFiConnection()) {
        SerialPrintLn("FTP server: WiFi not available - cancelled.");
        { if (ftpVirtualMounted) { vdfsUnmount(); ftpVirtualMounted = false; } return; }
      }
    }
    ftpServerApMode = false;
  }

  if (ftpUser.length() > 0) {
    ftpSrv.begin(ftpUser.c_str(), ftpPass.c_str());
  } else {
    ftpSrv.begin(); // anonymous
  }
  ftpSrv.setCallback(ftpOnConnect);
  ftpSrv.setTransferCallback(ftpOnTransfer);
  ftpSessRecv = ftpSessSent = ftpSessFail = 0;
  ftpSessBytes = 0;
  ftpServerRunning = true;
  SerialPrintLn(ftpServerApMode ? "FTP server started (AP mode)." : "FTP server started.");
  LOGI("FTP: server started (%s), %s\r\n", ftpServerApMode ? "AP mode" : "WiFi",
       (ftpServerApMode ? WiFi.softAPIP() : WiFi.localIP()).toString().c_str());
}

void ftpServerStop() {
  if (!ftpServerRunning) return;
  ftpSrv.end();
  ftpServerRunning = false;
  if (ftpVirtualMounted) { vdfsUnmount(); ftpVirtualMounted = false; }
  SerialPrintLn("FTP server stopped.");
  LOGI("FTP: server stopped - %lu file(s) received, %lu sent, %lu failed, %lu KB\r\n", (unsigned long) ftpSessRecv,
       (unsigned long) ftpSessSent, (unsigned long) ftpSessFail, (unsigned long) (ftpSessBytes / 1024));

  if (ftpServerApMode) {
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_STA);
    ftpServerApMode = false;
  }
}

void ftpServerPump() {
  if (ftpServerRunning) ftpSrv.handleFTP();
}
