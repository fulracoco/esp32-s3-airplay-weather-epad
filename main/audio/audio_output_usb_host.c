/**
 * USB Host UAC output -- ESP32 drives an external USB DAC.
 *
 * The USB Host Library owns the OTG peripheral. usb_host_uac discovers UAC
 * speaker interfaces, and this backend opens the first interface supporting
 * stereo 16-bit PCM at 48 kHz. AirPlay PCM is resampled and written into the
 * class driver's asynchronous TX ring buffer.
 */

#include "audio_output.h"

#include "audio_receiver.h"
#include "audio_resample.h"
#include "led.h"
#include "rtsp_server.h"
#include "esp_intr_alloc.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "usb/uac_host.h"
#include "usb/usb_host.h"
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

/* Prefix the USB-host backend symbols so it can coexist with the I2S
 * implementation in the runtime output dispatcher. */
#define audio_output_init                    audio_output_usb_host_init
#define audio_output_start                   audio_output_usb_host_start
#define audio_output_stop                    audio_output_usb_host_stop
#define audio_output_write                   audio_output_usb_host_write
#define audio_output_set_sample_rate        audio_output_usb_host_set_sample_rate
#define audio_output_flush                   audio_output_usb_host_flush
#define audio_output_set_source_rate         audio_output_usb_host_set_source_rate
#define audio_output_get_hardware_latency_us audio_output_usb_host_get_hardware_latency_us
#define audio_output_get_underruns           audio_output_usb_host_get_underruns

#define TAG           "audio_usb_host"
#define OUTPUT_RATE   CONFIG_OUTPUT_SAMPLE_RATE_HZ
#define OUTPUT_BITS   16
#define OUTPUT_CH     2
#define FRAME_SAMPLES 352

#define USB_HOST_BUFFER_MS 200
#define USB_HOST_BUFFER_BYTES                                                  \
  ((uint32_t)(OUTPUT_RATE * OUTPUT_CH * sizeof(int16_t) * USB_HOST_BUFFER_MS / \
              1000))
#define USB_HOST_BUFFER_THRESHOLD  (USB_HOST_BUFFER_BYTES / 2)
#define USB_TRANSACTION_LATENCY_US 2000

#if CONFIG_OUTPUT_SAMPLE_RATE_HZ != 48000
#error "USB Host UAC output currently requires a 48 kHz output rate"
#endif

#define MAX_RESAMPLE_FRAMES \
  ((size_t)((FRAME_SAMPLES + 2) * ((double)OUTPUT_RATE / 44100) + 16))

#if CONFIG_FREERTOS_UNICORE
#define PLAYBACK_CORE 0
#else
#define PLAYBACK_CORE 1
#endif

typedef enum {
  UAC_EVENT_CONNECTED,
  UAC_EVENT_DEVICE,
} uac_event_kind_t;

typedef struct {
  uac_event_kind_t kind;
  union {
    struct {
      uint8_t addr;
      uint8_t iface_num;
      uac_host_driver_event_t event;
    } driver;
    struct {
      uac_host_device_handle_t handle;
      uac_host_device_event_t event;
    } device;
  } data;
} uac_event_msg_t;

static QueueHandle_t s_event_queue;
static SemaphoreHandle_t s_device_lock;
static uac_host_device_handle_t s_speaker;
static TaskHandle_t s_playback_task;
static volatile bool s_playback_running;
static volatile bool s_flush_requested;
static volatile int s_source_rate = 44100;
static volatile bool s_resample_reinit_needed;
static uint32_t s_transfer_errors;

static const uac_host_stream_config_t s_stream_config = {
    .channels = OUTPUT_CH,
    .bit_resolution = OUTPUT_BITS,
    .sample_freq = OUTPUT_RATE,
    .flags = FLAG_STREAM_SUSPEND_AFTER_START,
};

