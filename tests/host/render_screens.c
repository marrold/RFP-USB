/* Renders every screen the firmware can show, by driving the real src/ui.c
 * through src/st7789.c into the emulated panel. Output is one PPM per state.
 *
 * This is a layout check as much as a picture: anything that overflows the
 * 30-column row width, or picks an unreadable colour pair, is visible here
 * rather than on the bench.
 */
#include <stdio.h>
#include <string.h>

#include "config.h"
#include "fat32lite.h"
#include "modes.h"
#include "ui.h"

extern uint32_t fake_now_ms;
bool fake_panel_save_ppm(const char *path);

/* ui.c maps an LBA back to a filename; the real lookup is covered by the
 * inspect tests, so here it just returns whatever the scene wants shown. */
static const char *scene_filename;
const char *fat_file_for_lba(uint32_t lba) { (void)lba; return scene_filename; }

/* ui.c re-reads the root directory when host writes settle, to see whether the
 * marker has gone. The scenes here are about layout, not about that lookup, so
 * the volume always reads as still having it. */
bool fat_can_inspect(void) { return true; }
bool fat_find_in_root(const char *name) { (void)name; return true; }

static void save(const char *name) {
    char path[256];
    snprintf(path, sizeof path, "%s.ppm", name);
    if (!fake_panel_save_ppm(path)) { fprintf(stderr, "cannot write %s\n", path); return; }
    printf("  %s\n", path);
}

/* What main() does when it applies a mode and phase. */
static void present(rfp_mode_t m, rfp_phase_t p) {
    static const char *const managed[] = SCREEN_FILES;

    ui_set_mode(m, p);
    for (int i = 0; i < 3; i++) {
        bool in_mode = false;
        for (int q = 0; q < 3; q++)
            if (mode_exposes(m, (rfp_phase_t)q, managed[i])) { in_mode = true; break; }
        ui_set_file_state(managed[i], in_mode, mode_exposes(m, p, managed[i]));
    }
}

int main(void) {
    fake_now_ms = 10000;
    ui_init();

    printf("rendering:\n");

    /* 1. RESET mode, recover phase: the failsafe image and the download on
     * offer, the marker greyed out until the target has finished with them. */
    present(MODE_RESET, PHASE_RECOVER);
    ui_tick();
    save("01-recover-waiting");

    /* 2. Partway through the first file -- no tick until it settles. */
    scene_filename = FILE_FAILSAFE;
    for (int i = 0; i < 16; i++) { fake_now_ms += 40; ui_note_read(2000 + i * 64); }
    fake_now_ms += 50;
    ui_tick();
    save("02-recover-reading");

    /* 3. That file has gone quiet, so it ticks off; the download is next. */
    fake_now_ms += SCREEN_FILE_SETTLE_MS;
    scene_filename = FILE_DOWNLOAD;
    for (int i = 0; i < 24; i++) { fake_now_ms += 40; ui_note_read(12000 + i * 64); }
    fake_now_ms += 50;
    ui_tick();
    save("03-recover-downloading");

    /* 4. Both read: the recover phase is done and the marker is about to be
     * put in front of the target. */
    fake_now_ms += SCREEN_FILE_SETTLE_MS;
    ui_tick();
    save("04-recover-complete");

    /* 5. The reset phase: the marker alone, the rest taken away so the reset's
     * reboot comes up off the target's own flash rather than over USB. */
    present(MODE_RESET, PHASE_RESET);
    fake_now_ms += 2000;
    ui_tick();
    save("05-reset-offered");

    /* 6. The quiet phase, one boot later: nothing on offer, nothing to do. */
    present(MODE_RESET, PHASE_QUIET);
    ui_set_finished(true);
    fake_now_ms += 3000;
    ui_tick();
    save("06-quiet-done");

    /* 7. UPGRADE mode: the download alone, every boot. The failsafe image and
     * the marker are not listed at all -- they are no part of this mode. */
    ui_set_finished(false);
    ui_reset_progress();
    present(MODE_UPGRADE, PHASE_RECOVER);
    fake_now_ms = 4000;
    ui_tick();
    save("07-upgrade-waiting");

    /* 8. ...read through, and done. */
    scene_filename = FILE_DOWNLOAD;
    for (int i = 0; i < 24; i++) { fake_now_ms += 40; ui_note_read(12000 + i * 64); }
    fake_now_ms += SCREEN_FILE_SETTLE_MS;
    ui_tick();
    save("08-upgrade-reading");
    ui_set_finished(true);
    fake_now_ms += PHASE_IDLE_MS;
    ui_tick();
    save("09-upgrade-done");

    /* 10. The menu, and what it says once a mode is saved. */
    ui_set_finished(false);
    ui_show_menu(MODE_UPGRADE);
    ui_tick();
    save("10-menu");

    ui_close_menu();

    /* 11. SD card mode: the whole panel, and nothing else on it. */
    present(MODE_SDCARD, PHASE_RECOVER);
    ui_tick();
    save("11-sdcard-mode");

    /* 12. A fault in SD card mode drops the splash for the error screen, so a
     * fault looks the same whichever mode raised it. */
    ui_set_warning("SD write failed");
    ui_tick();
    save("12-sdcard-error");

    /* 13. Host writing -- the square goes red, the list stays put. */
    present(MODE_RESET, PHASE_RECOVER);
    ui_set_warning(NULL);
    scene_filename = FILE_DOWNLOAD;
    for (int i = 0; i < 16; i++) { fake_now_ms += 40; ui_note_write(12000 + i * 64); }
    fake_now_ms += 30;
    ui_tick();
    save("13-writing");

    /* 14. No card fitted. */
    ui_set_warning("SD card not detected");
    fake_now_ms += 2000;
    ui_tick();
    save("14-no-card");

    /* 15. The card is readable but what this mode shows is not all there. */
    ui_set_warning("Files not detected");
    ui_tick();
    save("15-files-missing");

    /* 16. A card we cannot inspect. */
    ui_set_warning("exFAT: Unsupported");
    ui_tick();
    save("16-exfat");

    /* 17. Past 99:59 the clock stops counting and says so. It stays that way
     * for the rest of the run: the millisecond counter wraps after 49.7 days,
     * and must never come back round looking like a fresh boot. */
    ui_set_warning(NULL);
    fake_now_ms = 6000u * 1000u;
    ui_tick();
    save("17-clock-capped");

    return 0;
}
