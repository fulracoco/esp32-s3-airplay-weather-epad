# Repository Guidelines

## Project Structure & Module Organization

Firmware entry points and core services live in `main/`. Grouped subsystems include
`main/audio/`, `main/network/`, `main/rtsp/`, `main/hap/`, and `main/plist/`.
Reusable drivers, board definitions, and third-party integrations belong in
`components/`; keep board-specific pins and setup under `components/boards/<board>/`.
Layered ESP-IDF defaults are in `config/`. The SPIFFS web UI is served from
`data/www/`, while user documentation and images live in `docs/`. Build utilities are
in `scripts/`; generated `build/`, `sdkconfig`, and `sdkconfig.<env>` files are not
source files.

## Build, Test, and Development Commands

Install PlatformIO and clone submodules before building. The primary development board
is an ESP32-S3-N16R8 (16 MB flash, 8 MB PSRAM), represented by the default `esp32s3`
environment. Substitute another environment from `platformio.ini` only when targeting
different hardware.

```sh
pio run -e esp32s3 -t build      # compile firmware
pio run -e esp32s3 -t upload     # flash firmware over USB
pio run -e esp32s3 -t uploadfs   # flash data/ as SPIFFS
pio run -e esp32s3 -t monitor    # open the 115200-baud serial log
scripts/format.sh --check        # verify C/H formatting
scripts/lint.sh                  # run clang-tidy after a build
zensical serve                   # preview docs after installing docs/requirements.txt
```

ESP-IDF 5.5+ is also supported through `idf.py`. Remove the cached
`sdkconfig.<env>` after changing defaults so they are regenerated.

## Coding Style & Naming Conventions

C and headers follow `.clang-format`: LLVM base style, two-space indentation,
attached braces, and an 80-column limit. Use `snake_case` for functions and
variables, `UPPER_CASE` for macros, and descriptive module-prefixed public symbols
such as `audio_output_init`. Run `scripts/format.sh` to modify sources. `.clang-tidy`
enables analyzer, bug-prone, performance, portability, and selected readability
checks. Do not reformat vendored components.

## Testing Guidelines

There is no unit-test framework. At minimum, run the format check, lint, and a build
for each affected environment. For audio backends, boards, networking, or timing
changes, flash real hardware and exercise the changed behavior. Record the board,
environment, and manual test results (for example, `ESP32-S3-N16R8 / esp32s3`) in the
pull request.

## Commit & Pull Request Guidelines

Recent commits use concise, imperative subjects, for example `Fix USB device output
failing to link`; release housekeeping may use `chore:`. Keep each commit focused.
Pull requests should explain the problem and solution, link relevant issues, list
tested environments/hardware, and update related documentation in the same change.
Include screenshots for visible web UI or documentation changes, and ensure required
format, lint, build-matrix, and strict documentation checks pass.