static bool alt_supports_rate(const uac_host_dev_alt_param_t *alt,
                              uint32_t rate) {
  if (alt->sample_freq_type == 0) {
    return alt->sample_freq_lower <= rate && rate <= alt->sample_freq_upper;
  }

  uint8_t count = alt->sample_freq_type;
  if (count > UAC_FREQ_NUM_MAX) {
    count = UAC_FREQ_NUM_MAX;
  }
  for (uint8_t i = 0; i < count; i++) {
    if (alt->sample_freq[i] == rate) {
      return true;
    }
  }
  return false;
}

static bool device_supports_output(uac_host_device_handle_t handle) {
  uac_host_dev_info_t info = {0};
  esp_err_t err = uac_host_get_device_info(handle, &info);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to read DAC information: %s", esp_err_to_name(err));
    return false;
  }

  ESP_LOGI(TAG, "USB DAC %04x:%04x, interface %u, %u alternate settings",
           info.VID, info.PID, info.iface_num, info.iface_alt_num);
  for (uint8_t alt_num = 1; alt_num <= info.iface_alt_num; alt_num++) {
    uac_host_dev_alt_param_t alt = {0};
    err = uac_host_get_device_alt_param(handle, alt_num, &alt);
    if (err != ESP_OK) {
      ESP_LOGW(TAG, "Cannot inspect alternate setting %u: %s", alt_num,
               esp_err_to_name(err));
      continue;
    }

    if (alt.format == UAC_TYPE_I_PCM && alt.channels == OUTPUT_CH &&
        alt.subframe_size == sizeof(int16_t) &&
        alt.bit_resolution == OUTPUT_BITS &&
        alt_supports_rate(&alt, OUTPUT_RATE)) {
      ESP_LOGI(TAG, "Selected stereo 16-bit PCM at %d Hz", OUTPUT_RATE);
      return true;
    }
  }

  ESP_LOGE(TAG, "DAC has no stereo 16-bit %d Hz UAC 1.0 stream", OUTPUT_RATE);
  return false;
}

static void device_event_callback(uac_host_device_handle_t handle,
                                  const uac_host_device_event_t event,
                                  void *arg) {
  (void)arg;
  if (event == UAC_HOST_DEVICE_EVENT_TX_DONE ||
      event == UAC_HOST_DEVICE_EVENT_RX_DONE) {
    return;
  }
  const uac_event_msg_t message = {
      .kind = UAC_EVENT_DEVICE,
      .data.device = {.handle = handle, .event = event},
  };
  if (xQueueSend(s_event_queue, &message, 0) != pdTRUE) {
    ESP_LOGE(TAG, "UAC device event queue full");
  }
}

static void driver_event_callback(uint8_t addr, uint8_t iface_num,
                                  const uac_host_driver_event_t event,
                                  void *arg) {
  (void)arg;
  if (event != UAC_HOST_DRIVER_EVENT_TX_CONNECTED) {
    return;
  }

  const uac_event_msg_t message = {
      .kind = UAC_EVENT_CONNECTED,
      .data.driver =
          {
              .addr = addr,
              .iface_num = iface_num,
              .event = event,
          },
  };
  if (xQueueSend(s_event_queue, &message, 0) != pdTRUE) {
    ESP_LOGE(TAG, "UAC driver event queue full");
  }
}

static void close_speaker(uac_host_device_handle_t handle) {
  if (xSemaphoreTake(s_device_lock, portMAX_DELAY) == pdTRUE) {
    if (s_speaker == handle) {
      s_speaker = NULL;
    }
    xSemaphoreGive(s_device_lock);
  }

  esp_err_t err = uac_host_device_close(handle);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "Failed to close USB DAC: %s", esp_err_to_name(err));
  }
}

