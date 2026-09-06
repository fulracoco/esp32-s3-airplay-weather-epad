#include "weather_epd.h"
#include "weather_epd_config.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "esp_timer.h"
#include "esp_crt_bundle.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

esp_err_t weather_bme280_init(void);
esp_err_t weather_bme280_read(weather_sensor_data_t *out);
esp_err_t weather_epd_driver_init(void);
esp_err_t weather_epd_driver_refresh(weather_epaper_page_t page, const weather_sensor_data_t *sensor,
                                     const char *location, const char *weather_text);
esp_err_t weather_epd_driver_power_off(void);

static const char *TAG = "weather_epd";
static TaskHandle_t s_task;
static char s_location[48] = "配置城市";
static char s_weather_text[96] = "等待天气数据";
static weather_epaper_page_t s_page = WEATHER_EPAPER_PAGE_WEATHER;
static bool s_remote_valid;
static float s_remote_temp_c;
static int s_remote_weather_id = 800;
static char s_remote_icon[8] = "01d";
static size_t s_forecast_count;
static float s_forecast_temp[WEATHER_FORECAST_MAX];
static float s_forecast_pop[WEATHER_FORECAST_MAX];
static float s_forecast_rain[WEATHER_FORECAST_MAX];
static int64_t s_forecast_dt[WEATHER_FORECAST_MAX];
static int s_forecast_id[WEATHER_FORECAST_MAX];
static char s_forecast_icon[WEATHER_FORECAST_MAX][8];
static weather_remote_details_t s_remote_details;
static weather_remote_config_t s_remote_cfg;
static SemaphoreHandle_t s_cfg_mutex;
static bool s_sntp_initialized;
static bool s_time_sync_needed = true;

#define WEATHER_VALID_EPOCH ((time_t)1704067200) /* 2024-01-01 00:00:00 UTC */

static void weather_remote_config_defaults(weather_remote_config_t *cfg) {
  memset(cfg, 0, sizeof(*cfg));
  cfg->enabled = WEATHER_OWM_API_KEY[0] != '\0';
  strncpy(cfg->api_key, WEATHER_OWM_API_KEY, sizeof(cfg->api_key) - 1);
  strncpy(cfg->latitude, WEATHER_OWM_LATITUDE, sizeof(cfg->latitude) - 1);
  strncpy(cfg->longitude, WEATHER_OWM_LONGITUDE, sizeof(cfg->longitude) - 1);
  strncpy(cfg->city, WEATHER_OWM_CITY, sizeof(cfg->city) - 1);
  strncpy(cfg->timezone, WEATHER_DEFAULT_TIMEZONE, sizeof(cfg->timezone) - 1);
  strncpy(cfg->ntp_server, WEATHER_DEFAULT_NTP_SERVER, sizeof(cfg->ntp_server) - 1);
  strncpy(cfg->holiday_endpoint, WEATHER_DEFAULT_HOLIDAY_ENDPOINT,
          sizeof(cfg->holiday_endpoint) - 1);
  cfg->refresh_minutes = 30;
}

static void weather_remote_config_load(void) {
  weather_remote_config_defaults(&s_remote_cfg);
  nvs_handle_t nvs;
  if (nvs_open("weather_epd", NVS_READONLY, &nvs) != ESP_OK) return;
  uint8_t enabled;
  if (nvs_get_u8(nvs, "enabled", &enabled) == ESP_OK) s_remote_cfg.enabled = enabled != 0;
  size_t len;
  len = sizeof(s_remote_cfg.api_key); nvs_get_str(nvs, "owm_key", s_remote_cfg.api_key, &len);
  len = sizeof(s_remote_cfg.latitude); nvs_get_str(nvs, "latitude", s_remote_cfg.latitude, &len);
  len = sizeof(s_remote_cfg.longitude); nvs_get_str(nvs, "longitude", s_remote_cfg.longitude, &len);
  len = sizeof(s_remote_cfg.city); nvs_get_str(nvs, "city", s_remote_cfg.city, &len);
  len = sizeof(s_remote_cfg.timezone); nvs_get_str(nvs, "timezone", s_remote_cfg.timezone, &len);
  len = sizeof(s_remote_cfg.ntp_server); nvs_get_str(nvs, "ntp_server", s_remote_cfg.ntp_server, &len);
  len = sizeof(s_remote_cfg.holiday_endpoint);
  nvs_get_str(nvs, "holiday_url", s_remote_cfg.holiday_endpoint, &len);
  uint32_t refresh;
  if (nvs_get_u32(nvs, "refresh_min", &refresh) == ESP_OK && refresh > 0 && refresh <= 1440)
    s_remote_cfg.refresh_minutes = refresh;
  nvs_close(nvs);
  if (s_remote_cfg.city[0]) weather_epd_set_location(s_remote_cfg.city);
}

