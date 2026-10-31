#ifndef NETWORK_H
#define NETWORK_H

#include <Arduino.h>
#include <WiFi.h>

// WiFi and network management.
// The one entry point for every WiFi.disconnect(), like wifiBeginConnect() below: one place to tune disconnect-time behaviour for all callers.
void wifiDisconnect();
// wifiDisconnect() + a brief pause + an immediate icon redraw, for callers that want the UI visibly settled into "disconnected".
void disconnectWiFi();
String ipToString(IPAddress ip);
void showWifiIcon();
int getSignalBars();  // Returns 0-4 based on WiFi RSSI
// The one entry point for every WiFi.begin(): the place to tune connection-time behaviour (TX power, PHY mode, channel/BSSID hints) for all callers: the background manager (maintainWiFi), the blocking
// wait (waitForWiFiConnection), WiFi Setup's reconnect-on-cancel paths and its new-network test connect (each used to call WiFi.begin() on its own).
void wifiBeginConnect(const char *connSsid, const char *connPassword);
// Fire-and-forget reconnect to the saved ssid/password (no retry or poll of its own; maintainWiFi() resumes from its next tick). For WiFi Setup's several "abandon setup, resume the normal connection"
// paths, which each duplicated the ssid.length() > 0 check and WiFi.begin().
void wifiReconnectToSaved();
// Non-blocking, once per loop() pass: a configured network is connected and kept connected whatever the mode or menu. Retries for up to one minute, then gives up until retryWiFi() or
// waitForWiFiConnection() starts a fresh attempt (see network.cpp).
void maintainWiFi();
// Makes maintainWiFi()'s state machine re-issue WiFi.begin() on its next tick, as if the link had just dropped; wired to Utilities' "Retry WiFi", for after it has given up.
void retryWiFi();
// Blocking "wait for WiFi, with a retry/cancel prompt on timeout" for WiFi-requiring services (FTP Server, SLIP Gateway, BitWarden Relay) at entry (see network.cpp). True once connected, false if the
// user cancelled (BACK), and the caller must then abandon its startup.
bool waitForWiFiConnection();
// Paints the 3-bar animated/connected/disconnected WiFi icon into whatever header is on screen (every menu via showHeader(), and HotSync/SASD/SLIP/BitWarden's custom headers call it directly).
// maintainWiFi() also calls it on a timer, whatever menuMode, so the "searching" animation advances with no button pressed.
void drawWifiStatusIcon();

// Control link STATUS: 0 no network set up, 1 connecting, 2 connected, 3 gave up retrying.
uint8_t wifiStateCode();

#endif // NETWORK_H
