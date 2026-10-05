#include <string.h>

#include "hw_config.h"
#include "sd_card.h"

#include "config.h"
#include "sdcard.h"

static spi_t sd_spi = {
    .hw_inst  = SD_SPI,
    .miso_gpio = SD_PIN_MISO,
    .mosi_gpio = SD_PIN_MOSI,
    .sck_gpio  = SD_PIN_SCK,
    .baud_rate = SD_SPI_BAUD,
};

static sd_spi_if_t sd_spi_if = {
    .spi     = &sd_spi,
    .ss_gpio = SD_PIN_CS,
};

/* The GEEK's card slot has no card-detect line wired to a GPIO, so presence
 * is inferred from whether initialisation succeeds. */
static sd_card_t sd_card = {
    .type            = SD_IF_SPI,
    .spi_if_p        = &sd_spi_if,
    .use_card_detect = false,
};

static bool card_ok;

size_t sd_get_num(void) { return 1; }

sd_card_t *sd_get_by_num(size_t num) { return num == 0 ? &sd_card : NULL; }

bool sdcard_init(void) {
    card_ok = false;
    if (!sd_init_driver()) return false;
    if (sd_card.init(&sd_card) != 0) return false;  /* non-zero DSTATUS is a fault */
    card_ok = sd_card.get_num_sectors(&sd_card) > 0;
    return card_ok;
}

bool sdcard_present(void) { return card_ok; }

uint32_t sdcard_sector_count(void) {
    return card_ok ? sd_card.get_num_sectors(&sd_card) : 0;
}

bool sdcard_read(uint32_t lba, uint8_t *buf, uint32_t count) {
    if (!card_ok) return false;
    return sd_card.read_blocks(&sd_card, buf, lba, count) == SD_BLOCK_DEVICE_ERROR_NONE;
}

bool sdcard_write(uint32_t lba, const uint8_t *buf, uint32_t count) {
    if (!card_ok) return false;
    return sd_card.write_blocks(&sd_card, buf, lba, count) == SD_BLOCK_DEVICE_ERROR_NONE;
}

bool sdcard_sync(void) {
    if (!card_ok) return false;
    return sd_card.sync(&sd_card) == SD_BLOCK_DEVICE_ERROR_NONE;
}