typedef struct { char *buf; size_t len; size_t cap; } weather_http_buf_t;
static esp_err_t weather_http_event(esp_http_client_event_t *evt) {
  weather_http_buf_t *b = (weather_http_buf_t *)evt->user_data;
  if (evt->event_id == HTTP_EVENT_ON_DATA && evt->data && evt->data_len > 0 && b) {
    size_t need = b->len + (size_t)evt->data_len + 1;
    if (need > b->cap) {
      size_t cap = b->cap ? b->cap * 2 : 1024;
      while (cap < need) cap *= 2;
      char *p = (char *)realloc(b->buf, cap);
      if (!p) return ESP_ERR_NO_MEM;
      b->buf = p; b->cap = cap;
    }
    memcpy(b->buf + b->len, evt->data, evt->data_len);
    b->len += evt->data_len; b->buf[b->len] = '\0';
  }
  return ESP_OK;
}

static void weather_fetch_air_quality(const weather_remote_config_t *cfg) {
  char url[384];
  snprintf(url, sizeof(url),
           "https://api.openweathermap.org/data/2.5/air_pollution?lat=%s&lon=%s&appid=%s",
           cfg->latitude, cfg->longitude, cfg->api_key);
  weather_http_buf_t body = {0};
  esp_http_client_config_t http_cfg = {
      .url = url,
      .event_handler = weather_http_event,
      .user_data = &body,
      .timeout_ms = 12000,
      .crt_bundle_attach = esp_crt_bundle_attach,
  };
  esp_http_client_handle_t client = esp_http_client_init(&http_cfg);
  if (!client) return;
  esp_err_t err = esp_http_client_perform(client);
  int status = esp_http_client_get_status_code(client);
  if (err == ESP_OK && status == 200 && body.buf) {
    cJSON *root = cJSON_Parse(body.buf);
    cJSON *list = root ? cJSON_GetObjectItem(root, "list") : NULL;
    cJSON *item = (list && cJSON_IsArray(list)) ? cJSON_GetArrayItem(list, 0) : NULL;
    cJSON *main = item ? cJSON_GetObjectItem(item, "main") : NULL;
    cJSON *components = item ? cJSON_GetObjectItem(item, "components") : NULL;
    cJSON *value;
    if (main && components) {
      value = cJSON_GetObjectItem(main, "aqi");
      s_remote_details.air_aqi = (value && cJSON_IsNumber(value)) ? value->valueint : 0;
#define WEATHER_AIR_VALUE(name) \
      value = cJSON_GetObjectItem(components, #name); \
      s_remote_details.air_##name = (value && cJSON_IsNumber(value)) ? \
                                    (float)value->valuedouble : 0.0f
      WEATHER_AIR_VALUE(co);
      WEATHER_AIR_VALUE(nh3);
      WEATHER_AIR_VALUE(no);
      WEATHER_AIR_VALUE(no2);
      WEATHER_AIR_VALUE(o3);
      WEATHER_AIR_VALUE(so2);
      WEATHER_AIR_VALUE(pm10);
      WEATHER_AIR_VALUE(pm2_5);
#undef WEATHER_AIR_VALUE
      s_remote_details.air_valid = true;
    }
    cJSON_Delete(root);
  } else {
    ESP_LOGW(TAG, "air quality request failed: %s status=%d",
             esp_err_to_name(err), status);
  }
  esp_http_client_cleanup(client);
  free(body.buf);
}

static esp_err_t weather_fetch_remote(void) {
  weather_remote_config_t cfg;
  if (s_cfg_mutex) xSemaphoreTake(s_cfg_mutex, portMAX_DELAY);
  cfg = s_remote_cfg;
  if (s_cfg_mutex) xSemaphoreGive(s_cfg_mutex);
  if (!cfg.enabled || cfg.api_key[0] == '\0') return ESP_ERR_INVALID_STATE;
  s_remote_details.air_valid = false;
  char url[384];
  snprintf(url, sizeof(url),
           "https://api.openweathermap.org/data/3.0/onecall?lat=%s&lon=%s&exclude=minutely,alerts&units=metric&lang=zh_cn&appid=%s",
           cfg.latitude, cfg.longitude, cfg.api_key);
  weather_http_buf_t body = {0};
  esp_http_client_config_t http_cfg = {.url = url, .event_handler = weather_http_event,
                                  .user_data = &body, .timeout_ms = 12000,
                                  .crt_bundle_attach = esp_crt_bundle_attach};
  esp_http_client_handle_t client = esp_http_client_init(&http_cfg);
  if (!client) return ESP_FAIL;
  esp_err_t err = esp_http_client_perform(client);
  int status = esp_http_client_get_status_code(client);
  if (err == ESP_OK && status == 200 && body.buf) {
    cJSON *root = cJSON_Parse(body.buf);
    cJSON *cur = root ? cJSON_GetObjectItem(root, "current") : NULL;
    cJSON *temp = cur ? cJSON_GetObjectItem(cur, "temp") : NULL;
    cJSON *arr = cur ? cJSON_GetObjectItem(cur, "weather") : NULL;
    cJSON *w = (arr && cJSON_IsArray(arr)) ? cJSON_GetArrayItem(arr, 0) : NULL;
    cJSON *id = w ? cJSON_GetObjectItem(w, "id") : NULL;
    cJSON *icon = w ? cJSON_GetObjectItem(w, "icon") : NULL;
    cJSON *desc = w ? cJSON_GetObjectItem(w, "description") : NULL;
    bool updated = false;
    if (temp && cJSON_IsNumber(temp)) {
      weather_epd_set_remote_weather((float)temp->valuedouble,
        (id && cJSON_IsNumber(id)) ? id->valueint : 800,
        (icon && cJSON_IsString(icon)) ? icon->valuestring : "01d",
        (desc && cJSON_IsString(desc)) ? desc->valuestring : "",
        cfg.city);
      ESP_LOGI(TAG, "remote weather updated: %.1fC id=%d", (float)temp->valuedouble,
               (id && cJSON_IsNumber(id)) ? id->valueint : 800);
      updated = true;
    }
    if (cur) {
      cJSON *v;
      v = cJSON_GetObjectItem(cur, "sunrise"); if (v && cJSON_IsNumber(v)) s_remote_details.sunrise = (int64_t)v->valuedouble;
      v = cJSON_GetObjectItem(cur, "sunset"); if (v && cJSON_IsNumber(v)) s_remote_details.sunset = (int64_t)v->valuedouble;
      v = cJSON_GetObjectItem(cur, "wind_speed"); if (v && cJSON_IsNumber(v)) s_remote_details.wind_speed = (float)v->valuedouble;
      v = cJSON_GetObjectItem(cur, "uvi"); if (v && cJSON_IsNumber(v)) s_remote_details.uvi = (float)v->valuedouble;
      v = cJSON_GetObjectItem(cur, "visibility"); if (v && cJSON_IsNumber(v)) s_remote_details.visibility = v->valueint;
      v = cJSON_GetObjectItem(cur, "pressure"); if (v && cJSON_IsNumber(v)) s_remote_details.pressure = v->valueint;
      v = cJSON_GetObjectItem(cur, "humidity"); if (v && cJSON_IsNumber(v)) s_remote_details.humidity = v->valueint;
      v = cJSON_GetObjectItem(cur, "dew_point"); if (v && cJSON_IsNumber(v)) s_remote_details.dew_point = (float)v->valuedouble + 273.15f;
    }
    weather_fetch_air_quality(&cfg);
    cJSON *hourly = root ? cJSON_GetObjectItem(root, "hourly") : NULL;
    size_t count = (hourly && cJSON_IsArray(hourly)) ? (size_t)cJSON_GetArraySize(hourly) : 0;
    if (count > WEATHER_FORECAST_MAX) count = WEATHER_FORECAST_MAX;
    for (size_t i = 0; i < count; ++i) {
      cJSON *h = cJSON_GetArrayItem(hourly, (int)i);
      cJSON *ht = cJSON_GetObjectItem(h, "temp");
      cJSON *hp = cJSON_GetObjectItem(h, "pop");
      cJSON *hr = cJSON_GetObjectItem(h, "rain");
      cJSON *hs = cJSON_GetObjectItem(h, "snow");
      cJSON *hdt = cJSON_GetObjectItem(h, "dt");
      cJSON *harr = cJSON_GetObjectItem(h, "weather");
      cJSON *hw = (harr && cJSON_IsArray(harr)) ? cJSON_GetArrayItem(harr, 0) : NULL;
      cJSON *hid = hw ? cJSON_GetObjectItem(hw, "id") : NULL;
      cJSON *hic = hw ? cJSON_GetObjectItem(hw, "icon") : NULL;
      s_forecast_temp[i] = (ht && cJSON_IsNumber(ht)) ? (float)ht->valuedouble + 273.15f : NAN;
      s_forecast_pop[i] = (hp && cJSON_IsNumber(hp)) ? (float)hp->valuedouble : 0.0f;
      s_forecast_rain[i] = (hr && cJSON_IsNumber(hr) ? (float)hr->valuedouble : 0.0f) +
                           (hs && cJSON_IsNumber(hs) ? (float)hs->valuedouble : 0.0f);
      s_forecast_dt[i] = (hdt && cJSON_IsNumber(hdt)) ? (int64_t)hdt->valuedouble : 0;
      s_forecast_id[i] = (hid && cJSON_IsNumber(hid)) ? hid->valueint : 800;
      strncpy(s_forecast_icon[i], (hic && cJSON_IsString(hic)) ? hic->valuestring : "01d", 7);
      s_forecast_icon[i][7] = '\0';
    }
    if (cur && (!cJSON_IsNumber(cJSON_GetObjectItem(cur, "uvi"))) && count > 0) {
      cJSON *huvi = cJSON_GetObjectItem(cJSON_GetArrayItem(hourly, 0), "uvi");
      if (huvi && cJSON_IsNumber(huvi)) s_remote_details.uvi = (float)huvi->valuedouble;
    }
    s_forecast_count = count;
    cJSON *daily = root ? cJSON_GetObjectItem(root, "daily") : NULL;
    size_t dcount = (daily && cJSON_IsArray(daily)) ? (size_t)cJSON_GetArraySize(daily) : 0;
    if (dcount > WEATHER_DAILY_MAX) dcount = WEATHER_DAILY_MAX;
    s_remote_details.daily_count = dcount;
    for (size_t i = 0; i < dcount; ++i) {
      cJSON *d = cJSON_GetArrayItem(daily, (int)i), *v;
      v = cJSON_GetObjectItem(d, "dt"); s_remote_details.daily_dt[i] = (v && cJSON_IsNumber(v)) ? (int64_t)v->valuedouble : 0;
      cJSON *temps = cJSON_GetObjectItem(d, "temp");
      v = temps ? cJSON_GetObjectItem(temps, "min") : NULL; s_remote_details.daily_min[i] = (v && cJSON_IsNumber(v)) ? (float)v->valuedouble + 273.15f : NAN;
      v = temps ? cJSON_GetObjectItem(temps, "max") : NULL; s_remote_details.daily_max[i] = (v && cJSON_IsNumber(v)) ? (float)v->valuedouble + 273.15f : NAN;
      v = temps ? cJSON_GetObjectItem(temps, "day") : NULL; s_remote_details.daily_temp[i] = (v && cJSON_IsNumber(v)) ? (float)v->valuedouble + 273.15f : NAN;
      v = cJSON_GetObjectItem(d, "pop"); s_remote_details.daily_pop[i] = (v && cJSON_IsNumber(v)) ? (float)v->valuedouble : 0;
      v = cJSON_GetObjectItem(d, "rain"); s_remote_details.daily_rain[i] = (v && cJSON_IsNumber(v)) ? (float)v->valuedouble : 0;
      cJSON *arrd = cJSON_GetObjectItem(d, "weather"), *wd = (arrd && cJSON_IsArray(arrd)) ? cJSON_GetArrayItem(arrd, 0) : NULL;
      v = wd ? cJSON_GetObjectItem(wd, "id") : NULL; s_remote_details.daily_weather_id[i] = (v && cJSON_IsNumber(v)) ? v->valueint : 800;
      v = wd ? cJSON_GetObjectItem(wd, "icon") : NULL; strncpy(s_remote_details.daily_icon[i], (v && cJSON_IsString(v)) ? v->valuestring : "01d", 7); s_remote_details.daily_icon[i][7] = '\0';
    }
    s_remote_details.valid = updated;
    cJSON_Delete(root);
    esp_http_client_cleanup(client);
    free(body.buf);
    return updated ? ESP_OK : ESP_FAIL;
  } else {
    ESP_LOGW(TAG, "remote weather request failed: %s status=%d",
             esp_err_to_name(err), status);
  }
  esp_http_client_cleanup(client);
  free(body.buf);
  return err == ESP_OK ? ESP_FAIL : err;
}

static esp_err_t weather_fetch_remote_retry(void) {
  esp_err_t err = ESP_FAIL;
  for (int attempt = 0; attempt < 3; ++attempt) {
    err = weather_fetch_remote();
    if (err == ESP_OK || err == ESP_ERR_INVALID_STATE) return err;
    if (attempt < 2) vTaskDelay(pdMS_TO_TICKS(3000));
  }
  return err;
}

static bool weather_network_ready(void) {
  extern bool wifi_is_connected(void) __attribute__((weak));
  if (wifi_is_connected && wifi_is_connected()) return true;
  /* Ethernet is optional in this build. Resolve it weakly so the weather
   * component remains usable without a hard dependency on main/network. */
  extern bool ethernet_is_connected(void) __attribute__((weak));
  return ethernet_is_connected && ethernet_is_connected();
}

static bool weather_time_is_valid(void) {
  return time(NULL) >= WEATHER_VALID_EPOCH;
}

static esp_err_t weather_sync_time(void) {
  weather_remote_config_t cfg;
  weather_epd_get_remote_config(&cfg);
  if (!cfg.timezone[0]) strncpy(cfg.timezone, WEATHER_DEFAULT_TIMEZONE, sizeof(cfg.timezone) - 1);
  if (!cfg.ntp_server[0]) strncpy(cfg.ntp_server, WEATHER_DEFAULT_NTP_SERVER, sizeof(cfg.ntp_server) - 1);
  if (weather_time_is_valid() && !s_time_sync_needed) return ESP_OK;

  if (!s_sntp_initialized) {
    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG(cfg.ntp_server);
    esp_err_t err = esp_netif_sntp_init(&config);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
      ESP_LOGW(TAG, "SNTP init failed: %s", esp_err_to_name(err));
      return err;
    }
    s_sntp_initialized = true;
  }

  esp_err_t err = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(20000));
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "SNTP sync failed: %s", esp_err_to_name(err));
    return err;
  }

  setenv("TZ", cfg.timezone, 1);
  tzset();
  s_time_sync_needed = false;
  time_t now = time(NULL);
  ESP_LOGI(TAG, "system time synchronized: %lld", (long long)now);
  return ESP_OK;
}

