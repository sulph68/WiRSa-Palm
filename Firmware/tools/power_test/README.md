# power_test

The sketch behind the power table in `docs/Mesh-Palm.md`: eight 25 s phases (awake at 240/160/80/40 MHz, Bluetooth listening at 240/80 MHz, light sleep, deep sleep), each printing `PHASE n/8 ...` first. Build it as its own PlatformIO project (`platformio.ini` is not kept in this repository) with this configuration:

```ini
[env:t7s3]
platform = espressif32
board = esp32-s3-devkitc-1
framework = arduino
upload_protocol = esptool
build_flags =
    -DCORE_DEBUG_LEVEL=0
    -DARDUINO_USB_MODE=1
    -DARDUINO_USB_CDC_ON_BOOT=1
    -DBOARD_HAS_PSRAM
board_upload.flash_size = 16MB
board_build.arduino.memory_type = qio_opi
board_build.flash_mode = qio
board_build.f_cpu = 240000000L
board_build.f_flash = 80000000L
board_build.partitions = default_16MB.csv
lib_deps =
    h2zero/NimBLE-Arduino@^1.4.3
```

Put `src/main.cpp` next to it and run `pio run -t upload`. The board is on USB for the six awake phases (150 s of each 200 s cycle).
