#pragma once

#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "board_config.h"

/* Configures the 8 button pins as plain GPIO inputs — no internal pull,
 * the hardware already has an external 2.2kΩ series + 10kΩ pullup + 10nF
 * filter per input (see hardware/PINOUT.md). */
void input_hal_gpio_init(void);

/* Samples all 8 buttons, active-low at the pin, and writes true (pressed)
 * / false (released) logical values into raw_pressed. */
void input_hal_gpio_sample(bool raw_pressed[BOARD_INPUT_COUNT]);

/* Arms the two things that let a parked board still notice a button.
 *
 * mc_power slows the app loop to MC_POWER_TICK_PARKED_MS and lets the chip
 * light-sleep between ticks, and mc_input only observes a button at poll
 * time — so without this a press could be slept through entirely.
 *
 *   - gpio_wakeup_enable() on each button at LOW level (they are active-low
 *     with an external pullup), plus esp_sleep_enable_gpio_wakeup(): this is
 *     what brings the CPU out of light sleep. Level is not a choice — the
 *     API rejects edge modes for wake, and it rewrites the pin's interrupt
 *     type to match.
 *   - a per-pin ISR that masks its own interrupt and notifies
 *     `task_to_notify`, so the loop's wait is cut short instead of running
 *     out the full parked tick. The mask is mandatory: a low-level interrupt
 *     stays asserted while the pin is held, and a maintained switch (an
 *     ignition key) holds it for hours.
 *
 * Call after the app task exists, so the ISR always has something to notify. */
void input_hal_gpio_wake_init(TaskHandle_t task_to_notify);

/* Re-enables the interrupt for any button that has been released since its
 * ISR masked it. Call once per app-loop iteration; cheap and a no-op when
 * nothing is masked. Without it a button only ever wakes the board once. */
void input_hal_gpio_wake_rearm(void);
