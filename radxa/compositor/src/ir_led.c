#include "ir_led.h"

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <fcntl.h>
#include <unistd.h>

/* GPIO sysfs paths (filled in by ir_led_init) */
static char gpio_value_path[128];
static int  gpio_num_g = -1;
static int  gpio_ready = 0;

/* State */
static ir_mode_t current_mode = IR_MODE_AUTO;
static int       leds_on      = 0;

/*
 * Brightness threshold (0-255). IR LEDs turn ON when average
 * Y-plane luminance drops below this value.
 * 40 ≈ quite dark (dusk / tunnel / night).
 */
#define BRIGHTNESS_THRESHOLD 40

/*
 * Hysteresis: once LEDs turn on, brightness must exceed
 * THRESHOLD + HYSTERESIS before turning off. Prevents flicker
 * at the boundary (e.g. headlights sweeping past).
 */
#define BRIGHTNESS_HYSTERESIS 15

/* Rate limit: check brightness at most once per second */
#define CHECK_INTERVAL_NS 1000000000ULL

static uint64_t last_check_ns = 0;

/* ---- Helpers ---- */

static uint64_t monotonic_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static void gpio_write(int val)
{
    if (!gpio_ready) return;

    int fd = open(gpio_value_path, O_WRONLY);
    if (fd < 0) return;
    write(fd, val ? "1" : "0", 1);
    close(fd);
}

static void set_leds(int on)
{
    if (leds_on == on) return;
    leds_on = on;
    gpio_write(on);
    printf("ir_led: LEDs %s\n", on ? "ON" : "OFF");
}

/*
 * Compute average luminance from an NV12 Y plane.
 * Subsamples every 16th pixel in both dimensions for speed.
 * At 1280x720 this reads ~3600 pixels — negligible cost.
 */
static int compute_avg_brightness(const uint8_t *y_plane, int w, int h)
{
    uint64_t sum = 0;
    int count = 0;
    int step = 16;

    for (int row = 0; row < h; row += step) {
        const uint8_t *line = y_plane + row * w;
        for (int col = 0; col < w; col += step) {
            sum += line[col];
            count++;
        }
    }

    return count > 0 ? (int)(sum / count) : 128;
}

/* ---- Public API ---- */

int ir_led_init(int gpio_num)
{
    gpio_num_g = gpio_num;

    char path[128];

    /* Export GPIO if not already exported */
    snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d", gpio_num);
    if (access(path, F_OK) != 0) {
        int fd = open("/sys/class/gpio/export", O_WRONLY);
        if (fd < 0) {
            perror("ir_led: gpio export");
            printf("ir_led: running in dry-run mode (no GPIO)\n");
            return -1;
        }
        char num_str[16];
        int len = snprintf(num_str, sizeof(num_str), "%d", gpio_num);
        write(fd, num_str, len);
        close(fd);
        usleep(100000); /* wait for sysfs to create the node */
    }

    /* Set direction to output */
    snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d/direction", gpio_num);
    int fd = open(path, O_WRONLY);
    if (fd < 0) {
        perror("ir_led: gpio direction");
        printf("ir_led: running in dry-run mode (no GPIO)\n");
        return -1;
    }
    write(fd, "out", 3);
    close(fd);

    /* Save value path for fast writes */
    snprintf(gpio_value_path, sizeof(gpio_value_path),
             "/sys/class/gpio/gpio%d/value", gpio_num);
    gpio_ready = 1;

    /* Start with LEDs off */
    gpio_write(0);
    leds_on = 0;

    printf("ir_led: GPIO%d ready (threshold=%d, hysteresis=%d)\n",
           gpio_num, BRIGHTNESS_THRESHOLD, BRIGHTNESS_HYSTERESIS);
    return 0;
}

void ir_led_set_mode(ir_mode_t mode)
{
    if (mode == current_mode) return;

    const char *names[] = { "off", "on", "auto" };
    printf("ir_led: mode changed to %s\n", names[mode]);
    current_mode = mode;

    /* Immediate effect for on/off modes */
    if (mode == IR_MODE_ON)  set_leds(1);
    if (mode == IR_MODE_OFF) set_leds(0);
}

ir_mode_t ir_led_get_mode(void)
{
    return current_mode;
}

int ir_led_is_on(void)
{
    return leds_on;
}

void ir_led_update(const uint8_t *nv12_data, int w, int h)
{
    if (current_mode != IR_MODE_AUTO) return;
    if (!nv12_data || w <= 0 || h <= 0) return;

    /* Rate limit */
    uint64_t now = monotonic_ns();
    if (now - last_check_ns < CHECK_INTERVAL_NS) return;
    last_check_ns = now;

    int brightness = compute_avg_brightness(nv12_data, w, h);

    if (!leds_on && brightness < BRIGHTNESS_THRESHOLD) {
        printf("ir_led: dark detected (brightness=%d < %d) — turning ON\n",
               brightness, BRIGHTNESS_THRESHOLD);
        set_leds(1);
    } else if (leds_on && brightness > BRIGHTNESS_THRESHOLD + BRIGHTNESS_HYSTERESIS) {
        printf("ir_led: bright detected (brightness=%d > %d) — turning OFF\n",
               brightness, BRIGHTNESS_THRESHOLD + BRIGHTNESS_HYSTERESIS);
        set_leds(0);
    }
}

void ir_led_destroy(void)
{
    set_leds(0);

    if (gpio_num_g >= 0) {
        int fd = open("/sys/class/gpio/unexport", O_WRONLY);
        if (fd >= 0) {
            char num_str[16];
            int len = snprintf(num_str, sizeof(num_str), "%d", gpio_num_g);
            write(fd, num_str, len);
            close(fd);
        }
    }

    gpio_ready = 0;
    printf("ir_led: destroyed\n");
}
