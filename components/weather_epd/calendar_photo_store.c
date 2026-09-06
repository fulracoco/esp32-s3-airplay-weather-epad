#include "calendar_photo_store.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "sys/stat.h"

static const char *TAG = "calendar_store";
static const char *BASE_PATH = "/spiffs/calendar";

typedef struct {
  FILE *file;
  uint8_t slot;
  size_t expected_size;
  size_t written;
  uint32_t expected_crc;
  uint32_t crc;
} upload_state_t;

static upload_state_t s_upload;
static SemaphoreHandle_t s_lock;
static int s_selected = -1;

static uint32_t crc32_update(uint32_t crc, const uint8_t *data, size_t len) {
  while (len--) {
    crc ^= *data++;
    for (unsigned bit = 0; bit < 8; ++bit) {
      crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
  }
  return crc;
}

static void ensure_lock(void) {
  if (!s_lock) {
    s_lock = xSemaphoreCreateMutex();
  }
}

static void path_for(char *path, size_t capacity, uint8_t slot, bool temp) {
  snprintf(path, capacity, "%s/photo%u.bin%s", BASE_PATH, (unsigned)slot,
           temp ? ".tmp" : "");
}

static bool file_exists(uint8_t slot) {
  char path[64];
  path_for(path, sizeof(path), slot, false);
  struct stat st;
  return stat(path, &st) == 0 && (size_t)st.st_size == CALENDAR_PHOTO_BYTES;
}

esp_err_t calendar_photo_store_begin(uint8_t slot, size_t size, uint32_t crc32) {
  if (slot >= CALENDAR_PHOTO_SLOTS || size != CALENDAR_PHOTO_BYTES) {
    return ESP_ERR_INVALID_ARG;
  }
  ensure_lock();
  xSemaphoreTake(s_lock, portMAX_DELAY);
  calendar_photo_store_abort();

  (void)mkdir(BASE_PATH, 0775);
  char path[64];
  path_for(path, sizeof(path), slot, true);
  FILE *file = fopen(path, "wb");
  if (!file) {
    xSemaphoreGive(s_lock);
    return ESP_FAIL;
  }

  s_upload = (upload_state_t){
      .file = file,
      .slot = slot,
      .expected_size = size,
      .expected_crc = crc32,
      .crc = 0xFFFFFFFFu,
  };
  xSemaphoreGive(s_lock);
  return ESP_OK;
}

esp_err_t calendar_photo_store_write(size_t offset, const void *data, size_t len) {
  if (!data || len == 0) return ESP_ERR_INVALID_ARG;
  ensure_lock();
  xSemaphoreTake(s_lock, portMAX_DELAY);
  if (!s_upload.file || offset != s_upload.written ||
      len > s_upload.expected_size - s_upload.written) {
    xSemaphoreGive(s_lock);
    return ESP_ERR_INVALID_STATE;
  }
  size_t written = fwrite(data, 1, len, s_upload.file);
  if (written != len) {
    fclose(s_upload.file);
    s_upload.file = NULL;
    xSemaphoreGive(s_lock);
    return ESP_FAIL;
  }
  s_upload.written += len;
  s_upload.crc = crc32_update(s_upload.crc, data, len);
  xSemaphoreGive(s_lock);
  return ESP_OK;
}

esp_err_t calendar_photo_store_commit(void) {
  ensure_lock();
  xSemaphoreTake(s_lock, portMAX_DELAY);
  if (!s_upload.file) {
    xSemaphoreGive(s_lock);
    return ESP_ERR_INVALID_STATE;
  }
  bool valid = s_upload.written == s_upload.expected_size &&
               ~s_upload.crc == s_upload.expected_crc;
  uint8_t slot = s_upload.slot;
  fclose(s_upload.file);
  s_upload.file = NULL;
  if (!valid) {
    char tmp[64];
    path_for(tmp, sizeof(tmp), slot, true);
    remove(tmp);
    xSemaphoreGive(s_lock);
    return ESP_ERR_INVALID_CRC;
  }

  char tmp[64], dst[64];
  path_for(tmp, sizeof(tmp), slot, true);
  path_for(dst, sizeof(dst), slot, false);
  remove(dst);
  int rc = rename(tmp, dst);
  if (rc != 0) {
    remove(tmp);
    xSemaphoreGive(s_lock);
    return ESP_FAIL;
  }
  ESP_LOGI(TAG, "committed photo slot %u", (unsigned)slot);
  xSemaphoreGive(s_lock);
  return ESP_OK;
}

void calendar_photo_store_abort(void) {
  if (s_upload.file) {
    uint8_t slot = s_upload.slot;
    fclose(s_upload.file);
    s_upload.file = NULL;
    char tmp[64];
    path_for(tmp, sizeof(tmp), slot, true);
    remove(tmp);
  }
  memset(&s_upload, 0, sizeof(s_upload));
}

bool calendar_photo_store_has(uint8_t slot) {
  if (slot >= CALENDAR_PHOTO_SLOTS) return false;
  ensure_lock();
  xSemaphoreTake(s_lock, portMAX_DELAY);
  bool result = file_exists(slot);
  xSemaphoreGive(s_lock);
  return result;
}

size_t calendar_photo_store_count(void) {
  size_t count = 0;
  ensure_lock();
  xSemaphoreTake(s_lock, portMAX_DELAY);
  for (uint8_t slot = 0; slot < CALENDAR_PHOTO_SLOTS; ++slot) {
    if (file_exists(slot)) ++count;
  }
  xSemaphoreGive(s_lock);
  return count;
}

esp_err_t calendar_photo_store_load(uint8_t slot, uint8_t *data, size_t capacity,
                                    size_t *out_len) {
  if (slot >= CALENDAR_PHOTO_SLOTS || !data || capacity < CALENDAR_PHOTO_BYTES) {
    return ESP_ERR_INVALID_ARG;
  }
  ensure_lock();
  xSemaphoreTake(s_lock, portMAX_DELAY);
  char path[64];
  path_for(path, sizeof(path), slot, false);
  FILE *file = fopen(path, "rb");
  if (!file) {
    xSemaphoreGive(s_lock);
    return ESP_ERR_NOT_FOUND;
  }
  size_t got = fread(data, 1, capacity, file);
  fclose(file);
  xSemaphoreGive(s_lock);
  if (got != CALENDAR_PHOTO_BYTES) return ESP_FAIL;
  if (out_len) *out_len = got;
  return ESP_OK;
}

void calendar_photo_store_prepare_next(void) {
  ensure_lock();
  xSemaphoreTake(s_lock, portMAX_DELAY);
  uint8_t slots[CALENDAR_PHOTO_SLOTS];
  size_t count = 0;
  for (uint8_t slot = 0; slot < CALENDAR_PHOTO_SLOTS; ++slot) {
    if (file_exists(slot)) slots[count++] = slot;
  }
  if (count == 0) {
    s_selected = -1;
  } else {
    size_t next = 0;
    for (size_t i = 0; i < count; ++i) {
      if (slots[i] == s_selected) {
        next = (i + 1) % count;
        break;
      }
    }
    s_selected = slots[next];
  }
  xSemaphoreGive(s_lock);
}

int calendar_photo_store_selected(void) {
  ensure_lock();
  xSemaphoreTake(s_lock, portMAX_DELAY);
  int selected = s_selected;
  xSemaphoreGive(s_lock);
  return selected;
}