static void weather_task(void *arg) {
  (void)arg;
  weather_sensor_data_t data = {0};
  int64_t last_remote_us = 0;
  bool first_frame = true;
  while (true) {
    /* Keep the source project's alternating weather/calendar experience while
     * leaving the network/audio services running continuously. */
    esp_err_t sensor_err = weather_epd_read_sensors(&data);
    weather_remote_config_t cfg;
    weather_epd_get_remote_config(&cfg);
    int64_t now_us = esp_timer_get_time();
    int64_t interval_us = (int64_t)(cfg.refresh_minutes ? cfg.refresh_minutes : 30) * 60 * 1000000LL;
    if (s_time_sync_needed && weather_network_ready()) (void)weather_sync_time();
    if (first_frame) {
      /* weather_epd_init() runs before app_main starts Wi-Fi.  Do not draw a
       * placeholder frame here: the source project performs networking first
       * and then renders one coherent initial page.  Wait for a real STA IP
       * (or Ethernet link), rather than sleeping a fixed amount of time. */
      const int max_wait = 240; /* about two minutes, 500 ms per poll */
      int waited = 0;
      while (!weather_network_ready() && waited++ < max_wait) {
        vTaskDelay(pdMS_TO_TICKS(500));
      }
      if (weather_network_ready()) {
        (void)weather_sync_time();
        if (cfg.enabled && cfg.api_key[0]) {
          if (weather_fetch_remote_retry() == ESP_OK)
            last_remote_us = esp_timer_get_time();
          else
            ESP_LOGW(TAG, "initial remote weather fetch failed; rendering local data");
        }
      } else {
        ESP_LOGW(TAG, "network not ready after %d seconds; rendering local data",
                 max_wait / 2);
      }
    } else if (cfg.enabled && cfg.api_key[0] &&
               (last_remote_us == 0 || now_us - last_remote_us >= interval_us)) {
      if (weather_fetch_remote_retry() == ESP_OK) last_remote_us = now_us;
    }
    weather_epaper_page_t page = s_page;
    if (page == WEATHER_EPAPER_PAGE_CALENDAR) {
      weather_epd_refresh(page, NULL, NULL, NULL);
    } else if (sensor_err == ESP_OK || s_remote_valid) {
      if (sensor_err != ESP_OK) {
        data = (weather_sensor_data_t){.temperature_c = s_remote_temp_c,
                                      .humidity_pct = NAN,
                                      .pressure_hpa = NAN,
                                      .pm25_ugm3 = NAN,
                                      .pm10_ugm3 = NAN,
                                      .valid = true};
      }
      if (s_remote_valid) {
        data.temperature_c = s_remote_temp_c;
      }
      weather_epd_refresh(page, &data, s_location, s_weather_text);
    } else {
      weather_epd_refresh(page, NULL, s_location, "SENSOR OFFLINE");
    }
    s_page = page == WEATHER_EPAPER_PAGE_WEATHER
                 ? WEATHER_EPAPER_PAGE_CALENDAR
                 : WEATHER_EPAPER_PAGE_WEATHER;
    first_frame = false;
    /* Normally wake every ten minutes, but allow a newly fetched/configured
     * weather snapshot to trigger an immediate redraw. */
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(10 * 60 * 1000));
  }
}

