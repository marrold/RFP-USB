/* SD card bring-up and raw block access.
 *
 * Wraps the carlk3 no-OS-FatFS driver, and doubles as its hw_config
 * provider (sd_get_num / sd_get_by_num).
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Brings up the SPI driver and initialises the card. Safe to call when no
 * card is fitted -- returns false, and the caller falls back to the RAM
 * disk. */
bool sdcard_init(void);

bool     sdcard_present(void);
uint32_t sdcard_sector_count(void);

/* Raw 512-byte block access, bypassing any overlay. Both return true on
 * success. */
bool sdcard_read(uint32_t lba, uint8_t *buf, uint32_t count);
bool sdcard_write(uint32_t lba, const uint8_t *buf, uint32_t count);
bool sdcard_sync(void);
