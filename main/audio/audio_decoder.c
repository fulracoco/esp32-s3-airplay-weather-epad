#include <netinet/in.h>
#include <stdlib.h>
#include <string.h>

#include "audio_decoder.h"

#include "esp_log.h"

#include "alac_magic_cookie.h"
#include "decoder/impl/esp_aac_dec.h"
#include "decoder/impl/esp_alac_dec.h"
#include "decoder/impl/esp_flac_dec.h"
#include "decoder/impl/esp_mp3_dec.h"
#include "esp_audio_dec.h"
#include "esp_audio_dec_reg.h"
#include "simple_dec/esp_audio_simple_dec.h"

#define MAX_FALLBACK_CHANNELS 2

typedef enum {
  AUDIO_DECODER_NONE = 0,
  AUDIO_DECODER_PCM,
  AUDIO_DECODER_ALAC,
  AUDIO_DECODER_AAC,
  AUDIO_DECODER_MP3,
  AUDIO_DECODER_FLAC
} audio_decoder_kind_t;

struct audio_decoder {
  audio_decoder_kind_t kind;
  audio_format_t format;
  void *alac_decoder;
  void *aac_decoder;
  void *mp3_decoder;
  void *flac_decoder;
  uint8_t alac_magic_cookie[ALAC_MAGIC_COOKIE_SIZE];
};

static const char *TAG = "audio_dec";

// Reopen the AAC decoder to reset its internal state after a corrupt frame.
// The codec's state machine can get stuck after certain errors (e.g. error 20)
// and will continue failing every subsequent frame until it is recreated.
static void aac_decoder_reset(audio_decoder_t *decoder) {
  if (decoder->aac_decoder) {
    esp_aac_dec_close(decoder->aac_decoder);
    decoder->aac_decoder = NULL;
  }

  esp_aac_dec_cfg_t aac_cfg = ESP_AAC_DEC_CONFIG_DEFAULT();
  aac_cfg.sample_rate = decoder->format.sample_rate;
  aac_cfg.channel = decoder->format.channels;
  aac_cfg.bits_per_sample =
      decoder->format.bits_per_sample ? decoder->format.bits_per_sample : 16;
  aac_cfg.no_adts_header = true;
  aac_cfg.aac_plus_enable = true;

  esp_audio_err_t err =
      esp_aac_dec_open(&aac_cfg, sizeof(aac_cfg), &decoder->aac_decoder);
  if (err != ESP_AUDIO_ERR_OK) {
    ESP_LOGE(TAG, "AAC decoder reset failed: %d", err);
    decoder->aac_decoder = NULL;
  } else {
    ESP_LOGW(TAG, "AAC decoder reset OK");
  }
}

static bool codec_is_alac(const char *codec) {
  if (!codec) {
    return false;
  }
  return strcmp(codec, "AppleLossless") == 0 || strcmp(codec, "ALAC") == 0;
}

static bool codec_is_aac(const char *codec) {
  if (!codec) {
    return false;
  }
  return strstr(codec, "AAC") != NULL || strstr(codec, "aac") != NULL ||
         strstr(codec, "mpeg4-generic") != NULL;
}

static bool codec_is_mp3(const char *codec) {
  return codec && (strstr(codec, "mp3") || strstr(codec, "MP3") ||
                   strstr(codec, "mpeg"));
}

static bool codec_is_flac(const char *codec) {
  return codec && (strstr(codec, "flac") || strstr(codec, "FLAC"));
}

static esp_audio_err_t stream_decoder_open(audio_decoder_kind_t kind,
                                           void **handle) {
  esp_audio_type_t frame_type;
  esp_audio_simple_dec_type_t stream_type;
  esp_audio_err_t err;

  if (kind == AUDIO_DECODER_MP3) {
    frame_type = ESP_AUDIO_TYPE_MP3;
    stream_type = ESP_AUDIO_SIMPLE_DEC_TYPE_MP3;
    if (!esp_audio_dec_get_ops(frame_type)) {
      err = esp_mp3_dec_register();
      if (err != ESP_AUDIO_ERR_OK) {
        return err;
      }
    }
  } else if (kind == AUDIO_DECODER_FLAC) {
    frame_type = ESP_AUDIO_TYPE_FLAC;
    stream_type = ESP_AUDIO_SIMPLE_DEC_TYPE_FLAC;
    if (!esp_audio_dec_get_ops(frame_type)) {
      err = esp_flac_dec_register();
      if (err != ESP_AUDIO_ERR_OK) {
        return err;
      }
    }
  } else {
    return ESP_AUDIO_ERR_NOT_SUPPORT;
  }

  esp_audio_simple_dec_cfg_t cfg = {
      .dec_type = stream_type,
      .dec_cfg = NULL,
      .cfg_size = 0,
      .use_frame_dec = false,
  };
  return esp_audio_simple_dec_open(&cfg, handle);
}

