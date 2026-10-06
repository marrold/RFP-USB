/* rfp-usb -- a factory-reset / file-serving USB stick for the Waveshare
 * RP2040-GEEK and RP2350-GEEK.
 *
 * The stick presents an SD card to a target over USB, showing it only the
 * files the current mode calls for. The host may delete what it is shown --
 * and anything else -- but nothing reaches the card: writes are diverted into
 * a RAM overlay, so a power cycle restores the card exactly as it was. The one
 * exception is SD card mode, which exists to change the card.
 *
 * The modes, and the phases of a reset, are described in modes.h. The machine
 * that drives them is at the bottom of this file.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "ff.h"
#include "hardware/structs/ioqspi.h"
#include "hardware/structs/sio.h"
#include "hardware/sync.h"
#include "hardware/watchdog.h"
#include "hw_config.h"
#include "pico/stdlib.h"
#include "tusb.h"

#include "blockdev.h"
#include "config.h"
#include "fat32lite.h"
#include "modes.h"
#include "msc_disk.h"
#include "nvstate.h"
#include "overlay.h"
#include "sdcard.h"
#include "ui.h"

static uint32_t now_ms(void) { return to_ms_since_boot(get_absolute_time()); }

/* -------------------------------------------------------- BOOTSEL button */

/* The GEEK has no user button, so BOOT doubles as one. It is wired to the
 * flash chip select rather than a GPIO, so reading it means floating that
 * pin and sampling it -- which in turn means running from SRAM with
 * interrupts off, because flash is unreachable while we do.
 *
 * Note this only works after startup: BOOT held at power-on is caught by
 * the boot ROM and never reaches this firmware.
 */

/* SIO's HI GPIO registers observe the six QSPI pins on both chips, but not in
 * the same layout: on the RP2040 chip select is bit 1, and on the RP2350 the
 * QSPI pins sit at the top of the word alongside the USB ones. The index into
 * io_qspi is 1 either way. */
#ifdef PICO_RP2040
#define BOOTSEL_CS_IN_BIT (1u << 1)
#else
#define BOOTSEL_CS_IN_BIT SIO_GPIO_HI_IN_QSPI_CSN_BITS
#endif

static bool __no_inline_not_in_flash_func(bootsel_pressed)(void) {
    const uint CS_PIN_INDEX = 1;

    uint32_t flags = save_and_disable_interrupts();

    hw_write_masked(&ioqspi_hw->io[CS_PIN_INDEX].ctrl,
                    GPIO_OVERRIDE_LOW << IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_LSB,
                    IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_BITS);

    /* Let the line settle; it is pulled up through the flash chip. */
    for (volatile int i = 0; i < 1000; i++) continue;

    bool pressed = !(sio_hw->gpio_hi_in & BOOTSEL_CS_IN_BIT);

    hw_write_masked(&ioqspi_hw->io[CS_PIN_INDEX].ctrl,
                    GPIO_OVERRIDE_NORMAL << IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_LSB,
                    IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_BITS);

    restore_interrupts(flags);
    return pressed;
}

/* ------------------------------------------------------- what is on offer */

static const char *const managed[] = SCREEN_FILES;
#define MANAGED_N ((int)(sizeof managed / sizeof managed[0]))

/* The operator's standing choice, and where MODE_RESET has got to. */
static nv_state_t  nv;
static rfp_mode_t  mode;
static rfp_phase_t phase;

/* Set once the host has been told the job is over, so it is said once. */
static bool announced_finished;

/* Hides everything this mode and phase does not expose, and tells the screen
 * what it is looking at.
 *
 * The overlay is dropped first, so this always builds on the card's own
 * directory rather than on whatever the last phase hid. That makes it safe to
 * call again on a phase change: files come back as easily as they go.
 *
 * Writes go to the overlay, so the card is not touched. Only the directory
 * records are marked deleted -- the cluster chains stay as they are, so the
 * host sees less free space than the FAT claims. Nothing is allocating, so
 * that costs nothing.
 */
static bool apply_presentation(void) {
    overlay_reset();
    fat_scan_files();

    /* Presence has to be read before anything is hidden, or a file would be
     * reported missing on the strength of our own concealment. */
    bool present[MANAGED_N];
    for (int i = 0; i < MANAGED_N; i++) present[i] = fat_find_in_root(managed[i]);

    bool ok = true;
    for (int i = 0; i < MANAGED_N; i++) {
        bool expose = mode_exposes(mode, phase, managed[i]);
        if (!expose && present[i] && !fat_hide_in_root(managed[i])) ok = false;
    }

    fat_scan_files();     /* the root listing just changed under the file table */

    for (int i = 0; i < MANAGED_N; i++) {
        bool in_mode = false;
        for (int p = 0; p < 3; p++)
            if (mode_exposes(mode, (rfp_phase_t)p, managed[i])) { in_mode = true; break; }

        ui_set_file_state(managed[i],
                          in_mode && present[i],
                          mode_exposes(mode, phase, managed[i]) && present[i]);
    }
    return ok;
}