static void open_speaker(uint8_t addr, uint8_t iface_num) {
  if (xSemaphoreTake(s_device_lock, portMAX_DELAY) != pdTRUE) {
    return;
  }
  bool already_open = s_speaker != NULL;
  xSemaphoreGive(s_device_lock);
  if (already_open) {
    ESP_LOGW(TAG, "Ignoring an additional USB DAC");
    return;
  }

  const uac_host_device_config_t config = {
      .addr = addr,
      .iface_num = iface_num,
      .buffer_size = USB_HOST_BUFFER_BYTES,
      .buffer_threshold = USB_HOST_BUFFER_THRESHOLD,
      .callback = device_event_callback,
      .callback_arg = NULL,
  };
  uac_host_device_handle_t handle = NULL;
  esp_err_t err = uac_host_device_open(&config, &handle);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to open USB DAC: %s", esp_err_to_name(err));
    return;
  }

  if (!device_supports_output(handle)) {
    close_speaker(handle);
    return;
  }

  err = uac_host_device_start(handle, &s_stream_config);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to configure USB DAC stream: %s",
             esp_err_to_name(err));
    close_speaker(handle);
    return;
  }

  if (xSemaphoreTake(s_device_lock, portMAX_DELAY) != pdTRUE) {
    uac_host_device_stop(handle);
    close_speaker(handle);
    return;
  }
  s_speaker = handle;
  if (s_playback_running) {
    err = uac_host_device_resume(handle);
    if (err != ESP_OK) {
      s_speaker = NULL;
    } else {
      // Volume and mute controls are optional in UAC 1.0. Keep software
      // volume authoritative, but ensure DACs that implement them start open.
      uac_host_device_set_volume(handle, 100);
      uac_host_device_set_mute(handle, false);
    }
  }
  xSemaphoreGive(s_device_lock);

  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to resume USB DAC stream: %s", esp_err_to_name(err));
    uac_host_device_stop(handle);
    close_speaker(handle);
    return;
  }
  ESP_LOGI(TAG, "USB DAC connected and ready");
}

static void uac_event_task(void *arg) {
  (void)arg;
  uac_event_msg_t message;
  while (true) {
    if (xQueueReceive(s_event_queue, &message, portMAX_DELAY) != pdTRUE) {
      continue;
    }

    if (message.kind == UAC_EVENT_CONNECTED) {
      open_speaker(message.data.driver.addr, message.data.driver.iface_num);
      continue;
    }

    switch (message.data.device.event) {
    case UAC_HOST_DRIVER_EVENT_DISCONNECTED:
      close_speaker(message.data.device.handle);
      ESP_LOGW(TAG, "USB DAC disconnected");
      break;
    case UAC_HOST_DEVICE_EVENT_TRANSFER_ERROR:
      __atomic_add_fetch(&s_transfer_errors, 1, __ATOMIC_RELAXED);
      ESP_LOGW(TAG, "USB DAC transfer error");
      break;
    case UAC_HOST_DEVICE_EVENT_TX_DONE:
    case UAC_HOST_DEVICE_EVENT_RX_DONE:
      break;
    default:
      break;
    }
  }
}

static void usb_host_event_task(void *arg) {
  (void)arg;
  while (true) {
    uint32_t event_flags = 0;
    esp_err_t err = usb_host_lib_handle_events(portMAX_DELAY, &event_flags);
    (void)event_flags;
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "USB Host event handling failed: %s", esp_err_to_name(err));
      vTaskDelay(pdMS_TO_TICKS(100));
    }
  }
}

static esp_err_t write_to_speaker(const void *data, size_t bytes,
                                  TickType_t wait) {
  if (!data || bytes == 0) {
    return ESP_ERR_INVALID_ARG;
  }
  if (bytes > UINT32_MAX) {
    return ESP_ERR_INVALID_SIZE;
  }
  if (xSemaphoreTake(s_device_lock, wait) != pdTRUE) {
    return ESP_ERR_TIMEOUT;
  }

  esp_err_t err = ESP_ERR_INVALID_STATE;
  if (s_speaker) {
    err = uac_host_device_write(s_speaker, (uint8_t *)data, (uint32_t)bytes,
                                wait);
  }
  xSemaphoreGive(s_device_lock);
  return err;
}

