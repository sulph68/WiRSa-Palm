#ifndef CALDAV_RELAY_H
#define CALDAV_RELAY_H

// CalDAV Relay mode: a stateless serial-driven bridge between a Palm running the CalDAV Sync PalmOS client (apps/caldav_sync in the hackmaster_y2035 repo) and a CalDAV server or a plain ICS feed.
// The architecture is BitWarden Relay's (bitwarden_relay.h): the Palm cannot run TLS, so WiRSa terminates it, does the HTTP(S) CalDAV REPORT (or a plain GET for an ICS feed), expands recurring events
// (uICAL) into occurrences clamped to the sync window, and streams them back over the cradle line one at a time, each acked, with BitWarden's per-item flow control for the same hardware reason (a
// free-running burst overruns the Palm's small Serial Manager buffer).
// Server types: CalDAV-generic, iCloud, ICS Feed and Google Calendar (CDDVServerType on the Palm side). Google authenticates through a one-time OAuth handshake (CDDV_OAUTH_START/INFO/TOKEN) that gives WiRSa
// a refresh token, kept Sec35-encrypted on the Palm like a CalDAV/iCloud password; the Client ID is entered on the Palm per slot and sent in every SYNC_REQUEST, so WiRSa holds no resident Google-account
// state, as with every credential of this relay.
// Entered from the main menu like every WiRSa mode (SASD, HotSync, BitWarden Relay): it services what the Palm sends until BACK or +++ exits it.

void enterCalDavRelayMode();
void exitCalDavRelayMode();
void caldavRelayLoop();

#endif // CALDAV_RELAY_H
