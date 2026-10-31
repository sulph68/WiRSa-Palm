// FTP Server: a background FTP server for the SD card, replacing the old web config server and MODEM mode. It runs through a non-blocking pump called every loop() pass (ftpServerPump()),
// whatever menuMode is, like slip_nat.cpp's non-blocking socket idiom (this codebase has no FreeRTOS tasking).

// NOTE: the guard is deliberately NOT FTP_SERVER_H: the vendored MultiFTPServer library (lib/MultiFTPServer/FtpServer.h) uses that name. This header is included first in ftp_server.cpp, so a
// colliding guard would make the library's own #ifndef FTP_SERVER_H see it defined and skip its body (the class FtpServer declaration): the "'FtpServer' does not name a type" bug.
#ifndef WIRSA_FTP_SERVER_H
#define WIRSA_FTP_SERVER_H

#include <Arduino.h>

#define FTP_USER_MAX 32
#define FTP_PASS_MAX 32
#define AP_SSID_MAX  32
#define AP_PASS_MAX  63

// Persisted credentials (EEPROM-backed, see settings.cpp). A blank ftpUser/ftpPass allows anonymous login.
extern String ftpUser, ftpPass;
extern String apSsid, apPass;

// True while the FTP server accepts connections (STA-bound or AP mode). Checked by sasd.cpp's enterSasdMode() for the SASD/FTP mutual exclusion, and shown on the FTP Server menu's "Status" line.
extern bool ftpServerRunning;

// Start the FTP server. With apMode the ESP32 radio goes into Access Point mode (apSsid/apPass) first; otherwise it binds to the STA WiFi connection. Refuses to start (shows "Cannot start.\nStop SASD
// first") if SASD is active.
void ftpServerStart(bool apMode);

// Stop the FTP server and, if it was started in AP mode, tear the AP down and restore the prior WiFi mode.
void ftpServerStop();

// Pump the server's non-blocking accept/read/write state machine. Call it every loop() pass, outside the menuMode dispatch, so it keeps serving in HotSync, SASD, SLIP Gateway or any mode.
void ftpServerPump();

#endif // WIRSA_FTP_SERVER_H