static void reset_stream(void) {
  if (xSemaphoreTake(s_device_lock, portMAX_DELAY) != pdTRUE) {
    return;
  }
  if (!s_speaker) {
    xSemaphoreGive(s_device_lock);
    return;
  }

  /* A stream flush only needs to discard buffered audio and pending USB
   * transfers. Suspending does that while retaining the claimed interface.
   * Fully stopping here releases the interface and can fail with
   * ESP_ERR_INVALID_STATE after selecting alternate setting zero. */
  esp_err_t err = uac_host_device_suspend(s_speaker);
  if (err == ESP_OK && s_playback_running) {
    err = uac_host_device_resume(s_speaker);
  }
  xSemaphoreGive(s_device_lock);

  if (err != ESP_OK) {
    ESP_LOGW(TAG, "Failed to reset USB DAC stream: %s", esp_err_to_name(err));
  }
}

static void apply_volume(int16_t *buf, size_t samples) {
  static int32_t current_q15 = -1;
  int32_t target_q15 = airplay_get_volume_q15();
  if (current_q15 < 0) {
    current_q15 = target_q15;
  }

  for (size_t i = 0; i < samples; i++) {
    if ((i & 1) == 0 && current_q15 != target_q15) {
      int32_t difference = target_q15 - current_q15;
      int32_t step = difference / 256;
      if (step == 0) {
        step = difference > 0 ? 1 : -1;
      }
      current_q15 += step;
    }
    buf[i] = (int16_t)(((int32_t)buf[i] * current_q15) >> 15);
  }
}

static void playback_task(void *arg) {
  (void)arg;
  int16_t *pcm = malloc((FRAME_SAMPLES + 1) * OUTPUT_CH * sizeof(int16_t));
  int16_t *silence = calloc(FRAME_SAMPLES * OUTPUT_CH, sizeof(int16_t));
  int16_t *resampled =
      malloc(MAX_RESAMPLE_FRAMES * OUTPUT_CH * sizeof(int16_t));
  if (!pcm || !silence || !resampled) {
    ESP_LOGE(TAG, "Failed to allocate USB playback buffers");
    free(pcm);
    free(silence);
    free(resampled);
    s_playback_task = NULL;
    vTaskDelete(NULL);
    return;
  }

  while (s_playback_running) {
    if (audio_output_is_direct_mode()) {
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }
    if (s_resample_reinit_needed) {
      s_resample_reinit_needed = false;
      audio_resample_init((uint32_t)s_source_rate, OUTPUT_RATE, OUTPUT_CH);
    }
    if (s_flush_requested) {
      s_flush_requested = false;
      audio_resample_reset();
      reset_stream();
    }

    size_t frames = audio_receiver_read(pcm, FRAME_SAMPLES + 1);
    int16_t *output = pcm;
    if (frames > 0 && audio_resample_is_active()) {
      frames =
          audio_resample_process(pcm, frames, resampled, MAX_RESAMPLE_FRAMES);
      output = resampled;
    } else if (frames == 0) {
      frames = FRAME_SAMPLES;
      output = silence;
    }

    if (output != silence) {
      apply_volume(output, frames * OUTPUT_CH);
    }
    led_audio_feed(output, frames);

    esp_err_t err = write_to_speaker(
        output, frames * OUTPUT_CH * sizeof(int16_t), pdMS_TO_TICKS(100));
    if (err == ESP_ERR_INVALID_STATE) {
      vTaskDelay(pdMS_TO_TICKS(10));
    } else if (err != ESP_OK && err != ESP_ERR_TIMEOUT) {
      ESP_LOGW(TAG, "USB DAC write failed: %s", esp_err_to_name(err));
      vTaskDelay(pdMS_TO_TICKS(10));
    }
  }

  free(pcm);
  free(silence);
  free(resampled);
  s_playback_task = NULL;
  vTaskDelete(NULL);
}

