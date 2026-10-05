#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef unsigned int uint;
enum gpio_function { GPIO_FUNC_SPI = 1 };
enum gpio_drive_strength { GPIO_DRIVE_STRENGTH_4MA };
#define GPIO_OUT 1
#define GPIO_IN  0

void gpio_init(uint pin);
void gpio_set_dir(uint pin, int out);
void gpio_put(uint pin, bool value);
void gpio_set_function(uint pin, enum gpio_function fn);
