#include "weather_epd.h"
#include "weather_epd_config.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include <string.h>
#include <stdio.h>
#include <time.h>

#include "calendar_photo_store.h"

#if CONFIG_WEATHER_EPD_ARDUINO_RENDERER
extern esp_err_t weather_display_arduino_probe(void);
extern esp_err_t weather_display_arduino_refresh(weather_epaper_page_t page,
                                                  const weather_sensor_data_t *sensor,
                                                  const char *location,
                                                  const char *weather_text);
#endif

static spi_device_handle_t s_spi;
static bool s_ready;
static const char *TAG = "weather_epd_drv";
static uint8_t s_black[WEATHER_EPD_WIDTH * WEATHER_EPD_HEIGHT / 8];
static uint8_t s_red[WEATHER_EPD_WIDTH * WEATHER_EPD_HEIGHT / 8];

static void cmd(uint8_t c) { spi_transaction_t t = {.length = 8, .tx_buffer = &c}; gpio_set_level(WEATHER_EPD_DC_GPIO, 0); spi_device_transmit(s_spi, &t); }
static void data(const uint8_t *p, size_t n) {
  gpio_set_level(WEATHER_EPD_DC_GPIO, 1);
  while (n > 0) {
    size_t chunk = n > 4092 ? 4092 : n;
    spi_transaction_t t = {.length = chunk * 8, .tx_buffer = p};
    esp_err_t err = spi_device_transmit(s_spi, &t);
    if (err != ESP_OK) return;
    p += chunk;
    n -= chunk;
  }
}
static void set_full_window(void) {
  uint8_t x[] = {0x00, 0x00, 0x63, 0x00};
  uint8_t y[] = {0x00, 0x00, 0x01, 0xdf, 0x00};
  cmd(0x44); data(x, sizeof(x));
  cmd(0x45); data(y, sizeof(y));
  cmd(0x4e); uint8_t x0[] = {0x00}; data(x0, 1);
  cmd(0x4f); uint8_t y0[] = {0x00, 0x00}; data(y0, 2);
}
static void wait_busy(void) {
  int initial = gpio_get_level(WEATHER_EPD_BUSY_GPIO);
  int i = 0;
  for (; i < 300 && !gpio_get_level(WEATHER_EPD_BUSY_GPIO); ++i) vTaskDelay(pdMS_TO_TICKS(100));
  ESP_LOGI(TAG, "BUSY initial=%d final=%d wait_ms=%d", initial,
           gpio_get_level(WEATHER_EPD_BUSY_GPIO), i * 100);
}
/* GxEPD2/UC8179 bitmap polarity: 1 is white, 0 is the ink color. */
static void pixel(uint8_t *b, int x, int y, bool on) { if (x < 0 || x >= WEATHER_EPD_WIDTH || y < 0 || y >= WEATHER_EPD_HEIGHT) return; size_t n = (size_t)y * WEATHER_EPD_WIDTH + x; if (on) b[n / 8] &= ~(0x80 >> (n & 7)); else b[n / 8] |= (0x80 >> (n & 7)); }
static void box(uint8_t *b, int x, int y, int w, int h) { for (int yy = y; yy < y + h; ++yy) for (int xx = x; xx < x + w; ++xx) pixel(b, xx, yy, true); }
static void mark_red(uint8_t *b, int x, int y, int w, int h) { for (int yy = y; yy < y + h; ++yy) for (int xx = x; xx < x + w; ++xx) pixel(b, xx, yy, true); }

/* Compact 5x7 ASCII font used by the native port.  It keeps the display useful
 * even when the optional Arduino/GxEPD2 renderer is disabled. */
