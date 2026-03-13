#include "usb_bridge.h"
#include "wifi_sta.h"
#include "tcp_tunnel.h"
#include "ble_control.h"
#include "led_status.h"
#include "ota_update.h"

#include "esp_log.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "main";

/*
 * T-Dongle-S3 Firmware — USB-to-WiFi Bridge for Wireless AA/CarPlay
 *
 * Data flow:
 *   Car USB-A → T-Dongle (USB device) → WiFi → Radxa Cubie A7Z → Phone
 *   Car USB-A ← T-Dongle (USB device) ← WiFi ← Radxa Cubie A7Z ← Phone
 *
 * The T-Dongle is a transparent pipe. It doesn't understand the AA/CarPlay
 * protocol — it just shuttles bytes between USB and TCP.
 */

/* USB → TCP: car sends data, we forward to Radxa */
static void on_usb_rx(const uint8_t *data, size_t len)
{
    tcp_tunnel_send(data, len);
}

/* TCP → USB: Radxa sends data, we forward to car */
static void on_tcp_rx(const uint8_t *data, size_t len)
{
    usb_bridge_send(data, len);
}

/* Periodic status update task */
static void status_task(void *arg)
{
    (void)arg;

    while (1) {
        uint8_t status = 0;
        if (usb_bridge_connected()) status |= STATUS_USB_CONNECTED;
        if (wifi_sta_connected())   status |= STATUS_WIFI_CONNECTED;
        if (tcp_tunnel_connected()) status |= STATUS_TCP_CONNECTED;

        /* Update LED */
        if (status & STATUS_TCP_CONNECTED) {
            led_status_set(LED_TCP_CONNECTED);
        } else if (status & STATUS_WIFI_CONNECTED) {
            led_status_set(LED_WIFI_CONNECTED);
        } else {
            led_status_set(LED_WIFI_CONNECTING);
        }

        /* Update BLE status characteristic */
        ble_control_update_status(status);

        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "=== T-Dongle-S3 USB Bridge v1.0 ===");

    /* Initialize NVS (required for WiFi + BLE) */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* Event loop */
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    /* 1. LED — immediate visual feedback */
    ESP_ERROR_CHECK(led_status_init());
    led_status_set(LED_BOOTING);

    /* 2. USB device — present as AOA accessory to car */
    ESP_ERROR_CHECK(usb_bridge_init());
    usb_bridge_set_rx_callback(on_usb_rx);

    /* 3. BLE — control channel, WiFi credential provisioning */
    ESP_ERROR_CHECK(ble_control_init());

    /* 4. WiFi — connect to Radxa's hidden AP */
    ESP_ERROR_CHECK(wifi_sta_init());
    led_status_set(LED_WIFI_CONNECTING);

    /* 5. TCP tunnel — bidirectional USB data over WiFi */
    ESP_ERROR_CHECK(tcp_tunnel_init());
    tcp_tunnel_set_rx_callback(on_tcp_rx);

    /* 6. OTA — periodic firmware update check (every 6 hours) */
    ESP_ERROR_CHECK(ota_init());
    ESP_LOGI(TAG, "OTA initialized. Current firmware: %s",
             ota_get_current_version());

    /* 7. Status monitoring task */
    xTaskCreate(status_task, "status", 2048, NULL, 2, NULL);

    ESP_LOGI(TAG, "All subsystems initialized. Bridging USB <-> WiFi <-> Radxa");
    ESP_LOGI(TAG, "Write '1' to BLE characteristic 0x%04X to trigger OTA now.",
             BLE_CHR_OTA_TRIGGER);
}
