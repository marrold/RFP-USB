#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"

#include "config.h"
#include "fat32lite.h"
#include "modes.h"
#include "st7789.h"
#include "ui.h"

/* The status bar is its own band, taller than a text row so its label can
 * carry some weight; everything below it belongs to the block. */
#define BAR_H    24
#define BAR_HALF ST7789_X1_5
#define BODY_TOP BAR_H

static rfp_mode_t  mode  = MODE_RESET;
static rfp_phase_t phase = PHASE_RECOVER;
static char        warning[TEXT_COLS + 1] = "";

/* Set by the mode machine when the job is over and the stick can come out.
 * What counts as over differs by mode, so it is not worked out here. */
static bool finished;

/* The menu, while the operator is in it. It takes the panel and changes
 * nothing on its own. */
static bool       menu_open;
static rfp_mode_t menu_selected;

/* The body below the status bar is painted as one block rather than as rows,
 * so it is shadowed as one: a digest of every line and colour in it. */
static char           block_sig[192] = "";

static volatile uint32_t last_read_ms, last_write_ms;
/* A marker deletion changes directory/FAT metadata rather than the marker's
 * data sectors (the marker is zero length). Re-check the overlaid directory
 * once each burst of host writes has settled. */
static volatile bool inspect_marker_after_write;

/* The payload checklist. A file is `seen` from its first read and `done` once
 * reads of it have gone quiet; done is sticky, so a target that comes back for
 * a second pass does not un-tick it. */
static const char *const screen_files[] = SCREEN_FILES;
#define SCREEN_FILE_N ((int)(sizeof screen_files / sizeof screen_files[0]))

static volatile uint32_t file_last_ms[SCREEN_FILE_N];
static volatile bool     file_seen[SCREEN_FILE_N];
static bool              file_done[SCREEN_FILE_N];

/* What the current mode does with each managed file, set by the mode machine.
 *
 * `listed`  the file belongs to this mode, so the checklist names it. A file
 *           no mode of this kind touches is left off entirely -- in UPGRADE
 *           there is no failsafe image and no marker, and a row for either
 *           would only invite the question.
 * `exposed` the host can see it right now. A listed file that is not exposed
 *           is greyed out: it is part of the sequence but not this phase of
 *           it, which is neither "unread" nor "done".
 *
 * Both already account for the file being on the card at all, so nothing here
 * waits on something that cannot arrive. */
static bool file_listed[SCREEN_FILE_N];
static bool file_exposed[SCREEN_FILE_N];

/* True once the host has read inside an exposed file. That is the target
 * getting on with the job; a host merely browsing the volume reads the boot
 * sector, the FAT and the root directory and stops. */
static volatile bool payload_touched;

/* True while the panel is showing the full-screen update-mode splash, which
 * owns every pixel rather than the text grid. Deliberately outside
 * invalidate_all(): it tracks what is physically on the panel, not what the
 * row shadow believes. */
static bool splash_painted;

static uint32_t now_ms(void) { return to_ms_since_boot(get_absolute_time()); }

/* ------------------------------------------------------------- painting -- */

/* Paints the status bar, but only when it actually changed. */
static struct {
    char     left[TEXT_COLS + 1], right[16];
    uint16_t fg, bg, dot, rule;
    bool     valid;
} bar_shadow;

static void put_bar(const char *left, const char *right,
                    uint16_t fg, uint16_t bg, uint16_t dot, uint16_t rule) {
    if (bar_shadow.valid && bar_shadow.fg == fg && bar_shadow.bg == bg &&
        bar_shadow.dot == dot && bar_shadow.rule == rule &&
        strcmp(bar_shadow.left, left) == 0 && strcmp(bar_shadow.right, right) == 0)
        return;

    snprintf(bar_shadow.left,  sizeof bar_shadow.left,  "%s", left);
    snprintf(bar_shadow.right, sizeof bar_shadow.right, "%s", right);
    bar_shadow.fg = fg; bar_shadow.bg = bg;
    bar_shadow.dot = dot; bar_shadow.rule = rule;
    bar_shadow.valid = true;

    st7789_bar(BAR_H, BAR_HALF, fg, bg, left, right, dot, rule);
}

