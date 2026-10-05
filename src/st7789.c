#include <string.h>

#include "hardware/gpio.h"
#include "hardware/spi.h"
#include "pico/stdlib.h"

#include "font8x16.h"
#include "st7789.h"

/* One scanline, in the panel's big-endian pixel order. Everything is composed
 * a scanline at a time and streamed, so nothing needs a taller buffer. */
static uint16_t line[LCD_W];

static inline uint16_t swap16(uint16_t v) { return (uint16_t)((v >> 8) | (v << 8)); }

static void cmd(uint8_t c) {
    gpio_put(LCD_PIN_DC, 0);
    gpio_put(LCD_PIN_CS, 0);
    spi_write_blocking(LCD_SPI, &c, 1);
    gpio_put(LCD_PIN_CS, 1);
}

static void data(const uint8_t *d, size_t n) {
    gpio_put(LCD_PIN_DC, 1);
    gpio_put(LCD_PIN_CS, 0);
    spi_write_blocking(LCD_SPI, d, n);
    gpio_put(LCD_PIN_CS, 1);
}

static void cmd_data(uint8_t c, const uint8_t *d, size_t n) {
    cmd(c);
    if (n) data(d, n);
}

static void set_window(int x, int y, int w, int h) {
    int x0 = x + LCD_COL_OFFSET, x1 = x + w - 1 + LCD_COL_OFFSET;
    int y0 = y + LCD_ROW_OFFSET, y1 = y + h - 1 + LCD_ROW_OFFSET;
    uint8_t buf[4];

    buf[0] = x0 >> 8; buf[1] = x0 & 0xFF; buf[2] = x1 >> 8; buf[3] = x1 & 0xFF;
    cmd_data(0x2A, buf, 4);                       /* CASET */
    buf[0] = y0 >> 8; buf[1] = y0 & 0xFF; buf[2] = y1 >> 8; buf[3] = y1 & 0xFF;
    cmd_data(0x2B, buf, 4);                       /* RASET */
    cmd(0x2C);                                    /* RAMWR */
}

void st7789_init(void) {
    spi_init(LCD_SPI, LCD_SPI_BAUD);
    gpio_set_function(LCD_PIN_SCK,  GPIO_FUNC_SPI);
    gpio_set_function(LCD_PIN_MOSI, GPIO_FUNC_SPI);

    static const uint ctrl_pins[] = { LCD_PIN_CS, LCD_PIN_DC, LCD_PIN_RST };
    for (unsigned i = 0; i < count_of(ctrl_pins); i++) {
        gpio_init(ctrl_pins[i]);
        gpio_set_dir(ctrl_pins[i], GPIO_OUT);
        gpio_put(ctrl_pins[i], 1);
    }
    gpio_init(LCD_PIN_BL);
    gpio_set_dir(LCD_PIN_BL, GPIO_OUT);
    gpio_put(LCD_PIN_BL, 0);

    gpio_put(LCD_PIN_RST, 1); sleep_ms(10);
    gpio_put(LCD_PIN_RST, 0); sleep_ms(10);
    gpio_put(LCD_PIN_RST, 1); sleep_ms(120);

    cmd(0x11);                                    /* SLPOUT */
    sleep_ms(120);

    static const uint8_t madctl[]  = { 0x70 };    /* landscape, RGB order */
    static const uint8_t colmod[]  = { 0x05 };    /* 16 bits per pixel */
    static const uint8_t porch[]   = { 0x0C, 0x0C, 0x00, 0x33, 0x33 };
    static const uint8_t gctrl[]   = { 0x35 };
    static const uint8_t vcoms[]   = { 0x19 };
    static const uint8_t lcmctrl[] = { 0x2C };
    static const uint8_t vdvvrhen[]= { 0x01 };
    static const uint8_t vrhs[]    = { 0x12 };
    static const uint8_t vdvs[]    = { 0x20 };
    static const uint8_t frctrl2[] = { 0x0F };
    static const uint8_t pwctrl1[] = { 0xA4, 0xA1 };
    static const uint8_t gamma_p[] = { 0xD0, 0x04, 0x0D, 0x11, 0x13, 0x2B, 0x3F,
                                       0x54, 0x4C, 0x18, 0x0D, 0x0B, 0x1F, 0x23 };
    static const uint8_t gamma_n[] = { 0xD0, 0x04, 0x0C, 0x11, 0x13, 0x2C, 0x3F,
                                       0x44, 0x51, 0x2F, 0x1F, 0x1F, 0x20, 0x23 };

    cmd_data(0x36, madctl,   sizeof madctl);
    cmd_data(0x3A, colmod,   sizeof colmod);
    cmd_data(0xB2, porch,    sizeof porch);
    cmd_data(0xB7, gctrl,    sizeof gctrl);
    cmd_data(0xBB, vcoms,    sizeof vcoms);
    cmd_data(0xC0, lcmctrl,  sizeof lcmctrl);
    cmd_data(0xC2, vdvvrhen, sizeof vdvvrhen);
    cmd_data(0xC3, vrhs,     sizeof vrhs);
    cmd_data(0xC4, vdvs,     sizeof vdvs);
    cmd_data(0xC6, frctrl2,  sizeof frctrl2);
    cmd_data(0xD0, pwctrl1,  sizeof pwctrl1);
    cmd_data(0xE0, gamma_p,  sizeof gamma_p);
    cmd_data(0xE1, gamma_n,  sizeof gamma_n);

    cmd(0x21);                                    /* INVON: this panel is inverted */
    cmd(0x29);                                    /* DISPON */

    st7789_fill(COL_BLACK);
    st7789_backlight(true);
}

