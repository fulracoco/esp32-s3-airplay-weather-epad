#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CALENDAR_PHOTO_SLOTS 6
#define CALENDAR_PHOTO_WIDTH 360
#define CALENDAR_PHOTO_HEIGHT 460
#define CALENDAR_PHOTO_BYTES \
  ((CALENDAR_PHOTO_WIDTH * CALENDAR_PHOTO_HEIGHT + 7) / 8)

/* The store contains the raw, MSB-first 1-bit bitmap used by the calendar
 * renderer.  Uploaded files are committed atomically after their length and
 * CRC have been verified. */
esp_err_t calendar_photo_store_begin(uint8_t slot, size_t size, uint32_t crc32);
esp_err_t calendar_photo_store_write(size_t offset, const void *data, size_t len);
esp_err_t calendar_photo_store_commit(void);
void calendar_photo_store_abort(void);

bool calendar_photo_store_has(uint8_t slot);
size_t calendar_photo_store_count(void);
esp_err_t calendar_photo_store_load(uint8_t slot, uint8_t *data, size_t capacity,
                                    size_t *out_len);

/* Select the next uploaded photo for the next calendar refresh.  Returns -1
 * when no uploaded photo exists, in which case the built-in photo is used. */
void calendar_photo_store_prepare_next(void);
int calendar_photo_store_selected(void);

#ifdef __cplusplus
}
#endif
