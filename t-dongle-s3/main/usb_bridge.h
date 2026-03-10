#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"

/* Android Open Accessory (AOA) USB descriptors */
#define AOA_VID          0x18D1  /* Google */
#define AOA_PID          0x2D00  /* AOA accessory */
#define AOA_PID_ADB      0x2D01  /* AOA accessory + ADB */

/* USB bulk endpoint buffer size */
#define USB_BUF_SIZE     4096

/* Callback invoked when USB bulk data arrives from the car head unit */
typedef void (*usb_rx_cb_t)(const uint8_t *data, size_t len);

/**
 * Initialize USB device in AOA accessory mode.
 * The T-Dongle presents itself to the car as a wired Android Auto device.
 */
esp_err_t usb_bridge_init(void);

/**
 * Register callback for data received from car USB.
 */
void usb_bridge_set_rx_callback(usb_rx_cb_t cb);

/**
 * Send data to the car head unit over USB bulk IN endpoint.
 * Returns number of bytes written, or -1 on error.
 */
int usb_bridge_send(const uint8_t *data, size_t len);

/**
 * Check if USB is connected and enumerated by the car.
 */
bool usb_bridge_connected(void);
