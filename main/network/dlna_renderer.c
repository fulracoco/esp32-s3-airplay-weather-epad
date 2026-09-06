#include "dlna_renderer.h"

#include "audio_decoder.h"
#include "audio_output.h"
#include "audio_resample.h"
#include "audio_receiver.h"
#include "dac.h"
#include "playback_control.h"
#include "rtsp_server.h"

#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_ssdp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "media_lib_adapter.h"
#include <ctype.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TAG "dlna"
#define URI_MAX 512
#define STREAM_BUF 32768
#define SOAP_BODY_MAX 8192
/* Decode complete codec frames, then split PCM writes below the UAC
 * ring-buffer capacity (about 9.6 KB at 48 kHz). */
#define PCM_SAMPLES 2048
#define PCM_OUTPUT_SAMPLES 4096
#define PCM_WRITE_FRAMES 256

typedef enum {
  DLNA_STOPPED = 0,
  DLNA_PLAYING,
  DLNA_PAUSED,
} dlna_state_t;

static SemaphoreHandle_t s_lock;
static TaskHandle_t s_stream_task;
static EventGroupHandle_t s_events;
static char s_uri[URI_MAX];
static dlna_state_t s_state = DLNA_STOPPED;
static uint8_t s_volume = 50;

/* esp_ssdp adds the UUID prefix when constructing USN values; keep the UDN
 * itself unprefixed as required by the component API. */
static char s_ssdp_udn[] = "esp32-airplay-renderer";
static char s_ssdp_location[] = "http://${ip}:80/dlna/device.xml";
static const ssdp_service_t s_ssdp_services[] = {
    {(char *)"esp32-airplay-renderer", (char *)"upnp:rootdevice", NULL},
    {(char *)"esp32-airplay-renderer",
     (char *)"urn:schemas-upnp-org:device:MediaRenderer:1", NULL},
    {(char *)"esp32-airplay-renderer",
     (char *)"urn:schemas-upnp-org:service:AVTransport:1", NULL},
    {(char *)"esp32-airplay-renderer",
     (char *)"urn:schemas-upnp-org:service:RenderingControl:1", NULL},
    {(char *)"esp32-airplay-renderer",
     (char *)"urn:schemas-upnp-org:service:ConnectionManager:1", NULL},
    {NULL, NULL, NULL},
};

#define STREAM_STOP BIT0
#define STREAM_WAKE BIT1

static const char *state_text(void) {
  switch (s_state) {
  case DLNA_PLAYING: return "PLAYING";
  case DLNA_PAUSED: return "PAUSED_PLAYBACK";
  default: return "STOPPED";
  }
}

static int body_read(httpd_req_t *req, char *buf, size_t cap) {
  int total = req->content_len;
  if (total <= 0 || (size_t)total >= cap) return -1;
  int got = 0;
  while (got < total) {
    int n = httpd_req_recv(req, buf + got, total - got);
    if (n <= 0) return -1;
    got += n;
  }
  buf[got] = 0;
  return got;
}

static void xml_unescape(char *value) {
  char *src = value;
  char *dst = value;
  while (*src) {
    if (!strncmp(src, "&amp;", 5)) {
      *dst++ = '&';
      src += 5;
    } else if (!strncmp(src, "&lt;", 4)) {
      *dst++ = '<';
      src += 4;
    } else if (!strncmp(src, "&gt;", 4)) {
      *dst++ = '>';
      src += 4;
    } else if (!strncmp(src, "&quot;", 6)) {
      *dst++ = '"';
      src += 6;
    } else if (!strncmp(src, "&apos;", 6)) {
      *dst++ = '\'';
      src += 6;
    } else {
      *dst++ = *src++;
    }
  }
  *dst = 0;
}

