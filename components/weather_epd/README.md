# Weather EPD fusion component

This component is the ESP-IDF side of the `esp32-weather-epd` + `airplay-esp32`
fusion.  It is intentionally isolated from the audio tasks so a slow e-paper
refresh cannot interrupt AirPlay or DLNA playback.

The display profile follows the latest weather project adjustments:
GDEY075Z08 tri-color red accent, Simplified Chinese/GB2312 text, a combined
city-and-date header, and an alternating calendar page with Monday week start,
lunar dates, solar terms, and holiday markers. These options are centralized in
`include/weather_epd_display_config.h`.

第一阶段适配已完成：主工程通过 `include/weather_display.h` 调用显示功能，
Arduino 生命周期由 `arduino_display_bridge.cpp` 隔离。完整 renderer 快照位于
`arduino_port/`，默认不参与编译；需要锁定 Arduino-ESP32、GxEPD2、Adafruit
GFX、U8g2 和 Nongli 依赖后，再在 menuconfig 中启用
`CONFIG_WEATHER_EPD_ARDUINO_RENDERER`。

Default ESP32-S3-N16R8 wiring (copied from the weather project):

| Device | GPIO |
|---|---:|
| GDEY075Z08 SCK/MOSI/MISO/CS/DC/RST/BUSY | 12 / 11 / 13 / 10 / 17 / 16 / 7 |
| BME280 SDA/SCL | 8 / 9 |
| PCM5102A I2S BCLK/DOUT/LRCLK/MCLK | 38 / 40 / 39 / 41 |

The legacy PCM5102A wiring used BCK=GPIO11, DIN=GPIO12, LCK/LRCK=GPIO13,
SCK=GPIO8, which overlaps the e-paper and BME280 bus. The unified profile now
uses GPIO 38/40/39/41 (BCLK/DOUT/LRCLK/MCLK) to avoid conflicts; the base
`esp32s3-weather` environment uses this I²S output, while USB Host output is selected
with the `esp32s3-weather-usb-host` PlatformIO environment.

The current component provides the board bring-up path and scheduling API. The
GDEY waveform/LUT and sensor calibration tables should be validated against the
actual carrier revision before enabling unattended refreshes. Weather API and
Chinese holiday data are kept as the next integration layer so credentials are
not hard-coded into the firmware.
