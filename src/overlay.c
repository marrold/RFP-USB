#include <string.h>

#include "config.h"
#include "overlay.h"

/* Open addressing with linear probing. Entries are never removed, so a probe
 * can stop at the first empty slot. 0xFFFFFFFF is not a reachable LBA on any
 * card this tool will meet, so it serves as the empty marker. */
#define EMPTY_LBA 0xFFFFFFFFu

static uint32_t slot_lba[OVERLAY_SLOTS];
static uint8_t  slot_data[OVERLAY_SLOTS][512];
static uint16_t used;
static bool     overflowed;

static inline uint16_t hash_of(uint32_t lba) {
    /* Knuth multiplicative; LBAs touched by a directory update are clustered,
     * so a plain modulo would pile them into adjacent slots. */
    return (uint16_t)(((lba * 2654435761u) >> 16) % OVERLAY_SLOTS);
}

void overlay_reset(void) {
    for (int i = 0; i < OVERLAY_SLOTS; i++) slot_lba[i] = EMPTY_LBA;
    used = 0;
    overflowed = false;
}

bool overlay_get(uint32_t lba, uint8_t *buf) {
    uint16_t i = hash_of(lba);
    for (int n = 0; n < OVERLAY_SLOTS; n++) {
        if (slot_lba[i] == EMPTY_LBA) return false;
        if (slot_lba[i] == lba) {
            memcpy(buf, slot_data[i], 512);
            return true;
        }
        if (++i == OVERLAY_SLOTS) i = 0;
    }
    return false;
}

bool overlay_contains(uint32_t lba) {
    uint16_t i = hash_of(lba);
    for (int n = 0; n < OVERLAY_SLOTS; n++) {
        if (slot_lba[i] == EMPTY_LBA) return false;
        if (slot_lba[i] == lba) return true;
        if (++i == OVERLAY_SLOTS) i = 0;
    }
    return false;
}

void overlay_put(uint32_t lba, const uint8_t *buf) {
    uint16_t i = hash_of(lba);
    for (int n = 0; n < OVERLAY_SLOTS; n++) {
        if (slot_lba[i] == lba) {           /* rewrite in place */
            memcpy(slot_data[i], buf, 512);
            return;
        }
        if (slot_lba[i] == EMPTY_LBA) {
            slot_lba[i] = lba;
            memcpy(slot_data[i], buf, 512);
            used++;
            return;
        }
        if (++i == OVERLAY_SLOTS) i = 0;
    }
    /* Full, and this LBA is not already held, so the sector is discarded and
     * the write reported as having succeeded.
     *
     * Nothing is lost that was ever going to be kept: in normal mode no write
     * reaches the card under any circumstances, and the overlay dies with the
     * power. Failing the command instead would hand the target an I/O error
     * over a sector it is not going to get back either way, and a target that
     * sees the stick fail mid-reset is likely to abandon the reset. A target
     * that writes this much is doing something beyond what the stick is for;
     * letting it believe it succeeded keeps it moving. */
    overflowed = true;
}

uint16_t overlay_used(void)     { return used; }
bool     overlay_is_full(void)  { return used >= OVERLAY_SLOTS; }
bool     overlay_overflowed(void) { return overflowed; }
