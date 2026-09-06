#include "weather_epd.h"
#include "weather_epd_config.h"
#include "driver/i2c_master.h"
#include "driver/uart.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "bme280";
static i2c_master_dev_handle_t s_dev;
static bool s_ready;
static uint16_t dig_t1, dig_p1, dig_h1;
static int16_t dig_t2, dig_t3, dig_p2, dig_p3, dig_p4, dig_p5, dig_p6, dig_p7,
    dig_p8, dig_p9, dig_h2, dig_h3, dig_h4, dig_h5, dig_h6;
static int32_t t_fine;
#if CONFIG_WEATHER_PM1502A_ENABLED
static bool s_pm_ready;

static void pm1502_init(void) {
  if (s_pm_ready) return;
  const uart_config_t cfg = {
      .baud_rate = WEATHER_PM_BAUD,
      .data_bits = UART_DATA_8_BITS,
      .parity = UART_PARITY_DISABLE,
      .stop_bits = UART_STOP_BITS_1,
      .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
      .source_clk = UART_SCLK_DEFAULT,
  };
  if (uart_driver_install(WEATHER_PM_UART_NUM, 512, 0, 0, NULL, 0) == ESP_OK &&
      uart_param_config(WEATHER_PM_UART_NUM, &cfg) == ESP_OK &&
      uart_set_pin(WEATHER_PM_UART_NUM, WEATHER_PM_TX_GPIO, WEATHER_PM_RX_GPIO,
                   UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE) == ESP_OK) {
    s_pm_ready = true;
  }
}

static void pm1502_read(weather_sensor_data_t *out) {
  if (!s_pm_ready) return;
  uint8_t buf[96];
  int n = uart_read_bytes(WEATHER_PM_UART_NUM, buf, sizeof(buf), 0);
  for (int i = 0; i + 24 < n; ++i) {
    if (buf[i] != 0x42 || buf[i + 1] != 0x4d || buf[i + 2] != 0x00 ||
        buf[i + 3] != 0x1c) continue;
    uint16_t sum = 0;
    /* The PM1502A/Plantower checksum covers header, length and payload, but
     * excludes the final two checksum bytes themselves. */
    for (int j = 0; j < 30; ++j) sum += buf[i + j];
    uint16_t expected = ((uint16_t)buf[i + 30] << 8) | buf[i + 31];
    if (sum != expected) continue;
    out->pm25_ugm3 = (float)(((uint16_t)buf[i + 12] << 8) | buf[i + 13]);
    out->pm10_ugm3 = (float)(((uint16_t)buf[i + 14] << 8) | buf[i + 15]);
    return;
  }
}
#endif

static esp_err_t reg_write(uint8_t reg, uint8_t value) {
  uint8_t b[2] = {reg, value};
  return i2c_master_transmit(s_dev, b, sizeof(b), 100);
}
static esp_err_t reg_read(uint8_t reg, uint8_t *data, size_t len) {
  return i2c_master_transmit_receive(s_dev, &reg, 1, data, len, 100);
}

