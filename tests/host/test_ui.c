/* Exercises the payload checklist: a read lights its lamp, the lamp settles to
 * green once reads of that file stop, and a second pass does not undo it.
 *
 * The lamps are read back out of the emulated panel rather than out of ui.c's
 * private state, so this covers the whole path -- attribution, the settle
 * timer, the colour choice and the drawing -- the way the operator sees it.
 *
 * Two scenes, picked by argv[1], because ui.c's checklist is deliberately
 * sticky and so cannot be wound back inside one run:
 *
 *   offer     an ordinary boot, the marker on offer and consumed by the host
 *   handoff   the boot after, the marker already spent and withheld
 */
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "config.h"
#include "st7789.h"
#include "ui.h"

extern uint32_t fake_now_ms;
uint16_t fake_panel_pixel(int x, int y);

/* ui.c maps an incoming LBA back to a filename; here the scene decides. */
static const char *scene_filename;
const char *fat_file_for_lba(uint32_t lba) { (void)lba; return scene_filename; }
static bool scene_marker_present = true;
bool fat_can_inspect(void) { return true; }
bool fat_find_in_root(const char *name) {
    return strcmp(name, MARKER_FILENAME) != 0 || scene_marker_present;
}

/* What main() does when it applies a mode and phase: say which managed files
 * are on the checklist and which the host can currently see. */
static void present(bool failsafe, bool download, bool marker) {
    ui_set_file_state(FILE_FAILSAFE,   true, failsafe);
    ui_set_file_state(FILE_DOWNLOAD,   true, download);
    ui_set_file_state(MARKER_FILENAME, true, marker);
}

static int failures;

static void check(bool ok, const char *what) {
    printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) failures++;
}

/* The checklist sits under the 24px bar: three 1.5x lines of 24px with 6px
 * gaps, centred in what is left, each lamp inset from the right edge. */
#define LINE_H    24
#define LINE_GAP  6
#define BAR_H     24
#define STACK_H   (3 * LINE_H + 2 * LINE_GAP)
#define LAMP_X    (LCD_W - 8 - 6)
#define LAMP_Y(i) (BAR_H + (LCD_H - BAR_H - STACK_H) / 2 + (i) * (LINE_H + LINE_GAP) \
                   + LINE_H / 2)

static const char *lamp(int i) {
    uint16_t c = fake_panel_pixel(LAMP_X, LAMP_Y(i));
    if (c == COL_RED)   return "red";
    if (c == COL_AMBER) return "amber";
    if (c == COL_GREEN) return "green";
    if (c == COL_GREY)  return "grey";
    return "?";
}

/* The row's own text colour, sampled where the first glyph's stem falls. A
 * greyed-out row has to be obvious as a row, not just as a lamp. */
static bool row_is_grey(int i) {
    for (int x = 0; x < LCD_W / 2; x++)
        for (int dy = -6; dy <= 6; dy++)
            if (fake_panel_pixel(x, LAMP_Y(i) + dy) == COL_GREY) return true;
    return false;
}

/* A screen that owns the panel reaches all four corners in its own colour. */
static bool screen_is(uint16_t colour) {
    const int pts[][2] = { {1, 1}, {LCD_W - 2, 1},
                           {1, LCD_H - 2}, {LCD_W - 2, LCD_H - 2} };
    for (int i = 0; i < 4; i++)
        if (fake_panel_pixel(pts[i][0], pts[i][1]) != colour) return false;
    return true;
}
static bool screen_is_green(void) { return screen_is(COL_GREEN); }

/* The mode bar shares the body's background, so it is ruled off instead. The
 * rule has to be solid across the panel, and must not eat into the text. */
static bool bar_has_rule(void) {
    for (int x = 0; x < LCD_W; x++)
        if (fake_panel_pixel(x, BAR_H - 1) != COL_WHITE) return false;
    return true;
}

/* Whether any of the bar's text ink falls in [x0, x1). The menu's bar is black
 * on blue, so its ink is the one colour nothing else in that band uses. */
static bool bar_ink_between(int x0, int x1) {
    for (int x = x0; x < x1; x++)
        for (int y = 0; y < BAR_H; y++)
            if (fake_panel_pixel(x, y) == COL_BLACK) return true;
    return false;
}

/* The x of the leftmost lit pixel in a row, which is where its text actually
 * begins -- not where its glyph cell does. */
static int row_ink_x(int i) {
    for (int x = 0; x < LCD_W / 2; x++)
        for (int dy = -8; dy <= 8; dy++)
            if (fake_panel_pixel(x, LAMP_Y(i) + dy) != COL_BLACK) return x;
    return -1;
}

/* How many lamps are drawn at all, found by walking the lamp column rather
 * than by assuming a row count -- a list with fewer rows is centred
 * differently, so fixed positions only work for the full three. */