audio_decoder_t *audio_decoder_create(const audio_decoder_config_t *config) {
  if (!config) {
    return NULL;
  }

  audio_decoder_t *decoder = calloc(1, sizeof(*decoder));
  if (!decoder) {
    return NULL;
  }

  decoder->format = config->format;

  if (codec_is_alac(config->format.codec)) {
    decoder->kind = AUDIO_DECODER_ALAC;
    build_alac_magic_cookie(decoder->alac_magic_cookie, &config->format);

    esp_alac_dec_cfg_t alac_cfg = {.codec_spec_info =
                                       decoder->alac_magic_cookie,
                                   .spec_info_len = ALAC_MAGIC_COOKIE_SIZE};

    esp_audio_err_t err =
        esp_alac_dec_open(&alac_cfg, sizeof(alac_cfg), &decoder->alac_decoder);
    if (err != ESP_AUDIO_ERR_OK) {
      ESP_LOGE(TAG, "Failed to open ALAC decoder: %d", err);
      decoder->alac_decoder = NULL;
      decoder->kind = AUDIO_DECODER_NONE;
    }
  } else if (codec_is_aac(config->format.codec)) {
    decoder->kind = AUDIO_DECODER_AAC;

    esp_aac_dec_cfg_t aac_cfg = ESP_AAC_DEC_CONFIG_DEFAULT();
    aac_cfg.sample_rate = config->format.sample_rate;
    aac_cfg.channel = config->format.channels;
    aac_cfg.bits_per_sample =
        config->format.bits_per_sample ? config->format.bits_per_sample : 16;
    // AirPlay supplies raw AAC access units, not ADTS packets.  Supplying
    // the negotiated stream parameters directly avoids a synthetic header
    // with an incorrect fixed sample rate/channel layout.
    aac_cfg.no_adts_header = true;
    aac_cfg.aac_plus_enable = true;

    esp_audio_err_t err =
        esp_aac_dec_open(&aac_cfg, sizeof(aac_cfg), &decoder->aac_decoder);
    if (err != ESP_AUDIO_ERR_OK) {
      ESP_LOGE(TAG, "Failed to open AAC decoder: %d", err);
      decoder->aac_decoder = NULL;
      decoder->kind = AUDIO_DECODER_NONE;
    }
  } else if (codec_is_mp3(config->format.codec)) {
    decoder->kind = AUDIO_DECODER_MP3;
    esp_audio_err_t err =
        stream_decoder_open(decoder->kind, &decoder->mp3_decoder);
    if (err != ESP_AUDIO_ERR_OK) {
      ESP_LOGE(TAG, "Failed to open MP3 stream decoder: %d", err);
      decoder->mp3_decoder = NULL;
      decoder->kind = AUDIO_DECODER_NONE;
    }
  } else if (codec_is_flac(config->format.codec)) {
    decoder->kind = AUDIO_DECODER_FLAC;
    esp_audio_err_t err =
        stream_decoder_open(decoder->kind, &decoder->flac_decoder);
    if (err != ESP_AUDIO_ERR_OK) {
      ESP_LOGE(TAG, "Failed to open FLAC stream decoder: %d", err);
      decoder->flac_decoder = NULL;
      decoder->kind = AUDIO_DECODER_NONE;
    }
  } else if (strcmp(config->format.codec, "L16") == 0 ||
             strcmp(config->format.codec, "PCM") == 0 ||
             strcmp(config->format.codec, "PCM_LE") == 0) {
    decoder->kind = AUDIO_DECODER_PCM;
  } else {
    decoder->kind = AUDIO_DECODER_NONE;
  }

  return decoder;
}

