#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define WEATHER_FORECAST_MAX 24
#define WEATHER_DAILY_MAX 7
typedef struct {
  bool valid;
  bool air_valid;
  int air_aqi;
  float air_co, air_nh3, air_no, air_no2, air_o3, air_so2;
  float air_pm10, air_pm2_5;
  int64_t sunrise, sunset;
  float wind_speed, uvi;
  int visibility, pressure, humidity;
  float dew_point;
  size_t daily_count;
  int64_t daily_dt[WEATHER_DAILY_MAX];
  float daily_min[WEATHER_DAILY_MAX], daily_max[WEATHER_DAILY_MAX];
  float daily_temp[WEATHER_DAILY_MAX], daily_pop[WEATHER_DAILY_MAX];
  float daily_rain[WEATHER_DAILY_MAX];
  int daily_weather_id[WEATHER_DAILY_MAX];
  char daily_icon[WEATHER_DAILY_MAX][8];
} weather_remote_details_t;
#include "weather_epd_display_config.h"

typedef enum {
  WEATHER_EPAPER_PAGE_WEATHER = 0,
  WEATHER_EPAPER_PAGE_CALENDAR = 1,
} weather_epaper_page_t;

typedef struct {
  float temperature_c;
  float humidity_pct;
  float pressure_hpa;
  float pm25_ugm3;
  float pm10_ugm3;
  bool valid;
} weather_sensor_data_t;

/* Runtime OpenWeatherMap configuration. Values are persisted in NVS and can
 * be changed through the device web UI. */
typedef struct {
  bool enabled;
  char api_key[128];
  char latitude[24];
  char longitude[24];
  char city[48];
  char timezone[40];
  char ntp_server[64];
  char holiday_endpoint[256];
  uint32_t refresh_minutes;
} weather_remote_config_t;

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t weather_epd_init(void);
esp_err_t weather_epd_refresh(weather_epaper_page_t page,
                              const weather_sensor_data_t *sensor,
                              const char *location,
                              const char *weather_text);
esp_err_t weather_epd_read_sensors(weather_sensor_data_t *out);
esp_err_t weather_epd_power_off(void);
void weather_epd_set_location(const char *location);
void weather_epd_set_weather_text(const char *text);
void weather_epd_set_page(weather_epaper_page_t page);
void weather_epd_set_remote_weather(float temperature_c, int weather_id,
                                    const char *weather_icon,
                                    const char *weather_text,
                                    const char *location);
bool weather_epd_get_remote_weather(float *temperature_c, int *weather_id,
                                    char *weather_icon, size_t icon_len);
size_t weather_epd_get_forecast(float *temperature_c, float *precip_prob,
                                float *rain_mm, int64_t *timestamps,
                                int *weather_ids, char icons[][8], size_t max_points);
bool weather_epd_get_remote_details(weather_remote_details_t *out);
esp_err_t weather_epd_get_remote_config(weather_remote_config_t *out);
esp_err_t weather_epd_set_remote_config(const weather_remote_config_t *cfg);
esp_err_t weather_epd_refresh_remote_now(void);
/* Request an asynchronous refresh of the calendar page. The display task
 * performs the actual SPI/e-paper operation outside protocol callbacks. */
esp_err_t weather_epd_request_calendar_refresh(void);

#ifdef __cplusplus
}
#endif
