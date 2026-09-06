#include "weather_display.h"

#if CONFIG_WEATHER_EPD_ARDUINO_RENDERER
#include <Arduino.h>
#include <time.h>
#include <cmath>
#include "weather_epd_config.h"
#include "config.h"
#include "weather_epd.h"
#include "renderer.h"
#include "calendar.h"
#include "calendar_photo_store.h"
#include "holiday.h"

static bool s_arduino_ready = false;
static owm_hourly_t s_hourly_graph[WEATHER_FORECAST_MAX];
static owm_daily_t s_daily_graph[OWM_NUM_DAILY];
static float s_graph_temp[WEATHER_FORECAST_MAX];
static float s_graph_pop[WEATHER_FORECAST_MAX];
static float s_graph_rain[WEATHER_FORECAST_MAX];
static int64_t s_graph_dt[WEATHER_FORECAST_MAX];
static int s_graph_ids[WEATHER_FORECAST_MAX];
static char s_graph_icons[WEATHER_FORECAST_MAX][8];
static owm_current_t s_current;
static owm_daily_t s_today;
static owm_resp_air_pollution_t s_air;
static weather_remote_details_t s_details;
static owm_daily_t s_daily[OWM_NUM_DAILY];

extern "C" esp_err_t weather_display_arduino_probe(void) {
  if (s_arduino_ready) return ESP_OK;
  initArduino();
  pinMode(PIN_EPD_CS, OUTPUT);
  pinMode(PIN_EPD_DC, OUTPUT);
  pinMode(PIN_EPD_RST, OUTPUT);
  pinMode(PIN_EPD_BUSY, INPUT);
  pinMode(PIN_EPD_PWR, OUTPUT);
  digitalWrite(PIN_EPD_PWR, HIGH);
  initDisplay();
  s_arduino_ready = true;
  return ESP_OK;
}

