#include <string.h>

#include "pico/stdlib.h"
#include "tusb.h"

#include "blockdev.h"
#include "config.h"
#include "msc_disk.h"
#include "ui.h"

/* SCSI INQUIRY fields are fixed-width and are memcpy'd in whole, so a
 * mis-sized string in config.h would copy neighbouring bytes. */
_Static_assert(sizeof(SCSI_VENDOR_ID)   - 1 == 8,  "SCSI_VENDOR_ID must be 8 characters");
_Static_assert(sizeof(SCSI_PRODUCT_ID)  - 1 == 16, "SCSI_PRODUCT_ID must be 16 characters");
_Static_assert(sizeof(SCSI_PRODUCT_REV) - 1 == 4,  "SCSI_PRODUCT_REV must be 4 characters");

static volatile bool write_failed;
static volatile bool media_written;
static bool          ejected;

bool msc_write_failed(void) { return write_failed; }

bool msc_take_media_written(void) {
    bool written = media_written;
    media_written = false;
    return written;
}

void msc_reset_state(void) {
    write_failed  = false;
    media_written = false;
    ejected       = false;
}

/* ------------------------------------------------------------------ SCSI */

void tud_msc_inquiry_cb(uint8_t lun, uint8_t vendor_id[8], uint8_t product_id[16],
                        uint8_t product_rev[4]) {
    (void)lun;
    memcpy(vendor_id,   SCSI_VENDOR_ID,  8);
    memcpy(product_id,  SCSI_PRODUCT_ID, 16);
    memcpy(product_rev, SCSI_PRODUCT_REV, 4);
}

bool tud_msc_test_unit_ready_cb(uint8_t lun) {
    (void)lun;
    if (ejected) {
        /* 0x3A ASC_MEDIUM_NOT_PRESENT -- the host asked for the medium to be
         * removed, so report it as gone until it asks for it back. */
        tud_msc_set_sense(lun, SCSI_SENSE_NOT_READY, 0x3A, 0x00);
        return false;
    }
    return blockdev_sector_count() > 0;
}

void tud_msc_capacity_cb(uint8_t lun, uint32_t *block_count, uint16_t *block_size) {
    (void)lun;
    *block_count = blockdev_sector_count();
    *block_size  = 512;
}

bool tud_msc_start_stop_cb(uint8_t lun, uint8_t power_condition, bool start, bool load_eject) {
    (void)lun; (void)power_condition;

    if (load_eject) {
        if (start) {
            ejected = false;
        } else {
            blockdev_sync();
            ejected = true;
        }
    }
    return true;
}

/* The host must believe it can write, otherwise it will not even try to
 * delete the marker. Outside SD card mode they are diverted, not refused. */
bool tud_msc_is_writable_cb(uint8_t lun) {
    (void)lun;
    return true;
}

int32_t tud_msc_read10_cb(uint8_t lun, uint32_t lba, uint32_t offset,
                          void *buffer, uint32_t bufsize) {
    (void)lun;
    if (offset % 512 || bufsize % 512) return -1;

    uint32_t start = lba + offset / 512;
    uint32_t count = bufsize / 512;
    if (start + count > blockdev_sector_count()) return -1;

    if (!blockdev_read(start, (uint8_t *)buffer, count)) return -1;

    ui_note_read(start);
    return (int32_t)bufsize;
}

int32_t tud_msc_write10_cb(uint8_t lun, uint32_t lba, uint32_t offset,
                           uint8_t *buffer, uint32_t bufsize) {
    (void)lun;
    if (offset % 512 || bufsize % 512) return -1;

    uint32_t start = lba + offset / 512;
    uint32_t count = bufsize / 512;
    if (start + count > blockdev_sector_count()) return -1;

    if (!blockdev_write(start, buffer, count)) {
        write_failed = true;
        return -1;
    }

    media_written = true;
    ui_note_write(start);
    return (int32_t)bufsize;
}

int32_t tud_msc_scsi_cb(uint8_t lun, uint8_t const scsi_cmd[16], void *buffer,
                        uint16_t bufsize) {
    (void)lun; (void)buffer; (void)bufsize;

    switch (scsi_cmd[0]) {
        case SCSI_CMD_PREVENT_ALLOW_MEDIUM_REMOVAL:
            /* Nothing is physically locked, so just acknowledge it. */
            return 0;
        case 0x35:  /* SYNCHRONIZE CACHE (10); TinyUSB has no enum for it */
            blockdev_sync();
            return 0;
        default:
            tud_msc_set_sense(lun, SCSI_SENSE_ILLEGAL_REQUEST, 0x20, 0x00);
            return -1;
    }
}
