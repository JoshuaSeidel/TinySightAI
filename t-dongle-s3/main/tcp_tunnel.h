#pragma once

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

/* Radxa TCP server address */
#define RADXA_IP         "192.168.4.1"
#define RADXA_PORT       5277

/* Callback invoked when TCP data arrives from the Radxa */
typedef void (*tcp_rx_cb_t)(const uint8_t *data, size_t len);

/**
 * Initialize the TCP tunnel client.
 * Connects to the Radxa's TCP server and maintains the connection.
 */
esp_err_t tcp_tunnel_init(void);

/**
 * Register callback for data received from Radxa over TCP.
 */
void tcp_tunnel_set_rx_callback(tcp_rx_cb_t cb);

/**
 * Send data to the Radxa over TCP.
 * Returns number of bytes sent, or -1 on error.
 */
int tcp_tunnel_send(const uint8_t *data, size_t len);

/**
 * Check if the TCP tunnel is connected.
 */
bool tcp_tunnel_connected(void);
