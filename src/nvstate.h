/* The settings that have to outlive a power cut.
 *
 * The stick is powered by the target, so it is power-cycled whenever the
 * target reboots and loses all its SRAM. Two things have to survive that: the
 * operator's standing choice of mode, and -- in MODE_RESET -- the fact that a
 * reset has just happened and the next boot owes the target silence.
 *
 * Both live in one reserved flash sector. Neither is written on an ordinary
 * boot: saving a value it already holds costs no erase cycle.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "modes.h"

typedef struct {
    rfp_mode_t mode;        /* never MODE_SDCARD: that one is not stored */
    bool       quiet_next;  /* the next boot is MODE_RESET's quiet phase */
} nv_state_t;

/* Fills `out` from flash, or with defaults if the sector is blank, corrupt or
 * could not be reserved. Never fails: a stick that cannot read its settings
 * still has to do something sensible. */
void nv_load(nv_state_t *out);

/* True once flash reads back what was asked, including when it already did. */
bool nv_save(const nv_state_t *in);
