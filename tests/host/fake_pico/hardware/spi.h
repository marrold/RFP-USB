#pragma once
#include <stddef.h>
#include <stdint.h>

struct spi_inst { int unused; };
typedef struct spi_inst spi_inst_t;
extern spi_inst_t *spi0;
extern spi_inst_t *spi1;

unsigned spi_init(spi_inst_t *spi, unsigned baud);
int spi_write_blocking(spi_inst_t *spi, const uint8_t *src, size_t len);