/* Paints the body, but only when something in it actually changed. */
static void put_block(const st7789_line_t *lines, int n, uint16_t bg) {
    char sig[sizeof block_sig];
    size_t used = 0;

    int w = snprintf(sig, sizeof sig, "bg%04X|", bg);
    if (w > 0) used = (size_t)w;

    for (int i = 0; i < n && used < sizeof sig - 1; i++) {
        w = snprintf(sig + used, sizeof sig - used, "%s~%04X~%04X|",
                     lines[i].text, lines[i].fg,
                     lines[i].mark ? lines[i].mark_fg : 0);
        if (w < 0) break;
        used += (size_t)w;
    }
    if (used >= sizeof sig) used = sizeof sig - 1;
    sig[used] = '\0';

    if (strcmp(sig, block_sig) == 0) return;

    st7789_block(BODY_TOP, LCD_H - BODY_TOP, bg, lines, n);
    memcpy(block_sig, sig, sizeof sig);
}

/* Time since power-on, mm:ss, capped at the largest a two-and-two field can
 * hold. The stick is meant to be in and out inside an hour, so past 99:59 the
 * exact number has stopped being interesting -- and the cap is latched,
 * because to_ms_since_boot() truncates to 32 bits and wraps after 49.7 days.
 * Without the latch the readout would come back round looking like a fresh
 * run. Everything else that compares timestamps does so as unsigned
 * differences, which survive that wrap on their own. */
#define CLOCK_MAX_SEC 5999u                    /* 99:59 */

static bool clock_capped;

static void clock_str(char *out, size_t len, uint32_t ms) {
    uint32_t sec = ms / 1000;

    if (sec > CLOCK_MAX_SEC) clock_capped = true;
    if (clock_capped) {
        snprintf(out, len, "%s", ST7789_INF);
        return;
    }
    snprintf(out, len, "%02lu:%02lu",
             (unsigned long)(sec / 60), (unsigned long)(sec % 60));
}

/* Greedy word wrap into at most `max` lines of `cols` characters. A word
 * longer than the line is cut rather than dropped. */
static int wrap_text(const char *s, int cols, char out[][TEXT_COLS + 1], int max) {
    int n = 0;

    while (*s && n < max) {
        while (*s == ' ') s++;
        if (!*s) break;

        int take = 0, brk = 0;
        while (s[take] && take < cols) {
            if (s[take] == ' ') brk = take;
            take++;
        }
        if (s[take] && brk) take = brk;       /* back up to the last space */
        if (take > TEXT_COLS) take = TEXT_COLS;

        memcpy(out[n], s, (size_t)take);
        out[n][take] = '\0';
        n++;
        s += take;
    }
    return n;
}

static void invalidate_all(void) {
    bar_shadow.valid = false;
    block_sig[0] = '\0';
}

void ui_init(void) {
    st7789_init();
    invalidate_all();

    /* Everything is listed and exposed until the mode machine has read the
     * volume and says otherwise, so the usual case needs no call at all. */
    for (int i = 0; i < SCREEN_FILE_N; i++)
        file_listed[i] = file_exposed[i] = true;
}

/* --------------------------------------------------------------- events -- */

void ui_set_mode(rfp_mode_t m, rfp_phase_t p) {
    mode = m;
    phase = p;
    invalidate_all();
}

void ui_set_finished(bool done) { finished = done; }

void ui_show_menu(rfp_mode_t selected) {
    menu_open = true;
    menu_selected = selected;
    invalidate_all();
}

void ui_close_menu(void) {
    if (!menu_open) return;
    menu_open = false;
    invalidate_all();
}

void ui_set_warning(const char *msg) {
    snprintf(warning, sizeof warning, "%s", msg ? msg : "");
}

void ui_note_read(uint32_t lba) {
    uint32_t t = now_ms();
    last_read_ms = t;

    /* Attributed here rather than in ui_tick: a short file can be read and
     * done with between two ticks, and would otherwise never be seen. */
    const char *f = fat_file_for_lba(lba);
    if (!f) return;
    for (int i = 0; i < SCREEN_FILE_N; i++)
        if (strcmp(f, screen_files[i]) == 0) {
            if (!file_exposed[i]) break;   /* hidden: the host cannot be in it */
            file_seen[i] = true;
            file_last_ms[i] = t;
            payload_touched = true;
            break;
        }
}