esp_err_t audio_output_init(void) {
  ESP_LOGI(TAG, "Initialising USB Host UAC output");

  s_event_queue = xQueueCreate(8, sizeof(uac_event_msg_t));
  s_device_lock = xSemaphoreCreateMutex();
  if (!s_event_queue || !s_device_lock) {
    return ESP_ERR_NO_MEM;
  }

  const usb_host_config_t host_config = {
      .skip_phy_setup = false,
      .intr_flags = ESP_INTR_FLAG_LEVEL1,
  };
  esp_err_t err = usb_host_install(&host_config);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "USB Host install failed: %s", esp_err_to_name(err));
    return err;
  }

  const uac_host_driver_config_t uac_config = {
      .create_background_task = true,
      .task_priority = 10,
      .stack_size = 4096,
      .core_id = 0,
      .callback = driver_event_callback,
      .callback_arg = NULL,
  };
  err = uac_host_install(&uac_config);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "UAC Host driver install failed: %s", esp_err_to_name(err));
    return err;
  }

  BaseType_t created = xTaskCreatePinnedToCore(
      usb_host_event_task, "usb_host_events", 4096, NULL, 15, NULL, 0);
  if (created != pdPASS) {
    return ESP_ERR_NO_MEM;
  }
  created = xTaskCreatePinnedToCore(uac_event_task, "uac_host_events", 4096,
                                    NULL, 5, NULL, 0);
  if (created != pdPASS) {
    return ESP_ERR_NO_MEM;
  }

  audio_resample_init(44100, OUTPUT_RATE, OUTPUT_CH);
  ESP_LOGI(TAG, "Connect a UAC 1.0 USB DAC to the OTG Type-C port");
  return ESP_OK;
}

void audio_output_start(void) {
  if (s_playback_task) {
    return;
  }

  s_playback_running = true;
  if (xSemaphoreTake(s_device_lock, portMAX_DELAY) == pdTRUE) {
    if (s_speaker) {
      esp_err_t err = uac_host_device_resume(s_speaker);
      if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "Failed to resume USB DAC: %s", esp_err_to_name(err));
      } else if (err == ESP_OK) {
        uac_host_device_set_volume(s_speaker, 100);
        uac_host_device_set_mute(s_speaker, false);
      }
    }
    xSemaphoreGive(s_device_lock);
  }

  BaseType_t created = xTaskCreatePinnedToCore(
      playback_task, "usb_host_play", 4096, NULL, AUDIO_PLAYBACK_TASK_PRIORITY,
      &s_playback_task, PLAYBACK_CORE);
  if (created != pdPASS) {
    s_playback_running = false;
    s_playback_task = NULL;
    if (xSemaphoreTake(s_device_lock, portMAX_DELAY) == pdTRUE) {
      if (s_speaker) {
        uac_host_device_suspend(s_speaker);
      }
      xSemaphoreGive(s_device_lock);
    }
    ESP_LOGE(TAG, "Failed to create USB playback task");
  }
}

void audio_output_stop(void) {
  s_playback_running = false;
  for (int timeout = 0; s_playback_task && timeout < 40; timeout++) {
    vTaskDelay(pdMS_TO_TICKS(50));
  }

  if (xSemaphoreTake(s_device_lock, portMAX_DELAY) == pdTRUE) {
    if (s_speaker) {
      esp_err_t err = uac_host_device_suspend(s_speaker);
      if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "Failed to suspend USB DAC: %s", esp_err_to_name(err));
      }
    }
    xSemaphoreGive(s_device_lock);
  }
}

esp_err_t audio_output_write(const void *data, size_t bytes, TickType_t wait) {
  return write_to_speaker(data, bytes, wait);
}

void audio_output_set_sample_rate(uint32_t rate) {
  if (rate != OUTPUT_RATE) {
    ESP_LOGW(TAG, "USB DAC output is fixed at %d Hz; requested %" PRIu32,
             OUTPUT_RATE, rate);
  }
}

void audio_output_flush(void) {
  s_flush_requested = true;
}

void audio_output_set_source_rate(int rate) {
  if (rate > 0 && rate != s_source_rate) {
    s_source_rate = rate;
    s_resample_reinit_needed = true;
  }
}

uint32_t audio_output_get_hardware_latency_us(void) {
  return USB_HOST_BUFFER_MS * 1000 + USB_TRANSACTION_LATENCY_US;
}

uint32_t audio_output_get_underruns(void) {
  return __atomic_load_n(&s_transfer_errors, __ATOMIC_RELAXED);
}

/* The UAC class driver does not expose its live TX ring fill level. The weak
 * audio_output_get_pipeline_us() default therefore uses the model above. */
