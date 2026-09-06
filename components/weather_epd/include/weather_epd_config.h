#pragma once

#include "driver/i2c_master.h"
#include "driver/spi_master.h"
#include "driver/uart.h"

/* ESP32-S3-N16R8 + GDEY075Z08 wiring migrated from esp32-weather-epd.
 * Change these definitions for a custom carrier board. */
#define WEATHER_EPD_SPI_HOST      SPI2_HOST
#define WEATHER_EPD_SCK_GPIO      12
#define WEATHER_EPD_MOSI_GPIO     11
#define WEATHER_EPD_MISO_GPIO     13
#define WEATHER_EPD_CS_GPIO       10
#define WEATHER_EPD_DC_GPIO       17
#define WEATHER_EPD_RST_GPIO      16
#define WEATHER_EPD_BUSY_GPIO     7
#define WEATHER_EPD_PWR_GPIO      6

#define WEATHER_SENSOR_SDA_GPIO   8
#define WEATHER_SENSOR_SCL_GPIO   9
#define WEATHER_SENSOR_I2C_PORT   I2C_NUM_0
#define WEATHER_SENSOR_BME280_ADDR 0x76

/* PM1502A / Plantower-compatible particulate sensor.  TX from the sensor is
 * connected to the ESP32 RX pin through a 5 V to 3.3 V level shifter. */
#define WEATHER_PM_UART_NUM        UART_NUM_1
#define WEATHER_PM_RX_GPIO         5
#define WEATHER_PM_TX_GPIO         4
#define WEATHER_PM_BAUD            9600

/* PCM5102A external I2S DAC.  The legacy mapping (BCK=11, DIN=12,
 * LRCK=13, SCK=8) overlaps the e-paper SPI and BME280 SDA. */
#define WEATHER_I2S_BCK_GPIO      38
#define WEATHER_I2S_DIN_GPIO      40
#define WEATHER_I2S_LRCK_GPIO     39
#define WEATHER_I2S_SCK_GPIO      41

/* Optional OpenWeatherMap One Call 3.0 integration defaults. These are used
 * only when no values have been saved from the web configuration page. */
#define WEATHER_OWM_API_KEY       ""
#define WEATHER_OWM_LATITUDE      "40.7128"
#define WEATHER_OWM_LONGITUDE     "-74.0060"
#define WEATHER_OWM_CITY          "Weather"

/* Time settings used by the calendar when no values have been saved from the
 * Weather Settings page. */
#define WEATHER_DEFAULT_TIMEZONE  "CST-8"
#define WEATHER_DEFAULT_NTP_SERVER "pool.ntp.org"
#define WEATHER_DEFAULT_HOLIDAY_ENDPOINT \
  "https://cdn.jsdelivr.net/gh/NateScarlet/holiday-cn@master/{year}.json"
