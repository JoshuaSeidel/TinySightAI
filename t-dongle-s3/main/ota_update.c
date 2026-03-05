/*
 * ota_update.c — OTA Firmware Update Client
 *
 * Downloads and applies firmware updates from the Radxa HTTP server.
 * Uses esp_ota_ops for safe partition management.
 *
 * Flow:
 *   1. HTTP GET /dongle/latest  → parse JSON {version, size, sha256, url}
 *   2. Compare version with esp_app_get_description()->version
 *   3. If newer: HTTP GET /dongle/firmware.bin (streaming)
 *      a. esp_ota_begin() on next OTA partition
 *      b. Write chunks via esp_ota_write()
 *      c. esp_ota_end() — validates image
 *      d. Verify SHA-256 against manifest
 *      e. esp_ota_set_boot_partition()
 *      f. esp_restart()
 */

#include "ota_update.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_app_format.h"
#include "esp_http_client.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "mbedtls/sha256.h"

static const char *TAG = "ota";

/* Mutex to prevent concurrent OTA attempts */
static SemaphoreHandle_t s_ota_mutex = NULL;

/* -------------------------------------------------------------------------
 * Version comparison
 *
 * Simple lexicographic comparison of "MAJOR.MINOR.PATCH" strings.
 * Returns true if remote_ver > local_ver.
 * ---------------------------------------------------------------------- */

static bool version_is_newer(const char *local_ver, const char *remote_ver)
{
    /* Parse up to 3 numeric components from each string */
    int local[3]  = {0, 0, 0};
    int remote[3] = {0, 0, 0};

    sscanf(local_ver,  "%d.%d.%d", &local[0],  &local[1],  &local[2]);
    sscanf(remote_ver, "%d.%d.%d", &remote[0], &remote[1], &remote[2]);

    for (int i = 0; i < 3; i++) {
        if (remote[i] > local[i]) return true;
        if (remote[i] < local[i]) return false;
    }
    return false;  /* equal */
}

/* -------------------------------------------------------------------------
 * Minimal JSON value extractor (no external library)
 *
 * Extracts the value for a given key from a flat JSON object.
 * Only handles string and number values.  Copies result into out_buf.
 * Returns true on success.
 * ---------------------------------------------------------------------- */

static bool json_get_string(const char *json, const char *key,
                             char *out_buf, size_t out_size)
{
    /* Search for "key": */
    char search[64];
    snprintf(search, sizeof(search), "\"%s\"", key);

    const char *p = strstr(json, search);
    if (!p) return false;

    p += strlen(search);

    /* Skip whitespace and colon */
    while (*p == ' ' || *p == ':' || *p == '\t') p++;

    if (*p == '"') {
        /* String value */
        p++;  /* skip opening quote */
        size_t i = 0;
        while (*p && *p != '"' && i + 1 < out_size) {
            out_buf[i++] = *p++;
        }
        out_buf[i] = '\0';
        return true;
    } else if (*p >= '0' && *p <= '9') {
        /* Numeric value */
        size_t i = 0;
        while (*p >= '0' && *p <= '9' && i + 1 < out_size) {
            out_buf[i++] = *p++;
        }
        out_buf[i] = '\0';
        return true;
    }

    return false;
}

/* -------------------------------------------------------------------------
 * ota_get_current_version
 * ---------------------------------------------------------------------- */

const char *ota_get_current_version(void)
{
    const esp_app_desc_t *desc = esp_app_get_description();
    return desc->version;
}

/* -------------------------------------------------------------------------
 * Fetch the /dongle/latest JSON manifest
 *
 * Returns ESP_OK and fills out_json (caller-supplied, out_size bytes).
 * ---------------------------------------------------------------------- */

static esp_err_t fetch_latest_manifest(char *out_json, size_t out_size)
{
    esp_http_client_config_t cfg = {
        .url     = OTA_LATEST_URL,
        .timeout_ms = 10000,
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        ESP_LOGE(TAG, "esp_http_client_init failed");
        return ESP_FAIL;
    }

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP open failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return err;
    }

    int content_length = esp_http_client_fetch_headers(client);
    if (content_length <= 0) {
        ESP_LOGW(TAG, "Empty or unknown content length from /dongle/latest");
        content_length = 512;  /* read up to 512 bytes anyway */
    }

    int to_read = (content_length < (int)out_size - 1)
                  ? content_length : (int)out_size - 1;
    int n = esp_http_client_read(client, out_json, to_read);
    out_json[n > 0 ? n : 0] = '\0';

    int status = esp_http_client_get_status_code(client);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (status != 200) {
        ESP_LOGE(TAG, "HTTP status %d from /dongle/latest", status);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Manifest: %s", out_json);
    return ESP_OK;
}

