# User guide

The step-by-step user guide for the WiRSa is kept with the Palm apps it works with, so that pictures of the Palm screens sit next to it. It covers:

- Using the unit in **screen mode** (OLED and buttons) and **direct serial mode** (a terminal, or the Palm).
- First-time WiFi setup, each mode (FTP, HotSync, SASD, SLIP, BitWarden, CalDAV, Meshtastic) and **what it shows on the screen**.
- **Network HotSync** through the SLIP gateway.
- Which **card** (SD or virtual) each mode uses and how to see the free space.
- **Screenless builds:** how modes start and return to the main menu.
- The status LED, and **troubleshooting with PocketTerm II** over the Palm's serial line.

It is the file `hackmaster_y2035/apps/docs/user_guides/WiRSa.md` in the **Palm apps repository (`palmos_sdk`)**, next to the guides for each Palm app (`README.md` there is the index).

This repository's own reference documents:

| Document | Contents |
|---|---|
| [INSTALL.md](INSTALL.md) | Flashing the firmware and first start. |
| [BUILD.md](BUILD.md) | Building with VS Code and PlatformIO. |
| [README-Palm.md](README-Palm.md) | Every Palm mode in detail. |
| [Mesh-Palm.md](Mesh-Palm.md), [Relay-Modes.md](Relay-Modes.md) | The Meshtastic and relay modes. |
| [protocol/Protocol-wire.md](protocol/Protocol-wire.md) | The serial protocols between the Palm apps and the WiRSa. |
