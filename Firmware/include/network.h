#ifndef NETWORK_H
#define NETWORK_H

#include <Arduino.h>
#include <WiFi.h>

// WiFi and network management functions
// Single entry point for every WiFi.disconnect() call in the firmware -
// mirrors wifiBeginConnect() below for the same reason: one place to add/
// tune disconnect-time behavior and have it apply everywhere at once.
void wifiDisconnect();
// Convenience wrapper: wifiDisconnect() + a brief pause + an immediate icon
// redraw, for callers that want the UI visibly settled into "disconnected"
// right away rather than just the raw disconnect.
void disconnectWiFi();
String ipToString(IPAddress ip);
void showWifiIcon();
int getSignalBars();  // Returns 0-4 based on WiFi RSSI
// Single entry point for every WiFi.begin() call in the firmware - the one
// place to add/tune connection-time behavior (TX power, PHY mode, channel/
// BSSID hints, etc.) so a change here applies everywhere at once: the
// background manager (maintainWiFi), the blocking wait
// (waitForWiFiConnection), WiFi Setup's reconnect-on-cancel paths, and its
// new-network test-connect path. Previously each of those called
// WiFi.begin() directly and independently.
void wifiBeginConnect(const char *connSsid, const char *connPassword);
// Fire-and-forget reconnect to the globally saved ssid/password (no retry/
// poll logic of its own - maintainWiFi() picks the connection back up from
// its own next tick). Used by WiFi Setup's several "abandon setup, resume
// the normal connection" paths, which used to each duplicate this same
// ssid.length()>0 check + WiFi.begin() call inline.
void wifiReconnectToSaved();
// Non-blocking, called once per loop() iteration so a configured WiFi
// network is connected and kept connected in the background regardless of
// what mode/menu is currently active. Retries for up to one minute, then
// gives up until retryWiFi() or waitForWiFiConnection() (below) starts a
// fresh attempt - see network.cpp for the full design.
void maintainWiFi();
// Forces maintainWiFi()'s background state machine to immediately re-issue
// WiFi.begin() on its very next tick, as if the link had just dropped -
// wired to Utilities' "Retry WiFi" menu item, for use after maintainWiFi()
// has given up (see its own comment).
void retryWiFi();
// Blocking "wait for WiFi, with a retry/cancel prompt on timeout" used by
// WiFi-requiring services (FTP Server/SLIP Gateway/BitWarden Relay) at
// entry - see network.cpp's own comment for the full UX. Returns true once
// connected, false if the user cancelled (BACK) - caller must then abandon
// its own startup.
bool waitForWiFiConnection();
// Paints the 3-bar animated/connected/disconnected WiFi status icon into
// whichever header is currently on screen (every menu screen via
// showHeader(), plus HotSync/SASD/SLIP/BitWarden's own custom headers all
// call this directly too). Also invoked internally by maintainWiFi() on a
// timer, regardless of menuMode, so the "searching" animation advances
// even when the user isn't pressing any buttons.
void drawWifiStatusIcon();

#endif // NETWORK_H