/* -------------------------------------------------------------------------
 * Download firmware and flash it
 *
 * firmware_url: path component only, e.g. "/dongle/firmware.bin"
 * expected_sha256: 64-char hex string (may be empty to skip verification)
 * ---------------------------------------------------------------------- */

static esp_err_t download_and_flash(const char *firmware_url,
                                     const char *expected_sha256)
{
    /* Build full URL */
    char full_url[256];
    snprintf(full_url, sizeof(full_url), "%s%s", OTA_SERVER_URL, firmware_url);
    ESP_LOGI(TAG, "Downloading firmware from %s", full_url);

    /* Find the next OTA partition */
    const esp_partition_t *update_part =
        esp_ota_get_next_update_partition(NULL);
    if (!update_part) {
        ESP_LOGE(TAG, "No OTA partition available");
        return ESP_ERR_NOT_FOUND;
    }
    ESP_LOGI(TAG, "Target partition: %s (offset 0x%08lx)",
             update_part->label, (unsigned long)update_part->address);

    /* Begin OTA */
    esp_ota_handle_t ota_handle;
    esp_err_t err = esp_ota_begin(update_part, OTA_WITH_SEQUENTIAL_WRITES,
                                   &ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(err));
        return err;
    }

    /* Open HTTP connection for firmware download */
    esp_http_client_config_t cfg = {
        .url        = full_url,
        .timeout_ms = 30000,
        .buffer_size = OTA_CHUNK_SIZE,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        esp_ota_abort(ota_handle);
        return ESP_FAIL;
    }

    err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP open for firmware failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        esp_ota_abort(ota_handle);
        return err;
    }

    esp_http_client_fetch_headers(client);

    int status = esp_http_client_get_status_code(client);
    if (status != 200) {
        ESP_LOGE(TAG, "HTTP status %d for firmware download", status);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        esp_ota_abort(ota_handle);
        return ESP_FAIL;
    }

    /* SHA-256 context for verification */
    mbedtls_sha256_context sha_ctx;
    mbedtls_sha256_init(&sha_ctx);
    mbedtls_sha256_starts(&sha_ctx, 0);  /* 0 = SHA-256 (not SHA-224) */

    /* Download and flash in chunks */
    static uint8_t chunk_buf[OTA_CHUNK_SIZE];
    int total_written = 0;

    while (1) {
        int n = esp_http_client_read(client, (char *)chunk_buf, OTA_CHUNK_SIZE);
        if (n == 0) break;   /* EOF */
        if (n < 0) {
            ESP_LOGE(TAG, "HTTP read error during firmware download");
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            esp_ota_abort(ota_handle);
            mbedtls_sha256_free(&sha_ctx);
            return ESP_FAIL;
        }

        mbedtls_sha256_update(&sha_ctx, chunk_buf, n);

        err = esp_ota_write(ota_handle, chunk_buf, n);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_write failed: %s", esp_err_to_name(err));
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            esp_ota_abort(ota_handle);
            mbedtls_sha256_free(&sha_ctx);
            return err;
        }

        total_written += n;
        ESP_LOGD(TAG, "Written %d bytes...", total_written);
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    ESP_LOGI(TAG, "Downloaded %d bytes total", total_written);

    /* Compute final SHA-256 */
    uint8_t sha256_bytes[32];
    mbedtls_sha256_finish(&sha_ctx, sha256_bytes);
    mbedtls_sha256_free(&sha_ctx);

    /* Verify checksum if provided */
    if (expected_sha256[0] != '\0') {
        char computed_hex[65] = {0};
        for (int i = 0; i < 32; i++) {
            snprintf(computed_hex + i * 2, 3, "%02x", sha256_bytes[i]);
        }
        if (strcmp(computed_hex, expected_sha256) != 0) {
            ESP_LOGE(TAG, "SHA-256 mismatch!");
            ESP_LOGE(TAG, "  Expected: %s", expected_sha256);
            ESP_LOGE(TAG, "  Computed: %s", computed_hex);
            esp_ota_abort(ota_handle);
            return ESP_ERR_INVALID_CRC;
        }
        ESP_LOGI(TAG, "SHA-256 verified: %s", computed_hex);
    }

    /* Finalize OTA */
    err = esp_ota_end(ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end failed: %s", esp_err_to_name(err));
        return err;
    }

    /* Set new boot partition */
    err = esp_ota_set_boot_partition(update_part);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition failed: %s",
                 esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "OTA complete. Rebooting to apply update...");
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();

    /* unreachable */
    return ESP_OK;
}

