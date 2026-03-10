#include "usb_bridge.h"
#include "tinyusb.h"
#include "class/vendor/vendor_device.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "usb_bridge";

static usb_rx_cb_t s_rx_cb = NULL;
static bool s_connected = false;

/* AOA device descriptor */
static const tusb_desc_device_t desc_device = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = 0x0200,
    .bDeviceClass       = TUSB_CLASS_VENDOR_SPECIFIC,
    .bDeviceSubClass    = 0xFF,
    .bDeviceProtocol    = 0xFF,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor           = AOA_VID,
    .idProduct          = AOA_PID,
    .bcdDevice          = 0x0100,
    .iManufacturer      = 0x01,
    .iProduct           = 0x02,
    .iSerialNumber      = 0x03,
    .bNumConfigurations = 0x01,
};

/* String descriptors */
static const char *string_desc_arr[] = {
    "",                  /* 0: Language (handled by TinyUSB) */
    "AADongle",          /* 1: Manufacturer */
    "Wireless AA Bridge",/* 2: Product */
    "000001",            /* 3: Serial */
};

/* Configuration descriptor with vendor-specific interface + 2 bulk endpoints */
#define EPNUM_VENDOR_OUT  0x01
#define EPNUM_VENDOR_IN   0x81
#define CONFIG_TOTAL_LEN  (TUD_CONFIG_DESC_LEN + TUD_VENDOR_DESC_LEN)

static const uint8_t desc_configuration[] = {
    /* Config descriptor */
    TUD_CONFIG_DESCRIPTOR(1, 1, 0, CONFIG_TOTAL_LEN, 0x00, 500),
    /* Vendor interface: 2 bulk endpoints */
    TUD_VENDOR_DESCRIPTOR(0, 0, EPNUM_VENDOR_OUT, EPNUM_VENDOR_IN, 64),
};

/* TinyUSB callbacks */

void tud_mount_cb(void)
{
    ESP_LOGI(TAG, "USB mounted — car head unit enumerated us");
    s_connected = true;
}

void tud_umount_cb(void)
{
    ESP_LOGW(TAG, "USB unmounted");
    s_connected = false;
}

void tud_suspend_cb(bool remote_wakeup_en)
{
    ESP_LOGW(TAG, "USB suspended");
    (void)remote_wakeup_en;
}

void tud_resume_cb(void)
{
    ESP_LOGI(TAG, "USB resumed");
}

/* Vendor class RX callback — car sent us data */
void tud_vendor_rx_cb(uint8_t itf, uint8_t const *buffer, uint16_t bufsize)
{
    (void)itf;
    if (bufsize > 0 && s_rx_cb) {
        s_rx_cb(buffer, bufsize);
    }
}

esp_err_t usb_bridge_init(void)
{
    ESP_LOGI(TAG, "Initializing USB device (AOA VID=%04X PID=%04X)", AOA_VID, AOA_PID);

    const tinyusb_config_t tusb_cfg = {
        .device_descriptor = &desc_device,
        .string_descriptor = string_desc_arr,
        .string_descriptor_count = sizeof(string_desc_arr) / sizeof(string_desc_arr[0]),
        .configuration_descriptor = desc_configuration,
        .external_phy = false,
    };

    esp_err_t ret = tinyusb_driver_install(&tusb_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "TinyUSB driver install failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "USB bridge initialized — waiting for car to enumerate");
    return ESP_OK;
}

void usb_bridge_set_rx_callback(usb_rx_cb_t cb)
{
    s_rx_cb = cb;
}

int usb_bridge_send(const uint8_t *data, size_t len)
{
    if (!s_connected || !tud_vendor_mounted()) {
        return -1;
    }

    size_t sent = 0;
    while (sent < len) {
        size_t chunk = len - sent;
        if (chunk > USB_BUF_SIZE) chunk = USB_BUF_SIZE;

        uint32_t written = tud_vendor_write(data + sent, chunk);
        if (written == 0) {
            /* FIFO full — flush and retry once */
            tud_vendor_write_flush();
            written = tud_vendor_write(data + sent, chunk);
            if (written == 0) {
                ESP_LOGW(TAG, "USB TX stall, sent %zu/%zu", sent, len);
                break;
            }
        }
        sent += written;
    }
    tud_vendor_write_flush();
    return (int)sent;
}

bool usb_bridge_connected(void)
{
    return s_connected && tud_vendor_mounted();
}