static int lamp_count(void) {
    int runs = 0;
    bool in_run = false;

    for (int y = BAR_H; y < LCD_H; y++) {
        uint16_t c = fake_panel_pixel(LAMP_X, y);
        bool lit = c == COL_RED || c == COL_AMBER || c == COL_GREEN || c == COL_GREY;
        if (lit && !in_run) runs++;
        in_run = lit;
    }
    return runs;
}

/* The colour of the only lamp on screen, for a single-row list. */
static const char *sole_lamp(void) {
    for (int y = BAR_H; y < LCD_H; y++) {
        uint16_t c = fake_panel_pixel(LAMP_X, y);
        if (c == COL_RED)   return "red";
        if (c == COL_AMBER) return "amber";
        if (c == COL_GREEN) return "green";
        if (c == COL_GREY)  return "grey";
    }
    return "?";
}

/* Any pixel of this colour in the body, for text the lamps do not cover. */
static bool has_text_colour(uint16_t want) {
    for (int y = BAR_H; y < LCD_H; y++)
        for (int x = 0; x < LCD_W - 24; x++)
            if (fake_panel_pixel(x, y) == want) return true;
    return false;
}

/* ...with the word on it in black. */
static bool has_black_text(void) {
    int black = 0;
    for (int y = BAR_H; y < LCD_H; y++)
        for (int x = 0; x < LCD_W; x++)
            if (fake_panel_pixel(x, y) == COL_BLACK) black++;
    return black > 200;
}

static bool lamps_are(const char *a, const char *b, const char *c) {
    return strcmp(lamp(0), a) == 0 && strcmp(lamp(1), b) == 0 &&
           strcmp(lamp(2), c) == 0;
}

static void read_from(const char *name, int bursts) {
    scene_filename = name;
    for (int i = 0; i < bursts; i++) {
        fake_now_ms += 20;
        ui_note_read(1000 + (uint32_t)i * 64);
    }
}

static int scene_offer(void) {
    static const char *const files[] = SCREEN_FILES;

    fake_now_ms = 1000;
    ui_init();
    ui_set_mode(MODE_RESET, PHASE_RECOVER);
    present(true, true, true);
    ui_tick();

    printf("checklist lamps:\n");
    check(lamps_are("red", "red", "red"), "all red before the target reads anything");
    check(bar_has_rule(), "the black status bar is ruled off from the body");

    /* Flush left means flush left: the eye reads the ink, not the cell, and
     * 'i' sits two pixels into its cell where 'u' starts hard against it. */
    check(row_ink_x(0) == row_ink_x(1) && row_ink_x(1) == row_ink_x(2),
          "every row's text starts at the same pixel");

    read_from(files[0], 4);
    ui_tick();
    check(lamps_are("amber", "red", "red"), "the file being read goes amber");

    fake_now_ms += SCREEN_FILE_SETTLE_MS;
    ui_tick();
    check(lamps_are("green", "red", "red"), "it goes green once its reads settle");

    read_from(files[1], 2);
    ui_tick();
    check(lamps_are("green", "amber", "red"), "the next file goes amber, the first stays green");

    /* factoryReset is zero bytes, so there need not be an attributable data
     * read before its directory entry is deleted. */
    scene_marker_present = false;
    ui_note_write(42);
    fake_now_ms += ACTIVITY_IDLE_MS;
    ui_tick();
    check(lamps_are("green", "amber", "green"),
          "deleting the marker can move it directly from red to green");

    fake_now_ms += SCREEN_FILE_SETTLE_MS;
    read_from(files[2], 6);
    fake_now_ms += SCREEN_FILE_SETTLE_MS;
    ui_tick();
    check(lamps_are("green", "green", "green"), "the whole payload goes green");

    /* A target that comes back round must not drag a lamp backwards. */
    read_from(files[0], 3);
    ui_tick();
    check(lamps_are("green", "green", "green"), "a second pass does not un-green a lamp");

    /* Reads outside the listed payload are not attributed to anything. */
    fake_now_ms += SCREEN_FILE_SETTLE_MS;
    scene_filename = NULL;
    for (int i = 0; i < 4; i++) { fake_now_ms += 20; ui_note_read(50000 + (uint32_t)i); }
    ui_tick();
    check(lamps_are("green", "green", "green"), "an unattributable read changes nothing");

    return failures;
}

/* The recover phase of a reset withholds the marker, so the host never sees
 * it. Its row must say so, and the host's writes must not be read as it having
 * deleted anything -- that was the false "deleted" the operator saw on a boot
 * where nothing had been deleted.
 */