static bool xml_value(const char *xml, const char *tag, char *out,
                      size_t out_cap) {
  const size_t tag_len = strlen(tag);
  const char *p = xml;
  while ((p = strstr(p, tag)) != NULL) {
    const char before = p > xml ? p[-1] : 0;
    const char after = p[tag_len];
    if ((before == '<' || before == ':') &&
        (after == '>' || after == '/' || isspace((unsigned char)after))) {
      const char *open_end = strchr(p + tag_len, '>');
      if (!open_end) return false;
      const char *value_start = open_end + 1;
      const char *value_end = strchr(value_start, '<');
      if (!value_end) return false;
      while (value_start < value_end &&
             isspace((unsigned char)*value_start)) {
        value_start++;
      }
      while (value_end > value_start &&
             isspace((unsigned char)value_end[-1])) {
        value_end--;
      }
      size_t len = (size_t)(value_end - value_start);
      if (len >= out_cap) len = out_cap - 1;
      memcpy(out, value_start, len);
      out[len] = 0;
      xml_unescape(out);
      return true;
    }
    p += tag_len;
  }
  return false;
}

static void soap_reply(httpd_req_t *req, const char *service,
                       const char *action, const char *payload) {
  const char *body = payload ? payload : "";
  int needed = snprintf(NULL, 0,
           "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
           "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
           "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\"><s:Body>"
           "<u:%sResponse xmlns:u=\"urn:schemas-upnp-org:service:%s:1\">%s</u:%sResponse>"
           "</s:Body></s:Envelope>", action, service, body, action);
  if (needed < 0) {
    httpd_resp_send_500(req);
    return;
  }
  char *xml = malloc((size_t)needed + 1);
  if (!xml) {
    httpd_resp_send_500(req);
    return;
  }
  snprintf(xml, (size_t)needed + 1,
           "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
           "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
           "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\"><s:Body>"
           "<u:%sResponse xmlns:u=\"urn:schemas-upnp-org:service:%s:1\">%s</u:%sResponse>"
           "</s:Body></s:Envelope>", action, service, body, action);
  httpd_resp_set_type(req, "text/xml; charset=\"utf-8\"");
  httpd_resp_send(req, xml, HTTPD_RESP_USE_STRLEN);
  free(xml);
}

static void soap_error(httpd_req_t *req, int code, const char *description) {
  char xml[512];
  snprintf(xml, sizeof(xml),
           "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\"><s:Body>"
           "<s:Fault><faultcode>s:Client</faultcode><detail><UPnPError "
           "xmlns=\"urn:schemas-upnp-org:control-1-0\"><errorCode>%d</errorCode>"
           "<errorDescription>%s</errorDescription></UPnPError></detail></s:Fault>"
           "</s:Body></s:Envelope>", code, description);
  httpd_resp_set_status(req, "500 Internal Server Error");
  httpd_resp_set_type(req, "text/xml; charset=\"utf-8\"");
  httpd_resp_send(req, xml, HTTPD_RESP_USE_STRLEN);
}

static bool stop_stream(void) {
  TaskHandle_t task = NULL;
  if (xSemaphoreTake(s_lock, portMAX_DELAY) == pdTRUE) {
    task = (TaskHandle_t)s_stream_task;
    xSemaphoreGive(s_lock);
  }
  if (!task) {
    return true;
  }

  xEventGroupSetBits(s_events, STREAM_STOP | STREAM_WAKE);

  /* The stream task owns the HTTP client, decoder, output and resampler. Do
   * not start another stream until all of those resources have been released.
   * HTTP reads are bounded by the client timeout, so this also avoids an
   * unbounded wait when the peer disappears. */
  for (int i = 0; i < 600; ++i) {
    bool stopped;
    if (xSemaphoreTake(s_lock, portMAX_DELAY) != pdTRUE) {
      return false;
    }
    stopped = s_stream_task == NULL;
    xSemaphoreGive(s_lock);
    if (stopped) {
      return true;
    }
    vTaskDelay(pdMS_TO_TICKS(20));
  }

  ESP_LOGW(TAG, "Timed out waiting for DLNA stream to stop");
  return false;
}

