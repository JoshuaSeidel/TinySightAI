#pragma once

#include "esp_err.h"

/* Default Radxa AP credentials — can be overridden via BLE */
#define DEFAULT_WIFI_SSID     "AADongle"
#define DEFAULT_WIFI_PASS     "AADongle5GHz!"
#define WIFI_MAX_RETRY        10

/**
 * Initialize WiFi in station mode and connect to the Radxa's hidden AP.
 * Credentials are loaded from NVS (set via BLE) or fall back to defaults.
 */
esp_err_t wifi_sta_init(void);

/**
 * Update WiFi credentials in NVS and reconnect.
 */
esp_err_t wifi_sta_set_credentials(const char *ssid, const char *pass);

/**
 * Check if WiFi is connected to the Radxa AP.
 */
bool wifi_sta_connected(void);

/**
 * Get the current IP address as a string. Returns NULL if not connected.
 */
const char *wifi_sta_get_ip(void);
