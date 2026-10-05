/* Stands in for the real SD driver so the block layer, the overlay and the
 * FAT reader can be exercised on a host against a disk image file. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sdcard.h"

static uint8_t *image;
static uint32_t image_sectors;

bool fake_sdcard_load(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);

    image_sectors = (uint32_t)(n / 512);
    image = malloc((size_t)image_sectors * 512);
    bool ok = image && fread(image, 512, image_sectors, f) == image_sectors;
    fclose(f);
    return ok;
}

const uint8_t *fake_sdcard_image(void) { return image; }

bool sdcard_init(void)             { return image != NULL; }
bool sdcard_present(void)          { return image != NULL; }
uint32_t sdcard_sector_count(void) { return image_sectors; }
bool sdcard_sync(void)             { return true; }

bool sdcard_read(uint32_t lba, uint8_t *buf, uint32_t count) {
    if (!image || lba + count > image_sectors) return false;
    memcpy(buf, image + (size_t)lba * 512, (size_t)count * 512);
    return true;
}

bool sdcard_write(uint32_t lba, const uint8_t *buf, uint32_t count) {
    if (!image || lba + count > image_sectors) return false;
    memcpy(image + (size_t)lba * 512, buf, (size_t)count * 512);
    return true;
}
