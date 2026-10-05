/* USB Mass Storage callbacks. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* True once a write has been rejected, so the UI can say so. */
bool msc_write_failed(void);

/* Returns and clears the indication that the host wrote the medium. Edge
 * triggered, so a caller sees each burst of host writes once and can afford to
 * do something expensive about it. */
bool msc_take_media_written(void);

/* Forgets the eject state and any latched write failure. Called when the
 * medium is handed over afresh -- a change of mode or phase -- so a host that
 * had ejected the old volume is not told the medium is still gone. */
void msc_reset_state(void);