void audio_decoder_destroy(audio_decoder_t *decoder) {
  if (!decoder) {
    return;
  }

  if (decoder->alac_decoder) {
    esp_alac_dec_close(decoder->alac_decoder);
    decoder->alac_decoder = NULL;
  }

  if (decoder->aac_decoder) {
    esp_aac_dec_close(decoder->aac_decoder);
    decoder->aac_decoder = NULL;
  }

  if (decoder->mp3_decoder) {
    esp_audio_simple_dec_close(decoder->mp3_decoder);
    decoder->mp3_decoder = NULL;
  }

  if (decoder->flac_decoder) {
    esp_audio_simple_dec_close(decoder->flac_decoder);
    decoder->flac_decoder = NULL;
  }

  free(decoder);
}

void audio_decoder_reset(audio_decoder_t *decoder) {
  if (!decoder) {
    return;
  }
  if (decoder->kind == AUDIO_DECODER_AAC && decoder->aac_decoder) {
    esp_audio_err_t err = esp_aac_dec_reset(decoder->aac_decoder);
    if (err != ESP_AUDIO_ERR_OK) {
      ESP_LOGW(TAG, "AAC decoder reset failed: %d", err);
    }
  }
  if (decoder->kind == AUDIO_DECODER_MP3 && decoder->mp3_decoder) {
    (void)esp_audio_simple_dec_reset(decoder->mp3_decoder);
  }
  if (decoder->kind == AUDIO_DECODER_FLAC && decoder->flac_decoder) {
    (void)esp_audio_simple_dec_reset(decoder->flac_decoder);
  }
}