/* Forces the host to look again. It has the old directory cached and would not
 * re-read a volume it believes it knows, so the pull-up is dropped: the host
 * sees the device go and a new one arrive. The watchdog is armed by the time
 * this runs, so it has to be fed across the wait. */
static void reattach_usb(void) {
    tud_disconnect();

    uint32_t t0 = now_ms();
    while (now_ms() - t0 < USB_REATTACH_MS) {
        watchdog_update();
        sleep_ms(10);
    }

    msc_reset_state();
    ui_tick();
    tud_connect();
}

/* ------------------------------------------------------- SD card mode */

/* Hands the card to the host read-write, at once rather than at the next boot.
 *
 * Rebooting and picking the mode on the way up would be less code, but BOOT is
 * still held down at that moment and the boot ROM samples it on every
 * reset, watchdog resets included -- the chip would come back up in its USB
 * bootloader rather than in this firmware.
 *
 * One way. This mode is never stored, so a power cycle returns the stick to
 * whichever of the others was last chosen. */
static void enter_sdcard_mode(void) {
    printf("entering SD card mode\n");

    mode = MODE_SDCARD;

    /* Passthrough from here, and the overlay's diverted writes go with it:
     * they were never on the card, and this mode has to show what is. */
    blockdev_configure(BLOCKDEV_SD, true);
    fat_scan_volume();
    fat_scan_files();

    /* Whatever another mode complained about -- a missing or unreadable
     * payload -- is not a fault here, since this is where that gets fixed. A
     * write the card rejects will raise its own. */
    ui_set_warning(NULL);
    ui_reset_progress();
    ui_set_mode(mode, phase);
    for (int i = 0; i < MANAGED_N; i++) ui_set_file_state(managed[i], false, false);

    reattach_usb();
}

/* ----------------------------------------------------------- the payload */

/* True if every file this mode means to show is actually on the card. The
 * target cannot be served without them, and finding that out here is better
 * than leaving the operator to infer it from lamps that never go green. */
static bool payload_complete(void) {
    for (int i = 0; i < MANAGED_N; i++)
        for (int p = 0; p < 3; p++)
            if (mode_exposes(mode, (rfp_phase_t)p, managed[i]) &&
                !fat_find_in_root(managed[i]))
                return false;
    return true;
}

/* True if a file this mode shows is too fragmented to map in full. Such a file
 * is read normally by the target but cannot be attributed from its LBAs, so
 * its lamp would stay red through a perfectly good run -- which reads as a
 * failure. Worth saying out loud rather than leaving to be puzzled over. */
static bool payload_fragmented(void) {
    const fat_file_t *files = fat_files();

    for (int i = 0; i < fat_file_count(); i++) {
        if (!files[i].extents_truncated) continue;
        for (int j = 0; j < MANAGED_N; j++)
            for (int p = 0; p < 3; p++)
                if (strcmp(files[i].name, managed[j]) == 0 &&
                    mode_exposes(mode, (rfp_phase_t)p, managed[j]))
                    return true;
    }
    return false;
}

/* Creates the marker on the card if it is not already there. Runs before USB
 * is up and writes directly through FatFs, bypassing the overlay -- this is
 * the one time the firmware itself touches the card outside SD card mode, and
 * it is also a no-op on every boot after the first. Only MODE_RESET needs it;
 * there is no sense writing a marker a mode will never show. */
static bool ensure_marker(void) {
    FATFS fs;
    FRESULT fr = f_mount(&fs, "", 1);
    if (fr != FR_OK) {
        printf("f_mount failed: %d\n", fr);
        return false;
    }

    bool ok = true;
    FIL fil;
    fr = f_open(&fil, MARKER_FILENAME, FA_CREATE_NEW | FA_WRITE);
    if (fr == FR_OK) {
        f_close(&fil);
        printf("created %s\n", MARKER_FILENAME);
    } else if (fr != FR_EXIST) {
        printf("f_open(%s) failed: %d\n", MARKER_FILENAME, fr);
        ok = false;
    }

    f_unmount("");
    sdcard_sync();
    return ok;
}

/* --------------------------------------------------------- the mode machine */

/* True once everything on offer has been read and the host has stopped asking
 * for long enough to be believed. The idle run is measured from the last read
 * of any kind, so a target still working keeps pushing the deadline back and
 * is never cut off part way. */
static bool host_has_finished(void) {
    return ui_payload_through() && ui_ms_since_read() >= PHASE_IDLE_MS;
}

