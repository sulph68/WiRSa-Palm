#ifndef BITWARDEN_RELAY_H
#define BITWARDEN_RELAY_H

// BitWarden Relay mode: a stateless serial-driven bridge between a Palm running the BitWarden PalmOS client and a self-hosted Vaultwarden server.
// Architecture (the wire: docs/protocol/Protocol-wire.md section 11): the Palm cannot run TLS or the Bitwarden crypto (PBKDF2, HMAC-SHA256, AES-256-CBC), so WiRSa terminates all of it (the OAuth
// resource-owner-password-credentials grant, PBKDF2 key derivation, AES-256-CBC+HMAC decryption of every vault field) and relays plaintext items over the cradle line one at a time. WiRSa NEVER writes any
// of it to flash: the master password and the derived keys live in RAM for one SYNC request only and are wiped right after.
// Like every mode (SASD, HotSync) it is entered from the main menu and services what the Palm sends over PhysicalSerial until BACK or +++ exits it. Unlike ftpServerPump() (run every loop() tick, as FTP
// clients come over WiFi/TCP, apart from the UART), this protocol runs on the one physical UART that the text menu and every serial mode share, so it cannot run in the background as FTP Server does (as
// SASD and HotSync cannot): only one thing owns PhysicalSerial. It is dispatched from bitwardenRelayLoop() while menuMode == MODE_BITWARDEN; there is no submenu, the Palm drives everything.

void enterBitwardenRelayMode();
void exitBitwardenRelayMode();
void bitwardenRelayLoop();

#endif // BITWARDEN_RELAY_H
