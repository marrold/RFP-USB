/* Copy-on-write sector cache.
 *
 * Outside SD card mode every host write lands here instead of the SD card, and
 * every read checks here first. The card is therefore never modified, and
 * because this lives only in SRAM a power cycle restores the card's true
 * contents -- which is exactly the "the marker comes back" behaviour the
 * tool exists to provide.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

void overlay_reset(void);

/* Copies the stored sector into buf and returns true, or returns false if
 * this LBA has never been written. */
bool overlay_get(uint32_t lba, uint8_t *buf);

/* True if this LBA is held, without copying it out. Lets the block layer
 * batch runs of misses into one multi-sector card read. */
bool overlay_contains(uint32_t lba);

/* Always succeeds. If the overlay is full and this is a new LBA the sector is
 * dropped on the floor -- see the note in overlay.c. */
void overlay_put(uint32_t lba, const uint8_t *buf);

uint16_t overlay_used(void);
bool     overlay_is_full(void);
/* True once a write has been discarded for want of space. Nothing on screen
 * reports it; it is here for the tests and for anyone debugging a target that
 * writes more than the overlay can hold. */
bool     overlay_overflowed(void);
