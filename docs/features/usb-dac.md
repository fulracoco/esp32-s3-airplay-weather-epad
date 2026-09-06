# USB DAC output

An ESP32-S3 can use its native USB-OTG peripheral as a host and send decoded
AirPlay audio directly to an external USB DAC. This is different from the
existing USB Device UAC backend, where the ESP32 appears as an audio source to
a PC.

## Requirements and limitations

- ESP32-S3, ESP32-S2, or ESP32-P4 with the native USB D-/D+ signals exposed.
- A UAC 1.0 DAC supporting stereo, 16-bit PCM at 48 kHz. UAC 2.0-only DACs are
  not supported by the current Espressif host driver.
- A port or powered OTG hub that supplies regulated 5 V on USB VBUS. The ESP32
  USB peripheral does not power the DAC by itself.

On ESP32-S3, native USB uses GPIO20 (D+) and GPIO19 (D-). Verify that the
board's Type-C connector is wired to those pins rather than only to a
CH340/CP210x USB-to-UART bridge. Do not tie together independent 5 V supplies;
follow the board's VBUS-switch or OTG-power documentation.

## Build and flash

PlatformIO provides a dedicated environment:

```bash
# Build the firmware without flashing it
pio run -e esp32s3-usb-host

# Build and flash the firmware over USB
pio run -e esp32s3-usb-host -t upload

# Flash the SPIFFS image containing the web UI and data files
pio run -e esp32s3-usb-host -t uploadfs
```

The standalone application image is written to
`.pio/build/esp32s3-usb-host/firmware.bin`. The first `upload` command writes
the firmware, bootloader, and partition table; `uploadfs` is a separate,
required step for the web UI and bundled data files.

If the `pio` executable is not on `PATH` but PlatformIO is installed as a
Python module, replace `pio` with `python -m platformio` in these commands.

With ESP-IDF 5.5 or newer:

```bash
idf.py set-target esp32s3
idf.py -DSDKCONFIG_DEFAULTS="config/sdkconfig.defaults;config/sdkconfig.defaults.esp32s3;config/sdkconfig.defaults.usb-host" build flash
```

Flash before connecting the DAC when programming and USB Host share the same
native Type-C port. Use UART or OTA for later updates if the port must remain in
host mode.

## Runtime behavior

Connect the DAC through a USB-C OTG adapter or powered hub. The serial log
reports its VID/PID and whether a compatible playback interface was selected.
The first compatible DAC is used; it may be unplugged and reconnected while the
firmware is running. AirPlay 44.1 kHz audio is resampled to 48 kHz automatically.
