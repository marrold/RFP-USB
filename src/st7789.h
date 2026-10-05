/* ST7789V driver for the RP2040-GEEK's 1.14" 240x135 IPS panel.
 *
 * There is no full framebuffer: 240x135 at 16bpp would cost 63 KB, which is
 * better spent on the copy-on-write overlay. Text is composed one 8x16 row
 * at a time into a 7.7 KB line buffer and blitted.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "config.h"
#include "font8x16.h"

#define RGB565(r, g, b) \
    ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))

#define COL_BLACK   RGB565(0,   0,   0)
#define COL_WHITE   RGB565(255, 255, 255)
#define COL_GREY    RGB565(128, 128, 128)
#define COL_RED     RGB565(255, 60,  60)
#define COL_GREEN   RGB565(60,  220, 90)
#define COL_AMBER   RGB565(255, 180, 0)
#define COL_BLUE    RGB565(80,  150, 255)
/* Writing needs its own colour: red is spoken for by faults, and a host write
 * is not one. Orange is far enough from the blue of idle and the green of
 * reading to be told apart at a glance, and bright enough to hold black. It
 * sits deliberately below COL_AMBER, which the checklist lamps use for "being
 * read right now" -- the two are never side by side, but they should not be
 * the same colour either. */
#define COL_ORANGE  RGB565(255, 125, 0)

/* Nothing is laid out on a character grid any more; this is just the widest
 * line that can be drawn at 1x, and so the size every text buffer takes. */
#define TEXT_COLS   (LCD_W / FONT_W)   /* 30 */

void st7789_init(void);
void st7789_backlight(bool on);

void st7789_fill(uint16_t colour);

/* Takes over the whole panel: a solid background with two lines of
 * double-size text centred on it. */
void st7789_splash(uint16_t fg, uint16_t bg, const char *l1, const char *l2);

/* Symbols the ASCII font does not carry, selected by a control character so
 * they can sit inline in an ordinary string. */
#define ST7789_INF  "\x01"          /* infinity */

/* Scales are in half-glyph units: 2 is 1x, 3 is 1.5x, 4 is 2x. */
#define ST7789_X1   2
#define ST7789_X1_5 3
#define ST7789_X2   4

/* One line of a block. `scale` is in halves, or 0 for "the largest that
 * fits", resolved against the panel width and the room the other lines leave.
 * A `left` line is flush left with its mark against the right edge instead of
 * being centred; together they make a checklist. `mark` draws a filled disc in
 * that column, whose colour is the caller's to choose -- it carries the state,
 * not merely the fact of it. Left-justified lines are held to a common scale,
 * since a ragged list reads worse than a small one. Both edges are inset. */
typedef struct {
    const char *text;
    uint16_t    fg;
    int         scale;
    bool        left;
    bool        mark;
    uint16_t    mark_fg;
} st7789_line_t;

#define ST7789_BLOCK_LINES 6

/* Repaints everything from `top` to the bottom edge, with the lines stacked
 * and centred in it, horizontally and vertically. This owns
 * the pixels below the status bar outright -- it is not on the row grid, and
 * the two must not be mixed over the same region. */
/* Repaints the band from `top` for `h` pixels, with the lines stacked and
 * centred in it. Two calls partition the panel when something has to sit at
 * the bottom rather than in the middle. */
void st7789_block(int top, int h, uint16_t bg, const st7789_line_t *lines, int n);

/* The status bar: a coloured band `h` pixels tall, with text at `half` scale
 * flush left and a right-aligned trailer. Owns the top of the panel.
 *
 * `dot` draws a filled square before the left text, for state that should read
 * without being focused on -- the bar's words are spent naming the mode, so
 * what the host is doing is carried by colour alone. Square, not round: the
 * checklist's lamps are discs and mean something else entirely. Pass 0 for
 * none.
 *
 * `rule` draws a line along the bar's bottom edge, for a bar whose background
 * matches the body and would otherwise have no visible end. It takes the last
 * scanline rather than an extra one, so it does not move the body. Pass 0 for
 * no rule -- a bar with a colour of its own already has an edge. */
void st7789_bar(int h, int half, uint16_t fg, uint16_t bg,
                const char *left, const char *right, uint16_t dot,
                uint16_t rule);