void weather_epd_set_location(const char *location) {
  if (!location) return;
  strncpy(s_location, location, sizeof(s_location) - 1);
  s_location[sizeof(s_location) - 1] = '\0';
}

void weather_epd_set_weather_text(const char *text) {
  if (!text) return;
  strncpy(s_weather_text, text, sizeof(s_weather_text) - 1);
  s_weather_text[sizeof(s_weather_text) - 1] = '\0';
}

void weather_epd_set_page(weather_epaper_page_t page) {
  if (page == WEATHER_EPAPER_PAGE_WEATHER || page == WEATHER_EPAPER_PAGE_CALENDAR) {
    s_page = page;
  }
}

void weather_epd_set_remote_weather(float temperature_c, int weather_id,
                                    const char *weather_icon,
                                    const char *weather_text,
                                    const char *location) {
  s_remote_temp_c = temperature_c;
  s_remote_weather_id = weather_id;
  if (weather_icon) {
    strncpy(s_remote_icon, weather_icon, sizeof(s_remote_icon) - 1);
    s_remote_icon[sizeof(s_remote_icon) - 1] = '\0';
  }
  s_remote_valid = true;
  if (weather_text) weather_epd_set_weather_text(weather_text);
  if (location) weather_epd_set_location(location);
  /* A remote fetch normally runs from the web/network task and should wake
   * the display task.  During the boot fetch, however, this function is
   * called by weather_task itself; notifying the same task would make
   * ulTaskNotifyTake() return immediately and cause an unintended second
   * e-paper refresh/page toggle. */
  if (s_task && xTaskGetCurrentTaskHandle() != s_task) xTaskNotifyGive(s_task);
}

