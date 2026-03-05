#include "ble_control.h"
#include "wifi_sta.h"
#include "usb_bridge.h"
#include "tcp_tunnel.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "host/ble_hs.h"
#include "host/ble_uuid.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include <string.h>

static const char *TAG = "ble_ctl";

static uint8_t s_status = 0;
static uint16_t s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static uint16_t s_status_val_handle;

/* 128-bit service UUID: a1b2c3d4-e5f6-7890-abcd-ef1234567890 */
static const ble_uuid128_t svc_uuid =
    BLE_UUID128_INIT(0x90, 0x78, 0x56, 0x34, 0x12, 0xef, 0xcd, 0xab,
                     0x90, 0x78, 0xf6, 0xe5, 0xd4, 0xc3, 0xb2, 0xa1);

static const ble_uuid128_t chr_ssid_uuid =
    BLE_UUID128_INIT(0x01, 0x00, 0x56, 0x34, 0x12, 0xef, 0xcd, 0xab,
                     0x90, 0x78, 0xf6, 0xe5, 0xd4, 0xc3, 0xb2, 0xa1);

static const ble_uuid128_t chr_pass_uuid =
    BLE_UUID128_INIT(0x02, 0x00, 0x56, 0x34, 0x12, 0xef, 0xcd, 0xab,
                     0x90, 0x78, 0xf6, 0xe5, 0xd4, 0xc3, 0xb2, 0xa1);

static const ble_uuid128_t chr_status_uuid =
    BLE_UUID128_INIT(0x03, 0x00, 0x56, 0x34, 0x12, 0xef, 0xcd, 0xab,
                     0x90, 0x78, 0xf6, 0xe5, 0xd4, 0xc3, 0xb2, 0xa1);

static const ble_uuid128_t chr_mode_uuid =
    BLE_UUID128_INIT(0x04, 0x00, 0x56, 0x34, 0x12, 0xef, 0xcd, 0xab,
                     0x90, 0x78, 0xf6, 0xe5, 0xd4, 0xc3, 0xb2, 0xa1);

/* Temporary buffers for WiFi credential writes */
static char s_new_ssid[33] = {0};
static char s_new_pass[65] = {0};

static int chr_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                         struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    const ble_uuid_t *uuid = ctxt->chr->uuid;

    /* WiFi SSID — read/write */
    if (ble_uuid_cmp(uuid, &chr_ssid_uuid.u) == 0) {
        if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
            size_t len = OS_MBUF_PKTLEN(ctxt->om);
            if (len >= sizeof(s_new_ssid)) len = sizeof(s_new_ssid) - 1;
            os_mbuf_copydata(ctxt->om, 0, len, s_new_ssid);
            s_new_ssid[len] = '\0';
            ESP_LOGI(TAG, "BLE: WiFi SSID set to '%s'", s_new_ssid);
        }
        return 0;
    }

    /* WiFi password — write only */
    if (ble_uuid_cmp(uuid, &chr_pass_uuid.u) == 0) {
        if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
            size_t len = OS_MBUF_PKTLEN(ctxt->om);
            if (len >= sizeof(s_new_pass)) len = sizeof(s_new_pass) - 1;
            os_mbuf_copydata(ctxt->om, 0, len, s_new_pass);
            s_new_pass[len] = '\0';
            ESP_LOGI(TAG, "BLE: WiFi password updated");

            /* Apply credentials if both SSID and password are set */
            if (strlen(s_new_ssid) > 0) {
                wifi_sta_set_credentials(s_new_ssid, s_new_pass);
                s_new_ssid[0] = '\0';
                s_new_pass[0] = '\0';
            }
        }
        return 0;
    }

    /* Status — read/notify */
    if (ble_uuid_cmp(uuid, &chr_status_uuid.u) == 0) {
        if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
            os_mbuf_append(ctxt->om, &s_status, sizeof(s_status));
        }
        return 0;
    }

    /* Display mode — write */
    if (ble_uuid_cmp(uuid, &chr_mode_uuid.u) == 0) {
        if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
            uint8_t mode;
            os_mbuf_copydata(ctxt->om, 0, 1, &mode);
            ESP_LOGI(TAG, "BLE: Display mode set to %d", mode);
            /* Forward to Radxa via TCP control message */
            uint8_t cmd[2] = {0x01, mode}; /* 0x01 = mode command */
            tcp_tunnel_send(cmd, sizeof(cmd));
        }
        return 0;
    }

    return BLE_ATT_ERR_UNLIKELY;
}

