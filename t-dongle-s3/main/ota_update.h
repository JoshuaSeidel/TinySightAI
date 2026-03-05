#pragma once

#include "esp_err.h"

/*
 * OTA Update Client for T-Dongle-S3
 *
 * Checks for firmware updates from the Radxa HTTP server and applies them
 * using the ESP-IDF OTA API (esp_ota_ops).
 *
 * Update server URL: http://192.168.4.1:8081/dongle/latest
 *
 * The server returns JSON:
 *   {
 *     "version": "1.2.3",
 *     "size": 987654,
 *     "sha256": "abcdef...",
 *     "url": "/dongle/firmware.bin"
 *   }
 *
 * If the version string is newer than the running firmware, the binary is
 * downloaded in chunks and written to the next OTA partition.  After
 * checksum verification the boot partition is updated and the device
 * reboots.
 *
 * Triggers:
 *   1. Periodic timer — every OTA_CHECK_INTERVAL_SEC seconds.
 *   2. BLE OTA characteristic write (BLE_CHR_OTA_TRIGGER).
 */

/* Radxa OTA server base URL */
#define OTA_SERVER_URL          "http://192.168.4.1:8081"

/* Endpoint that returns JSON version info */
#define OTA_LATEST_URL          OTA_SERVER_URL "/dongle/latest"

/* Periodic check interval (6 hours) */
#define OTA_CHECK_INTERVAL_SEC  (6 * 60 * 60)

/* Download chunk size */
#define OTA_CHUNK_SIZE          4096

/**
 * Initialize the OTA subsystem.
 *
 * Registers a periodic esp_timer that fires every OTA_CHECK_INTERVAL_SEC
 * seconds and calls ota_check_and_update() in a background task.
 * Must be called after WiFi is connected.
 *
 * Returns ESP_OK on success.
 */
esp_err_t ota_init(void);

/**
 * Check the OTA server for a newer firmware version.
 * If a newer version is found:
 *   1. Download the firmware binary in chunks.
 *   2. Verify SHA-256 checksum.
 *   3. Set the new partition as the next boot partition.
 *   4. Reboot the device.
 *
 * If no update is available, returns ESP_OK without rebooting.
 * On any error, returns the relevant esp_err_t without modifying flash.
 *
 * Safe to call from any task.  Internally serialized with a mutex so
 * concurrent calls are harmless.
 */
esp_err_t ota_check_and_update(void);

/**
 * Return the running firmware version string (e.g. "1.0.0").
 *
 * The string is stored in the app description compiled into the firmware
 * image (esp_app_get_description()->version).
 *
 * Returns a pointer to a static buffer — do not free.
 */
const char *ota_get_current_version(void);
