/* An ST7789 just faithful enough to show what the panel would display.
 *
 * src/st7789.c is compiled unmodified against this; we watch the DC pin and
 * the SPI byte stream, honour CASET / RASET / RAMWR, and paint into a
 * controller-sized framebuffer. The visible 240x135 window is then cut out
 * at the panel's offsets -- so a wrong offset, a wrong window end or a
 * byte-order slip would all show up in the output rather than being
 * assumed away.
 *
 * Not emulated: MADCTL rotation. The driver's addressing is self-consistent,
 * so the extracted window matches the panel regardless.
 */
#include <stdio.h>
#include <string.h>

#include "config.h"
#include "hardware/gpio.h"
#include "hardware/spi.h"

uint32_t fake_now_ms;

static struct spi_inst spi0_inst, spi1_inst;
spi_inst_t *spi0 = &spi0_inst;
spi_inst_t *spi1 = &spi1_inst;

#define PANEL_W 320
#define PANEL_H 320

static uint16_t panel[PANEL_H][PANEL_W];   /* controller RAM, native RGB565 */
static bool     dc_data;                   /* DC pin: 0 command, 1 data */
static uint8_t  cur_cmd;
static uint16_t caset_x0, caset_x1, raset_y0, raset_y1;
static uint16_t cursor_x, cursor_y;
static uint8_t  param[8];
static int      param_n;
static uint8_t  pixel_hi;
static bool     have_hi;

void gpio_init(uint pin) { (void)pin; }
void gpio_set_dir(uint pin, int out) { (void)pin; (void)out; }
void gpio_set_function(uint pin, enum gpio_function fn) { (void)pin; (void)fn; }

void gpio_put(uint pin, bool value) {
    if (pin == LCD_PIN_DC) dc_data = value;
}

unsigned spi_init(spi_inst_t *spi, unsigned baud) { (void)spi; return baud; }

int spi_write_blocking(spi_inst_t *spi, const uint8_t *src, size_t len) {
    (void)spi;

    for (size_t i = 0; i < len; i++) {
        uint8_t b = src[i];

        if (!dc_data) {                 /* command byte */
            cur_cmd = b;
            param_n = 0;
            have_hi = false;
            if (b == 0x2C) {            /* RAMWR homes the write cursor */
                cursor_x = caset_x0;
                cursor_y = raset_y0;
            }
            continue;
        }

        switch (cur_cmd) {
            case 0x2A:                  /* CASET: column start/end */
            case 0x2B:                  /* RASET: row start/end */
                if (param_n < 4) param[param_n++] = b;
                if (param_n == 4) {
                    uint16_t a = (uint16_t)((param[0] << 8) | param[1]);
                    uint16_t z = (uint16_t)((param[2] << 8) | param[3]);
                    if (cur_cmd == 0x2A) { caset_x0 = a; caset_x1 = z; }
                    else                 { raset_y0 = a; raset_y1 = z; }
                }
                break;

            case 0x2C:                  /* RAMWR: 16bpp, big-endian on the wire */
                if (!have_hi) { pixel_hi = b; have_hi = true; break; }
                have_hi = false;
                if (cursor_x < PANEL_W && cursor_y < PANEL_H)
                    panel[cursor_y][cursor_x] = (uint16_t)((pixel_hi << 8) | b);
                if (++cursor_x > caset_x1) { cursor_x = caset_x0; cursor_y++; }
                break;

            default:
                break;                  /* init parameters, nothing to model */
        }
    }
    return (int)len;
}

/* One pixel of the visible window, in native RGB565 -- directly comparable
 * with the COL_* constants, so a test can assert on what was drawn. */
uint16_t fake_panel_pixel(int x, int y) {
    if (x < 0 || x >= LCD_W || y < 0 || y >= LCD_H) return 0;
    return panel[y + LCD_ROW_OFFSET][x + LCD_COL_OFFSET];
}

/* Writes the visible window as a binary PPM. */
bool fake_panel_save_ppm(const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    fprintf(f, "P6\n%d %d\n255\n", LCD_W, LCD_H);

    for (int y = 0; y < LCD_H; y++) {
        for (int x = 0; x < LCD_W; x++) {
            uint16_t c = panel[y + LCD_ROW_OFFSET][x + LCD_COL_OFFSET];
            uint8_t rgb[3] = {
                (uint8_t)(((c >> 11) & 0x1F) * 255 / 31),
                (uint8_t)(((c >> 5)  & 0x3F) * 255 / 63),
                (uint8_t)(( c        & 0x1F) * 255 / 31),
            };
            fwrite(rgb, 1, 3, f);
        }
    }
    fclose(f);
    return true;
}
