#include "power_mode.h"
#include "weather_epd.h"
#include "network/wifi.h"
#include "settings.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdatomic.h>

static atomic_bool s_low;
static atomic_bool s_weather_network;

void power_mode_toggle(void) {
  bool low = !atomic_load(&s_low);
  power_mode_set_low(low);
}

void power_mode_set_low(bool low) {
  atomic_store(&s_low, low);
  ESP_LOGI("power_mode", "Requested %s mode", low ? "low-power" : "normal");
}

bool power_mode_is_low(void) {
  return atomic_load(&s_low);
}

bool power_mode_weather_network_requested(void) {
  return atomic_load(&s_weather_network);
}

bool weather_epd_network_begin(void) {
  /* Claim even in normal mode: a button press must not interrupt a fetch. */
  atomic_store(&s_weather_network, true);
  if (!power_mode_is_low()) {
    return true;
  }
  if (!settings_has_wifi_credentials()) {
    return false;
  }
  for (int i = 0; i < 150; ++i) {
    if (wifi_is_connected()) {
      return true;
    }
    vTaskDelay(pdMS_TO_TICKS(200));
  }
  ESP_LOGW("power_mode", "Weather network window timed out");
  return false;
}

void weather_epd_network_end(void) {
  atomic_store(&s_weather_network, false);
}
