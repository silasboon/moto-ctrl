#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "mc_power.h"

/* power_hal — applies mc_power's profile to the chip.
 *
 * Two ESP-IDF mechanisms do the work, and neither is visible to the
 * portable policy:
 *
 *   - esp_pm's automatic light sleep, entered by the FreeRTOS tickless idle
 *     path whenever every task is blocked for long enough. mc_power's
 *     "light sleep allowed" is expressed as an ESP_PM_NO_LIGHT_SLEEP lock
 *     that is HELD while sleep is refused, which is the inverse of how it
 *     reads in the policy — an acquired lock blocks sleep.
 *   - an ESP_PM_CPU_FREQ_MAX lock, held alongside it. Dynamic frequency
 *     scaling would otherwise drop the CPU below 160MHz while outputs are
 *     being driven, and LEDC-based PWM dimming derives its timing from the
 *     peripheral clock. Holding both locks in ACTIVE means the bike in use
 *     behaves exactly as it did before any of this existed.
 *
 * The BLE controller's own modem sleep is a build-time option rather than
 * anything callable (sdkconfig.defaults), and needs the main crystal as its
 * low-power clock — the only choice this board can make, since it has no
 * external 32kHz crystal and the internal RC oscillator is too imprecise
 * for a BLE connection to hold.
 */

/* Configures DFS + automatic light sleep and creates the two locks, both
 * acquired, so the caller starts fully awake. Call once, before the app
 * task starts. `app_task` is notified by a button GPIO interrupt so a press
 * cuts a slow tick short — see input_hal_gpio_wake_init(). */
void power_hal_init(void);

/* Applies a profile. Cheap and idempotent, but the caller should still only
 * invoke it on an actual change (mc_power_take_profile_change()), since a
 * change of advertising class restarts advertising. */
void power_hal_apply(const mc_power_profile_t *profile);
