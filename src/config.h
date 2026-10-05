/* Build-time configuration for rfp-usb.
 *
 * Everything a target device might be fussy about -- USB identity, SCSI
 * INQUIRY strings, the marker filename -- lives here so it can be changed
 * without touching any logic.
 */
#pragma once

/* ------------------------------------------------------------------ board */
/* Waveshare RP2040-GEEK. LCD and SD are on different SPI peripherals, so
 * there is no bus arbitration to worry about. */

#define LCD_SPI         spi1
#define LCD_PIN_SCK     10
#define LCD_PIN_MOSI    11
#define LCD_PIN_CS       9
#define LCD_PIN_DC       8
#define LCD_PIN_RST     12
#define LCD_PIN_BL      25
#define LCD_SPI_BAUD    (62500 * 1000)

#define SD_SPI          spi0
#define SD_PIN_SCK      18
#define SD_PIN_MOSI     19
#define SD_PIN_MISO     20
#define SD_PIN_CS       23
/* Cards negotiate down from this during init; 12.5 MHz is a safe ceiling for
 * the GEEK's unshielded card slot and still outruns full-speed USB. */
#define SD_SPI_BAUD     (12500 * 1000)

/* The 1.14" 240x135 panel is a window into a 240x320 ST7789 framebuffer. */
#define LCD_W           240
#define LCD_H           135
#define LCD_COL_OFFSET  40
#define LCD_ROW_OFFSET  53

/* ----------------------------------------------------------------- timing */

/* BOOT held for this long, at any point after start-up, switches to update
 * mode. Nothing is asked before USB comes up: the target powers the stick and
 * reads it straight away, so there is no window in which to answer, and every
 * millisecond spent offering one is a millisecond the target spends looking at
 * a device that has not enumerated. */
#define UPDATE_HOLD_MS          5000
/* bootsel_pressed() floats the flash chip select with interrupts disabled,
 * so it is sampled on an interval rather than every turn of the main loop. */
#define BOOTSEL_POLL_MS         50
/* How long the USB pull-up is held off when forcing the host to re-enumerate.
 * The host sees SE0 within a frame or two; this is slack on top of that. */
#define USB_REATTACH_MS         500

/* Activity readout drops back to idle after this long with no transfer. */
#define ACTIVITY_IDLE_MS        300
#define UI_REFRESH_MS           100

/* --------------------------------------------------------------- behaviour */

/* The three files the firmware manages. Which of them the host is shown is
 * decided per mode and phase -- see modes.c; the rest are hidden in the RAM
 * overlay, so the card itself always holds all three.
 *
 * FILE_FAILSAFE   the image the target boots when it is present. Booting it
 *                 happens over USB and is slow, so it is exposed only when a
 *                 recovery is actually wanted.
 * FILE_DOWNLOAD   the payload the failsafe image writes to the target's flash.
 * MARKER_FILENAME consumed by a process on the target to trigger a factory
 *                 reset. Created in the volume root if absent. */
#define FILE_FAILSAFE           "uImageFailSafe"
#define FILE_DOWNLOAD           "iprfp3G.dnld"
#define MARKER_FILENAME         "factoryReset"

/* Copy-on-write overlay size, in 512-byte sectors. Deleting one file on FAT32
 * dirties roughly a dozen sectors (directory entry, both FAT copies, FSInfo),
 * so 128 leaves a lot of headroom. Each slot costs 512 bytes of SRAM. */
#define OVERLAY_SLOTS           128

/* Every file the firmware manages, in the order the screen lists them. Only
 * the ones the current mode exposes are shown and ticked off; anything else on
 * the card is read without comment. Names must match the card exactly. */
#define SCREEN_FILES            { FILE_FAILSAFE, FILE_DOWNLOAD, MARKER_FILENAME }
/* A listed file counts as finished once this long has passed with no read
 * landing inside it. The target reads them one after another, so this only
 * has to outlast the host's own gaps within a single file. */
#define SCREEN_FILE_SETTLE_MS   500

/* How long the host must be silent, after everything the mode exposes has been
 * read, before the firmware accepts that it has finished. In RESET mode this
 * is what separates the recover phase from the reset phase; in UPGRADE mode it
 * is what brings up the finished screen. Long enough that a target pausing
 * between files is not mistaken for one that has stopped. */
#define PHASE_IDLE_MS           20000

/* ------------------------------------------------------------ the button */

/* BOOT is the only control. Held this long it opens the mode menu; inside the
 * menu a short press moves to the next mode and a hold of the same length
 * saves. Comfortably longer than any accidental brush, and short enough not to
 * feel like a hang. */
#define MENU_HOLD_MS            1000
/* The menu closes itself after this long untouched, changing nothing, so a
 * stick left in a target does not sit in a menu forever. */
#define MENU_TIMEOUT_MS         10000

/* Root-directory entries tracked for the activity readout. */
#define MAX_TRACKED_FILES       24
/* Contiguous runs recorded per file. A file copied onto a freshly formatted
 * card is normally one run; fragmented files just lose some LBA coverage. */
#define MAX_EXTENTS_PER_FILE    8

/* ------------------------------------------------------------------- USB */

/* 0x2E8A is Raspberry Pi's vendor ID; 0x000A is their reserved range for
 * "RP2040 user-supplied device". Replace both if a target demands a
 * particular identity. */
#define USB_VID                 0x2E8A
#define USB_PID                 0x000A
#define USB_BCD_DEVICE          0x0100

#define USB_MANUFACTURER_STR    "rfp-usb"
#define USB_PRODUCT_STR         "Factory Reset Stick"

/* SCSI INQUIRY response. Padded/truncated to 8/16/4 bytes respectively. */
#define SCSI_VENDOR_ID          "rfp-usb "
#define SCSI_PRODUCT_ID         "Factory Reset   "
#define SCSI_PRODUCT_REV        "1.0 "

/* Volume label for the RAM-only fallback disk used when no card is fitted.
 * Exactly 11 characters, space padded. */
#define FALLBACK_VOLUME_LABEL   "FACTORYRST "
