/* Synthetic FAT12 volume used when no SD card is fitted, so the factory
 * reset function still works with an empty card slot.
 *
 * Sectors are generated on demand rather than held in a buffer -- only
 * three of them carry anything -- and writes are captured by the same
 * copy-on-write overlay the SD path uses.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define RAMDISK_SECTORS 128  /* 64 KB */

void ramdisk_read_sector(uint32_t lba, uint8_t *buf);

/* Builds the LFN + 8.3 directory entry pair for a long filename.
 * Returns the number of 32-byte entries written, or 0 if the name is too
 * long. Shared with the fallback volume's root directory. */
int fat_build_dir_entries(const char *name, uint32_t size, uint8_t attr, uint8_t *out, int max_entries);
