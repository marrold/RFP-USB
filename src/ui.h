/* Screen state and layout.
 *
 * Callbacks feed events in from interrupt-ish context; ui_tick() does the
 * drawing from the main loop, and only repaints what actually changed, so an
 * idle screen costs no SPI traffic.
 *
 * This decides how things look, never what they mean: the mode machine in
 * main.c works out which files are exposed and when the job is over, and tells
 * the screen. Nothing here reads the volume to make up its own mind.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "modes.h"

void ui_init(void);

/* The mode and phase name the bar and decide nothing else. */
void ui_set_mode(rfp_mode_t mode, rfp_phase_t phase);
void ui_set_warning(const char *msg);   /* NULL clears */

/* What this mode does with a managed file: `listed` puts it on the checklist,
 * `exposed` says the host can see it now. Listed but not exposed is greyed
 * out. Both are expected to account for the file being on the card at all. */
void ui_set_file_state(const char *name, bool listed, bool exposed);

/* Forgets which files have been read. For a change of mode, where carrying the
 * old mode's ticks over would be a lie; a change of phase keeps them. */
void ui_reset_progress(void);

/* Takes the panel with the finished screen. The mode machine decides when,
 * because what finishes a job differs by mode. */
void ui_set_finished(bool done);

/* The mode menu. It takes the panel and changes nothing by itself; the menu
 * opens on the mode that is stored, not the one running, so reopening it is
 * how a pending change is confirmed. */
void ui_show_menu(rfp_mode_t selected);
void ui_close_menu(void);

void ui_note_read(uint32_t lba);
void ui_note_write(uint32_t lba);

/* True once the host has read inside a file this mode exposes -- the target
 * getting on with the job, as opposed to a host browsing the volume. */
bool ui_payload_touched(void);

/* True once every exposed file has been read and has settled. False when the
 * mode exposes nothing, which is not the same as having finished. */
bool ui_payload_through(void);

/* How long since the host last read anything, or UINT32_MAX if it never has
 * (which, before enumeration, is the normal state rather than a silence). */
uint32_t ui_ms_since_read(void);

void ui_tick(void);
