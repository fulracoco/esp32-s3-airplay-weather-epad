#include "audio_output.h"

#include "settings.h"

#include "esp_log.h"

/* Backend entry points are private symbols produced by the two compiled
 * implementations.  Only one is initialized at a time. */
extern esp_err_t audio_output_i2s_init(void);
extern void audio_output_i2s_start(void);
extern void audio_output_i2s_stop(void);
extern esp_err_t audio_output_i2s_write(const void *, size_t, TickType_t);
extern void audio_output_i2s_set_sample_rate(uint32_t);
extern void audio_output_i2s_flush(void);
extern void audio_output_i2s_set_source_rate(int);
extern uint32_t audio_output_i2s_get_hardware_latency_us(void);
extern bool audio_output_i2s_get_pipeline_us(int64_t *, uint32_t *);
extern uint32_t audio_output_i2s_get_underruns(void);
extern bool audio_output_i2s_channel_mode_locked(void);
extern bool audio_output_i2s_channel_mode_in_dsp(void);
extern audio_channel_mode_t audio_output_i2s_cycle_channel_mode(void);
extern void audio_output_i2s_set_channel_mode(audio_channel_mode_t);
extern audio_channel_mode_t audio_output_i2s_get_channel_mode(void);

extern esp_err_t audio_output_usb_host_init(void);
extern void audio_output_usb_host_start(void);
extern void audio_output_usb_host_stop(void);
extern esp_err_t audio_output_usb_host_write(const void *, size_t, TickType_t);
extern void audio_output_usb_host_set_sample_rate(uint32_t);
extern void audio_output_usb_host_flush(void);
extern void audio_output_usb_host_set_source_rate(int);
extern uint32_t audio_output_usb_host_get_hardware_latency_us(void);
extern uint32_t audio_output_usb_host_get_underruns(void);

static const char *TAG = "audio_output";

#ifdef CONFIG_AUDIO_OUTPUT_USB_HOST
static audio_output_backend_t s_backend = AUDIO_OUTPUT_BACKEND_USB_HOST;
#else
static audio_output_backend_t s_backend = AUDIO_OUTPUT_BACKEND_I2S;
#endif
static bool s_initialized;
static volatile bool s_direct_mode;
/* Keep the normal I2S producer alive while a direct source is preparing its
 * first decoded frame.  MP3/HTTP setup can take longer than the DMA ring's
 * underrun window; stopping the producer immediately leaves the channel with
 * no clocked descriptors and every first direct write then times out. */
static volatile bool s_direct_write_started;

void audio_output_set_direct_mode(bool enabled) {
  s_direct_write_started = false;
  s_direct_mode = enabled;
  ESP_LOGI(TAG, "Direct output mode: %s", enabled ? "on" : "off");
}

bool audio_output_is_direct_mode(void) {
  return s_direct_mode && s_direct_write_started;
}

static bool use_usb(void) {
  return s_backend == AUDIO_OUTPUT_BACKEND_USB_HOST;
}

esp_err_t audio_output_init(void) {
  uint8_t saved = (uint8_t)s_backend;
  if (settings_get_output_backend(&saved) == ESP_OK && saved <= 1) {
    s_backend = (audio_output_backend_t)saved;
  }
  ESP_LOGI(TAG, "Selected output backend: %s",
           use_usb() ? "USB Host DAC" : "I2S DAC");
  esp_err_t err = use_usb() ? audio_output_usb_host_init()
                            : audio_output_i2s_init();
  s_initialized = err == ESP_OK;
  return err;
}

void audio_output_start(void) {
  if (use_usb()) {
    audio_output_usb_host_start();
  } else {
    audio_output_i2s_start();
  }
}

void audio_output_stop(void) {
  if (use_usb()) {
    audio_output_usb_host_stop();
  } else {
    audio_output_i2s_stop();
  }
}

esp_err_t audio_output_write(const void *data, size_t bytes, TickType_t wait) {
  if (s_direct_mode) {
    if (!s_direct_write_started) {
      ESP_LOGI(TAG, "Direct output first write: %u bytes", (unsigned)bytes);
    }
    s_direct_write_started = true;
  }
  return use_usb() ? audio_output_usb_host_write(data, bytes, wait)
                   : audio_output_i2s_write(data, bytes, wait);
}

void audio_output_set_sample_rate(uint32_t rate) {
  if (use_usb()) {
    audio_output_usb_host_set_sample_rate(rate);
  } else {
    audio_output_i2s_set_sample_rate(rate);
  }
}

void audio_output_flush(void) {
  if (use_usb()) {
    audio_output_usb_host_flush();
  } else {
    audio_output_i2s_flush();
  }
}

void audio_output_set_source_rate(int rate) {
  if (use_usb()) {
    audio_output_usb_host_set_source_rate(rate);
  } else {
    audio_output_i2s_set_source_rate(rate);
  }
}

uint32_t audio_output_get_hardware_latency_us(void) {
  return use_usb() ? audio_output_usb_host_get_hardware_latency_us()
                   : audio_output_i2s_get_hardware_latency_us();
}

bool audio_output_get_pipeline_us(int64_t *now_us, uint32_t *pipeline_us) {
  return use_usb() ? false
                   : audio_output_i2s_get_pipeline_us(now_us, pipeline_us);
}

uint32_t audio_output_get_underruns(void) {
  return use_usb() ? audio_output_usb_host_get_underruns()
                   : audio_output_i2s_get_underruns();
}

bool audio_output_channel_mode_locked(void) {
  return use_usb() ? true : audio_output_i2s_channel_mode_locked();
}

bool audio_output_channel_mode_in_dsp(void) {
  return use_usb() ? false : audio_output_i2s_channel_mode_in_dsp();
}

audio_channel_mode_t audio_output_cycle_channel_mode(void) {
  return use_usb() ? AUDIO_CHANNEL_STEREO
                   : audio_output_i2s_cycle_channel_mode();
}

void audio_output_set_channel_mode(audio_channel_mode_t mode) {
  if (!use_usb()) {
    audio_output_i2s_set_channel_mode(mode);
  }
}

audio_channel_mode_t audio_output_get_channel_mode(void) {
  return use_usb() ? AUDIO_CHANNEL_STEREO
                   : audio_output_i2s_get_channel_mode();
}

audio_output_backend_t audio_output_get_backend(void) { return s_backend; }

esp_err_t audio_output_set_backend(audio_output_backend_t backend) {
  if (backend > AUDIO_OUTPUT_BACKEND_USB_HOST) {
    return ESP_ERR_INVALID_ARG;
  }
  if (backend == s_backend) {
    return ESP_OK;
  }

  esp_err_t err = settings_set_output_backend((uint8_t)backend);
  if (err != ESP_OK) {
    return err;
  }

  /* Backends own hardware/tasks which cannot coexist.  Stop the active one;
   * the web handler reboots immediately so the selected backend is initialized
   * cleanly and no USB/I2S resources remain held by the old implementation. */
  if (s_initialized) {
    audio_output_stop();
  }
  s_backend = backend;
  ESP_LOGI(TAG, "Output backend changed to %s; restart required",
           use_usb() ? "USB Host DAC" : "I2S DAC");
  return ESP_OK;
}