void st7789_backlight(bool on) { gpio_put(LCD_PIN_BL, on); }

void st7789_fill(uint16_t colour) {
    uint16_t c = swap16(colour);
    for (int i = 0; i < LCD_W; i++) line[i] = c;

    set_window(0, 0, LCD_W, LCD_H);
    gpio_put(LCD_PIN_DC, 1);
    gpio_put(LCD_PIN_CS, 0);
    for (int y = 0; y < LCD_H; y++)
        spi_write_blocking(LCD_SPI, (const uint8_t *)line, LCD_W * 2);
    gpio_put(LCD_PIN_CS, 1);
}

/* Two-pixel strokes, to match the weight of the bold face beside it. */
static const uint8_t glyph_inf[FONT_H] = {
    0x00, 0x00, 0x00, 0x00, 0x00,
    0x66,  /* .##..##. */
    0x99,  /* #..##..# */
    0x99,  /* #..##..# */
    0x99,  /* #..##..# */
    0x66,  /* .##..##. */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

static uint8_t glyph_row(unsigned char ch, int gy) {
    if (ch == (unsigned char)ST7789_INF[0]) return glyph_inf[gy];
    if (ch < FONT_FIRST || ch > FONT_LAST) ch = '?';
    return font8x16[ch - FONT_FIRST][gy];
}

/* Scales are carried in half-glyph units, so 2 is 1x, 3 is 1.5x and 4 is 2x.
 * Halves exist because 1x is cramped and 2x is full-width for a fourteen
 * character name -- there has to be a step between them. */
#define CELL_W(h)    (FONT_W * (h) / 2)
#define CELL_H(h)    (FONT_H * (h) / 2)

/* Inset on both edges, so nothing sits against the bezel. */
#define PAD          8

#define SPLASH_SCALE 4
#define SPLASH_CW    CELL_W(SPLASH_SCALE)
#define SPLASH_CH    CELL_H(SPLASH_SCALE)
#define SPLASH_GAP   8

/* Draws one scanline of `s` into `line`, scaled. Destination-driven -- each
 * output pixel is mapped back to a source pixel -- so a half-step scale works
 * the same as a whole one. The face is already bold, so nothing is thickened
 * here; doing so would close up the counters at these sizes. */
static void draw_scanline(const char *s, int y, int top, int x0, int half,
                          uint16_t fg) {
    if (y < top || y >= top + CELL_H(half)) return;

    int gy = (y - top) * 2 / half;
    if (gy >= FONT_H) gy = FONT_H - 1;

    int len = (int)strlen(s);
    int w   = len * CELL_W(half);

    for (int dx = 0; dx < w; dx++) {
        int px = x0 + dx;
        if (px < 0) continue;
        if (px >= LCD_W) break;

        int src = dx * 2 / half;
        int c   = src / FONT_W;
        if (c >= len) break;

        uint8_t bits = glyph_row((unsigned char)s[c], gy);
        if (bits & (0x80 >> (src % FONT_W))) line[px] = fg;
    }
}

/* How many blank columns a glyph carries before its ink begins.
 *
 * Aligning left-justified text on cell edges leaves it looking ragged, because
 * 'i' sits two pixels into its cell where 'u' starts hard against the edge.
 * The eye reads the ink, not the cell, so a flush-left list is aligned on the
 * first glyph's ink and the cell is allowed to hang into the margin. */
static int glyph_left_bearing(unsigned char c) {
    int best = FONT_W;

    for (int gy = 0; gy < FONT_H; gy++) {
        uint8_t bits = glyph_row(c, gy);
        if (!bits) continue;

        int b = 0;
        while (b < FONT_W && !(bits & (0x80 >> b))) b++;
        if (b < best) best = b;
    }
    /* A blank glyph has no ink to align, so it shifts nothing. */
    return best == FONT_W ? 0 : best;
}

/* Where a flush-left run of text starts, so that its ink lands on `x`. */
static int flush_left_x(const char *s, int x, int half) {
    if (!s[0]) return x;
    return x - glyph_left_bearing((unsigned char)s[0]) * CELL_W(half) / FONT_W;
}

/* One scanline of a filled square, for the status bar's activity marker. A
 * square rather than a disc on purpose: the checklist's lamps are discs, and
 * the two say different things -- one is what the host is doing right now, the
 * other is how far a file has got. Shape keeps them apart at a glance, before
 * colour has been read at all. */
static void square_scanline(int y, int cy, int cx, int r, uint16_t fg) {
    if (y < cy - r || y > cy + r) return;

    for (int dx = -r; dx <= r; dx++) {
        int px = cx + dx;
        if (px >= 0 && px < LCD_W) line[px] = fg;
    }
}

/* One scanline of a filled disc. Drawn from its equation rather than from a
 * bitmap: a check mark small enough to sit in a glyph cell has single-pixel
 * strokes that break up badly once scaled, where a disc stays a disc. */
static void disc_scanline(int y, int cy, int cx, int r, uint16_t fg) {
    int dy = y - cy;
    if (dy < -r || dy > r) return;

    /* Testing against r*r+r rather than r*r approximates a radius of r+0.5,
     * which flattens the top and bottom rows; the strict test leaves them a
     * single pixel wide and the disc comes out pointed. */
    int w = 0;
    while ((w + 1) * (w + 1) + dy * dy <= r * r + r) w++;

    for (int dx = -w; dx <= w; dx++) {
        int px = cx + dx;
        if (px >= 0 && px < LCD_W) line[px] = fg;
    }
}

static int centred_x(const char *s, int half) {
    int x = (LCD_W - (int)strlen(s) * CELL_W(half)) / 2;
    return x < 0 ? 0 : x;
}

void st7789_bar(int h, int half, uint16_t fg, uint16_t bg,
                const char *left, const char *right, uint16_t dot,
                uint16_t rule) {
    uint16_t f = swap16(fg), b = swap16(bg);

    int top = (h - CELL_H(half)) / 2;
    int rx  = LCD_W - PAD - (int)strlen(right) * CELL_W(half);

    /* The disc takes a column the width of a glyph cell and the text starts
     * after it, so a bar with one and a bar without stay aligned to the same
     * left inset. */
    int dot_r  = (CELL_W(half) - 2) / 2;
    int dot_cx = PAD + dot_r;
    int lx     = flush_left_x(left, dot ? dot_cx + dot_r + CELL_W(half) / 2 : PAD,
                              half);

    /* The rule is the bar's last scanline rather than an extra one, so adding
     * it does not move the body down. The bar carries capitals and digits
     * only, whose ink stops well above the descender rows, so nothing of the
     * text reaches the row it occupies. */
    int rule_y = rule ? h - 1 : -1;

    set_window(0, 0, LCD_W, h);
    gpio_put(LCD_PIN_DC, 1);
    gpio_put(LCD_PIN_CS, 0);
    for (int y = 0; y < h; y++) {
        if (y == rule_y) {
            uint16_t r = swap16(rule);
            for (int i = 0; i < LCD_W; i++) line[i] = r;
        } else {
            for (int i = 0; i < LCD_W; i++) line[i] = b;
            if (dot) square_scanline(y, h / 2, dot_cx, dot_r, swap16(dot));
            draw_scanline(left, y, top, lx, half, f);
            if (right[0]) draw_scanline(right, y, top, rx, half, f);
        }
        spi_write_blocking(LCD_SPI, (const uint8_t *)line, LCD_W * 2);
    }
    gpio_put(LCD_PIN_CS, 1);
}

void st7789_splash(uint16_t fg, uint16_t bg, const char *l1, const char *l2) {
    uint16_t f = swap16(fg), b = swap16(bg);

    int top1 = (LCD_H - (2 * SPLASH_CH + SPLASH_GAP)) / 2;
    int top2 = top1 + SPLASH_CH + SPLASH_GAP;
    int x1 = centred_x(l1, SPLASH_SCALE), x2 = centred_x(l2, SPLASH_SCALE);

    /* Composed a scanline at a time, so this needs no more RAM than a text
     * row does; the address window is set once and the controller
     * auto-increments through it. */
    set_window(0, 0, LCD_W, LCD_H);
    gpio_put(LCD_PIN_DC, 1);
    gpio_put(LCD_PIN_CS, 0);
    for (int y = 0; y < LCD_H; y++) {
        for (int i = 0; i < LCD_W; i++) line[i] = b;
        draw_scanline(l1, y, top1, x1, SPLASH_SCALE, f);
        draw_scanline(l2, y, top2, x2, SPLASH_SCALE, f);
        spi_write_blocking(LCD_SPI, (const uint8_t *)line, LCD_W * 2);
    }
    gpio_put(LCD_PIN_CS, 1);
}

/* Vertical breathing room between stacked lines, and the largest a line may
 * grow to -- 6 halves is 3x. */
#define BLOCK_GAP      6
#define BLOCK_MAX_HALF 6

/* Holds every auto-sized left line to the smallest scale any of them needs,
 * so a checklist stays aligned instead of going ragged on its longest name. */
static void level_list(int *sc, const st7789_line_t *lines, int n) {
    int smallest = 0;

    for (int i = 0; i < n; i++)
        if (lines[i].left && lines[i].scale == 0)
            if (!smallest || sc[i] < smallest) smallest = sc[i];

    if (!smallest) return;
    for (int i = 0; i < n; i++)
        if (lines[i].left && lines[i].scale == 0) sc[i] = smallest;
}

void st7789_block(int top, int h, uint16_t bg, const st7789_line_t *lines, int n) {
    uint16_t b = swap16(bg);
    int avail = h;

    if (n > ST7789_BLOCK_LINES) n = ST7789_BLOCK_LINES;
    if (n < 0) n = 0;

    int sc[ST7789_BLOCK_LINES], ytop[ST7789_BLOCK_LINES];
    uint16_t fg[ST7789_BLOCK_LINES];

    for (int i = 0; i < n; i++) {
        fg[i] = swap16(lines[i].fg);
        if (lines[i].scale > 0) {
            sc[i] = lines[i].scale;
            continue;
        }
        /* Budget the insets, a cell of slack, and a left line's mark column. */
        int cells = (int)strlen(lines[i].text) + 1 + (lines[i].left ? 1 : 0);
        sc[i] = (LCD_W - 2 * PAD) * 2 / (cells * FONT_W);
        if (sc[i] > BLOCK_MAX_HALF) sc[i] = BLOCK_MAX_HALF;
        if (sc[i] < 2) sc[i] = 2;
    }
    level_list(sc, lines, n);

    /* An auto-sized line is only as large as the panel width allows; it may
     * still be more than the other lines have left it, so give the tallest of
     * them back a step at a time until the stack fits. */
    for (;;) {
        int h = n ? (n - 1) * BLOCK_GAP : 0;
        for (int i = 0; i < n; i++) h += CELL_H(sc[i]);
        if (h <= avail) break;

        int worst = -1;
        for (int i = 0; i < n; i++)
            if (lines[i].scale == 0 && sc[i] > 2 && (worst < 0 || sc[i] > sc[worst]))
                worst = i;
        if (worst < 0) break;                 /* nothing left to give back */
        sc[worst]--;
        level_list(sc, lines, n);
    }

    int total = n ? (n - 1) * BLOCK_GAP : 0;
    for (int i = 0; i < n; i++) total += CELL_H(sc[i]);

    int y = top + (avail - total) / 2;
    int x0[ST7789_BLOCK_LINES], tx[ST7789_BLOCK_LINES];
    for (int i = 0; i < n; i++) {
        ytop[i] = y;
        y += CELL_H(sc[i]) + BLOCK_GAP;
        x0[i] = lines[i].left ? flush_left_x(lines[i].text, PAD, sc[i])
                              : centred_x(lines[i].text, sc[i]);
        tx[i] = LCD_W - PAD - CELL_W(sc[i]) / 2;   /* mark centre, inset */
    }

    set_window(0, top, LCD_W, avail);
    gpio_put(LCD_PIN_DC, 1);
    gpio_put(LCD_PIN_CS, 0);
    for (int py = top; py < top + avail; py++) {
        for (int i = 0; i < LCD_W; i++) line[i] = b;
        for (int i = 0; i < n; i++) {
            if (lines[i].mark)
                disc_scanline(py, ytop[i] + CELL_H(sc[i]) / 2, tx[i],
                              (CELL_W(sc[i]) - 2) / 2, swap16(lines[i].mark_fg));
            draw_scanline(lines[i].text, py, ytop[i], x0[i], sc[i], fg[i]);
        }
        spi_write_blocking(LCD_SPI, (const uint8_t *)line, LCD_W * 2);
    }
    gpio_put(LCD_PIN_CS, 1);
}


