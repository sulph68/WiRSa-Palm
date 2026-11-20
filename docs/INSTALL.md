# Installing the WiRSa firmware

How to put the firmware on the board and start using it. To build the firmware first, see [BUILD.md](BUILD.md). Using the WiRSa after that is covered in the user guide ([USER_GUIDE.md](USER_GUIDE.md)).

## What you need
- The WiRSa board (a generic ESP32-S3 board with OLED and buttons, or a LilyGO T7-S3).
- A USB cable that carries data (not a charge-only cable).
- The firmware: built from this repository ([BUILD.md](BUILD.md)). The steps below use PlatformIO inside VS Code, or its `pio` command.
- Optional: a micro SD card (FAT16 or FAT32) and the serial cable to the Palm.

## 1. Connect the board
1. Plug the USB cable into the board's USB port. On the generic board this is the port with the USB-serial chip; on the T7-S3 it is the board's USB-C port.
2. The computer shows a new serial port:

   | System | Port looks like |
   |---|---|
   | Linux | `/dev/ttyACM0` or `/dev/ttyUSB0` (a stable name is in `/dev/serial/by-id/`) |
   | macOS | `/dev/cu.usbmodem...` or `/dev/cu.wchusbserial...` |
   | Windows | `COM3`, `COM4`... (see Device Manager > Ports) |

   Nothing shows up? Try another cable (data, not charge-only) or install the USB-serial driver for your board.

## 2. Flash
**In VS Code:** open the `Firmware` folder, choose the environment (`esp32s3` for the OLED board, `t7s3` for the T7-S3), then click the **right arrow** in the status bar (*PlatformIO: Upload*).

**On the command line,** from `Firmware`:

```
pio run -e esp32s3 -t upload
pio run -e esp32s3 -t upload --upload-port /dev/ttyACM0
```

PlatformIO picks the port itself when only one board is connected. If you have several serial devices, name the port with `--upload-port`, or set `upload_port` in `platformio.ini` (the file has a commented example). Close any serial monitor first; only one program can use the port.

The upload builds the firmware if needed, writes it, and restarts the board.

### If the upload cannot connect
The board has to be in its download mode:
1. **Hold BOOT** (GPIO0 button).
2. While holding it, press and release **RESET** (or plug in the USB cable).
3. Release BOOT, then start the upload again.

After flashing, press RESET once to leave the download mode.

### What the first flash does
- It writes the **partition table** `partitions_vdisk.csv`: two 1.9 MB application slots and a **12 MB raw partition for the virtual card**. The table is rewritten by every normal upload; **settings** (WiFi, FTP user, and so on) and the application slots keep their places.
- The virtual card is formatted the first time something uses it (SASD, HotSync or the FTP server). It is named `ESP32`.
- To erase the virtual card's contents, erase the partition: `esptool.py erase_region 0x400000 0xC00000` (it is formatted again on next use).
- A board that has run other firmware may need its settings area cleared first: `pio run -e esp32s3 -t erase` erases the **whole flash**, including the virtual card.

## 3. First start
1. Open a serial monitor at **115200** baud (`pio device monitor -b 115200`), then reset the board. The console shows the start-up messages and then the main menu:

   ```
   -=-=- MAIN MENU -=-=-
    [F]TP Server
    [H]otSync
    SAS[D] SDCard
    [S]LIP Gateway
    [B]itWarden Relay
    C[A]lDAV Relay
    Mesh[T]astic
    [U]tilities
    [C]onfig
   ```

   On the OLED version the same menu is on the screen.
2. **Join your WiFi:** `C` then `W` (or Config > WiFi Setup with the buttons). Pick your network and enter its password. The WiFi icon in the title bar fills with bars when connected, and **Utilities > Information** shows the IP address.
3. Optional: **Config > FTP Setup** (user and password), **Config > SLIP Setup** (addresses), and **Baud Rate (Max)**.
4. Connect the serial cable to the Palm and continue with the Palm side: install the apps (`hackmaster_y2035/apps/docs/INSTALL.md` in the Palm apps repository) and read the user guide ([USER_GUIDE.md](USER_GUIDE.md)).

## 4. Updating
Flash the new build the same way. Settings are kept. If a release changes the partition layout, the release notes say so; in that case flash once with the board's flash erased.

## 5. If it does not work
| Symptom | Try |
|---|---|
| The port is not listed | A data USB cable; the board's driver; a different USB port. |
| `Failed to connect to ESP32-S3` | Put the board in download mode (hold BOOT, tap RESET); close the serial monitor. |
| The upload works but the screen stays dark | The wrong environment for the board; for the T7-S3 (no screen) use `t7s3` and a serial console. |
| The console shows nothing | 115200 baud; press RESET with the monitor already open. Some boards restart when the port is opened: wait for the menu. |
| Settings are odd after switching firmware | Erase the flash once (`-t erase`) and flash again. |