static const struct ble_gatt_svc_def gatt_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &svc_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]){
            {
                .uuid = &chr_ssid_uuid.u,
                .access_cb = chr_access_cb,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE,
            },
            {
                .uuid = &chr_pass_uuid.u,
                .access_cb = chr_access_cb,
                .flags = BLE_GATT_CHR_F_WRITE,
            },
            {
                .uuid = &chr_status_uuid.u,
                .access_cb = chr_access_cb,
                .val_handle = &s_status_val_handle,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
            },
            {
                .uuid = &chr_mode_uuid.u,
                .access_cb = chr_access_cb,
                .flags = BLE_GATT_CHR_F_WRITE,
            },
            {0}, /* sentinel */
        },
    },
    {0}, /* sentinel */
};

static int gap_event_cb(struct ble_gap_event *event, void *arg)
{
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            s_conn_handle = event->connect.conn_handle;
            ESP_LOGI(TAG, "BLE connected (handle=%d)", s_conn_handle);
        } else {
            ESP_LOGW(TAG, "BLE connection failed, status=%d", event->connect.status);
            /* Restart advertising */
            ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, NULL, BLE_HS_FOREVER,
                             NULL, gap_event_cb, NULL);
        }
        break;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "BLE disconnected, reason=%d", event->disconnect.reason);
        s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        /* Restart advertising */
        ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, NULL, BLE_HS_FOREVER,
                         NULL, gap_event_cb, NULL);
        break;

    default:
        break;
    }
    return 0;
}

static void ble_on_sync(void)
{
    ESP_LOGI(TAG, "BLE host synced, starting advertising");

    /* Set device name */
    ble_svc_gap_device_name_set("AADongle");

    /* Start advertising */
    struct ble_gap_adv_params adv_params = {
        .conn_mode = BLE_GAP_CONN_MODE_UND,
        .disc_mode = BLE_GAP_DISC_MODE_GEN,
        .itvl_min = 0x20,  /* 20ms */
        .itvl_max = 0x40,  /* 40ms */
    };

    struct ble_hs_adv_fields fields = {0};
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.name = (const uint8_t *)"AADongle";
    fields.name_len = 8;
    fields.name_is_complete = 1;
    ble_gap_adv_set_fields(&fields);

    ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, NULL, BLE_HS_FOREVER,
                     &adv_params, gap_event_cb, NULL);
}

static void ble_host_task(void *param)
{
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

esp_err_t ble_control_init(void)
{
    ESP_LOGI(TAG, "Initializing BLE control");

    esp_err_t ret = nimble_port_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ble_svc_gap_init();
    ble_svc_gatt_init();

    int rc = ble_gatts_count_cfg(gatt_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gatts_count_cfg failed: %d", rc);
        return ESP_FAIL;
    }
    rc = ble_gatts_add_svcs(gatt_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gatts_add_svcs failed: %d", rc);
        return ESP_FAIL;
    }

    ble_hs_cfg.sync_cb = ble_on_sync;

    nimble_port_freertos_init(ble_host_task);

    ESP_LOGI(TAG, "BLE control initialized");
    return ESP_OK;
}

void ble_control_update_status(uint8_t status_bits)
{
    s_status = status_bits;

    /* Notify connected BLE client */
    if (s_conn_handle != BLE_HS_CONN_HANDLE_NONE) {
        struct os_mbuf *om = ble_hs_mbuf_from_flat(&s_status, sizeof(s_status));
        if (om) {
            ble_gatts_notify_custom(s_conn_handle, s_status_val_handle, om);
        }
    }
}