static const char *codec_from_type(const char *type, const char *uri) {
  if ((type && strstr(type, "flac")) || (uri && strstr(uri, ".flac"))) return "FLAC";
  if ((type && (strstr(type, "mpeg") || strstr(type, "mp3"))) ||
      (uri && (strstr(uri, ".mp3") || strstr(uri, ".mpeg")))) return "MP3";
  if ((type && (strstr(type, "aac") || strstr(type, "mp4"))) ||
      (uri && (strstr(uri, ".aac") || strstr(uri, ".m4a")))) return "AAC";
  if ((type && (strstr(type, "wav") || strstr(type, "l16") || strstr(type, "pcm"))) ||
      (uri && strstr(uri, ".wav"))) return "WAV";
  return NULL;
}

static bool parse_wav(const uint8_t *h, size_t n, audio_format_t *fmt,
                      size_t *data_offset) {
  if (n < 44 || memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4)) return false;
  uint16_t format = h[20] | ((uint16_t)h[21] << 8);
  uint16_t channels = h[22] | ((uint16_t)h[23] << 8);
  uint32_t rate = h[24] | ((uint32_t)h[25] << 8) | ((uint32_t)h[26] << 16) |
                 ((uint32_t)h[27] << 24);
  if (format != 1 || channels == 0 || channels > 2 || rate == 0) return false;
  strcpy(fmt->codec, "PCM_LE");
  fmt->channels = channels;
  fmt->sample_rate = rate;
  fmt->bits_per_sample = h[34] | ((uint16_t)h[35] << 8);
  *data_offset = 44;
  return true;
}

