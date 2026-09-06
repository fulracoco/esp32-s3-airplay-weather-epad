#pragma once

#include "esp_err.h"
#include "weather_epd.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Stable display-facing API. The AirPlay application should use this API
 * instead of depending on renderer implementation details. */
esp_err_t weather_display_init(void);
esp_err_t weather_display_show_weather(const weather_sensor_data_t *sensor,
                                        const char *location,
                                        const char *weather_text);
esp_err_t weather_display_show_calendar(void);
esp_err_t weather_display_request_calendar_refresh(void);
esp_err_t weather_display_set_page(weather_epaper_page_t page);
esp_err_t weather_display_power_off(void);

#ifdef __cplusplus
}
#endif
