#pragma once

#include "esp_err.h"

/* BLE GATT service UUID for dongle control */
#define BLE_SVC_UUID         0x1234

/* BLE GATT characteristic UUIDs */
#define BLE_CHR_WIFI_SSID    0x0001  /* Read/Write: WiFi SSID */
#define BLE_CHR_WIFI_PASS    0x0002  /* Write-only: WiFi password */
#define BLE_CHR_STATUS       0x0003  /* Read/Notify: connection status bitmask */
#define BLE_CHR_DISPLAY_MODE 0x0004  /* Write: display mode command */
#define BLE_CHR_OTA_TRIGGER  0x0005  /* Write: trigger OTA update */

/* Status bitmask bits */
#define STATUS_USB_CONNECTED   (1 << 0)
#define STATUS_WIFI_CONNECTED  (1 << 1)
#define STATUS_TCP_CONNECTED   (1 << 2)
#define STATUS_AA_ACTIVE       (1 << 3)

/* Display modes */
typedef enum {
    DISPLAY_FULL_AA = 0,
    DISPLAY_FULL_CP = 1,
    DISPLAY_FULL_CAM = 2,
    DISPLAY_SPLIT_AA_CAM = 3,
    DISPLAY_SPLIT_CP_CAM = 4,
} display_mode_t;

/**
 * Initialize BLE GATT server for dongle configuration and control.
 * Advertises as "AADongle" for the Radxa to discover.
 */
esp_err_t ble_control_init(void);

/**
 * Update the status characteristic and notify connected clients.
 */
void ble_control_update_status(uint8_t status_bits);