static void stream_task(void *arg) {
  (void)arg;
  char uri[URI_MAX];
  xSemaphoreTake(s_lock, portMAX_DELAY);
  strncpy(uri, s_uri, sizeof(uri) - 1);
  uri[sizeof(uri) - 1] = 0;
  xSemaphoreGive(s_lock);

  esp_http_client_config_t cfg = {
      .url = uri,
      .timeout_ms = 10000,
      .buffer_size = 8192,
      .buffer_size_tx = 1024,
      .keep_alive_enable = true,
      .user_agent = "ESP32-DLNA/1.0",
  };
  esp_http_client_handle_t client = esp_http_client_init(&cfg);
  audio_decoder_t *decoder = NULL;
  uint8_t *encoded = malloc(STREAM_BUF);
  int16_t *pcm = malloc(PCM_SAMPLES * 2 * sizeof(int16_t));
  int16_t *output_pcm = malloc(PCM_OUTPUT_SAMPLES * 2 * sizeof(int16_t));
  int source_rate = 0;
  int source_channels = 2;
  if (!client || !encoded || !pcm || !output_pcm ||
      esp_http_client_open(client, 0) != ESP_OK) {
    ESP_LOGE(TAG, "Unable to open media URL: %s", uri);
    goto done;
  }
  int64_t content_len = esp_http_client_fetch_headers(client);
  if (content_len < 0) goto done;
  char content_type[96] = {0};
  char *content_type_value = NULL;
  if (esp_http_client_get_header(client, "Content-Type", &content_type_value) ==
          ESP_OK &&
      content_type_value) {
    strncpy(content_type, content_type_value, sizeof(content_type) - 1);
  }
  ESP_LOGI(TAG, "Media response: status=%d length=%" PRId64 " type=%s",
           esp_http_client_get_status_code(client), content_len,
           content_type[0] ? content_type : "(none)");
  const char *codec = codec_from_type(content_type, uri);
  audio_format_t fmt = {0};
  size_t used = 0;
  bool wav = codec && strcmp(codec, "WAV") == 0;
  if (wav) {
    int n = esp_http_client_read(client, (char *)encoded, 44);
    size_t offset = 0;
    if (n < 44 || !parse_wav(encoded, 44, &fmt, &offset)) goto done;
    used = (size_t)n - offset;
    memmove(encoded, encoded + offset, used);
  } else {
    if (!codec) codec = "MP3";
    strncpy(fmt.codec, codec, sizeof(fmt.codec) - 1);
    fmt.channels = 2;
    fmt.sample_rate = 0;
    fmt.bits_per_sample = 16;
  }
  audio_decoder_config_t dec_cfg = {.format = fmt};
  decoder = audio_decoder_create(&dec_cfg);
  if (!decoder) goto done;
  rtsp_server_stop();
  audio_receiver_stop();
  audio_output_stop();
  audio_output_set_direct_mode(true);
  audio_output_start();
  playback_control_set_source(PLAYBACK_SOURCE_DLNA);
  ESP_LOGI(TAG, "DLNA playback started: codec=%s uri=%s", fmt.codec, uri);

  while (!(xEventGroupGetBits(s_events) & STREAM_STOP)) {
    if (xEventGroupGetBits(s_events) & STREAM_WAKE) {
      xEventGroupClearBits(s_events, STREAM_WAKE);
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool paused = s_state == DLNA_PAUSED;
    xSemaphoreGive(s_lock);
    if (paused) {
      xEventGroupWaitBits(s_events, STREAM_WAKE | STREAM_STOP, pdFALSE, pdFALSE,
                          pdMS_TO_TICKS(200));
      continue;
    }
    if (used < STREAM_BUF) {
      int n = esp_http_client_read(client, (char *)encoded + used, STREAM_BUF - used);
      if (n == 0) break;
      if (n < 0) { ESP_LOGW(TAG, "HTTP stream read failed"); break; }
      used += (size_t)n;
    }
    size_t offset = 0;
    while (offset < used) {
      audio_decode_info_t info = {0};
      int samples = audio_decoder_decode(decoder, encoded + offset, used - offset,
                                         pcm, PCM_SAMPLES, &info);
      if (samples < 0 || info.consumed == 0) break;
      offset += info.consumed;
      if (samples == 0) continue;
      int channels = info.channels > 0 ? info.channels : 2;
      if (info.sample_rate > 0 &&
          (info.sample_rate != source_rate || channels != source_channels)) {
        source_rate = info.sample_rate;
        source_channels = channels;
        audio_output_set_source_rate(source_rate);
        audio_resample_init((uint32_t)source_rate,
                            CONFIG_OUTPUT_SAMPLE_RATE_HZ, source_channels);
        ESP_LOGI(TAG, "DLNA audio format: %d Hz, %d channels", source_rate,
                 source_channels);
      }
      int16_t *write_buf = pcm;
      size_t write_frames = (size_t)samples;
      if (audio_resample_is_active()) {
        write_frames = audio_resample_process(
            pcm, (size_t)samples, output_pcm, PCM_OUTPUT_SAMPLES);
        write_buf = output_pcm;
      }
      size_t written_frames = 0;
      while (written_frames < write_frames &&
             !(xEventGroupGetBits(s_events) & STREAM_STOP)) {
        size_t chunk_frames = write_frames - written_frames;
        if (chunk_frames > PCM_WRITE_FRAMES) {
          chunk_frames = PCM_WRITE_FRAMES;
        }
        esp_err_t write_err = audio_output_write(
            write_buf + written_frames * (size_t)channels,
            chunk_frames * (size_t)channels * sizeof(int16_t),
            pdMS_TO_TICKS(100));
        if (write_err != ESP_OK) {
          ESP_LOGW(TAG, "DLNA audio output failed: %s",
                   esp_err_to_name(write_err));
          break;
        }
        written_frames += chunk_frames;
      }
    }
    if (offset) { memmove(encoded, encoded + offset, used - offset); used -= offset; }
    if (used == STREAM_BUF) { ESP_LOGW(TAG, "DLNA decoder stalled"); break; }
  }

done:
  if (decoder) audio_decoder_destroy(decoder);
  if (client) { esp_http_client_close(client); esp_http_client_cleanup(client); }
  free(encoded); free(pcm); free(output_pcm);
  audio_output_stop();
  audio_output_set_direct_mode(false);
  audio_resample_destroy();
  xEventGroupClearBits(s_events, STREAM_STOP | STREAM_WAKE);
  xSemaphoreTake(s_lock, portMAX_DELAY);
  s_state = DLNA_STOPPED;
  s_stream_task = NULL;
  xSemaphoreGive(s_lock);
  vTaskDelete(NULL);
}

static bool start_stream(void) {
  if (!stop_stream()) {
    return false;
  }
  xSemaphoreTake(s_lock, portMAX_DELAY);
  if (!s_uri[0] || s_stream_task) {
    xSemaphoreGive(s_lock);
    return false;
  }
  xEventGroupClearBits(s_events, STREAM_STOP | STREAM_WAKE);
  s_state = DLNA_PLAYING;
  BaseType_t created = xTaskCreate(stream_task, "dlna_stream", 8192, NULL, 5,
                                   &s_stream_task);
  xSemaphoreGive(s_lock);
  if (created != pdPASS) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_state = DLNA_STOPPED;
    s_stream_task = NULL;
    xSemaphoreGive(s_lock);
  }
  return created == pdPASS;
}