void ui_note_write(uint32_t lba) {
    (void)lba;   /* which sector it landed in is not worth a line of screen */
    last_write_ms = now_ms();
    inspect_marker_after_write = true;
}

void ui_set_file_state(const char *name, bool listed, bool exposed) {
    for (int i = 0; i < SCREEN_FILE_N; i++)
        if (strcmp(screen_files[i], name) == 0) {
            file_listed[i]  = listed;
            file_exposed[i] = exposed;
            /* A file the host cannot see cannot be deleted by it either, so
             * nothing may read its absence as the host having done something.
             * That was the false "deleted" a withheld marker used to show. */
            if (!exposed) inspect_marker_after_write = false;
            break;
        }
}

/* The checklist survives a phase change, so a file already ticked off stays
 * ticked; this is for a change of mode, where it would be a lie. */
void ui_reset_progress(void) {
    for (int i = 0; i < SCREEN_FILE_N; i++) {
        file_seen[i] = false;
        file_done[i] = false;
        file_last_ms[i] = 0;
    }
    payload_touched = false;
    finished = false;
}

bool ui_payload_touched(void) { return payload_touched; }

uint32_t ui_ms_since_read(void) {
    uint32_t t = last_read_ms;
    return t ? now_ms() - t : UINT32_MAX;
}

bool ui_payload_through(void) {
    int exposed = 0;

    for (int i = 0; i < SCREEN_FILE_N; i++) {
        if (!file_exposed[i]) continue;
        exposed++;
        if (!file_done[i]) return false;
    }
    /* Nothing exposed is not the same as everything read: a phase that shows
     * the host an empty volume has no reading to finish. */
    return exposed > 0;
}

/* ----------------------------------------------------------- main screen -- */