static int scene_recover(void) {
    static const char *const files[] = SCREEN_FILES;

    fake_now_ms = 1000;
    ui_init();
    ui_set_mode(MODE_RESET, PHASE_RECOVER);

    /* What main() does for the recover phase: the marker is hidden. */
    scene_marker_present = false;
    present(true, true, false);
    ui_tick();

    printf("recover phase lamps:\n");
    check(lamps_are("red", "red", "grey"),
          "the withheld marker is greyed out, not lamped");
    check(row_is_grey(2), "and its name is greyed out with it");

    read_from(files[0], 4);
    fake_now_ms += SCREEN_FILE_SETTLE_MS;
    read_from(files[1], 4);
    fake_now_ms += SCREEN_FILE_SETTLE_MS;
    ui_tick();
    check(lamps_are("green", "green", "grey"),
          "the rest of the payload ticks off as usual, the marker stays grey");

    /* The host writing -- a filesystem timestamp, a recovery journal, anything
     * -- used to set the marker's lamp off a check that could only ever come
     * back "absent", since the firmware is what made it absent. */
    ui_note_write(42);
    fake_now_ms += ACTIVITY_IDLE_MS;
    ui_tick();
    check(lamps_are("green", "green", "grey"),
          "a host write while it is withheld changes nothing");

    /* Everything on offer has been read, which is what ends this phase. */
    check(ui_payload_through(), "the exposed files read through");
    check(ui_payload_touched(), "and the read is attributed to the payload");

    /* A read of a file the host cannot see is not the host reading it. */
    ui_reset_progress();
    read_from(MARKER_FILENAME, 4);
    check(!ui_payload_touched(), "a read attributed to a hidden file is ignored");

    return failures;
}

/* The payload has to be read through for the finished screen to be owed: a
 * hand-off boot where the target never reads anything must keep showing the
 * checklist, however long it sits there. */
static int scene_upgrade(void) {
    fake_now_ms = 1000;
    ui_init();
    ui_set_mode(MODE_UPGRADE, PHASE_RECOVER);

    /* UPGRADE has nothing to do with the failsafe image or the marker, so
     * neither is listed at all -- a greyed row would only invite the question
     * of when it is coming. */
    ui_set_file_state(FILE_FAILSAFE,   false, false);
    ui_set_file_state(FILE_DOWNLOAD,   true,  true);
    ui_set_file_state(MARKER_FILENAME, false, false);
    ui_tick();

    printf("upgrade mode:\n");
    check(lamp_count() == 1, "exactly one row is listed");
    check(strcmp(sole_lamp(), "red") == 0,
          "and it is the download, not yet read");
    check(!ui_payload_through(), "nothing is through before anything is read");

    fake_now_ms += PHASE_IDLE_MS * 2;
    ui_tick();
    check(!screen_is_green(), "no finished screen until the mode machine says so");

    read_from(FILE_DOWNLOAD, 6);
    fake_now_ms += SCREEN_FILE_SETTLE_MS;
    ui_tick();
    check(ui_payload_through(), "the download reads through on its own");

    /* main() is what decides the job is over; the screen only shows it. */
    ui_set_finished(true);
    ui_tick();
    check(screen_is_green(), "the finished screen takes the whole panel");
    check(has_black_text(), "with the word on it");

    return failures;
}

/* The menu takes the panel, names the choice, and changes nothing by itself. */
static int scene_menu(void) {
    fake_now_ms = 1000;
    ui_init();
    ui_set_mode(MODE_RESET, PHASE_RECOVER);
    present(true, true, true);
    ui_tick();

    printf("menu:\n");
    ui_show_menu(MODE_UPGRADE);
    ui_tick();
    check(!screen_is_green(), "the menu is not the finished screen");
    check(has_text_colour(COL_WHITE), "the selected mode is picked out");
    check(has_text_colour(COL_GREY), "and the others are not");

    /* The bar carries the board on the left and the build's version on the
     * right. Right-aligned text grows leftwards, so what is worth checking is
     * not that each is there but that the longest version the convention
     * allows still clears the label -- the two meeting in the middle is the
     * only way this breaks. */
    check(bar_ink_between(0, 64), "the board is on the bar");
    check(bar_ink_between(LCD_W / 2, LCD_W), "and the version beside it");
    check(!bar_ink_between(64, 96), "with the two clear of each other");

    ui_close_menu();
    ui_tick();
    check(lamps_are("red", "red", "red"), "closing it puts the checklist back");

    /* A write no longer takes the body over, so the list stays put through
     * one -- it was flickering away for every stray sector the host touched. */
    ui_note_write(42);
    ui_tick();
    check(lamps_are("red", "red", "red"), "a host write leaves the checklist up");

    return failures;
}

int main(int argc, char **argv) {
    const char *scene = argc > 1 ? argv[1] : "offer";

    if (strcmp(scene, "offer") == 0)        failures = scene_offer();
    else if (strcmp(scene, "recover") == 0) failures = scene_recover();
    else if (strcmp(scene, "upgrade") == 0) failures = scene_upgrade();
    else if (strcmp(scene, "menu") == 0)    failures = scene_menu();
    else { fprintf(stderr, "usage: %s offer|recover|upgrade|menu\n", argv[0]); return 2; }

    printf("%s\n", failures ? "FAILURES" : "  all lamp checks passed");
    return failures ? 1 : 0;
}