static esp_err_t avtransport_post(httpd_req_t *req) {
  char *body = malloc(SOAP_BODY_MAX);
  if (!body) {
    httpd_resp_send_500(req);
    return ESP_OK;
  }
  if (body_read(req, body, SOAP_BODY_MAX) < 0) {
    free(body);
    return ESP_FAIL;
  }
  static const char *const actions[] = {
      "SetAVTransportURI", "SetNextAVTransportURI", "GetTransportInfo",
      "GetPositionInfo",   "GetMediaInfo",          "GetDeviceCapabilities",
      "GetTransportSettings", "GetTransportActions", "Play", "Pause",
      "Stop", "Seek", NULL};
  const char *action = NULL;
  for (size_t i = 0; actions[i]; ++i) {
    if (strstr(body, actions[i])) {
      action = actions[i];
      break;
    }
  }
  if (!action) { soap_error(req, 401, "Invalid Action"); free(body); return ESP_OK; }
  if (!strcmp(action, "SetAVTransportURI")) {
    char uri[URI_MAX];
    if (!xml_value(body, "CurrentURI", uri, sizeof(uri)) || !uri[0]) {
      ESP_LOGW(TAG, "SetAVTransportURI missing CurrentURI");
      soap_error(req, 402, "Invalid Args");
    } else {
      if (!stop_stream()) {
        soap_error(req, 501, "Action Failed");
        free(body);
        return ESP_OK;
      }
      xSemaphoreTake(s_lock, portMAX_DELAY);
      strncpy(s_uri, uri, sizeof(s_uri) - 1);
      s_uri[sizeof(s_uri) - 1] = 0;
      s_state = DLNA_STOPPED;
      xSemaphoreGive(s_lock);
      ESP_LOGI(TAG, "DLNA URI set: %s", s_uri);
      soap_reply(req, "AVTransport", action, "");
    }
  } else if (!strcmp(action, "Play")) {
    bool already_playing = false;
    bool resume = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    already_playing = s_stream_task != NULL && s_state == DLNA_PLAYING;
    resume = s_stream_task != NULL && s_state == DLNA_PAUSED;
    if (resume) {
      s_state = DLNA_PLAYING;
    }
    xSemaphoreGive(s_lock);
    if (resume) {
      xEventGroupSetBits(s_events, STREAM_WAKE);
      soap_reply(req, "AVTransport", action, "");
    } else if (already_playing) {
      /* Controllers commonly retry Play while waiting for the SOAP reply.
       * Restarting the stream here makes the first seconds loop forever. */
      soap_reply(req, "AVTransport", action, "");
    } else if (start_stream()) {
      soap_reply(req, "AVTransport", action, "");
    } else {
      ESP_LOGW(TAG, "Play requested without a media URI");
      soap_error(req, 701, "Transition not available");
    }
  } else if (!strcmp(action, "Pause")) {
    xSemaphoreTake(s_lock, portMAX_DELAY); s_state = DLNA_PAUSED; xSemaphoreGive(s_lock); soap_reply(req, "AVTransport", action, "");
  } else if (!strcmp(action, "Stop")) {
    stop_stream(); soap_reply(req, "AVTransport", action, "");
  } else if (!strcmp(action, "GetPositionInfo")) {
    soap_reply(req, "AVTransport", action,
               "<Track>1</Track><TrackDuration>00:00:00</TrackDuration>"
               "<TrackMetaData></TrackMetaData><TrackURI></TrackURI>"
               "<RelTime>00:00:00</RelTime><AbsTime>00:00:00</AbsTime>"
               "<RelCount>0</RelCount><AbsCount>0</AbsCount>");
  } else if (!strcmp(action, "GetMediaInfo")) {
    char p[URI_MAX + 384];
    xSemaphoreTake(s_lock, portMAX_DELAY);
    snprintf(p, sizeof(p),
             "<NrTracks>1</NrTracks><MediaDuration>00:00:00</MediaDuration>"
             "<CurrentURI>%s</CurrentURI><CurrentURIMetaData></CurrentURIMetaData>"
             "<NextURI></NextURI><NextURIMetaData></NextURIMetaData>"
             "<PlayMedium>NETWORK</PlayMedium><RecordMedium>NOT_IMPLEMENTED</RecordMedium>"
             "<WriteStatus>NOT_IMPLEMENTED</WriteStatus>",
             s_uri);
    xSemaphoreGive(s_lock);
    soap_reply(req, "AVTransport", action, p);
  } else if (!strcmp(action, "GetDeviceCapabilities")) {
    soap_reply(req, "AVTransport", action,
               "<PlayMedia>NETWORK</PlayMedia><RecMedia>NOT_IMPLEMENTED</RecMedia>"
               "<RecQualityModes>NOT_IMPLEMENTED</RecQualityModes>");
  } else if (!strcmp(action, "GetTransportSettings")) {
    soap_reply(req, "AVTransport", action,
               "<PlayMode>NORMAL</PlayMode><RecQualityMode>NOT_IMPLEMENTED</RecQualityMode>");
  } else if (!strcmp(action, "GetTransportActions")) {
    soap_reply(req, "AVTransport", action, "<Actions>Play,Pause,Stop</Actions>");
  } else {
    char p[256]; xSemaphoreTake(s_lock, portMAX_DELAY); snprintf(p,sizeof(p),"<CurrentTransportState>%s</CurrentTransportState><CurrentTransportStatus>OK</CurrentTransportStatus><CurrentSpeed>1</CurrentSpeed>",state_text()); xSemaphoreGive(s_lock); soap_reply(req,"AVTransport",action,p);
  }
  free(body);
  return ESP_OK;
}