int audio_decoder_decode(audio_decoder_t *decoder, const uint8_t *input,
                         size_t input_len, int16_t *output,
                         size_t output_capacity_samples,
                         audio_decode_info_t *info) {
  if (!decoder || !input || !output || output_capacity_samples == 0) {
    return -1;
  }

  int channels = decoder->format.channels;
  if (channels <= 0) {
    channels = MAX_FALLBACK_CHANNELS;
  }

  if (decoder->kind == AUDIO_DECODER_PCM) {
    size_t decoded_samples = input_len / (channels * sizeof(int16_t));
    if (decoded_samples > output_capacity_samples) {
      decoded_samples = output_capacity_samples;
    }

    const int16_t *src = (const int16_t *)input;
    bool little_endian = strcmp(decoder->format.codec, "PCM_LE") == 0;
    for (size_t i = 0; i < decoded_samples * channels; i++) {
      output[i] = little_endian ? src[i] : ntohs(src[i]);
    }

    if (info) {
      info->channels = channels;
      info->sample_rate = decoder->format.sample_rate;
      info->bits_per_sample = 16;
      info->consumed = decoded_samples * channels * sizeof(int16_t);
    }
    return (int)decoded_samples;
  }

  if (decoder->kind == AUDIO_DECODER_ALAC) {
    if (!decoder->alac_decoder) {
      return -1;
    }

    esp_audio_dec_in_raw_t raw = {.buffer = (uint8_t *)input,
                                  .len = (uint32_t)input_len,
                                  .consumed = 0,
                                  .frame_recover = ESP_AUDIO_DEC_RECOVERY_NONE};
    esp_audio_dec_out_frame_t frame = {
        .buffer = (uint8_t *)output,
        .len = (uint32_t)(output_capacity_samples * channels * sizeof(int16_t)),
        .decoded_size = 0};
    esp_audio_dec_info_t dec_info = {0};

    esp_audio_err_t err =
        esp_alac_dec_decode(decoder->alac_decoder, &raw, &frame, &dec_info);
    if (err != ESP_AUDIO_ERR_OK) {
      return -1;
    }

    int dec_channels = dec_info.channel > 0 ? dec_info.channel : channels;
    if (dec_channels <= 0) {
      dec_channels = MAX_FALLBACK_CHANNELS;
    }

    size_t decoded_samples =
        frame.decoded_size / (dec_channels * sizeof(int16_t));
    if (decoded_samples > output_capacity_samples) {
      decoded_samples = output_capacity_samples;
    }

    if (info) {
      info->channels = dec_channels;
      info->sample_rate = dec_info.sample_rate;
      info->bits_per_sample = dec_info.bits_per_sample;
      info->consumed = raw.consumed;
    }
    return (int)decoded_samples;
  }

  if (decoder->kind == AUDIO_DECODER_AAC) {
    if (!decoder->aac_decoder) {
      return -1;
    }

    esp_audio_dec_in_raw_t raw = {.buffer = (uint8_t *)input,
                                  .len = (uint32_t)input_len,
                                  .consumed = 0,
                                  .frame_recover = ESP_AUDIO_DEC_RECOVERY_NONE};
    esp_audio_dec_out_frame_t frame = {
        .buffer = (uint8_t *)output,
        .len = (uint32_t)(output_capacity_samples * channels * sizeof(int16_t)),
        .decoded_size = 0};
    esp_audio_dec_info_t dec_info = {0};

    esp_audio_err_t err =
        esp_aac_dec_decode(decoder->aac_decoder, &raw, &frame, &dec_info);
    if (err != ESP_AUDIO_ERR_OK) {
      ESP_LOGW(TAG, "AAC decode error %d — resetting decoder", err);
      aac_decoder_reset(decoder);
      return -1;
    }

    int dec_channels = dec_info.channel > 0 ? dec_info.channel : channels;
    if (dec_channels <= 0) {
      dec_channels = MAX_FALLBACK_CHANNELS;
    }

    size_t decoded_samples =
        frame.decoded_size / (dec_channels * sizeof(int16_t));
    if (decoded_samples > output_capacity_samples) {
      decoded_samples = output_capacity_samples;
    }

    if (info) {
      info->channels = dec_channels;
      info->sample_rate = dec_info.sample_rate;
      info->bits_per_sample = dec_info.bits_per_sample;
      info->consumed = raw.consumed;
    }
    return (int)decoded_samples;
  }

  if (decoder->kind == AUDIO_DECODER_MP3 ||
      decoder->kind == AUDIO_DECODER_FLAC) {
    void *handle = decoder->kind == AUDIO_DECODER_MP3
                       ? decoder->mp3_decoder
                       : decoder->flac_decoder;
    if (!handle) {
      return -1;
    }
    esp_audio_simple_dec_raw_t raw = {
        .buffer = (uint8_t *)input,
        .len = (uint32_t)input_len,
        .eos = false,
        .consumed = 0,
        .frame_recover = ESP_AUDIO_SIMPLE_DEC_RECOVERY_NONE,
    };
    esp_audio_simple_dec_out_t frame = {
        .buffer = (uint8_t *)output,
        .len = (uint32_t)(output_capacity_samples * channels * sizeof(int16_t)),
        .decoded_size = 0,
    };
    esp_audio_err_t err = esp_audio_simple_dec_process(handle, &raw, &frame);
    if (err != ESP_AUDIO_ERR_OK) {
      ESP_LOGW(TAG, "%s stream decode error: %d",
               decoder->kind == AUDIO_DECODER_MP3 ? "MP3" : "FLAC", err);
      return -1;
    }

    esp_audio_simple_dec_info_t dec_info = {0};
    int dec_channels = channels;
    if (frame.decoded_size > 0 &&
        esp_audio_simple_dec_get_info(handle, &dec_info) == ESP_AUDIO_ERR_OK &&
        dec_info.channel > 0) {
      dec_channels = dec_info.channel;
    }
    if (dec_channels <= 0) {
      dec_channels = MAX_FALLBACK_CHANNELS;
    }

    size_t decoded_samples =
        frame.decoded_size / (dec_channels * sizeof(int16_t));
    if (decoded_samples > output_capacity_samples) {
      decoded_samples = output_capacity_samples;
    }
    if (info) {
      info->channels = dec_channels;
      info->sample_rate = (int)dec_info.sample_rate;
      info->bits_per_sample = dec_info.bits_per_sample;
      info->consumed = raw.consumed;
    }
    return (int)decoded_samples;
  }

  return -1;
}

bool audio_decoder_is_aac(const audio_decoder_t *decoder) {
  return decoder && decoder->kind == AUDIO_DECODER_AAC;
}

bool audio_decoder_is_alac(const audio_decoder_t *decoder) {
  return decoder && decoder->kind == AUDIO_DECODER_ALAC;
}