/* PHASE_RECOVER is over: the target has booted the failsafe image, written the
 * download to its own flash and gone quiet. It will not reboot by itself, so
 * the marker is put in front of it instead -- and the failsafe image taken
 * away, so that when the reset does reboot the target it comes up off its own
 * flash rather than over USB again. That second slow boot is the whole reason
 * this phase exists. */
static void enter_reset_phase(void) {
    printf("recover phase complete: offering %s\n", MARKER_FILENAME);

    phase = PHASE_RESET;
    if (!apply_presentation()) ui_set_warning("Could not hide files");
    ui_set_mode(mode, phase);

    reattach_usb();
}

/* The marker has gone, so the target is about to reset and cut our power. The
 * flag has to reach flash now rather than at the screen's leisure -- there may
 * be only milliseconds left. */
static void watch_for_marker_consumed(void) {
    if (mode != MODE_RESET || phase != PHASE_RESET || nv.quiet_next) return;
    if (blockdev_backend() != BLOCKDEV_SD || !fat_can_inspect()) return;

    /* The directory walk is far too costly for every turn of the loop, so a
     * burst of host writes is what prompts it. */
    if (!msc_take_media_written()) return;
    if (fat_find_in_root(MARKER_FILENAME)) return;

    watchdog_update();
    nv.quiet_next = true;
    if (nv_save(&nv)) {
        printf("%s consumed: next boot is the quiet phase\n", MARKER_FILENAME);
    } else {
        nv.quiet_next = false;
        ui_set_warning("Could not save reset state");
    }
}

/* The quiet phase has served its purpose once the host has read the volume at
 * all: it enumerated, found nothing, and is booting off its own flash. Clear
 * the flag so the next power cycle starts the sequence again.
 *
 * Reading, rather than a timer from start-up: a target that takes minutes to
 * power up still gets the silence it is owed, and a flag that is never cleared
 * would leave the stick serving nothing for ever. */
static void watch_for_quiet_done(void) {
    if (mode != MODE_RESET || phase != PHASE_QUIET || !nv.quiet_next) return;
    if (ui_ms_since_read() == UINT32_MAX) return;

    watchdog_update();
    nv.quiet_next = false;
    if (nv_save(&nv)) printf("quiet phase served: sequence complete\n");
    else              ui_set_warning("Could not clear reset state");
}

/* ------------------------------------------------------------ the button */

/* BOOT is the only control, and it cannot be used at power-on: held then, it
 * is caught by the boot ROM and never reaches this firmware. So the menu is a
 * runtime affair, and a mode chosen in it applies at the next boot -- except
 * SD card mode, which is wanted now or not at all.
 *
 * Held opens the menu, a short press moves to the next mode, and a hold saves.
 * Sampling floats the flash chip select with interrupts off, so it is done on
 * an interval rather than every turn of the loop; MENU_HOLD_MS is far longer
 * than that interval, so nothing needs finer timing than it gives.
 */
static bool       menu_open;
static rfp_mode_t menu_sel;
static uint32_t   menu_touched_ms;

static void menu_save(void) {
    rfp_mode_t chosen = menu_sel;
    menu_open = false;
    ui_close_menu();

    if (chosen == MODE_SDCARD) {
        /* Wanted now, and never stored: a power cycle brings back whichever
         * of the others is in flash. */
        enter_sdcard_mode();
        return;
    }

    nv.mode = chosen;
    if (!nv_save(&nv)) {
        ui_set_warning("Could not save mode");
        return;
    }

    /* Nothing is shown to say so. The menu opens on what is stored rather than
     * on what is running, so reopening it is how the choice is confirmed --
     * and unlike a notice that clears itself, that answer is still there an
     * hour later. */
    printf("mode saved: %s (takes effect at the next power cycle)\n",
           mode_label(chosen));
}

static void poll_button(uint32_t t) {
    static bool     down;
    static uint32_t down_ms;
    static bool     consumed;      /* this press has already done its job */

    bool pressed = bootsel_pressed();

    if (pressed && !down) {
        down = true;
        down_ms = t;
        consumed = false;
        return;
    }

    if (pressed) {
        if (consumed || t - down_ms < MENU_HOLD_MS) return;
        consumed = true;                 /* a hold acts once, not repeatedly */

        if (!menu_open) {
            menu_open = true;
            /* The stored mode, not the running one: after a save they differ
             * until the next power cycle, and what the operator wants to see
             * is what they chose. */
            menu_sel  = nv.mode;
            menu_touched_ms = t;
            ui_show_menu(menu_sel);
        } else {
            menu_save();
        }
        return;
    }

    if (!down) return;
    down = false;

    /* A release that the hold did not already answer is a short press. */
    if (consumed || !menu_open) return;
    menu_sel = (rfp_mode_t)((menu_sel + 1) % MODE_COUNT);
    menu_touched_ms = t;
    ui_show_menu(menu_sel);
}

