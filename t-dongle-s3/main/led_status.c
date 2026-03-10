#include "led_status.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "led";

static led_state_t s_state = LED_OFF;

/*
 * T-Dongle-S3 uses an APA102 RGB LED (SPI-based, 2-wire):
 *   Data  = GPIO 39
 *   Clock = GPIO 40
 *
 * APA102 protocol:
 *   Start frame:  4 bytes 0x00
 *   LED frame:    0xE0 | brightness(0-31), Blue, Green, Red
 *   End frame:    4 bytes 0xFF
 */

static void apa102_write_byte(uint8_t byte)
{
    for (int i = 7; i >= 0; i--) {
        gpio_set_level(LED_DATA_GPIO, (byte >> i) & 1);
        gpio_set_level(LED_CLK_GPIO, 1);
        gpio_set_level(LED_CLK_GPIO, 0);
    }
}

static void set_color(uint8_t r, uint8_t g, uint8_t b)
{
    /* Start frame */
    for (int i = 0; i < 4; i++) apa102_write_byte(0x00);
    /* LED frame: max brightness (31) */
    apa102_write_byte(0xE0 | 15);
    apa102_write_byte(b);
    apa102_write_byte(g);
    apa102_write_byte(r);
    /* End frame */
    for (int i = 0; i < 4; i++) apa102_write_byte(0xFF);
}

static void led_off(void)
{
    /* Start frame */
    for (int i = 0; i < 4; i++) apa102_write_byte(0x00);
    /* LED frame: brightness 0 */
    apa102_write_byte(0xE0);
    apa102_write_byte(0);
    apa102_write_byte(0);
    apa102_write_byte(0);
    /* End frame */
    for (int i = 0; i < 4; i++) apa102_write_byte(0xFF);
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
    ESP_LOGI(TAG, "Initializing APA102 LED (data=%d, clk=%d)", LED_DATA_GPIO, LED_CLK_GPIO);

    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << LED_DATA_GPIO) | (1ULL << LED_CLK_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);

    gpio_set_level(LED_DATA_GPIO, 0);
    gpio_set_level(LED_CLK_GPIO, 0);

    led_off();

    xTaskCreate(led_task, "led", 2048, NULL, 1, NULL);

    ESP_LOGI(TAG, "LED initialized");
    return ESP_OK;
}

void led_status_set(led_state_t state)
{
    s_state = state;
}
