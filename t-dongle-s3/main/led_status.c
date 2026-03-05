#include "led_status.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "led_strip.h"

static const char *TAG = "led";

static led_state_t s_state = LED_OFF;
static led_strip_handle_t s_strip = NULL;

/* T-Dongle-S3 uses an addressable RGB LED (WS2812) on GPIO 39 */

static void set_color(uint8_t r, uint8_t g, uint8_t b)
{
    if (s_strip) {
        led_strip_set_pixel(s_strip, 0, r, g, b);
        led_strip_refresh(s_strip);
    }
}

static void led_off(void)
{
    if (s_strip) {
        led_strip_clear(s_strip);
    }
}

static void led_task(void *arg)
{
    (void)arg;
    int tick = 0;

    while (1) {
        switch (s_state) {
        case LED_OFF:
            led_off();
            break;

        case LED_BOOTING:
            /* Slow blue pulse */
            {
                int brightness = (tick % 20 < 10) ? (tick % 10) * 25 : (10 - tick % 10) * 25;
                set_color(0, 0, brightness);
            }
            break;

        case LED_WIFI_CONNECTING:
            /* Fast blue blink */
            if (tick % 4 < 2) set_color(0, 0, 100);
            else led_off();
            break;

        case LED_WIFI_CONNECTED:
            set_color(0, 0, 50);
            break;

        case LED_TCP_CONNECTED:
            set_color(0, 50, 0);
            break;

        case LED_AA_ACTIVE:
            set_color(40, 40, 40);
            break;

        case LED_ERROR:
            set_color(80, 0, 0);
            break;
        }

        tick++;
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

esp_err_t led_status_init(void)
{
    ESP_LOGI(TAG, "Initializing LED on GPIO %d", LED_GPIO);

    led_strip_config_t strip_config = {
        .strip_gpio_num = LED_GPIO,
        .max_leds = 1,
    };
    led_strip_rmt_config_t rmt_config = {
        .resolution_hz = 10 * 1000 * 1000, /* 10 MHz */
    };

    esp_err_t ret = led_strip_new_rmt_device(&strip_config, &rmt_config, &s_strip);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "LED strip init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    led_off();

    xTaskCreate(led_task, "led", 2048, NULL, 1, NULL);

    ESP_LOGI(TAG, "LED initialized");
    return ESP_OK;
}

void led_status_set(led_state_t state)
{
    s_state = state;
}