void ui_tick(void) {
    uint32_t t = now_ms();

    /* Reads cannot identify a zero-byte file by data LBA. Deletion, however,
     * is unambiguous in the root directory. Inspect only after the current
     * write burst so we do not parse a temporarily half-updated FAT view.
     * file_done is deliberately allowed to move straight from false to true:
     * some targets delete the marker without a separately observable read. */
    if (mode != MODE_SDCARD && inspect_marker_after_write &&
        t - last_write_ms >= ACTIVITY_IDLE_MS) {
        inspect_marker_after_write = false;
        for (int i = 0; i < SCREEN_FILE_N; i++) {
            if (strcmp(screen_files[i], MARKER_FILENAME) != 0) continue;
            if (!file_exposed[i]) break;      /* hidden: its absence is ours */
            if (fat_can_inspect() && !fat_find_in_root(MARKER_FILENAME))
                file_done[i] = true;
            break;
        }
    }

    /* Tick a file off once its reads have gone quiet. Sticky: the target is
     * expected to take them in order, but a second pass must not undo one. */
    for (int i = 0; i < SCREEN_FILE_N; i++)
        if (file_seen[i] && !file_done[i] &&
            t - file_last_ms[i] >= SCREEN_FILE_SETTLE_MS)
            file_done[i] = true;

    /* The splash is for SD card mode working normally. A fault drops it in
     * favour of the error screen the other modes use, so there is one way a
     * fault ever looks. */
    if (mode == MODE_SDCARD && !warning[0] && !menu_open) {
        if (!splash_painted) {
            st7789_splash(COL_BLACK, COL_AMBER, "SD CARD", "UPDATE MODE");
            splash_painted = true;
        }
        return;
    }

    if (splash_painted) {
        /* Coming back from the splash, neither the bar nor the block can be
         * trusted to have erased it, so start from a clean panel. */
        st7789_fill(COL_BLACK);
        invalidate_all();
        splash_painted = false;
    }

    st7789_line_t body[ST7789_BLOCK_LINES];
    int n = 0;
    uint16_t body_bg = COL_BLACK;

    char clock[16];
    clock_str(clock, sizeof clock, t);

    if (menu_open) {
        /* The menu owns the panel while it is up: the operator is choosing,
         * not watching. The current choice is white, the rest grey. */

        /* The build's version takes the slot the clock has everywhere else.
         * The menu times itself out in seconds, so there is no job being timed
         * from it, and it is the one screen the operator is looking at on
         * purpose rather than glancing at -- which makes it the only place
         * worth spending on something that never changes.
         *
         * Passed as it comes: config.h holds it to what the slot can show, so
         * there is nothing to truncate here. A version cut down to fit would
         * read as a different build, which is worse than not showing one. */
        put_bar("MODE", RFP_VERSION, COL_BLACK, COL_BLUE, 0, 0);

        for (int i = 0; i < MODE_COUNT && n < ST7789_BLOCK_LINES; i++) {
            body[n] = (st7789_line_t){ 0 };
            body[n].text  = mode_label((rfp_mode_t)i);
            body[n].fg    = (rfp_mode_t)i == menu_selected ? COL_WHITE : COL_GREY;
            body[n].scale = ST7789_X1_5;
            n++;
        }
    } else if (warning[0]) {
        /* A fault takes the screen over: the bar names it, the body is the
         * message itself, wrapped as wide as double-size text allows. */
        static char wrapped[ST7789_BLOCK_LINES][TEXT_COLS + 1];

        put_bar("ERROR", mode == MODE_SDCARD ? "" : clock, COL_BLACK, COL_RED, 0, 0);

        n = wrap_text(warning, (LCD_W - 16) / (FONT_W * 2), wrapped,
                      ST7789_BLOCK_LINES);
        for (int i = 0; i < n; i++) {
            body[i] = (st7789_line_t){ 0 };
            body[i].text  = wrapped[i];
            body[i].fg    = COL_RED;
            body[i].scale = ST7789_X2;
        }
    } else if (finished) {
        /* The job is over and the stick can come out. Meant to be read from
         * across a workshop, so it takes the whole panel: the word, on green,
         * and nothing else but the clock to say how long the job took. */
        body_bg = COL_GREEN;
        put_bar("", clock, COL_BLACK, COL_GREEN, 0, 0);

        body[n] = (st7789_line_t){ 0 };
        body[n].text = "DONE";
        body[n].fg   = COL_BLACK;
        n++;
    } else {
        bool reading = (t - last_read_ms)  < ACTIVITY_IDLE_MS && last_read_ms;
        bool writing = (t - last_write_ms) < ACTIVITY_IDLE_MS && last_write_ms;

        /* The bar's words name the mode, which is the thing an operator has to
         * be sure of before plugging the stick into anything. What the host is
         * doing is carried by the dot beside it: colour alone, because it
         * changes constantly and reading it should not need attention. */
        /* The square carries what the host is doing and nothing else does:
         * a write no longer takes the body over to name the sector it landed
         * in, which flickered the checklist away for every stray write the
         * host made. The list stays put; only the square changes. */
        uint16_t dot = writing ? COL_RED : reading ? COL_GREEN : COL_GREY;

        /* This bar shares the body's background, so it is ruled off along the
         * bottom edge -- otherwise it has no visible end and the mode name
         * reads as just another line of the list. */
        put_bar(mode_label(mode), clock, COL_WHITE, COL_BLACK, dot, COL_WHITE);

        {
            /* The payload. Each file carries a lamp: red until the target
             * touches it, amber while reads are landing in it, green once
             * they have stopped. */
            for (int i = 0; i < SCREEN_FILE_N && n < ST7789_BLOCK_LINES; i++) {
                if (!file_listed[i]) continue;

                /* A listed file the host cannot currently see is greyed out
                 * rather than lamped: it belongs to the sequence but not to
                 * this phase of it, so neither "not read yet" nor "done" would
                 * be the truth. Grey says it is simply not in play. */
                bool withheld = !file_exposed[i];

                body[n] = (st7789_line_t){ 0 };
                body[n].text    = screen_files[i];
                body[n].fg      = withheld ? COL_GREY : COL_WHITE;
                body[n].left    = true;
                body[n].mark    = true;
                body[n].mark_fg = withheld     ? COL_GREY
                                : file_done[i] ? COL_GREEN
                                : file_seen[i] ? COL_AMBER : COL_RED;
                n++;
            }
        }
    }

    put_block(body, n, body_bg);
}
