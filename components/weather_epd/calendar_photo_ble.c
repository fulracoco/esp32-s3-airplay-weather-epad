#include "calendar_photo_ble.h"

#if CONFIG_BT_NIMBLE_ENABLED

#include <string.h>

#include "calendar_photo_store.h"
#include "weather_display.h"
#include "esp_log.h"
#include "esp_nimble_hci.h"
#include "host/ble_hs.h"
#include "host/ble_uuid.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

static const char *TAG = "calendar_ble";
static uint16_t s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static uint16_t s_command_handle;
static int gap_event(struct ble_gap_event *event, void *arg);

static const ble_uuid128_t s_service_uuid =
    BLE_UUID128_INIT(0x62, 0x75, 0x00, 0x01, 0xd8, 0x28, 0x91, 0x8d,
                     0xfb, 0x46, 0xb6, 0xc1, 0x1c, 0x67, 0x5a, 0xec);
static const ble_uuid128_t s_command_uuid =
    BLE_UUID128_INIT(0x62, 0x75, 0x00, 0x02, 0xd8, 0x28, 0x91, 0x8d,
                     0xfb, 0x46, 0xb6, 0xc1, 0x1c, 0x67, 0x5a, 0xec);

static int command_access(uint16_t conn_handle, uint16_t attr_handle,
                          struct ble_gatt_access_ctxt *ctxt, void *arg);

static const struct ble_gatt_svc_def s_services[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &s_service_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]){
            {
                .uuid = &s_command_uuid.u,
                .access_cb = command_access,
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP |
                         BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &s_command_handle,
            },
            {0},
        },
    },
    {0},
};

static void notify_status(uint8_t status, uint8_t detail) {
  if (s_conn_handle == BLE_HS_CONN_HANDLE_NONE) return;
  uint8_t payload[2] = {status, detail};
  struct os_mbuf *om = ble_hs_mbuf_from_flat(payload, sizeof(payload));
  if (om) ble_gatts_notify_custom(s_conn_handle, s_command_handle, om);
}

static uint32_t read_le32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
         ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int command_access(uint16_t conn_handle, uint16_t attr_handle,
                          struct ble_gatt_access_ctxt *ctxt, void *arg) {
  (void)conn_handle;
  (void)attr_handle;
  (void)arg;
  if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR || !ctxt->om) return BLE_ATT_ERR_UNLIKELY;

  uint8_t data[256];
  int len = OS_MBUF_PKTLEN(ctxt->om);
  if (len <= 0 || len > (int)sizeof(data) || os_mbuf_copydata(ctxt->om, 0, len, data) != 0) {
    return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
  }

  switch (data[0]) {
    case 0x01: /* BEGIN: command, slot, little-endian size, little-endian crc */
      if (len != 10 || calendar_photo_store_begin(data[1], read_le32(&data[2]),
                                                    read_le32(&data[6])) != ESP_OK) {
        notify_status(0xE1, 1);
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
      }
      notify_status(0x00, 0x01);
      break;
    case 0x02: /* DATA: command, little-endian offset, bytes */
      if (len < 6 || calendar_photo_store_write(read_le32(&data[1]), &data[5],
                                                 (size_t)len - 5) != ESP_OK) {
        notify_status(0xE1, 2);
        return BLE_ATT_ERR_UNLIKELY;
      }
      break;
    case 0x03: /* COMMIT */
      if (len != 1 || calendar_photo_store_commit() != ESP_OK) {
        notify_status(0xE1, 3);
        return BLE_ATT_ERR_UNLIKELY;
      }
      if (weather_display_request_calendar_refresh() != ESP_OK) {
        notify_status(0xE2, 3);
        return BLE_ATT_ERR_UNLIKELY;
      }
      notify_status(0x00, 0x03);
      break;
    case 0x04: /* ABORT */
      calendar_photo_store_abort();
      notify_status(0x00, 0x04);
      break;
    case 0x10: /* LIST: notify six presence bits */
      if (len != 1) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
      {
        uint8_t mask = 0;
        for (uint8_t slot = 0; slot < CALENDAR_PHOTO_SLOTS; ++slot) {
          if (calendar_photo_store_has(slot)) mask |= (uint8_t)(1u << slot);
        }
        notify_status(0x10, mask);
      }
      break;
    default:
      /* NimBLE versions shipped with ESP-IDF 5.x do not expose a separate
       * unsupported-operation ATT error.  Use the generic unlikely error
       * while keeping the command rejected at the GATT boundary. */
      return BLE_ATT_ERR_UNLIKELY;
  }
  return 0;
}

static void advertise(void) {
  uint8_t own_addr_type;
  struct ble_gap_adv_params params = {0};
  struct ble_hs_adv_fields fields = {0};
  if (ble_hs_id_infer_auto(0, &own_addr_type) != 0) return;
  fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
  fields.name = (uint8_t *)ble_svc_gap_device_name();
  fields.name_len = strlen((char *)fields.name);
  fields.name_is_complete = 1;
  fields.uuids128 = (ble_uuid128_t *)&s_service_uuid;
  fields.num_uuids128 = 1;
  fields.uuids128_is_complete = 1;
  if (ble_gap_adv_set_fields(&fields) != 0) return;
  params.conn_mode = BLE_GAP_CONN_MODE_UND;
  params.disc_mode = BLE_GAP_DISC_MODE_GEN;
  ble_gap_adv_start(own_addr_type, NULL, BLE_HS_FOREVER, &params, gap_event, NULL);
}

static int gap_event(struct ble_gap_event *event, void *arg) {
  (void)arg;
  if (event->type == BLE_GAP_EVENT_CONNECT) {
    if (event->connect.status == 0) s_conn_handle = event->connect.conn_handle;
    else advertise();
  } else if (event->type == BLE_GAP_EVENT_DISCONNECT) {
    s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
    calendar_photo_store_abort();
    advertise();
  }
  return 0;
}

static void on_sync(void) { advertise(); }
static void on_reset(int reason) { ESP_LOGW(TAG, "NimBLE reset: %d", reason); }

static void host_task(void *arg) {
  (void)arg;
  nimble_port_run();
  nimble_port_freertos_deinit();
}

esp_err_t calendar_photo_ble_init(void) {
  esp_err_t err = nimble_port_init();
  if (err != ESP_OK) return err;
  ble_svc_gap_init();
  ble_svc_gatt_init();
  ble_svc_gap_device_name_set("Weather EPD");
  int rc = ble_gatts_count_cfg(s_services);
  if (rc != 0) return ESP_FAIL;
  rc = ble_gatts_add_svcs(s_services);
  if (rc != 0) return ESP_FAIL;
  ble_hs_cfg.reset_cb = on_reset;
  ble_hs_cfg.sync_cb = on_sync;
  nimble_port_freertos_init(host_task);
  ESP_LOGI(TAG, "calendar BLE service started");
  return ESP_OK;
}

#else

esp_err_t calendar_photo_ble_init(void) { return ESP_ERR_NOT_SUPPORTED; }

#endif
