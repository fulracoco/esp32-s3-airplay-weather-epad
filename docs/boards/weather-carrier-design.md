# ESP32-S3 Weather / AirPlay Carrier Design Draft

Latest: `hardware/weather-carrier/weather-carrier-r1-cs2-review.epro` connects
J4.9 CS2 to J1.4 GPIO4 in the schematic and PCB. GPIO4 is unused in this
project profile, and is not a strap, USB or N16R8 memory pin. New routing
requires EDA DRC. Firmware still uses CS1 only; verify CS2 pull-up before
connecting a second-CS load, since GPIO4 is high impedance during reset.

Previous: `hardware/weather-carrier/weather-carrier-r0-routed-review.epro` is
the latest editable project, exported from EDA Pro V3.2.186 after two-layer
trial routing. PCB DRC checked 124 items and reported zero issues on
2026-09-10. Schematic checks reported zero fatal/errors and 25 warnings
(reserved single-pin nets, J4.9 dangling, and supplier-attribute differences).
The generated `weather-carrier-r0.epro` remains the unrouted source draft.
Power/ground routing, ground pours, USB clearance and antenna keepout still
require final review; this is not a manufacturing release. The reference was
preserved in `reference-carrier.epro`. The development-board sockets are now photo-matched 1x22,
2.54 mm pitch, 25.4 mm row spacing. Module mating pitch remains to be checked. See `hardware/weather-carrier/README.md` for the
current review state and connector pin assignments.

## Configuration Basis

- Target: ESP32-S3-N16R8, `esp32s3-weather` environment.
- Sources: `config/sdkconfig.defaults.esp32s3-weather`,
  `components/weather_epd/include/weather_epd_config.h`, and
  `硬件接线定义.md`.
- Current generated `sdkconfig.esp32s3-weather` agrees with the I2S mapping
  and enables the page button on GPIO15.
- The generated native EPD configuration currently has its page button
  disabled. Regenerate that configuration from defaults to use this button.

## Proposed Circuit

Use a carrier for the purchased development board and external modules.
Retain access to both development-board USB connectors and the reset/boot
buttons. Use the development board's existing regulated supplies.

The following tables define signal connections, NOT physical connector pin
numbers. Confirm actual module pin order before assigning numbered pins or
selecting footprints. N16R8 describes memory capacity, not header geometry.

### J1/J2: Development Board Sockets

Two 1x22 sockets, 2.54 mm pitch, 25.4 mm row centers, 53.34 mm pin span.
Component-side view with antenna left and USB right: J1 lower, J2 upper;
both pin 1 at the antenna end. See connector-pinout.csv for all 44 pins.

### J3: PCM5102A Module Interface

| Module signal | Carrier net | ESP32 connection |
| --- | --- | --- |
| VIN | +5V | Board 5V supply |
| GND | GND | Real ground pin |
| BCK | I2S_BCK | GPIO38 |
| LCK / LRCK | I2S_LRCK | GPIO39 |
| DIN | I2S_DOUT | GPIO40 |
| SCK | I2S_MCLK_OPTION | GPIO41 through optional 0-ohm link |

The user confirmed that the purchased PCM5102A module VIN connects to 5V.
J3 pin 6 therefore remains on V5. All digital signals are 3.3V.

Check SCK grounding and FMT/XSMT module straps. Fit the MCLK link only when
the module accepts external MCLK and SCK is not strapped to ground. For a
three-wire configuration, follow the module's documented SCK strap procedure.
The firmware may continue outputting GPIO41 with the link absent.

Provide optional series-resistor footprints near the ESP32 for BCK, LRCK,
DOUT and MCLK; initially use 0 ohms and evaluate 22-33 ohms if needed after
signal measurement. Keep I2S routes short with a continuous ground reference.
Use the DAC module's existing analog output connector.

### J4: E-Paper Driver Interface

| Driver signal | Carrier net | ESP32 connection |
| --- | --- | --- |
| 3.3V | +3V3 | Board regulated 3V3 |
| GND | GND | Ground |
| BUSY | EPD_BUSY | GPIO7 |
| RST | EPD_RST | GPIO16 |
| DC | EPD_DC | GPIO17 |
| CS | EPD_CS | GPIO10 |
| SCK | EPD_SCK | GPIO12 |
| SDI / DIN | EPD_MOSI | GPIO11 |