/* -------------------------------------------------------------------------
 * ota_check_and_update
 * ---------------------------------------------------------------------- */

esp_err_t ota_check_and_update(void)
{
    /* Serialize concurrent calls */
    if (xSemaphoreTake(s_ota_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        ESP_LOGW(TAG, "OTA already in progress, skipping");
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t result = ESP_OK;
    char manifest[512] = {0};

    /* Fetch manifest */
    esp_err_t err = fetch_latest_manifest(manifest, sizeof(manifest));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Could not reach OTA server: %s", esp_err_to_name(err));
        result = err;
        goto done;
    }

    /* Parse version */
    char remote_version[32] = {0};
    if (!json_get_string(manifest, "version", remote_version,
                          sizeof(remote_version))) {
        ESP_LOGE(TAG, "Failed to parse version from manifest");
        result = ESP_ERR_INVALID_RESPONSE;
        goto done;
    }

    const char *local_version = ota_get_current_version();
    ESP_LOGI(TAG, "Local firmware: %s  Remote firmware: %s",
             local_version, remote_version);

    if (!version_is_newer(local_version, remote_version)) {
        ESP_LOGI(TAG, "Firmware is up to date.");
        goto done;
    }

    ESP_LOGI(TAG, "New firmware available: %s → %s",
             local_version, remote_version);

    /* Parse firmware URL (path) */
    char fw_url[128] = {0};
    if (!json_get_string(manifest, "url", fw_url, sizeof(fw_url))) {
        /* Default to /dongle/firmware.bin if not specified */
        strncpy(fw_url, "/dongle/firmware.bin", sizeof(fw_url) - 1);
    }

    /* Parse expected SHA-256 (optional) */
    char sha256[65] = {0};
    json_get_string(manifest, "sha256", sha256, sizeof(sha256));

    /* Download and flash — this reboots on success */
    result = download_and_flash(fw_url, sha256);

done:
    xSemaphoreGive(s_ota_mutex);
    return result;
}

/* -------------------------------------------------------------------------
 * Periodic timer callback — runs in timer task context
 * ---------------------------------------------------------------------- */

static void ota_timer_callback(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "Periodic OTA check triggered");

    /* Offload to a task because HTTP operations can't run in timer context */
    xTaskCreate(
        (TaskFunction_t)ota_check_and_update,
        "ota_check",
        8192,       /* stack: HTTP + SHA-256 need headroom */
        NULL,
        2,          /* low priority */
        NULL
    );
}

/* -------------------------------------------------------------------------
 * ota_init
 * ---------------------------------------------------------------------- */

esp_err_t ota_init(void)
{
    s_ota_mutex = xSemaphoreCreateMutex();
    if (!s_ota_mutex) {
        ESP_LOGE(TAG, "Failed to create OTA mutex");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Running firmware: %s", ota_get_current_version());

    /* Create periodic timer (fires every OTA_CHECK_INTERVAL_SEC) */
    esp_timer_create_args_t timer_args = {
        .callback        = ota_timer_callback,
        .arg             = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name            = "ota_check",
    };

    esp_timer_handle_t timer;
    esp_err_t err = esp_timer_create(&timer_args, &timer);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_timer_create failed: %s", esp_err_to_name(err));
        return err;
    }

    /* Convert seconds to microseconds for esp_timer */
    uint64_t period_us = (uint64_t)OTA_CHECK_INTERVAL_SEC * 1000000ULL;
    err = esp_timer_start_periodic(timer, period_us);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_timer_start_periodic failed: %s",
                 esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "OTA periodic check every %d hours",
             OTA_CHECK_INTERVAL_SEC / 3600);
    return ESP_OK;
}
