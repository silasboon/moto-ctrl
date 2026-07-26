#pragma once

#include <stdbool.h>

#include "board_config.h"

/* Configures the 8 button pins as plain GPIO inputs — no internal pull,
 * the hardware already has an external 2.2kΩ series + 10kΩ pullup + 10nF
 * filter per input (see hardware/PINOUT.md). */
void input_hal_gpio_init(void);

/* Samples all 8 buttons, active-low at the pin, and writes true (pressed)
 * / false (released) logical values into raw_pressed. */
void input_hal_gpio_sample(bool raw_pressed[BOARD_INPUT_COUNT]);