static esp_err_t rendering_post(httpd_req_t *req) {
  char body[SOAP_BODY_MAX];
  if (body_read(req, body, sizeof(body)) < 0) return ESP_FAIL;
  if (strstr(body, "SetVolume")) {
    const char *p = strstr(body, "<DesiredVolume>");
    if (p) { p = strchr(p, '>'); if (p) { int v = atoi(p+1); if (v<0) v=0; if (v>100) v=100; s_volume=(uint8_t)v; float db=-30.0f + 30.0f*v/100.0f; dac_set_volume(db); } }
    soap_reply(req,"RenderingControl","SetVolume","");
  } else if (strstr(body,"GetVolume")) {
    char p[64]; snprintf(p,sizeof(p),"<CurrentVolume>%u</CurrentVolume>",s_volume); soap_reply(req,"RenderingControl","GetVolume",p);
  } else { soap_error(req,401,"Invalid Action"); }
  return ESP_OK;
}

static esp_err_t connection_post(httpd_req_t *req) {
  char body[512];
  if (body_read(req, body, sizeof(body)) < 0) return ESP_FAIL;
  if (!strstr(body, "GetProtocolInfo")) {
    soap_error(req, 401, "Invalid Action");
    return ESP_OK;
  }
  soap_reply(req, "ConnectionManager", "GetProtocolInfo",
             "<Source></Source><Sink>http-get:*:audio/mpeg:*,http-get:*:audio/flac:*,http-get:*:audio/wav:*,http-get:*:audio/aac:*</Sink>");
  return ESP_OK;
}

/* Hi-Fi Cast and other UPnP controllers subscribe to these URLs immediately
 * after reading device.xml.  Event delivery is not required for basic playback,
 * but accepting the subscription keeps controllers from treating the renderer
 * as unavailable. */
