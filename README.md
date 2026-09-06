<div align="center">

<img src="docs/assets/logo_airplay_esp32.png" alt="AirPlay ESP32" width="400">

# ESP32 AirPlay 2 Receiver

**Stream music from your Apple devices — or from any phone over Bluetooth — to any speaker for about $10**

[![GitHub stars](https://img.shields.io/github/stars/rbouteiller/airplay-esp32?style=flat-square)](https://github.com/rbouteiller/airplay-esp32/stargazers)
[![GitHub forks](https://img.shields.io/github/forks/rbouteiller/airplay-esp32?style=flat-square)](https://github.com/rbouteiller/airplay-esp32/network)
[![License](https://img.shields.io/badge/license-Non--Commercial-blue?style=flat-square)](LICENSE)
[![ESP-IDF](https://img.shields.io/badge/ESP--IDF-v5.5+-red?style=flat-square)](https://docs.espressif.com/projects/esp-idf/)
[![Platform](https://img.shields.io/badge/platform-ESP32%20%7C%20S2%20%7C%20S3%20%7C%20C5-green?style=flat-square)](https://www.espressif.com/en/products/socs)

### [Documentation](https://rbouteiller.github.io/airplay-esp32/) · [Install from your browser](https://rbouteiller.github.io/airplay-esp32/getting-started/flashing/) · [Troubleshooting](https://rbouteiller.github.io/airplay-esp32/troubleshooting/)

</div>

---

## What is this?

This turns a cheap ESP32 board into a wireless AirPlay 2 speaker. Plug it into any
amplifier or powered speakers and it shows up on your iPhone, iPad or Mac just like a
HomePod or an AirPlay TV.

This repository is a weather e-paper extension of
[rbouteiller/airplay-esp32](https://github.com/rbouteiller/airplay-esp32). It keeps the
AirPlay/DLNA audio receiver as the real-time core and adds an ESP32-S3 weather display
profile for a GDEY075Z08 e-paper panel, BME280 sensor, calendar pages and a web-based
configuration flow. The display service runs independently from the audio tasks so
e-paper refreshes do not block playback.

Works with **ESP32**, **ESP32-S2**, **ESP32-S3** and **ESP32-C5** chips, including the
[SqueezeAMP](https://github.com/philippe44/SqueezeAMP) (ESP32 + TAS5756) and
[Esparagus Audio Brick](https://sonocotta.com/espragus-audio-brick/) (ESP32 + TAS5825M)
boards, which have amplifiers built in.

ESP32-based boards also support **Bluetooth A2DP**, so anything that can pair with a
Bluetooth speaker can play to them when AirPlay is idle. The Esparagus Audio Brick
additionally supports **wired Ethernet** through an optional W5500 module.

**No cloud. No app. Just tap and play.**

## Quick start

The fastest route is the browser installer — no toolchain, no command line:

**[→ Install from your browser](https://rbouteiller.github.io/airplay-esp32/getting-started/flashing/)**

Building from parts instead? You need an ESP32-S3 dev board, a PCM5102A DAC and a female
pin header, roughly $10 total, and no soldering. See the
[getting started guide](https://rbouteiller.github.io/airplay-esp32/getting-started/).

Building from source:

```bash
git clone --recursive https://github.com/rbouteiller/airplay-esp32
cd airplay-esp32
pio run -e esp32s3 -t upload
pio run -e esp32s3 -t uploadfs   # required — writes the web UI to SPIFFS
```

## Features

- **AirPlay 2** — appears natively in Control Center and every AirPlay-capable app
- **ALAC and AAC decoding** — live streaming (Siri, calls) and music playback
- **Multi-room** — PTP-based timing for synchronised playback
- **Bluetooth A2DP** — receive audio from phones and tablets (ESP32 boards only)
- **W5500 Ethernet** — wired networking with automatic WiFi failover
- **Web configuration** and **OTA updates** — USB is only needed for the first flash
- **48 kHz output** — optional 44.1 → 48 kHz conversion via a sinc resampler
- **USB DAC output** — ESP32-S3 USB Host support for UAC 1.0 DACs
- **Displays** — optional OLED or 320×170 colour TFT with track metadata
- **Hardware buttons** — optional play/pause, volume and track skip

Audio only, one speaker per board, and a decent WiFi signal is required.

## Documentation

| | |
| --- | --- |
| [Getting started](https://rbouteiller.github.io/airplay-esp32/getting-started/) | Shopping list, assembly, flashing, first boot |
| [Supported boards](https://rbouteiller.github.io/airplay-esp32/boards/) | SqueezeAMP, Esparagus Audio Brick, XIAO ESP32-C5, custom boards |
| [Features](https://rbouteiller.github.io/airplay-esp32/features/bluetooth/) | Bluetooth, Ethernet, displays, buttons, AirPlay tuning |
| [Reference](https://rbouteiller.github.io/airplay-esp32/reference/build-environments/) | Build environments, SPIFFS, OTA, architecture |
| [Troubleshooting](https://rbouteiller.github.io/airplay-esp32/troubleshooting/) | No sound, no setup WiFi, dropouts, build errors |

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md) and the
[contributing guide](https://rbouteiller.github.io/airplay-esp32/contributing/).
Documentation lives in [`docs/`](docs/) and is built with [Zensical](https://zensical.org/)
— every page on the site has an edit link that takes you straight to the GitHub editor.

## Acknowledgements

- [Shairport Sync](https://github.com/mikebrady/shairport-sync) — the reference AirPlay implementation
- [openairplay/airplay2-receiver](https://github.com/openairplay/airplay2-receiver) — Python AirPlay 2 implementation
- [Espressif](https://github.com/espressif) — ESP-IDF framework and codec libraries

## Upstream projects and references

This project combines code, design ideas and protocol knowledge from the following
open-source projects. Please consult each upstream repository for its complete license
and attribution requirements.

- [rbouteiller/airplay-esp32](https://github.com/rbouteiller/airplay-esp32) — upstream
  AirPlay 2/RAOP receiver foundation, including RTSP/HAP handling, DLNA rendering,
  network setup, audio output backends and the web configuration interface.
- [tsl0922/EPD-nRF5](https://github.com/tsl0922/EPD-nRF5) — reference for e-paper
  calendar interaction and presentation ideas, including Chinese lunar dates, solar
  terms, holidays and workday adjustments across different panel sizes.
- [lmarzen/esp32-weather-epd](https://github.com/lmarzen/esp32-weather-epd) — source
  of the weather display renderer and related configuration concepts used by the
  `components/weather_epd/arduino_port/` snapshot, including OpenWeatherMap data,
  BME280 readings, weather icons and GDEY075Z08-oriented rendering.

Code imported or adapted from upstream projects retains its original copyright and
license notices in the relevant source files. The top-level project license applies
only where it does not conflict with those notices.

## Legal

**Non-commercial use only.** Commercial use requires explicit permission — see [LICENSE](LICENSE).

This is an independent project based on protocol analysis. Not affiliated with Apple Inc.
Not guaranteed to work with future iOS or macOS versions. Provided as-is without warranty.
