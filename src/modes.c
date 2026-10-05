#include <string.h>

#include "config.h"
#include "modes.h"

/* NULL-terminated, so the tables below read as the lists they are. */
static const char *const show_recover[] = { FILE_FAILSAFE, FILE_DOWNLOAD, NULL };
static const char *const show_reset[]   = { MARKER_FILENAME, NULL };
static const char *const show_quiet[]   = { NULL };
static const char *const show_upgrade[] = { FILE_DOWNLOAD, NULL };
/* Update mode is the one that is allowed to see everything: it exists to put
 * these files on the card in the first place. */
static const char *const show_sdcard[]  = { FILE_FAILSAFE, FILE_DOWNLOAD,
                                            MARKER_FILENAME, NULL };

static const char *const *exposed_list(rfp_mode_t mode, rfp_phase_t phase) {
    switch (mode) {
        case MODE_UPGRADE: return show_upgrade;
        case MODE_SDCARD:  return show_sdcard;
        case MODE_RESET:
            switch (phase) {
                case PHASE_RECOVER: return show_recover;
                case PHASE_RESET:   return show_reset;
                case PHASE_QUIET:   return show_quiet;
            }
            return show_quiet;
    }
    return show_quiet;
}

const char *mode_label(rfp_mode_t mode) {
    switch (mode) {
        case MODE_RESET:   return "RESET";
        case MODE_UPGRADE: return "UPGRADE";
        case MODE_SDCARD:  return "SD CARD";
    }
    return "?";
}

bool mode_exposes(rfp_mode_t mode, rfp_phase_t phase, const char *name) {
    for (const char *const *p = exposed_list(mode, phase); *p; p++)
        if (strcmp(*p, name) == 0) return true;
    return false;
}

bool mode_is_stateless(rfp_mode_t mode) { return mode != MODE_RESET; }
