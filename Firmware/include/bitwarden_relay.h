#ifndef BITWARDEN_RELAY_H
#define BITWARDEN_RELAY_H

// BitWarden Relay mode - a stateless serial-driven bridge between a Palm
// running the BitWarden PalmOS client and a self-hosted Vaultwarden server.
//
// Architecture (see apps/bitwarden/PROTOCOL.md in the hackmaster_y2035 repo
// for the authoritative wire format both sides code against): the Palm
// cannot run TLS or the Bitwarden crypto stack (PBKDF2/HMAC-SHA256/
// AES-256-CBC), so WiRSa terminates all of it - OAuth (resource-owner-
// password-credentials grant), PBKDF2 key derivation, and AES-256-CBC+HMAC
// decryption of every vault field - and relays plaintext items back over the
// cradle serial line one at a time. WiRSa NEVER writes any of this to flash;
// the master password and every derived key live in RAM only for the
// duration of a single SYNC request and are wiped immediately after.
//
// Same shape as every other WiRSa mode (SASD/HotSync): entered from the main
// menu, then passively services whatever the Palm sends over PhysicalSerial
// until BACK/+++ exits it. Unlike ftpServerPump() (which runs every loop()
// tick regardless of menuMode, because FTP clients connect over WiFi/TCP
// independent of the local UART), this mode's protocol runs directly on the
// same physical UART the local text menu and every other serial mode also
// share - it cannot be "always on in the background" the way FTP Server is,
// for the same reason SASD/HotSync can't be either: only one thing can own
// PhysicalSerial at a time. It's dispatched purely from bitwardenRelayLoop()
// while menuMode==MODE_BITWARDEN, which is the correct analog of
// "background" here (no interactive submenu once entered - the Palm drives
// everything, matching the plan's "select the mode and it works" request).

void enterBitwardenRelayMode();
void exitBitwardenRelayMode();
void bitwardenRelayLoop();

#endif // BITWARDEN_RELAY_H
