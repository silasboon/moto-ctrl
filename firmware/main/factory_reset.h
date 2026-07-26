#pragma once

#include <stdbool.h>

/*
 * factory_reset — the physical BOOT-hold-at-power-on factory reset
 * (AGENTS.md #3: "Physical factory reset: hold BOOT during power-on for
 * 10s -> wipes bonds + config after a distinct LED pattern confirmation").
 *
 * Call factory_reset_check() once, early in app_main(): after
 * output_hal_gpio_init() (needed for the confirmation pattern) and after
 * nvs_flash_init() succeeds (needed to erase NVS), but BEFORE
 * mc_config_load() / nvs_keystore_load() / nvs_lock_load() run, so a
 * confirmed reset takes effect before anything reads the blobs about to be
 * wiped.
 */

/* Returns immediately (false) if BOARD_BOOT_PIN is not held at the moment
 * this is called — the overwhelmingly common case, so a normal boot pays
 * no delay. If it IS held, blocks polling the pin for up to 10s: released
 * early cancels (returns false); held the full 10s confirms the reset.
 *
 * On confirmation: this board's LEDs passively mirror the 12 output rails
 * (hardware/PINOUT.md — there is no independently-controllable status
 * LED), so the "distinct LED pattern confirmation" AGENTS.md requires is
 * given by pulsing all 12 outputs together in a fast on/off pattern well
 * outside anything a normal lighting function would ever produce — via
 * the raw output HAL, bypassing mc_output/config entirely (nothing is
 * loaded yet at this point in boot). Then erases the mc_cfg, mc_keys, and
 * mc_lock NVS namespaces — keystore, output/input config, and the
 * immobilizer's cheat-code + lock config all go back to a fresh, unlocked,
 * re-enrollable factory state. Returns true if a reset was performed. */
bool factory_reset_check(void);
