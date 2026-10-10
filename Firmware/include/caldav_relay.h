#ifndef CALDAV_RELAY_H
#define CALDAV_RELAY_H

// CalDAV Relay mode - a stateless serial-driven bridge between a Palm
// running the CalDAV Sync PalmOS client (apps/caldav_sync in the
// hackmaster_y2035 repo) and a CalDAV server or a plain ICS feed.
//
// Same architecture as BitWarden Relay (see bitwarden_relay.h): the Palm
// cannot run TLS, so WiRSa terminates it, does the actual HTTP(S)
// CalDAV REPORT (or a plain GET for an ICS feed), expands any recurring
// events (uICAL) into concrete occurrences clamped to the requested sync
// window, and streams them back over the cradle serial line one at a
// time, each individually acked - exactly like BitWarden Relay's own
// per-item flow control, for the same real-hardware reason (a free-
// running burst overruns the Palm's small fixed Serial Manager buffer).
//
// Supports CalDAV-generic, iCloud, ICS Feed, and Google Calendar server
// types (CDDVServerType on the Palm side). Google authenticates via a
// one-time OAuth handshake (CDDV_OAUTH_START/INFO/TOKEN) that hands WiRSa
// a refresh token, stored Sec35-encrypted on the Palm exactly like a
// CalDAV/iCloud password. The Google OAuth Client ID is likewise entered
// on the Palm (per slot) and sent fresh in every SYNC_REQUEST - WiRSa
// holds no resident Google-account-identifying state of its own, same as
// every other credential this relay handles.
//
// Entered from the main menu, same shape as every other WiRSa mode
// (SASD/HotSync/BitWarden Relay): passively services whatever the Palm
// sends until BACK/+++ exits it.

void enterCalDavRelayMode();
void exitCalDavRelayMode();
void caldavRelayLoop();

#endif // CALDAV_RELAY_H
