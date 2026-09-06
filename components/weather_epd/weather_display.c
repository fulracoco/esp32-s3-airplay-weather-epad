#include "weather_display.h"

#if CONFIG_WEATHER_EPD_ARDUINO_RENDERER
extern esp_err_t weather_display_arduino_probe(void);
#endif

esp_err_t weather_display_init(void) {
  esp_err_t err = weather_epd_init();
#if CONFIG_WEATHER_EPD_ARDUINO_RENDERER
  if (err == ESP_OK) {
    err = weather_display_arduino_probe();
  }
#endif
  return err;
}

esp_err_t weather_display_show_weather(const weather_sensor_data_t *sensor,
                                        const char *location,
                                        const char *weather_text) {
  weather_epd_set_page(WEATHER_EPAPER_PAGE_WEATHER);
  return weather_epd_refresh(WEATHER_EPAPER_PAGE_WEATHER, sensor, location,
                              weather_text);
}

esp_err_t weather_display_show_calendar(void) {
  weather_epd_set_page(WEATHER_EPAPER_PAGE_CALENDAR);
  return weather_epd_refresh(WEATHER_EPAPER_PAGE_CALENDAR, NULL, NULL, NULL);
}

esp_err_t weather_display_request_calendar_refresh(void) {
  weather_epd_set_page(WEATHER_EPAPER_PAGE_CALENDAR);
  return weather_epd_request_calendar_refresh();
}

esp_err_t weather_display_set_page(weather_epaper_page_t page) {
  if (page != WEATHER_EPAPER_PAGE_WEATHER &&
      page != WEATHER_EPAPER_PAGE_CALENDAR) {
    return ESP_ERR_INVALID_ARG;
  }
  weather_epd_set_page(page);
  return ESP_OK;
}

esp_err_t weather_display_power_off(void) {
  return weather_epd_power_off();
}
