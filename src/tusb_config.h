#pragma once

#include "tusb_option.h"

#ifndef CFG_TUSB_MCU
#error "CFG_TUSB_MCU should come from the Pico SDK's TinyUSB integration"
#endif

#ifndef CFG_TUSB_OS
#define CFG_TUSB_OS             OPT_OS_PICO
#endif

/* TinyUSB still wants the legacy root-port declaration in addition to
 * CFG_TUD_ENABLED, and asserts if neither port is described. */
#define CFG_TUSB_RHPORT0_MODE   (OPT_MODE_DEVICE | OPT_MODE_FULL_SPEED)

#define CFG_TUD_ENABLED         1
#define CFG_TUH_ENABLED         0
#define CFG_TUD_MAX_SPEED       OPT_MODE_FULL_SPEED

#define CFG_TUSB_MEM_SECTION
#define CFG_TUSB_MEM_ALIGN      __attribute__((aligned(4)))

#define CFG_TUD_ENDPOINT0_SIZE  64

#define CFG_TUD_CDC             0
#define CFG_TUD_MSC             1
#define CFG_TUD_HID             0
#define CFG_TUD_MIDI            0
#define CFG_TUD_VENDOR          0

/* One sector per callback. Full-speed USB tops out around 1 MB/s on either
 * chip, well under what the card can supply, so a larger buffer would buy
 * nothing but SRAM. */
#define CFG_TUD_MSC_EP_BUFSIZE  512
