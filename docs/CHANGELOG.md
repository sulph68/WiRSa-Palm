# Changes from the original WiRSa firmware

This fork against the original [nullvalue0/WiRSa](https://github.com/nullvalue0/WiRSa) firmware. (It was the history block at the top of `Firmware/src/main.cpp`.)

## Version 1.1

- The signal monitor shows on screen in modem mode.
- Telnet call mode asserts DCD.
- Some pin polarity fixes.

## Added

- **HotSync Install mode:** pushes `.PRC`/`.PDB` files from an SD card to the Palm over a real HotSync session.
- **SASD (Serial-Attached SD Card) mode:** the Palm browses and uses the SD card as mass storage over serial.
- **BitWarden Relay mode:** bridges a Palm password-manager client to a Vaultwarden server over WiFi.
- **FTP Server mode:** a background FTP server for the SD card over WiFi.
- Background WiFi connect and reconnect, with a live signal-strength icon on every title bar.
- Per-feature compile-time switches (`WIRSA_FEATURE_*`) for a trimmed-down firmware.
- **Boards:** the LilyGO T7-S3 (`WIRSA_BOARD_T7S3`) and a plain ESP32-S3 board with the OLED, built with its own flags (PSRAM, the 16 MB partition table, NimBLE); deep sleep is opt-in there (`-DWIRSA_DEEP_SLEEP`).
- **RGB status LED** (`-DWIRSA_LED_NEOPIXEL=<gpio>`): blue / red for WiFi on / off, a dark flash for SD traffic, green pulses for Mesh. See `README-Palm.md`, "Status LED".

## Removed

- **MODEM mode:** the Hayes AT-command dial-up/telnet/BBS emulation.
- **PPP Gateway** (replaced by SLIP Gateway).
- **Built-in web server** for status and file management (replaced by the FTP Server).
- **File Transfer** (XMODEM/YMODEM/ZMODEM) as a menu: its mode slot became HotSync.
- **Text Playback** from the SD card: its mode slot became SASD.
- **Speed-dial entries:** they were only usable in MODEM mode. The EEPROM storage (`speedDials[]`, `DIAL0-9_ADDRESS`) went in a later size cleanup.
- **RTS/CTS flow control and DTR:** not wired on this hardware.
- **Utilities items:** Auto-Detect Baud, Hex Dump Mode, Loopback Test and Signal Monitor, and the `AT$SIG`/`AT$LOOP`/`AT$HEX`/`AT$BAUD?`/`AT$SYS`/`AT$STAT` dispatcher. They had stayed unreachable after MODEM mode went. `hexDumpByte()` remains (still called from `serial_io.cpp`), and is inert because its enabling flag can no longer be set.
- **Settings with no consumer:** echo, autoAnswer, petTranslate, consoleMode and IP_TYPE, with their EEPROM storage (removed in the same cleanup).
- **`serialConfig`, `bits[]` and `bitsDisp[]`** (the 24-entry serial format table and labels): `serialConfig` was only ever assigned from the EEPROM and never written by the UI. The five `PhysicalSerial.begin()` calls now use `WIRSA_SERIAL_FORMAT`, as the relay modes already did.

## Other behaviour

- SLIP Gateway starts at once from the main menu; BACK returns to its submenu.
- The live baud rate shows on every screen's title bar, including BitWarden Relay and SLIP Gateway.
- The Utilities Information screen scrolls instead of using a fixed popup that could overflow.
- `globals.h` holds the pin and EEPROM `#define` blocks; `main.cpp`, `network.cpp` and `settings.cpp` no longer keep copies.

Later work (CalDAV and CardDAV, Mesh, WiRSaCtl, the Palm wire protocol) is described in `Relay-Modes.md`, `Mesh-Palm.md`, `README-Palm.md` and `protocol/Protocol-wire.md`.
