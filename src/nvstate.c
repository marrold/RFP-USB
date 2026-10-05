#include <stdint.h>
#include <string.h>

#include "hardware/flash.h"
#include "hardware/regs/addressmap.h"
#include "pico/flash.h"

#include "nvstate.h"

/* The final flash sector. The firmware is currently far smaller than the
 * flash; the linker symbol check prevents a future, larger image from
 * overwriting itself.
 */
#define NV_FLASH_OFFSET (PICO_FLASH_SIZE_BYTES - FLASH_SECTOR_SIZE)
#define NV_MAGIC         0x52465033u  /* "RFP3" */
#define NV_MAGIC_INVERSE (~NV_MAGIC)

/* What is actually laid down, little-endian and fixed width, so a future
 * firmware can still read what this one wrote. The inverse of the magic is
 * stored as well: a sector caught half-written by the power cut these settings
 * exist to survive then fails to validate rather than reading as real. */
typedef struct {
    uint32_t magic;
    uint32_t magic_inverse;
    uint8_t  mode;
    uint8_t  quiet_next;
    uint8_t  reserved[2];
} nv_raw_t;

_Static_assert(sizeof(nv_raw_t) <= FLASH_PAGE_SIZE, "settings must fit one page");

extern uint8_t __flash_binary_end;

static bool sector_is_reserved(void) {
    uintptr_t binary_end = (uintptr_t)&__flash_binary_end - XIP_BASE;
    return binary_end <= NV_FLASH_OFFSET;
}

static bool read_raw(nv_raw_t *out) {
    if (!sector_is_reserved()) return false;

    memcpy(out, (const void *)(XIP_BASE + NV_FLASH_OFFSET), sizeof *out);
    if (out->magic != NV_MAGIC || out->magic_inverse != NV_MAGIC_INVERSE)
        return false;
    /* A mode this firmware does not know, or one that should never have been
     * stored, is as good as no settings at all. */
    if (out->mode >= MODE_STORED_N) return false;
    return true;
}

void nv_load(nv_state_t *out) {
    nv_raw_t raw;
    if (!read_raw(&raw)) {
        /* The default is the whole job rather than the subset: a stick with no
         * settings yet is likelier to be wanted for a reset than for a bare
         * firmware push, and the subset is one menu press away. */
        out->mode       = MODE_RESET;
        out->quiet_next = false;
        return;
    }
    out->mode       = (rfp_mode_t)raw.mode;
    out->quiet_next = raw.quiet_next != 0;
}

/* The callback runs with XIP off, so everything it touches has to be in SRAM.
 * Carrying the page inside the argument puts it on the caller's stack, which
 * is, rather than in a static buffer held for the life of the firmware. */
typedef struct {
    uint8_t page[FLASH_PAGE_SIZE];
} nv_op_t;

static void __not_in_flash_func(rewrite_sector)(void *arg) {
    nv_op_t *op = arg;
    flash_range_erase(NV_FLASH_OFFSET, FLASH_SECTOR_SIZE);
    flash_range_program(NV_FLASH_OFFSET, op->page, sizeof op->page);
}

bool nv_save(const nv_state_t *in) {
    if (!sector_is_reserved()) return false;

    /* Flash has a finite erase budget and this sits on paths that run every
     * boot, so saying what is already there must cost nothing. */
    nv_state_t current;
    nv_load(&current);
    if (current.mode == in->mode && current.quiet_next == in->quiet_next)
        return true;

    nv_raw_t raw = {
        .magic         = NV_MAGIC,
        .magic_inverse = NV_MAGIC_INVERSE,
        .mode          = (uint8_t)in->mode,
        .quiet_next    = in->quiet_next ? 1 : 0,
    };

    nv_op_t op;
    memset(op.page, 0xFF, sizeof op.page);
    memcpy(op.page, &raw, sizeof raw);

    if (flash_safe_execute(rewrite_sector, &op, 1000) != PICO_OK) return false;

    nv_load(&current);
    return current.mode == in->mode && current.quiet_next == in->quiet_next;
}