static esp_err_t event_subscription(httpd_req_t *req) {
  if (req->method == HTTP_SUBSCRIBE) {
    httpd_resp_set_hdr(req, "SID", "uuid:esp32-airplay-events");
    httpd_resp_set_hdr(req, "TIMEOUT", "Second-1800");
  }
  httpd_resp_set_status(req, "200 OK");
  httpd_resp_send(req, NULL, 0);
  return ESP_OK;
}

static esp_err_t device_xml(httpd_req_t *req) {
  const char *xml = "<?xml version=\"1.0\"?><root xmlns=\"urn:schemas-upnp-org:device-1-0\"><specVersion><major>1</major><minor>0</minor></specVersion><device><deviceType>urn:schemas-upnp-org:device:MediaRenderer:1</deviceType><friendlyName>ESP32 DLNA Renderer</friendlyName><manufacturer>ESP32</manufacturer><modelName>AirPlay ESP32</modelName><UDN>uuid:esp32-airplay-renderer</UDN><serviceList><service><serviceType>urn:schemas-upnp-org:service:AVTransport:1</serviceType><serviceId>urn:upnp-org:serviceId:AVTransport</serviceId><SCPDURL>/dlna/AVTransport1.xml</SCPDURL><controlURL>/dlna/control/AVTransport1</controlURL><eventSubURL>/dlna/event/AVTransport1</eventSubURL></service><service><serviceType>urn:schemas-upnp-org:service:RenderingControl:1</serviceType><serviceId>urn:upnp-org:serviceId:RenderingControl</serviceId><SCPDURL>/dlna/RenderingControl1.xml</SCPDURL><controlURL>/dlna/control/RenderingControl1</controlURL><eventSubURL>/dlna/event/RenderingControl1</eventSubURL></service><service><serviceType>urn:schemas-upnp-org:service:ConnectionManager:1</serviceType><serviceId>urn:upnp-org:serviceId:ConnectionManager</serviceId><SCPDURL>/dlna/ConnectionManager1.xml</SCPDURL><controlURL>/dlna/control/ConnectionManager1</controlURL><eventSubURL>/dlna/event/ConnectionManager1</eventSubURL></service></serviceList></device></root>";
  httpd_resp_set_type(req,"text/xml"); httpd_resp_send(req,xml,HTTPD_RESP_USE_STRLEN); return ESP_OK;
}

static esp_err_t scpd_send(httpd_req_t *req, const char *xml) {
  httpd_resp_set_type(req, "text/xml; charset=\"utf-8\"");
  httpd_resp_send(req, xml, HTTPD_RESP_USE_STRLEN);
  return ESP_OK;
}

static esp_err_t avtransport_scpd_xml(httpd_req_t *req) {
  const char *xml =
      "<?xml version=\"1.0\"?><scpd xmlns=\"urn:schemas-upnp-org:service-1-0\">"
      "<specVersion><major>1</major><minor>0</minor></specVersion><actionList>"
      "<action><name>SetAVTransportURI</name></action><action><name>SetNextAVTransportURI</name></action>"
      "<action><name>GetTransportInfo</name></action><action><name>GetPositionInfo</name></action>"
      "<action><name>GetMediaInfo</name></action><action><name>GetDeviceCapabilities</name></action>"
      "<action><name>GetTransportSettings</name></action><action><name>GetTransportActions</name></action>"
      "<action><name>Play</name></action><action><name>Pause</name></action><action><name>Stop</name></action>"
      "<action><name>Seek</name></action></actionList><serviceStateTable>"
      "<stateVariable sendEvents=\"yes\"><name>TransportState</name><dataType>string</dataType></stateVariable>"
      "<stateVariable sendEvents=\"no\"><name>CurrentTrackURI</name><dataType>uri</dataType></stateVariable>"
      "</serviceStateTable></scpd>";
  return scpd_send(req, xml);
}

static esp_err_t rendering_scpd_xml(httpd_req_t *req) {
  const char *xml =
      "<?xml version=\"1.0\"?><scpd xmlns=\"urn:schemas-upnp-org:service-1-0\">"
      "<specVersion><major>1</major><minor>0</minor></specVersion><actionList>"
      "<action><name>GetVolume</name></action><action><name>SetVolume</name></action>"
      "</actionList><serviceStateTable><stateVariable sendEvents=\"no\"><name>Volume</name>"
      "<dataType>ui2</dataType><allowedValueRange><minimum>0</minimum><maximum>100</maximum>"
      "<step>1</step></allowedValueRange></stateVariable></serviceStateTable></scpd>";
  return scpd_send(req, xml);
}