extern "C" esp_err_t weather_display_arduino_refresh(
    weather_epaper_page_t page, const weather_sensor_data_t *sensor,
    const char *location, const char *weather_text) {
  if (!s_arduino_ready && weather_display_arduino_probe() != ESP_OK)
    return ESP_FAIL;
  time_t now = time(nullptr);
  tm ti{};
  localtime_r(&now, &ti);
  if (page == WEATHER_EPAPER_PAGE_CALENDAR) {
    calendar_photo_store_prepare_next();
    weather_remote_config_t config{};
    if (weather_epd_get_remote_config(&config) == ESP_OK &&
        config.holiday_endpoint[0]) {
      HOLIDAY_API_ENDPOINT = config.holiday_endpoint;
    }
    holiday_data_t holidays{};
    load_or_fetch_holidays(ti, holidays);
    display.firstPage();
    do { draw_calendar(ti, holidays); } while (display.nextPage());
    return ESP_OK;
  }
  owm_current_t &current = s_current; current = owm_current_t{};
  owm_daily_t &today = s_today; today = owm_daily_t{};
  owm_resp_air_pollution_t &air = s_air; air = owm_resp_air_pollution_t{};
  weather_remote_details_t &details = s_details; details = weather_remote_details_t{};
  owm_daily_t *daily = s_daily; memset(daily, 0, sizeof(s_daily));
  for (int i = 0; i < OWM_NUM_DAILY; ++i) { daily[i].dt = now + i * 86400; daily[i].weather.id = 800; daily[i].weather.icon = "01d"; }
  current.dt = now;
  current.weather.id = 800;       // local sensor fallback: clear sky
  current.weather.icon = "01d";
  current.weather.main = "Clear";
  current.weather.description = String(weather_text ? weather_text : "");
  today.dt = now;
  today.moonrise = now - 6 * 3600;
  today.moonset = now + 6 * 3600;
  today.moon_phase = 0.5f;
  today.weather.id = 800;
  today.weather.icon = "01d";
  if (weather_epd_get_remote_details(&details)) {
    current.sunrise = details.sunrise; current.sunset = details.sunset;
    current.wind_speed = details.wind_speed; current.uvi = details.uvi;
    current.visibility = details.visibility; current.pressure = details.pressure;
    current.humidity = details.humidity; current.dew_point = details.dew_point;
    if (details.air_valid) {
      air.main_aqi[0] = details.air_aqi;
      air.components.co[0] = details.air_co;
      air.components.nh3[0] = details.air_nh3;
      air.components.no[0] = details.air_no;
      air.components.no2[0] = details.air_no2;
      air.components.o3[0] = details.air_o3;
      air.components.so2[0] = details.air_so2;
      air.components.pm10[0] = details.air_pm10;
      air.components.pm2_5[0] = details.air_pm2_5;
    }
    for (size_t i = 0; i < details.daily_count && i < OWM_NUM_DAILY; ++i) {
      daily[i].dt = details.daily_dt[i]; daily[i].temp.min = details.daily_min[i];
      daily[i].temp.max = details.daily_max[i]; daily[i].temp.day = details.daily_temp[i];
      daily[i].pop = details.daily_pop[i]; daily[i].rain = details.daily_rain[i];
      daily[i].weather.id = details.daily_weather_id[i]; daily[i].weather.icon = details.daily_icon[i];
    }
  }
  float remote_temp = 0;
  int remote_id = 800;
  char remote_icon[8] = "01d";
  if (weather_epd_get_remote_weather(&remote_temp, &remote_id,
                                     remote_icon, sizeof(remote_icon))) {
    current.weather.id = remote_id;
    current.weather.icon = remote_icon;
    today.weather.id = remote_id;
    today.weather.icon = remote_icon;
    current.temp = remote_temp + 273.15f;
    today.temp.day = current.temp;
    today.temp.min = current.temp;
    today.temp.max = current.temp;
  }
  if (sensor && sensor->valid) {
    // esp32-weather-epd renderer stores OpenWeatherMap temperatures in
    // Kelvin and converts them according to config.h at draw time.
    current.temp = sensor->temperature_c + 273.15f;
    current.humidity = (int)sensor->humidity_pct;
    current.pressure = (int)sensor->pressure_hpa;
    current.feels_like = current.temp;
    current.clouds = 0;
    if (!details.valid || current.visibility <= 0) current.visibility = 10000;
    today.temp.day = current.temp;
    today.temp.min = current.temp;
    today.temp.max = current.temp;
    if (!details.air_valid) {
      if (std::isfinite(sensor->pm25_ugm3) && sensor->pm25_ugm3 > 0.0f)
        air.components.pm2_5[0] = sensor->pm25_ugm3;
      if (std::isfinite(sensor->pm10_ugm3) && sensor->pm10_ugm3 > 0.0f)
        air.components.pm10[0] = sensor->pm10_ugm3;
    }
  } else {
    current.temp = NAN;
    today.temp.day = NAN;
    today.temp.min = NAN;
    today.temp.max = NAN;
  }
  display.firstPage();
  do {
    drawLocationDate(String(location ? location : "Weather"), String(weather_text ? weather_text : ""));
    drawCurrentConditions(current, today, air, sensor ? sensor->temperature_c : NAN,
                          sensor ? sensor->humidity_pct : NAN);
    /* Plot the 24-hour precipitation probability and temperature outlook on
     * the right side, matching the source project's graph layout. */
    size_t n = weather_epd_get_forecast(s_graph_temp, s_graph_pop, s_graph_rain,
                                         s_graph_dt, s_graph_ids, s_graph_icons,
                                         WEATHER_FORECAST_MAX);
    if (n >= 2) {
      size_t points = n < WEATHER_FORECAST_MAX ? n : WEATHER_FORECAST_MAX;
      for (size_t i = points; i < WEATHER_FORECAST_MAX; ++i) {
        size_t src = points - 1;
        s_graph_temp[i] = s_graph_temp[src]; s_graph_pop[i] = s_graph_pop[src];
        s_graph_rain[i] = s_graph_rain[src]; s_graph_dt[i] = s_graph_dt[src] + (int64_t)(i - src) * 3600;
        s_graph_ids[i] = s_graph_ids[src]; strncpy(s_graph_icons[i], s_graph_icons[src], 7);
        s_graph_icons[i][7] = '\0';
      }
      for (size_t i = 0; i < WEATHER_FORECAST_MAX; ++i) {
        s_hourly_graph[i].dt = s_graph_dt[i]; s_hourly_graph[i].temp = s_graph_temp[i];
        s_hourly_graph[i].pop = s_graph_pop[i]; s_hourly_graph[i].rain_1h = s_graph_rain[i];
        s_hourly_graph[i].snow_1h = 0; s_hourly_graph[i].weather.id = s_graph_ids[i];
        s_hourly_graph[i].weather.icon = s_graph_icons[i];
      }
      for (int i = 0; i < OWM_NUM_DAILY; ++i) s_daily_graph[i] = daily[i];
      drawOutlookGraph(s_hourly_graph, s_daily_graph, ti);
    }
    if (details.valid) drawForecast(daily, ti);
  } while (display.nextPage());
  return ESP_OK;
}
#endif