static const uint8_t font5x7[128][5] = {
  ['0']={0x3e,0x51,0x49,0x45,0x3e}, ['1']={0,0x42,0x7f,0x40,0},
  ['2']={0x62,0x51,0x49,0x49,0x46}, ['3']={0x22,0x49,0x49,0x49,0x36},
  ['4']={0x18,0x14,0x12,0x7f,0x10}, ['5']={0x2f,0x49,0x49,0x49,0x31},
  ['6']={0x3e,0x49,0x49,0x49,0x32}, ['7']={1,0x71,9,5,3},
  ['8']={0x36,0x49,0x49,0x49,0x36}, ['9']={0x26,0x49,0x49,0x49,0x3e},
  ['-']={8,8,8,8,8}, ['.']={0,0x60,0x60,0,0}, [':']={0,0x36,0x36,0,0},
  ['/']={0x60,0x10,8,4,3}, ['C']={0x3e,0x41,0x41,0x41,0x22},
  ['%']={0x63,0x13,8,0x64,0x63}, ['h']={0x7f,8,8,8,0x70},
  ['P']={0x7f,9,9,9,6}, ['a']={0x20,0x54,0x54,0x54,0x78},
  ['i']={0,0x44,0x7d,0x40,0}, ['r']={0x7c,8,4,4,8},
  ['W']={0x7e,1,0x0e,1,0x7e}, ['E']={0x7f,0x49,0x49,0x49,0x41},
  ['A']={0x7e,9,9,9,0x7e}, ['T']={1,1,0x7f,1,1}, ['R']={0x7f,9,0x19,0x29,0x46},
  ['D']={0x7f,0x41,0x41,0x22,0x1c}, ['Y']={3,4,0x78,4,3},
  ['M']={0x7f,2,4,2,0x7f}, ['O']={0x3e,0x41,0x41,0x41,0x3e},
  ['N']={0x7f,2,4,8,0x7f}, ['H']={0x7f,8,8,8,0x7f},
  ['U']={0x3f,0x40,0x40,0x40,0x3f}, ['F']={0x7f,9,9,1,1},
  ['L']={0x7f,0x40,0x40,0x40,0x40}, ['K']={0x7f,8,0x14,0x22,0x41},
  ['S']={0x46,0x49,0x49,0x49,0x31}, ['G']={0x3e,0x41,0x49,0x49,0x7a},
  ['V']={7,8,0x70,8,7}, ['B']={0x7f,0x49,0x49,0x49,0x36},
};

static void text5(uint8_t *b, int x, int y, const char *s, int scale) {
  for (; *s; ++s) {
    unsigned char ch = (unsigned char)*s;
    if (ch > 127) { x += 6 * scale; continue; }
    for (int col = 0; col < 5; ++col) for (int row = 0; row < 7; ++row)
      if (font5x7[ch][col] & (1u << row))
        box(b, x + col * scale, y + row * scale, scale, scale);
    x += 6 * scale;
  }
}

static void photo(uint8_t *b, int x, int y) {
  static uint8_t uploaded_photo[CALENDAR_PHOTO_BYTES];
  int selected = calendar_photo_store_selected();
  if (selected < 0 ||
      calendar_photo_store_load((uint8_t)selected, uploaded_photo,
                                sizeof(uploaded_photo), NULL) != ESP_OK) {
    return;
  }
  for (int yy = 0; yy < CALENDAR_PHOTO_HEIGHT && y + yy < WEATHER_EPD_HEIGHT; ++yy) {
    for (int xx = 0; xx < CALENDAR_PHOTO_WIDTH && x + xx < WEATHER_EPD_WIDTH; ++xx) {
      size_t bit = (size_t)yy * CALENDAR_PHOTO_WIDTH + xx;
      if (uploaded_photo[bit / 8] & (0x80u >> (bit & 7)))
        pixel(b, x + xx, y + yy, true);
    }
  }
}

