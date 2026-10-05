/* The block device the host sees.
 *
 * Sits between the USB MSC callbacks and the storage, and is the single
 * place that decides whether a write reaches the SD card or is diverted
 * into the copy-on-write overlay.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    BLOCKDEV_NONE,
    BLOCKDEV_SD,       /* the real card */
    BLOCKDEV_RAMDISK,  /* synthetic fallback, no card fitted */
} blockdev_backend_t;

/* writes_passthrough selects SD card mode: writes reach the card instead of
 * the overlay. Resets the overlay. */
void blockdev_configure(blockdev_backend_t backend, bool writes_passthrough);

blockdev_backend_t blockdev_backend(void);
uint32_t           blockdev_sector_count(void);

/* Both return true on success. Reads always reflect the overlay, so they
 * show the host its own writes. */
bool blockdev_read(uint32_t lba, uint8_t *buf, uint32_t count);
bool blockdev_write(uint32_t lba, const uint8_t *buf, uint32_t count);
void blockdev_sync(void);