esp_err_t weather_bme280_init(void) {
  if (s_ready) return ESP_OK;
  i2c_master_bus_config_t bus_cfg = {
      .i2c_port = WEATHER_SENSOR_I2C_PORT,
      .sda_io_num = WEATHER_SENSOR_SDA_GPIO,
      .scl_io_num = WEATHER_SENSOR_SCL_GPIO,
      .clk_source = I2C_CLK_SRC_DEFAULT,
      .glitch_ignore_cnt = 7,
      .flags.enable_internal_pullup = true,
  };
  i2c_master_bus_handle_t bus;
  esp_err_t err = i2c_new_master_bus(&bus_cfg, &bus);
  if (err != ESP_OK) return err;
  i2c_device_config_t dev_cfg = {.dev_addr_length = I2C_ADDR_BIT_LEN_7,
                                 .device_address = WEATHER_SENSOR_BME280_ADDR,
                                 .scl_speed_hz = 100000};
  err = i2c_master_bus_add_device(bus, &dev_cfg, &s_dev);
  if (err != ESP_OK) return err;
  uint8_t id = 0;
  err = reg_read(0xD0, &id, 1);
  if (err != ESP_OK || id != 0x60) {
    ESP_LOGW(TAG, "BME280 not detected (id=0x%02x)", id);
    return ESP_ERR_NOT_FOUND;
  }
  uint8_t c[24];
  err = reg_read(0x88, c, sizeof(c));
  if (err != ESP_OK) return err;
  dig_t1 = c[0] | (c[1] << 8); dig_t2 = (int16_t)(c[2] | (c[3] << 8));
  dig_t3 = (int16_t)(c[4] | (c[5] << 8)); dig_p1 = c[6] | (c[7] << 8);
  dig_p2 = (int16_t)(c[8] | (c[9] << 8)); dig_p3 = (int16_t)(c[10] | (c[11] << 8));
  dig_p4 = (int16_t)(c[12] | (c[13] << 8)); dig_p5 = (int16_t)(c[14] | (c[15] << 8));
  dig_p6 = (int16_t)(c[16] | (c[17] << 8)); dig_p7 = (int16_t)(c[18] | (c[19] << 8));
  dig_p8 = (int16_t)(c[20] | (c[21] << 8)); dig_p9 = (int16_t)(c[22] | (c[23] << 8));
  uint8_t h1;
  err = reg_read(0xA1, &h1, 1); if (err != ESP_OK) return err;
  dig_h1 = h1;
  uint8_t h[7]; err = reg_read(0xE1, h, sizeof(h)); if (err != ESP_OK) return err;
  dig_h2 = (int16_t)(h[0] | (h[1] << 8)); dig_h3 = h[2];
  dig_h4 = (int16_t)((h[3] << 4) | (h[4] & 0x0f));
  dig_h5 = (int16_t)((h[5] << 4) | (h[4] >> 4)); dig_h6 = (int8_t)h[6];
  /* Forced measurement, 16x oversampling humidity/pressure/temperature. */
  reg_write(0xF2, 0x05); reg_write(0xF4, 0xB7); reg_write(0xF5, 0x10);
#if CONFIG_WEATHER_PM1502A_ENABLED
  pm1502_init();
#endif
  s_ready = true;
  return ESP_OK;
}

esp_err_t weather_bme280_read(weather_sensor_data_t *out) {
  if (!out) return ESP_ERR_INVALID_ARG;
  if (!s_ready) { esp_err_t e = weather_bme280_init(); if (e != ESP_OK) return e; }
  uint8_t raw[8];
  esp_err_t err = reg_read(0xF7, raw, sizeof(raw));
  if (err != ESP_OK) return err;
  int32_t adc_t = (raw[3] << 12) | (raw[4] << 4) | (raw[5] >> 4);
  int32_t adc_p = (raw[0] << 12) | (raw[1] << 4) | (raw[2] >> 4);
  int32_t adc_h = (raw[6] << 8) | raw[7];
  int32_t v1 = ((((adc_t >> 3) - ((int32_t)dig_t1 << 1)) * dig_t2) >> 11);
  int32_t v2 = (((((adc_t >> 4) - dig_t1) * ((adc_t >> 4) - dig_t1)) >> 12) * dig_t3) >> 14;
  t_fine = v1 + v2; out->temperature_c = (t_fine * 5 + 128) / 25600.0f;
  int64_t p1 = t_fine - 128000, p2 = p1 * p1 * dig_p6;
  p2 += (p1 * dig_p5) << 17; p2 += ((int64_t)dig_p4) << 35;
  p1 = ((p1 * p1 * dig_p3) >> 8) + ((p1 * dig_p2) << 12);
  p1 = (((((int64_t)1) << 47) + p1) * dig_p1) >> 33;
  if (p1 != 0) {
    int64_t p = 1048576 - adc_p;
    p = (((p << 31) - p2) * 3125) / p1;
    p1 = (dig_p9 * (p >> 13) * (p >> 13)) >> 25;
    p2 = (dig_p8 * p) >> 19; p = ((p + p1 + p2) >> 8) + ((int64_t)dig_p7 << 4);
    out->pressure_hpa = p / 25600.0f;
  }
  int32_t hv = t_fine - 76800;
  hv = ((((adc_h << 14) - ((int32_t)dig_h4 << 20) - ((int32_t)dig_h5 * hv)) +
         16384) >> 15) * (((((((hv * dig_h6) >> 10) * (((hv * dig_h3) >> 11) + 32768)) >> 10) +
         2097152) * dig_h2 + 8192) >> 14);
  hv -= (((((hv >> 15) * (hv >> 15)) >> 7) * dig_h1) >> 4);
  if (hv < 0) hv = 0;
  if (hv > 419430400) hv = 419430400;
  out->humidity_pct = hv / 4194304.0f; out->valid = true;
#if CONFIG_WEATHER_PM1502A_ENABLED
  pm1502_read(out);
#endif
  return ESP_OK;
}
