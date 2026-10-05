/* What the host is shown, and when.
 *
 * The stick has three modes. Two of them are the operator's standing choice
 * and survive a power cycle; the third is a runtime state that deliberately
 * does not.
 *
 *   MODE_RESET    The full job, in three phases across two power cycles.
 *                 Recover first, reset last -- see rfp_phase_t.
 *   MODE_UPGRADE  The download alone, every boot, with no reset and no
 *                 failsafe image. For a target that only needs new firmware.
 *   MODE_SDCARD   The card handed to the host read-write so its contents can
 *                 be changed. Entered at once from the menu rather than at the
 *                 next boot, and never stored: a power cycle returns the stick
 *                 to whichever of the other two was last chosen.
 *
 * The card always holds all three managed files. A mode presents a subset by
 * hiding the rest in the RAM overlay, so nothing here ever changes the card.
 */
#pragma once

#include <stdbool.h>

typedef enum {
    MODE_RESET,
    MODE_UPGRADE,
    MODE_SDCARD,
} rfp_mode_t;

/* The menu offers all three; only the first two are ever written to flash. */
#define MODE_COUNT      3
#define MODE_STORED_N   2

/* The phases of MODE_RESET.
 *
 * The target boots whatever failsafe image it can see, and booting it happens
 * over USB, which is slow. Doing that twice is the thing this sequence exists
 * to avoid, so the failsafe image is exposed exactly once and the marker is
 * held back until the target has finished with it.
 *
 *   RECOVER  uImageFailSafe + iprfp3G.dnld. The target boots the failsafe
 *            image, writes the download to its own flash and goes quiet. It
 *            does not reboot, so this phase ends on a timer, not a power cut.
 *   RESET    factoryReset alone, reached by re-enumerating in place. A process
 *            on the target picks the marker up and resets, which reboots the
 *            target and cuts our power. The failsafe image is hidden by now,
 *            so that reboot comes up off the target's own flash -- fast.
 *   QUIET    Nothing at all, for the one boot that follows the reset. Carried
 *            across the power cut in flash, since nothing on the card can say
 *            a reset has just happened. The boot after is RECOVER again.
 */
typedef enum {
    PHASE_RECOVER,
    PHASE_RESET,
    PHASE_QUIET,
} rfp_phase_t;

/* For the status bar. At most 12 characters -- what is left beside the clock
 * once the activity dot has had its column. */
const char *mode_label(rfp_mode_t mode);

/* True if this mode and phase show `name` to the host. Phase is ignored
 * outside MODE_RESET. A name the firmware does not manage is not exposed by
 * anything, and is simply left alone on the card. */
bool mode_exposes(rfp_mode_t mode, rfp_phase_t phase, const char *name);

/* True if the mode runs the same way on every boot, so there is no phase to
 * carry and nothing to arm. */
bool mode_is_stateless(rfp_mode_t mode);
