# Building the WiRSa firmware

The firmware is an Arduino-framework project for the ESP32-S3, built with **PlatformIO** inside **Visual Studio Code**. This page sets up the environment and builds it. To put the result on a board, see [INSTALL.md](INSTALL.md).

## 1. Install the tools
1. **Visual Studio Code** (https://code.visualstudio.com).
2. In VS Code, open **Extensions** and install **PlatformIO IDE** (publisher: PlatformIO). It installs PlatformIO Core, the `pio` command, and the compilers on first start. Wait for its first-run setup to finish (the status bar shows PlatformIO's home icon when it is done).
3. **Git**, to get the source.
4. A **USB driver** for your board's USB-serial chip if your operating system does not already have one (the generic ESP32-S3 boards use a WCH CH343 or similar; the LilyGO T7-S3 uses the chip's own USB and needs none on Linux and macOS).

PlatformIO downloads everything else when the project is first built: the **espressif32** platform (pinned to **7.0.1** in `platformio.ini`), the Arduino core, the toolchain, and the libraries listed under `lib_deps` (Adafruit GFX, Adafruit SSD1306, arduino-timer, ArduinoJson, NimBLE-Arduino). That first build needs an internet connection and takes several minutes.

Linux only: add your user to the serial-port group (`dialout` on most distributions) so that PlatformIO can open the port, then log out and in again.

## 2. Open the project
1. Get the source: `git clone` the WiRSa repository.
2. In VS Code choose **File > Open Folder** and open the **`Firmware`** folder (the one that contains `platformio.ini`), not the repository root. PlatformIO detects the project and indexes it.

The layout of `Firmware/`:

| Path | What it is |
|---|---|
| `platformio.ini` | The environments (one per board type), build flags and libraries. |
| `partitions_vdisk.csv` | The flash partition table (the 12 MB virtual card lives here). |
| `src/main.cpp`, `src/modules/` | The firmware. One file per mode (FTP, HotSync, SASD, SLIP, relays) and shared parts. |
| `include/` | Headers. `globals.h` holds the pin layouts, feature switches and defaults. |
| `lib/` | Project-local libraries. |
| `test_host/`, `tools/` | Tests and checks that run on the computer, without a board. |

## 3. Pick the environment
`platformio.ini` has one environment per hardware:

| Environment | Hardware |
|---|---|
| `esp32s3` (the default) | A generic ESP32-S3 board with the OLED screen, four buttons and an RGB status LED, and its console on a USB-serial chip. |
| `t7s3` | The LilyGO T7-S3: no OLED or buttons (control it from the Palm), three status LEDs, native USB. |

Both main environments set a 16 MB flash, the octal PSRAM, `board_build.partitions = partitions_vdisk.csv`, and the flags that the firmware needs (for example `-Wl,--wrap=uart_write_bytes` and `-Wl,--wrap=uart_read_bytes`, which let it see Palm serial traffic for the status LED). Leave those as they are.

In VS Code, click the environment name in the status bar (bottom of the window) to choose one, or the PlatformIO sidebar's **Project Tasks**. On the command line the choice is `-e <name>`.

## 4. Build
- **VS Code:** click the **checkmark** in the status bar (*PlatformIO: Build*), or in the PlatformIO sidebar choose *Project Tasks > <environment> > General > Build*.
- **Command line** (PlatformIO's terminal, or any shell where `pio` is on the path), from the `Firmware` folder:

```
pio run -e esp32s3
pio run -e t7s3
```

The result is `.pio/build/<environment>/firmware.bin`. A successful build ends with a table of RAM and flash use. The program uses about 1.3 MB of flash; each application slot in the partition table is 1.9 MB, so there is room for roughly 600 KB more. A build that comes close to the slot size fails at the link step.

## 5. Options you may want to change
These are in `include/globals.h` unless noted. Rebuild after any change.

| Setting | Effect |
|---|---|
| `WIRSA_SCREENLESS` | `1` builds for hardware with **no OLED and no buttons**: display calls do nothing and everything is controlled from the Palm (WiRSaControl). |
| `WIRSA_FEATURE_FTPSERVER`, `_HOTSYNC`, `_SASD`, `_SLIP`, `_BITWARDEN`, `_CALDAV`, `_MESH`, `_UTILITIES` | `0` removes that mode and its menu entry, and saves flash. |
| `WIRSA_FEATURE_PALMCTL` | `0` turns off the Palm control link used by WiRSaControl. |
| `WIRSA_FEATURE_DUAL_BAUD`, `_AUTOBAUD` | The 9600-first menu speed handling and the `SETBAUD`/`BR` speed measurement (see the user guide). |
| `WIRSA_LED_*` and `-DWIRSA_LED_NEOPIXEL=<pin>` (build flag) | The status LED type and its brightness. |
| `WIRSA_DEEP_SLEEP` (build flag) | Deep sleep support (needed for Meshtastic Background mode). |

## 6. Checks that need no board
`Firmware/tools/verify.sh` runs, in one go: the frame-table check against `docs/protocol/Protocol-wire.md` (`tools/check_wire.py`), the host tests in `test_host/` (they only need `g++`), and, if you give it a PlatformIO project directory, builds with each feature switch off in turn. Run it before flashing a change:

```
Firmware/tools/verify.sh --no-build
```

## 7. Serial monitor
To read the firmware's console output: *PlatformIO: Serial Monitor* (the plug icon in the status bar), or `pio device monitor -b 115200`. The monitor decodes crash backtraces (`esp32_exception_decoder`). Opening the port can restart some boards; wait for the menu.

## LED testing
For checking the status LED hardware only, not for normal use. The `t7s3_ledtest` environment is the T7-S3 build, but the firmware does nothing except light the red, green and blue LEDs one after the other (half a second each) and print which one on the console at 115200 baud. It is not listed with the environments above because the board is not a WiRSa while it runs.

```
pio run -e t7s3_ledtest -t upload
```

Flash a normal environment (`t7s3`) again afterwards. The test code is in `src/modules/led.cpp` under `WIRSA_LED_TEST`.

## Common problems
| Problem | Fix |
|---|---|
| The first build is very slow or fails downloading | It is fetching the platform and libraries; check the internet connection and retry. |
| `Please specify upload_port` / the port is busy | See [INSTALL.md](INSTALL.md): close the serial monitor and pick the port. |
| A library version error | PlatformIO caches libraries per project; use *PlatformIO: Clean* and build again. |
| The build runs out of flash | A feature switch is on that you do not need: set it to `0`, or use the T7-S3 environment if your board has 16 MB. |
| Everything compiles but the board does not behave | You built the wrong environment for the board. |