static esp_err_t connection_scpd_xml(httpd_req_t *req) {
  const char *xml =
      "<?xml version=\"1.0\"?><scpd xmlns=\"urn:schemas-upnp-org:service-1-0\">"
      "<specVersion><major>1</major><minor>0</minor></specVersion><actionList>"
      "<action><name>GetProtocolInfo</name></action></actionList></scpd>";
  return scpd_send(req, xml);
}

esp_err_t dlna_renderer_init(void) {
  if (s_lock) {
    return ESP_OK;
  }

  // Referencing both APIs explicitly keeps the default adapter objects linked
  // even when constructor sections from managed components are discarded.
  esp_err_t err = media_lib_add_default_os_adapter();
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to register media OS adapter: %s",
             esp_err_to_name(err));
    return err;
  }
  err = media_lib_add_default_adapter();
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to register media adapters: %s",
             esp_err_to_name(err));
    return err;
  }

  s_lock = xSemaphoreCreateMutex();
  s_events = xEventGroupCreate();
  if (!s_lock || !s_events) {
    err = ESP_ERR_NO_MEM;
    goto fail;
  }

  ssdp_config_t ssdp = SSDP_DEFAULT_CONFIG();
  ssdp.udn = s_ssdp_udn;
  ssdp.location = s_ssdp_location;
  ssdp.notify_interval_ms = 30000;
  ssdp.stack_in_ext = true;
  err = esp_ssdp_start(&ssdp, s_ssdp_services);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "SSDP startup failed: %s", esp_err_to_name(err));
    goto fail;
  }

  ESP_LOGI(TAG, "DLNA renderer initialized");
  return ESP_OK;

fail:
  if (s_events) {
    vEventGroupDelete(s_events);
    s_events = NULL;
  }
  if (s_lock) {
    vSemaphoreDelete(s_lock);
    s_lock = NULL;
  }
  return err;
}

esp_err_t dlna_renderer_register(httpd_handle_t server) {
  httpd_uri_t u={0}; u.method=HTTP_GET; u.uri="/dlna/device.xml"; u.handler=device_xml; httpd_register_uri_handler(server,&u);
  u.uri="/dlna/AVTransport1.xml"; u.handler=avtransport_scpd_xml; httpd_register_uri_handler(server,&u);
  u.uri="/dlna/RenderingControl1.xml"; u.handler=rendering_scpd_xml; httpd_register_uri_handler(server,&u);
  u.uri="/dlna/ConnectionManager1.xml"; u.handler=connection_scpd_xml; httpd_register_uri_handler(server,&u);
  u.method=HTTP_POST; u.uri="/dlna/control/AVTransport1"; u.handler=avtransport_post; httpd_register_uri_handler(server,&u);
  u.uri="/dlna/control/RenderingControl1"; u.handler=rendering_post; httpd_register_uri_handler(server,&u);
  u.uri="/dlna/control/ConnectionManager1"; u.handler=connection_post; httpd_register_uri_handler(server,&u);
  u.method=HTTP_SUBSCRIBE; u.uri="/dlna/event/AVTransport1"; u.handler=event_subscription; httpd_register_uri_handler(server,&u);
  u.uri="/dlna/event/RenderingControl1"; httpd_register_uri_handler(server,&u);
  u.uri="/dlna/event/ConnectionManager1"; httpd_register_uri_handler(server,&u);
  u.method=HTTP_UNSUBSCRIBE; u.uri="/dlna/event/AVTransport1"; httpd_register_uri_handler(server,&u);
  u.uri="/dlna/event/RenderingControl1"; httpd_register_uri_handler(server,&u);
  u.uri="/dlna/event/ConnectionManager1"; httpd_register_uri_handler(server,&u);
  return ESP_OK;
}
