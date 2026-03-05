#include "tcp_tunnel.h"
#include "wifi_sta.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include <string.h>
#include <errno.h>

static const char *TAG = "tcp_tunnel";

static int s_sock = -1;
static bool s_connected = false;
static tcp_rx_cb_t s_rx_cb = NULL;
static TaskHandle_t s_rx_task = NULL;

#define RX_BUF_SIZE  8192
#define CONNECT_RETRY_MS  1000

static int tcp_connect(void)
{
    struct sockaddr_in dest = {
        .sin_family = AF_INET,
        .sin_port = htons(RADXA_PORT),
    };
    inet_pton(AF_INET, RADXA_IP, &dest.sin_addr);

    int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock < 0) {
        ESP_LOGE(TAG, "socket() failed: errno %d", errno);
        return -1;
    }

    /* TCP_NODELAY for minimum latency */
    int flag = 1;
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));

    /* Small send buffer for low latency */
    int bufsize = 16384;
    setsockopt(sock, SOL_SOCKET, SO_SNDBUF, &bufsize, sizeof(bufsize));
    setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &bufsize, sizeof(bufsize));

    /* Keep-alive to detect broken connection */
    setsockopt(sock, SOL_SOCKET, SO_KEEPALIVE, &flag, sizeof(flag));

    ESP_LOGI(TAG, "Connecting to %s:%d...", RADXA_IP, RADXA_PORT);
    int err = connect(sock, (struct sockaddr *)&dest, sizeof(dest));
    if (err != 0) {
        ESP_LOGW(TAG, "connect() failed: errno %d", errno);
        close(sock);
        return -1;
    }

    ESP_LOGI(TAG, "TCP tunnel connected");
    return sock;
}

static void tcp_rx_task(void *arg)
{
    (void)arg;
    uint8_t buf[RX_BUF_SIZE];

    while (1) {
        /* Wait for WiFi */
        while (!wifi_sta_connected()) {
            vTaskDelay(pdMS_TO_TICKS(500));
        }

        /* Connect to Radxa */
        s_sock = tcp_connect();
        if (s_sock < 0) {
            vTaskDelay(pdMS_TO_TICKS(CONNECT_RETRY_MS));
            continue;
        }
        s_connected = true;

        /* Read loop */
        while (1) {
            int len = recv(s_sock, buf, sizeof(buf), 0);
            if (len <= 0) {
                if (len == 0) {
                    ESP_LOGW(TAG, "TCP connection closed by Radxa");
                } else {
                    ESP_LOGW(TAG, "TCP recv error: errno %d", errno);
                }
                break;
            }
            if (s_rx_cb) {
                s_rx_cb(buf, len);
            }
        }

        /* Connection lost — clean up and retry */
        s_connected = false;
        close(s_sock);
        s_sock = -1;
        ESP_LOGW(TAG, "TCP disconnected, retrying in %dms", CONNECT_RETRY_MS);
        vTaskDelay(pdMS_TO_TICKS(CONNECT_RETRY_MS));
    }
}

esp_err_t tcp_tunnel_init(void)
{
    ESP_LOGI(TAG, "Initializing TCP tunnel to %s:%d", RADXA_IP, RADXA_PORT);

    BaseType_t ret = xTaskCreatePinnedToCore(
        tcp_rx_task, "tcp_rx", 4096, NULL, 5, &s_rx_task, 1 /* core 1 */);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create TCP RX task");
        return ESP_FAIL;
    }
    return ESP_OK;
}

void tcp_tunnel_set_rx_callback(tcp_rx_cb_t cb)
{
    s_rx_cb = cb;
}

int tcp_tunnel_send(const uint8_t *data, size_t len)
{
    if (!s_connected || s_sock < 0) {
        return -1;
    }

    size_t sent = 0;
    while (sent < len) {
        int n = send(s_sock, data + sent, len - sent, 0);
        if (n < 0) {
            ESP_LOGW(TAG, "TCP send error: errno %d", errno);
            return -1;
        }
        sent += n;
    }
    return (int)sent;
}

bool tcp_tunnel_connected(void)
{
    return s_connected;
}
