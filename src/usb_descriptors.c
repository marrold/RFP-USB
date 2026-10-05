#include <string.h>

#include "pico/unique_id.h"
#include "tusb.h"

#include "config.h"

#define USB_BCD 0x0200

static const tusb_desc_device_t desc_device = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = USB_BCD,
    .bDeviceClass       = 0x00,
    .bDeviceSubClass    = 0x00,
    .bDeviceProtocol    = 0x00,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,

    .idVendor           = USB_VID,
    .idProduct          = USB_PID,
    .bcdDevice          = USB_BCD_DEVICE,

    .iManufacturer      = 0x01,
    .iProduct           = 0x02,
    .iSerialNumber      = 0x03,

    .bNumConfigurations = 0x01,
};

uint8_t const *tud_descriptor_device_cb(void) {
    return (uint8_t const *)&desc_device;
}

/* ---------------------------------------------------------- configuration */

enum { ITF_NUM_MSC, ITF_NUM_TOTAL };

#define EPNUM_MSC_OUT 0x01
#define EPNUM_MSC_IN  0x81

#define CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_MSC_DESC_LEN)

static uint8_t const desc_configuration[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN,
                          TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),
    TUD_MSC_DESCRIPTOR(ITF_NUM_MSC, 0, EPNUM_MSC_OUT, EPNUM_MSC_IN, 64),
};

uint8_t const *tud_descriptor_configuration_cb(uint8_t index) {
    (void)index;
    return desc_configuration;
}

/* --------------------------------------------------------------- strings */

/* The serial number is derived from the flash chip's unique ID so a host
 * does not confuse two of these on the same bus. */
static char serial_str[2 * PICO_UNIQUE_BOARD_ID_SIZE_BYTES + 1];

static char const *string_desc_arr[] = {
    (const char[]){ 0x09, 0x04 },   /* 0: en-US */
    USB_MANUFACTURER_STR,
    USB_PRODUCT_STR,
    serial_str,
};

static uint16_t _desc_str[32];

uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t langid) {
    (void)langid;

    if (!serial_str[0]) pico_get_unique_board_id_string(serial_str, sizeof serial_str);

    size_t chr_count;
    if (index == 0) {
        memcpy(&_desc_str[1], string_desc_arr[0], 2);
        chr_count = 1;
    } else {
        if (index >= TU_ARRAY_SIZE(string_desc_arr)) return NULL;

        const char *str = string_desc_arr[index];
        chr_count = strlen(str);
        if (chr_count > TU_ARRAY_SIZE(_desc_str) - 1) chr_count = TU_ARRAY_SIZE(_desc_str) - 1;
        for (size_t i = 0; i < chr_count; i++) _desc_str[1 + i] = str[i];
    }

    _desc_str[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 * chr_count + 2));
    return _desc_str;
}