bool weather_epd_get_remote_weather(float *temperature_c, int *weather_id,
                                    char *weather_icon, size_t icon_len) {
  if (!s_remote_valid) return false;
  if (temperature_c) *temperature_c = s_remote_temp_c;
  if (weather_id) *weather_id = s_remote_weather_id;
  if (weather_icon && icon_len) {
    strncpy(weather_icon, s_remote_icon, icon_len - 1);
    weather_icon[icon_len - 1] = '\0';
  }
  return true;
}

size_t weather_epd_get_forecast(float *temperature_c, float *precip_prob,
                                float *rain_mm, int64_t *timestamps,
                                int *weather_ids, char icons[][8], size_t max_points) {
  size_t n = s_forecast_count < max_points ? s_forecast_count : max_points;
  for (size_t i = 0; i < n; ++i) {
    if (temperature_c) temperature_c[i] = s_forecast_temp[i];
    if (precip_prob) precip_prob[i] = s_forecast_pop[i];
    if (rain_mm) rain_mm[i] = s_forecast_rain[i];
    if (timestamps) timestamps[i] = s_forecast_dt[i];
    if (weather_ids) weather_ids[i] = s_forecast_id[i];
    if (icons) { strncpy(icons[i], s_forecast_icon[i], 7); icons[i][7] = '\0'; }
  }
  return n;
}