static void draw_calendar_page(uint8_t *b, uint8_t *r) {
  time_t now = time(NULL); struct tm tmv = {0}; localtime_r(&now, &tmv);
  char title[32]; snprintf(title, sizeof(title), "%04d-%02d", tmv.tm_year + 1900, tmv.tm_mon + 1);
  text5(b, 22, 18, "CALENDAR", 3); text5(b, 230, 22, title, 2);
  const int gx = 20, gy = 72, cw = 52, ch = 55;
  const char *wd = "MON TUE WED THU FRI SAT SUN";
  text5(b, gx, 54, wd, 1);
  for (int i = 0; i <= 7; ++i) box(b, gx + i * cw, gy, 2, ch * 7);
  for (int i = 0; i <= 6; ++i) box(b, gx, gy + i * ch, cw * 7, 2);
  struct tm first = tmv; first.tm_mday = 1; mktime(&first);
  int start = (first.tm_wday + 6) % 7;
  static const int mdays[] = {31,28,31,30,31,30,31,31,30,31,30,31};
  int days = mdays[tmv.tm_mon]; if (tmv.tm_mon == 1 && ((tmv.tm_year+1900)%4==0)) days = 29;
  for (int d = 1; d <= days; ++d) {
    int pos = start + d - 1, col = pos % 7, row = pos / 7;
    char ds[3]; ds[0] = (char)('0' + d / 10); ds[1] = (char)('0' + d % 10); ds[2] = '\0';
    const char *day_text = d < 10 ? ds + 1 : ds;
    if (d == tmv.tm_mday) box(b, gx + col*cw + 5, gy + row*ch + 7, cw - 10, ch - 14);
    text5(b, gx + col*cw + 14, gy + row*ch + 17, day_text, 2);
    if (d == tmv.tm_mday) { /* redraw current day in red for the tri-color panel */
      for (int yy = gy + row*ch + 7; yy < gy + row*ch + ch - 7; ++yy)
        for (int xx = gx + col*cw + 5; xx < gx + col*cw + cw - 5; ++xx) pixel(r, xx, yy, true);
    }
  }
  photo(b, 430, 8); box(b, 425, 4, 3, 468); box(b, 795, 4, 3, 468);
}

esp_err_t weather_epd_driver_init(void) {
#if CONFIG_WEATHER_EPD_ARDUINO_RENDERER
  return weather_display_arduino_probe();
#else
  if (s_ready) return ESP_OK;
  gpio_set_direction(WEATHER_EPD_DC_GPIO, GPIO_MODE_OUTPUT); gpio_set_direction(WEATHER_EPD_RST_GPIO, GPIO_MODE_OUTPUT); gpio_set_direction(WEATHER_EPD_BUSY_GPIO, GPIO_MODE_INPUT); gpio_set_direction(WEATHER_EPD_PWR_GPIO, GPIO_MODE_OUTPUT); gpio_set_level(WEATHER_EPD_PWR_GPIO, 1);
  spi_bus_config_t bus = {.mosi_io_num = WEATHER_EPD_MOSI_GPIO, .miso_io_num = WEATHER_EPD_MISO_GPIO, .sclk_io_num = WEATHER_EPD_SCK_GPIO, .quadwp_io_num = -1, .quadhd_io_num = -1, .max_transfer_sz = sizeof(s_black)};
  esp_err_t e = spi_bus_initialize(WEATHER_EPD_SPI_HOST, &bus, SPI_DMA_CH_AUTO); if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) return e;
  spi_device_interface_config_t dev = {.clock_speed_hz = 4000000, .mode = 0, .spics_io_num = WEATHER_EPD_CS_GPIO, .queue_size = 1}; e = spi_bus_add_device(WEATHER_EPD_SPI_HOST, &dev, &s_spi); if (e != ESP_OK) return e;
  gpio_set_level(WEATHER_EPD_RST_GPIO, 0); vTaskDelay(pdMS_TO_TICKS(10)); gpio_set_level(WEATHER_EPD_RST_GPIO, 1); vTaskDelay(pdMS_TO_TICKS(10));
  cmd(0x01); uint8_t panel[] = {0x07, 0x07, 0x3f, 0x3f}; data(panel, sizeof(panel));
  cmd(0x06); uint8_t booster[] = {0x17, 0x17, 0x28, 0x17}; data(booster, sizeof(booster));
  cmd(0x00); uint8_t panel_cfg = 0x0f; data(&panel_cfg, 1);
  cmd(0x61); uint8_t size[] = {0x03, 0x20, 0x01, 0xe0}; data(size, sizeof(size));
  cmd(0x15); uint8_t x = 0x00; data(&x, 1);
  cmd(0x50); uint8_t vcom[] = {0x13, 0x07}; data(vcom, sizeof(vcom));
  cmd(0x60); uint8_t tcon = 0x22; data(&tcon, 1);
  cmd(0x04); wait_busy();
  s_ready = true; return ESP_OK;