Repository documentation names DESPI-C02, but the supplied photo shows a
v1.4 driver with nine pins: GND, 3V3, SCK, SDA, RST, DC, CS1, BUSY, CS2.
J4 follows that order; SDA means SPI MOSI and CS2 connects to GPIO4.
The user requested that CS2 connect to an available GPIO for future use;
current firmware uses one CS.
Do not connect the panel's FPC directly to this carrier interface.

Leave GPIO13 without an external EPD connection. Reserve GPIO6 as a labeled
EPD_EN test point; it is a logic control output, not a power source. The
initial design uses continuous 3.3V power as allowed by repository wiring
documentation. A later switched-power variant needs a rated load switch and
checks for back-power through the signal pins.

### J5: Optional BME280 Interface

| Module signal | Carrier net | ESP32 connection |
| --- | --- | --- |
| VCC | +3V3 | Regulated 3V3 |
| GND | GND | Ground |
| SDA | SENSOR_SDA | GPIO8 |
| SCL | SENSOR_SCL | GPIO9 |

For a six-pin module, tie CSB to 3V3 and SDO to GND for address 0x76.
Reserve two 4.7k pull-up footprints to 3V3, not populated when adequate
pull-ups already exist on the module. Confirm the effective bus pull-up
resistance before fitting additional resistors.

### SW1 / J6: Page Button

Connect a normally-open momentary switch between GPIO15 and GND, with a
parallel two-pin connector for an enclosure button. Firmware enables the
internal pull-up and debounces the button. Other playback buttons are
disabled in the current profile and are outside the initial circuit.

## Power and Layout

Firmware now reserves GPIO5 (J1.5) for a normally-open momentary power-mode
button to GND. See [button behavior and tests](../features/buttons.md#weather-power-mode-button).
This is an additional hand-wired connection; the existing EDA files do not
yet contain this button. GPIO4 remains EPD CS2 and GPIO15 remains page toggle.

- Use a single intended 5V source through the development board. Do not add
  a second directly paralleled supply connector without power-path design.
- Verify the board's regulator and USB power path can supply the ESP32,
  DAC module, sensor, and EPD refresh peak current together.
- Reserve local 100nF and 10uF bypass footprints at the DAC and EPD supply
  connectors; confirm module capacitance and regulator stability.
- Keep GPIO19/20 available for native USB. Do not route external GPIO loads
  onto memory-reserved pins or alter boot-strapping pins.
- Do not add a battery divider to GPIO7: it is EPD_BUSY. The generic
  configuration contains BAT_CHANNEL=7, which requires separate review
  before any battery-monitor function is enabled.
- Prefer two copper layers with continuous ground beneath signal routes.
  Determine board outline from the reference and measured modules.
- Put the antenna at the carrier edge and honor the actual module antenna
  keepout on every copper layer, including the ground plane.
- Place the DAC toward an edge with its audio socket accessible. Place the
  EPD connector near the display cable exit and BME280 away from heat sources.
- Add explicit signal/power silkscreen, pin-1 marks, and GND/3V3/5V test pads.
  Confirm mating orientation instead of relying on connector color.

## Required Before Fabrication

1. Inspect and preserve the reference EDA design, then create a separate
   carrier revision based on the verified development-board socket geometry.
2. Verify product photos or drawings for all three purchased modules,
   particularly connector order, pitch, operating voltage and mounting holes.
3. Capture the schematic with numbered pins and verified footprints.
4. Complete PCB placement/routing and inspect ERC, DRC, unconnected nets,
   outline, hole clearances, antenna keepout and connector accessibility.
5. Inspect fabrication previews and compare 1:1 printed placement against
   the physical boards before ordering.
6. On assembled hardware, check resistance between supply and ground, power
   from a current-limited source, and measure rails before inserting modules.
   Validate simultaneous AirPlay playback, EPD refresh, BME280 readings and
   page-button operation using ESP32-S3-N16R8 / esp32s3-weather.
