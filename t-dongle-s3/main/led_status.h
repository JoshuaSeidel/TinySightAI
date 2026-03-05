#pragma once

#include "esp_err.h"

/* T-Dongle-S3 onboard LED (directly on the PCB) */
#define LED_GPIO   39  /* LilyGO T-Dongle-S3 onboard addressable LED */

typedef enum {
    LED_OFF,
    LED_BOOTING,        /* slow blue pulse */
    LED_WIFI_CONNECTING, /* fast blue blink */
    LED_WIFI_CONNECTED, /* solid blue */
    LED_TCP_CONNECTED,  /* solid green */
    LED_AA_ACTIVE,      /* solid white */
    LED_ERROR,          /* solid red */
} led_state_t;

/**
 * Initialize LED GPIO and start the LED status task.
 */
esp_err_t led_status_init(void);

/**
 * Set the LED state. Thread-safe.
 */
void led_status_set(led_state_t state);
