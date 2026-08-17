#pragma once

#include "mc_output.h"

/* Configures the 12 PROFET IN pins as GPIO outputs. Does not assert any
 * level — mc_output_restore_from_config() (called immediately after, in
 * app_main) is what should set the first real level on each pin, to keep
 * the reset-to-restored-state window as short as possible (ride-safe
 * failure: <250ms). */
void output_hal_gpio_init(void);

mc_output_hal_t output_hal_gpio_get(void);
