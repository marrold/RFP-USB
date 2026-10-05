/* Minimal stand-in for the Pico SDK, so the display and UI code can be
 * compiled and rendered on a host. Only what src/ui.c and src/st7789.c
 * actually reach for. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "hardware/gpio.h"
#include "hardware/spi.h"

typedef uint32_t absolute_time_t;

/* The fake clock is driven by the renderer so each screen can be shown at a
 * chosen moment, e.g. mid-transfer. */
extern uint32_t fake_now_ms;

static inline absolute_time_t get_absolute_time(void) { return fake_now_ms; }
static inline uint32_t to_ms_since_boot(absolute_time_t t) { return t; }
static inline void sleep_ms(uint32_t ms) { (void)ms; }

#define count_of(a) (sizeof(a) / sizeof((a)[0]))