bool weather_epd_get_remote_details(weather_remote_details_t *out) {
  if (!out || !s_remote_details.valid) return false;
  *out = s_remote_details;
  return true;
}

esp_err_t weather_epd_get_remote_config(weather_remote_config_t *out) {
  if (!out) return ESP_ERR_INVALID_ARG;
  if (s_cfg_mutex) xSemaphoreTake(s_cfg_mutex, portMAX_DELAY);
  *out = s_remote_cfg;
  if (s_cfg_mutex) xSemaphoreGive(s_cfg_mutex);
  return ESP_OK;
}

esp_err_t weather_epd_set_remote_config(const weather_remote_config_t *cfg) {
  if (!cfg || !cfg->latitude[0] || !cfg->longitude[0] || !cfg->city[0]) return ESP_ERR_INVALID_ARG;
  weather_remote_config_t next = *cfg;
  next.api_key[sizeof(next.api_key) - 1] = '\0';
  next.latitude[sizeof(next.latitude) - 1] = '\0';
  next.longitude[sizeof(next.longitude) - 1] = '\0';
  next.city[sizeof(next.city) - 1] = '\0';
  next.timezone[sizeof(next.timezone) - 1] = '\0';
  next.ntp_server[sizeof(next.ntp_server) - 1] = '\0';
  next.holiday_endpoint[sizeof(next.holiday_endpoint) - 1] = '\0';
  if (!next.timezone[0]) strncpy(next.timezone, WEATHER_DEFAULT_TIMEZONE, sizeof(next.timezone) - 1);
  if (!next.ntp_server[0]) strncpy(next.ntp_server, WEATHER_DEFAULT_NTP_SERVER, sizeof(next.ntp_server) - 1);
  if (!next.holiday_endpoint[0])
    strncpy(next.holiday_endpoint, WEATHER_DEFAULT_HOLIDAY_ENDPOINT,
            sizeof(next.holiday_endpoint) - 1);
  if (next.refresh_minutes < 1) next.refresh_minutes = 30;
  if (next.refresh_minutes > 1440) next.refresh_minutes = 1440;
  nvs_handle_t nvs;
  esp_err_t err = nvs_open("weather_epd", NVS_READWRITE, &nvs);
  if (err != ESP_OK) return err;
  if ((err = nvs_set_u8(nvs, "enabled", next.enabled ? 1 : 0)) == ESP_OK)
    err = nvs_set_str(nvs, "owm_key", next.api_key);
  if (err == ESP_OK) err = nvs_set_str(nvs, "latitude", next.latitude);
  if (err == ESP_OK) err = nvs_set_str(nvs, "longitude", next.longitude);
  if (err == ESP_OK) err = nvs_set_str(nvs, "city", next.city);
  if (err == ESP_OK) err = nvs_set_str(nvs, "timezone", next.timezone);
  if (err == ESP_OK) err = nvs_set_str(nvs, "ntp_server", next.ntp_server);
  if (err == ESP_OK) err = nvs_set_str(nvs, "holiday_url", next.holiday_endpoint);
  if (err == ESP_OK) err = nvs_set_u32(nvs, "refresh_min", next.refresh_minutes);
  if (err == ESP_OK) err = nvs_commit(nvs);
  nvs_close(nvs);
  if (err != ESP_OK) return err;
  if (s_cfg_mutex) xSemaphoreTake(s_cfg_mutex, portMAX_DELAY);
  bool time_config_changed =
      strncmp(s_remote_cfg.timezone, next.timezone, sizeof(next.timezone)) != 0 ||
      strncmp(s_remote_cfg.ntp_server, next.ntp_server, sizeof(next.ntp_server)) != 0;
  s_remote_cfg = next;
  if (s_cfg_mutex) xSemaphoreGive(s_cfg_mutex);
  weather_epd_set_location(next.city);
  if (time_config_changed) {
    if (s_sntp_initialized) esp_netif_sntp_deinit();
    s_sntp_initialized = false;
    s_time_sync_needed = true;
  }
  if (s_task) xTaskNotifyGive(s_task);
  ESP_LOGI(TAG, "remote weather config saved: %s (%s,%s), every %um",
           next.enabled ? "enabled" : "disabled", next.latitude,
           next.longitude, (unsigned)next.refresh_minutes);
  return ESP_OK;
}

