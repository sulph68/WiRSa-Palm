// FTP Server module
// Background FTP server serving the SD card, replacing the old web config
// server and MODEM mode. Runs via a non-blocking pump called every loop()
// iteration (see ftpServerPump()), independent of whatever menuMode is
// active - mirrors slip_nat.cpp's own non-blocking socket idiom, since this
// codebase has no FreeRTOS tasking anywhere.

// NOTE: guard is deliberately NOT named FTP_SERVER_H - the vendored
// MultiFTPServer library (lib/MultiFTPServer/FtpServer.h) uses that exact
// guard name for itself. Since this header is included first in
// ftp_server.cpp, a colliding guard here would make the library's own
// #ifndef FTP_SERVER_H see it already defined and silently skip its entire
// body (including the class FtpServer declaration) when included later in
// the same translation unit - exactly the "'FtpServer' does not name a
// type" bug this comment is here to prevent regressing.
#ifndef WIRSA_FTP_SERVER_H
#define WIRSA_FTP_SERVER_H

#include <Arduino.h>

#define FTP_USER_MAX 32
#define FTP_PASS_MAX 32
#define AP_SSID_MAX  32
#define AP_PASS_MAX  63

// Persisted credentials (EEPROM-backed, see settings.cpp). Blank
// ftpUser/ftpPass means anonymous login is allowed.
extern String ftpUser, ftpPass;
extern String apSsid, apPass;

// True while the FTP server is accepting connections (either STA-bound or
// AP-mode). Checked by sasd.cpp's enterSasdMode() for the SASD<->FTP mutual
// exclusion, and reflected on the FTP Server menu's "Status" line.
extern bool ftpServerRunning;

// Start the FTP server. If apMode is true, puts the ESP32 WiFi radio into
// Access Point mode (using apSsid/apPass) before starting the server;
// otherwise binds to the existing STA WiFi connection. Refuses to start
// (shows "Cannot start.\nStop SASD first") if SASD mode is currently active.
void ftpServerStart(bool apMode);

// Stop the FTP server and, if it was started in AP mode, tear the AP back
// down and restore the prior WiFi mode.
void ftpServerStop();

// Pump the FTP server's non-blocking accept/read/write state machine. Must
// be called unconditionally every loop() iteration, outside the menuMode
// dispatch chain, so the server keeps accepting connections and moving data
// while the user is in HotSync/SASD/SLIP Gateway/any other mode.
void ftpServerPump();

#endif // WIRSA_FTP_SERVER_H