/* ------------------------------------------------------------------ main */

int main(void) {
    stdio_init_all();
    ui_init();

    /* Nothing is asked before USB: the target powers the stick and reads it
     * straight away, so every millisecond before enumeration is one it spends
     * looking at a device that is not there yet. The mode is whatever was
     * chosen last; the menu is reached by holding BOOT once running. */
    nv_load(&nv);
    mode  = nv.mode;
    phase = nv.quiet_next ? PHASE_QUIET : PHASE_RECOVER;

    printf("mode: %s%s\n", mode_label(mode),
           phase == PHASE_QUIET ? " (quiet phase)" : "");

    bool card = sdcard_init();

    /* There is one warning row, so a later check must not talk over an
     * earlier, more specific one. */
    bool warned = false;

    if (!card) {
        blockdev_configure(BLOCKDEV_RAMDISK, false);
        ui_set_warning("SD card not detected");
        warned = true;
    } else {
        /* Only MODE_RESET ever shows the marker, so only it needs one. */
        if (mode == MODE_RESET && !ensure_marker()) {
            ui_set_warning("Could not create marker");
            warned = true;
        }
        blockdev_configure(BLOCKDEV_SD, false);
    }

    ui_set_mode(mode, phase);

    /* The volume layout has to be read through the configured block device,
     * so this comes after blockdev_configure. */
    bool parsed = fat_scan_volume();
    if (parsed) {
        fat_scan_files();
    } else if (fat_type() == FATFS_EXFAT) {
        ui_set_warning("exFAT: Unsupported");
        warned = true;
    } else if (card) {
        ui_set_warning("Unreadable filesystem");
        warned = true;
    }

    /* Only files the current mode would show are worth complaining about: a
     * missing failsafe image is a fault for a reset and nothing at all for a
     * plain upgrade. */
    if (!warned && parsed) {
        if (!payload_complete())       ui_set_warning("Files not detected");
        else if (payload_fragmented()) ui_set_warning("Payload fragmented");
    }

    /* Last of the start-up work, so the checks above still see the volume as
     * the card holds it. Before USB, so the host never catches sight of a file
     * this mode means to withhold -- not even briefly, which a host that
     * caches the directory would hold on to. */
    if (parsed && !apply_presentation()) {
        ui_set_warning("Could not hide files");
        warned = true;
    }

    /* The quiet phase has nothing to show and nothing to wait for: the target
     * is booting off its own flash, which is the whole point of it. */
    if (mode == MODE_RESET && phase == PHASE_QUIET && !warned) {
        ui_set_finished(true);
        announced_finished = true;
    }

    ui_tick();

    tusb_init();

    /* Armed only now: card bring-up, the marker write and USB enumeration can
     * all take their time, and none of them is in a position to feed it. From
     * here the loop turns over continuously, so two seconds is generous. A
     * hang would otherwise leave the target holding a half-mounted volume
     * with nothing on the screen to say why. */
    if (watchdog_caused_reboot())
        printf("recovered from a watchdog reset\n");
    watchdog_enable(2000, true);

    uint32_t last_ui = 0;
    uint32_t last_button = 0;
    bool warned_write = false;

    for (;;) {
        watchdog_update();
        tud_task();

        uint32_t t = now_ms();

        if (mode == MODE_RESET) {
            watch_for_marker_consumed();
            watch_for_quiet_done();

            /* The recover phase ends on the host falling silent, not on a
             * power cut: the target does not reboot after writing the
             * download to its flash. */
            if (phase == PHASE_RECOVER && host_has_finished())
                enter_reset_phase();
        }

        /* UPGRADE has no second act -- once the download has been read and the
         * host has stopped asking, the job is done. */
        if (mode == MODE_UPGRADE && !announced_finished && host_has_finished()) {
            announced_finished = true;
            ui_set_finished(true);
            printf("download read: upgrade complete\n");
        }

        if (t - last_button >= BOOTSEL_POLL_MS) {
            last_button = t;
            poll_button(t);
        }

        /* The menu closes itself if it is left alone, changing nothing, so a
         * stick forgotten in a target does not sit in a menu for ever. */
        if (menu_open && t - menu_touched_ms >= MENU_TIMEOUT_MS) {
            menu_open = false;
            ui_close_menu();
        }

        /* A write the card rejects is a real fault, and only SD card mode can
         * raise one. A full overlay is not: those sectors were never going to
         * reach the card, so discarding them changes nothing that matters. */
        if (!warned_write && msc_write_failed()) {
            warned_write = true;
            ui_set_warning("SD write failed");
        }

        if (t - last_ui >= UI_REFRESH_MS) {
            last_ui = t;
            ui_tick();
        }
    }
}