esp_err_t weather_epd_refresh_remote_now(void) { return weather_fetch_remote_retry(); }

esp_err_t weather_epd_request_calendar_refresh(void) {
  if (!s_task) return ESP_ERR_INVALID_STATE;
  s_page = WEATHER_EPAPER_PAGE_CALENDAR;
  xTaskNotifyGive(s_task);
  return ESP_OK;
}

esp_err_t weather_epd_init(void) {
  s_cfg_mutex = xSemaphoreCreateMutex();
  weather_remote_config_load();
  esp_err_t e1 = weather_epd_driver_init();
  esp_err_t e2 = weather_bme280_init();
  ESP_LOGI(TAG, "weather stack init: epd=%s bme=%s", esp_err_to_name(e1), esp_err_to_name(e2));
  /* The task performs the first coherent refresh. When remote weather is
   * configured it waits for network data first, matching the source project. */
  /* HTTPS/cJSON plus the renderer require more than the original 4 KiB task
   * stack, especially when a manual refresh wakes the display immediately. */
  if (!s_task)
    xTaskCreatePinnedToCore(weather_task, "weather_epd", 6144, NULL, 3,
                            &s_task, 0);
  return e1;
}

esp_err_t weather_epd_read_sensors(weather_sensor_data_t *out) {
  if (!out) return ESP_ERR_INVALID_ARG;
  *out = (weather_sensor_data_t){0};
  esp_err_t bme = weather_bme280_read(out);
  out->valid = bme == ESP_OK;
  return out->valid ? ESP_OK : ESP_FAIL;
}

esp_err_t weather_epd_refresh(weather_epaper_page_t page, const weather_sensor_data_t *sensor,
                              const char *location, const char *weather_text) {
  return weather_epd_driver_refresh(page, sensor, location, weather_text);
}

esp_err_t weather_epd_power_off(void) {
  return weather_epd_driver_power_off();
}