#endif
}
esp_err_t weather_epd_driver_refresh(weather_epaper_page_t page, const weather_sensor_data_t *sensor, const char *location, const char *weather_text) {
#if CONFIG_WEATHER_EPD_ARDUINO_RENDERER
  return weather_display_arduino_refresh(page, sensor, location, weather_text);
#else
  if (!s_ready) return ESP_ERR_INVALID_STATE;
  memset(s_black, 0xFF, sizeof(s_black));
  memset(s_red, 0xFF, sizeof(s_red));
  if (page == WEATHER_EPAPER_PAGE_CALENDAR) {
    calendar_photo_store_prepare_next();
    draw_calendar_page(s_black, s_red);
  } else {
    char line[96];
    text5(s_black, 34, 30, location ? location : "WEATHER", 2);
    text5(s_black, 560, 30, weather_text ? weather_text : "READY", 2);
    if (sensor && sensor->valid) {
      snprintf(line, sizeof(line), "TEMP %.1f C", sensor->temperature_c); text5(s_black, 70, 95, line, 3);
      snprintf(line, sizeof(line), "HUM %.1f %%", sensor->humidity_pct); text5(s_black, 70, 180, line, 2);
      snprintf(line, sizeof(line), "PRESS %.0f hPa", sensor->pressure_hpa); text5(s_black, 70, 235, line, 2);
      if (sensor->pm25_ugm3 > 0.0f) {
        snprintf(line, sizeof(line), "PM2.5 %.0f", sensor->pm25_ugm3);
      } else {
        snprintf(line, sizeof(line), "PM2.5 --");
      }
      text5(s_black, 470, 180, line, 2);
      if (sensor->pm10_ugm3 > 0.0f) {
        snprintf(line, sizeof(line), "PM10 %.0f", sensor->pm10_ugm3);
      } else {
        snprintf(line, sizeof(line), "PM10 --");
      }
      text5(s_black, 470, 235, line, 2);
      /* A red accent block mirrors the source renderer's alert/status color. */
      mark_red(s_red, 700, 92, 52, 52);
    } else {
      text5(s_black, 70, 110, "WAITING FOR SENSOR", 2);
    }
  }
  box(s_black, 20, 20, WEATHER_EPD_WIDTH - 40, 4); box(s_black, 20, WEATHER_EPD_HEIGHT - 24, WEATHER_EPD_WIDTH - 40, 4);
  box(s_black, 20, 20, 4, WEATHER_EPD_HEIGHT - 44); box(s_black, WEATHER_EPD_WIDTH - 24, 20, 4, WEATHER_EPD_HEIGHT - 44);
  ESP_LOGI(TAG, "refresh frame black=%u red=%u", (unsigned)sizeof(s_black), (unsigned)sizeof(s_red));
  set_full_window();
  cmd(0x10); data(s_black, sizeof(s_black));
  set_full_window();
  cmd(0x13); data(s_red, sizeof(s_red));
  cmd(0x00); uint8_t panel_cfg = 0x0f; data(&panel_cfg, 1);
  cmd(0x50); uint8_t vcom[] = {0x13, 0x07}; data(vcom, sizeof(vcom));
  cmd(0xE0); uint8_t cascade = 0x00; data(&cascade, 1);
  cmd(0x41); uint8_t temp_sensor = 0x00; data(&temp_sensor, 1);
  cmd(0x04); wait_busy();
  cmd(0x12); wait_busy();
  cmd(0x02); wait_busy();
  return ESP_OK;
#endif
}
esp_err_t weather_epd_driver_power_off(void) {
#if CONFIG_WEATHER_EPD_ARDUINO_RENDERER
  return ESP_OK;
#else
  if (!s_ready) return ESP_OK; cmd(0x02); wait_busy(); gpio_set_level(WEATHER_EPD_PWR_GPIO, 0); return ESP_OK;
#endif
}
