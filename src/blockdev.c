#include <string.h>

#include "blockdev.h"
#include "config.h"
#include "overlay.h"
#include "ramdisk.h"
#include "sdcard.h"

static blockdev_backend_t backend = BLOCKDEV_NONE;
static bool passthrough;

void blockdev_configure(blockdev_backend_t be, bool writes_passthrough) {
    backend = be;
    passthrough = writes_passthrough;
    overlay_reset();
}

blockdev_backend_t blockdev_backend(void) { return backend; }

uint32_t blockdev_sector_count(void) {
    switch (backend) {
        case BLOCKDEV_SD:      return sdcard_sector_count();
        case BLOCKDEV_RAMDISK: return RAMDISK_SECTORS;
        default:               return 0;
    }
}

/* Reads straight from the backing store, ignoring the overlay. */
static bool backend_read(uint32_t lba, uint8_t *buf, uint32_t count) {
    if (backend == BLOCKDEV_SD) return sdcard_read(lba, buf, count);
    if (backend == BLOCKDEV_RAMDISK) {
        for (uint32_t i = 0; i < count; i++) ramdisk_read_sector(lba + i, buf + i * 512);
        return true;
    }
    return false;
}

bool blockdev_read(uint32_t lba, uint8_t *buf, uint32_t count) {
    if (backend == BLOCKDEV_NONE) return false;

    while (count) {
        if (overlay_get(lba, buf)) {
            lba++; buf += 512; count--;
            continue;
        }
        /* Gather the run of sectors the overlay does not hold so the card
         * sees one multi-block read rather than one command per sector. */
        uint32_t run = 1;
        while (run < count && !overlay_contains(lba + run)) run++;

        if (!backend_read(lba, buf, run)) return false;
        lba += run; buf += run * 512; count -= run;
    }
    return true;
}

bool blockdev_write(uint32_t lba, const uint8_t *buf, uint32_t count) {
    if (backend == BLOCKDEV_NONE) return false;

    if (passthrough) {
        if (backend != BLOCKDEV_SD) return false;
        return sdcard_write(lba, buf, count);
    }

    for (uint32_t i = 0; i < count; i++) overlay_put(lba + i, buf + i * 512);
    return true;
}

void blockdev_sync(void) {
    if (passthrough && backend == BLOCKDEV_SD) sdcard_sync();
}
