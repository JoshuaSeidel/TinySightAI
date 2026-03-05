#include "wifi_sta.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "lwip/ip4_addr.h"
#include <string.h>

static const char *TAG = "wifi_sta";

static bool s_connected = false;
static int s_retry_count = 0;
static char s_ip_str[16] = {0};
static esp_netif_t *s_netif = NULL;

/* NVS keys for WiFi credentials */
#define NVS_NAMESPACE  "wifi_cfg"
#define NVS_KEY_SSID   "ssid"
#define NVS_KEY_PASS   "pass"

static void event_handler(void *arg, esp_event_base_t base,
                          int32_t event_id, void *event_data)
{
    if (base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        s_connected = false;
        s_ip_str[0] = '\0';
        if (s_retry_count < WIFI_MAX_RETRY) {
            s_retry_count++;
            ESP_LOGW(TAG, "WiFi disconnected, retrying (%d/%d)...",
                     s_retry_count, WIFI_MAX_RETRY);
            esp_wifi_connect();
        } else {
            ESP_LOGE(TAG, "WiFi connection failed after %d retries", WIFI_MAX_RETRY);
            /* Will keep retrying via BLE trigger or periodic timer */
        }
    } else if (base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        snprintf(s_ip_str, sizeof(s_ip_str), IPSTR, IP2STR(&event->ip_info.ip));
        ESP_LOGI(TAG, "Connected to Radxa AP, IP: %s", s_ip_str);
        s_connected = true;
        s_retry_count = 0;
    }
}

static esp_err_t load_credentials(char *ssid, size_t ssid_len,
                                   char *pass, size_t pass_len)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs);
    if (err != ESP_OK) return err;

    err = nvs_get_str(nvs, NVS_KEY_SSID, ssid, &ssid_len);
    if (err == ESP_OK) {
        nvs_get_str(nvs, NVS_KEY_PASS, pass, &pass_len);
    }
    nvs_close(nvs);
    return err;
}

esp_err_t wifi_sta_init(void)
{
    ESP_LOGI(TAG, "Initializing WiFi STA");

    ESP_ERROR_CHECK(esp_netif_init());
    s_netif = esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL, NULL));

    /* Load credentials from NVS or use defaults */
    wifi_config_t wifi_config = {
        .sta = {
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
            .scan_method = WIFI_ALL_CHANNEL_SCAN,
            .sort_method = WIFI_CONNECT_AP_BY_SIGNAL,
        },
    };

    char ssid[33] = {0};
    char pass[65] = {0};
    if (load_credentials(ssid, sizeof(ssid), pass, sizeof(pass)) == ESP_OK
        && strlen(ssid) > 0) {
        ESP_LOGI(TAG, "Loaded WiFi credentials from NVS: SSID=%s", ssid);
        strncpy((char *)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid));
        strncpy((char *)wifi_config.sta.password, pass, sizeof(wifi_config.sta.password));
    } else {
        ESP_LOGI(TAG, "Using default credentials: SSID=%s", DEFAULT_WIFI_SSID);
        strncpy((char *)wifi_config.sta.ssid, DEFAULT_WIFI_SSID,
                sizeof(wifi_config.sta.ssid));
        strncpy((char *)wifi_config.sta.password, DEFAULT_WIFI_PASS,
                sizeof(wifi_config.sta.password));
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));

    /* Optimize for low latency */
    esp_wifi_set_ps(WIFI_PS_NONE);

    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "WiFi STA started, connecting...");

    return ESP_OK;
}

esp_err_t wifi_sta_set_credentials(const char *ssid, const char *pass)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) return err;

    nvs_set_str(nvs, NVS_KEY_SSID, ssid);
    nvs_set_str(nvs, NVS_KEY_PASS, pass);
    nvs_commit(nvs);
    nvs_close(nvs);

    ESP_LOGI(TAG, "WiFi credentials updated, reconnecting...");

    /* Apply new credentials */
    wifi_config_t wifi_config = {0};
    strncpy((char *)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid));
    strncpy((char *)wifi_config.sta.password, pass, sizeof(wifi_config.sta.password));
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    esp_wifi_disconnect();
    esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    s_retry_count = 0;
    esp_wifi_connect();

    return ESP_OK;
}

bool wifi_sta_connected(void)
{
    return s_connected;
}

const char *wifi_sta_get_ip(void)
{
    return s_connected ? s_ip_str : NULL;
}
