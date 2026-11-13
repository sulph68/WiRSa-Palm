# WiRSa-Palm Introduction

The discovery of the [WiRSa project](https://github.com/nullvalue0/WiRSa) heavily inspired me on how legacy hardware can be made to interface with the modern internet. Together with the SASD project by [Dmitry](https://dmitry.gr/index.php?r=05.Projects&proj=32.%20SASD), and the help of Claude, i have managed to modify the firmware to mode accurate fit how my Palm work for me.

This is a copy from the original [WiRSa project](https://github.com/nullvalue0/WiRSa). 
As it is so heavily modified, i decided to recreate a copy of it instead of just a fork.

Among other things, the goal of this port is to be a gateway for my older PalmPIlot devices, specifically the PalmIII and the PalmV, and the internet services that we now use.

Do review [README-Palm.md](docs/README-Palm.md) for more details.
The wire protocol between the Palm apps and WiRSa (every frame, the secured link, the control commands) is specified in [Protocol-wire.md](docs/protocol/Protocol-wire.md).

## Branches

- `main` is the general release.
- `improved` (this branch) is the variant I run myself. It differs from `main` in these areas:
  - The SLIP Gateway is reduced to start/stop from the main menu, its addresses are set under Config → SLIP Setup, and port forwarding is removed.
  - WiRSa can be controlled from the Palm with the WiRSaCtl app, and CalDAV Sync and BitWarden can switch WiRSa into their mode themselves.
  - **Mesh Relay (Meshtastic) mode** links the Palm app `meshtastic.prc` to a Meshtastic node over Bluetooth LE: chats, direct messages, the node list and the node's settings from the Palm (see `docs/Mesh-Palm.md`). With the Background mode on, WiRSa checks the node by itself at the chosen interval and keeps the texts for the Palm: it deep-sleeps between checks (a timer wake runs a 12 s Bluetooth check at 80 MHz; the Palm line wakes it for real), keeps the texts in a small RTC ring and on the SD card in `/Meshtastic/<node number>/` (up to four nodes, each with its own folder, one of them in Background mode), and the Palm app can empty or keep that copy (SD History). Direct messages show who really acknowledged them (the recipient or only a relay) and a failed one is retried once after a key exchange; WiRSa holds up to 500 nodes; WiRSa and the Palm keep each other alive (5 s PING, BYE both ways).
  - The seven Palm rates 9600, 14400, 19200, 28800, 38400, 57600 and 115200 are the ones offered everywhere; a Palm login script can set the speed with `SETBAUD` / `BR`, and the Palm line is flushed for 1.5 s after every reset.
  - It can be built without the OLED and buttons (`WIRSA_SCREENLESS` in `globals.h`).
  - The BOOT button starts HotSync; modes return to the main menu on their own (Config → Exit Timers); SLIP Gateway stops after a quiet spell.
  - The 16 MB flash holds a 12 MB **virtual card** (volume `ESP32`) next to the SD card. SASD and HotSync use the SD card or the virtual card (WiRSaCtl Setup → Disk), and the FTP server shows both as folders. It needs the `partitions_vdisk.csv` partition table.
  - Leveled logging with a session log you can read from the Palm (WiRSaCtl → Help → Show Logs), and a Help → System page with the chip, memory, flash and partitions.
  - The main menu starts at 9600 after every boot, and each Palm's WiRSaCtl moves it to that Palm's own speed (its "Pref Baud"), so Palms of different speeds share one WiRSa. The configured baud rate is the maximum for syncs.

  See [README-Palm.md](docs/README-Palm.md), "Controlling WiRSa from the Palm".

# Hardware

The basic hardware requirement to recreate this is as follows

1. ESP32-S3 (with 2 UART outputs)
2. OLED screen SD1306 128x64 over I2C
3. 4 Push buttons (directly connected to GPIO pins)
4. MicroSD Card reader over SPI
5. MAX3232 RS232 to TTL (tested up to 115200 baud)

The virtual card on the `improved` branch needs 16 MB of flash (a LilyGO T7-S3 has it) and the partition table `Firmware/partitions_vdisk.csv`, set in `Firmware/platformio.ini` as `board_build.partitions = partitions_vdisk.csv` (with `board_upload.flash_size = 16MB`). The table keeps `min_spiffs.csv`'s two 1.9 MB app slots, so an upload does not move anything, and without it WiRSa works as before without the virtual card.

On the `improved` branch the OLED and the push buttons are optional: build with `WIRSA_SCREENLESS 1` and control WiRSa from the Palm instead.

`Firmware/platformio.ini` has two environments: `pio run -e t7s3` for the LilyGO T7-S3 and `pio run -e esp32s3` for a plain ESP32-S3 board with the OLED and buttons (UART console, no `-DWIRSA_BOARD_T7S3`), which builds with the default pins of `globals.h`. The second environment sets these (for another board of that kind): `-DCONFIG_BT_NIMBLE_ROLE_PERIPHERAL_DISABLED -DCONFIG_BT_NIMBLE_ROLE_BROADCASTER_DISABLED -DCONFIG_BT_NIMBLE_MAX_CONNECTIONS=1 -DCONFIG_NIMBLE_CPP_LOG_LEVEL=0` and `h2zero/NimBLE-Arduino@^1.4.3` (Mesh), and `board_upload.flash_size = 16MB` with `partitions_vdisk.csv` (without it the bootloader rejects the table and the board resets in a loop). With 8 MB octal PSRAM add `-DBOARD_HAS_PSRAM` and `board_build.arduino.memory_type = qio_opi`; then buttons BACK and ENTER must not be on GPIO 36/37 (PSRAM pins), so `SW2_PIN` and `SW3_PIN` are 18 and 8. Deep sleep is off on that board unless you add `-DWIRSA_DEEP_SLEEP`: the Palm RX (`RXD2`, GPIO 16) wakes the board and needs no extra wire; the OLED is switched off before the sleep, and a pressed button counts as activity.

The `esp32s3` environment has `-DWIRSA_LED_NEOPIXEL=48` for its WS2812 status LED (an RGB module on GPIO 48); the `t7s3` environment has three LEDs (red GPIO4, green GPIO7, blue GPIO8). Blue is steady while the WiFi or BLE is up and flickers on WiFi / BLE traffic, green flickers on Palm serial traffic, red on SD traffic; the BOOT button (HotSync) blinks red. The level (default 1 % on the WS2812) is set in `globals.h` or with build flags. The plain LED on `LED_PIN` keeps showing the WiFi state and SD traffic. The full table is in `docs/README-Palm.md`, "Status LED".

Optionals
1. 5V voltage regulator
2. Li-ion single cell battery charger
3. Hardware switch

# Further Notes

I'm not a great one at doing documentation but i'll try.

## AI Involvement Disclosure

Claude did generate a large amount of the code. It can generate, troubleshoot, consolidate much more than i can with the time i can spend on this.
While i tested it manually and reviewed some of it, it might not be without issues.
I have tried to keep most of the comments and code generated by Claude and tried my best to remove any potentially sensitive information.

What the AI generates, the AI can fix. 

If you find issues and bugs, feel free to improve upon the project, but do provide a link back to this.

## Hardware Schematics

I did not update nor document my hardware schematics as it matches very closely with the original. But it is nothing special about it. The only thing to take note is that the UART lines should be connected to the specific UART lines in the ESP32-S3. Otherwise, you can use any line you desire. Just update the code to reference the pins correctly. These are all defined in `globals.h`.

## Original WiRSa README

The original WiRSa README is [here](docs/README-Original.md).

# License

MIT License.

Copyright (C) 2026 Benjamin Khoo 

