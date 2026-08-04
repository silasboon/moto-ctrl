#pragma once

#include <stdbool.h>
#include <stdint.h>

/*
 * factory_reset — the physical BOOT-hold factory reset (AGENTS.md #3:
 * "wipes bonds + config after a distinct LED pattern confirmation").
 *
 * Why this is a post-boot window rather than a check at power-on.
 *
 * The original design sampled BOARD_BOOT_PIN once, early in app_main(), and
 * gave up unless it was already down. That could not be triggered by hand:
 *   - Holding BOOT *through* reset is the ESP32-S3's strapping for UART
 *     download mode (BOOT is GPIO0). The ROM waits for esptool and the
 *     application firmware never runs at all, so the check never executed.
 *   - Releasing reset first and then pressing BOOT left a window of a few
 *     hundred milliseconds — bootloader, three HAL inits and nvs_flash_init()
 *     — to get a finger down before the single sample had already passed.
 * The net effect was a board whose only real recovery path was a laptop and a
 * UART adapter, which is not good enough for a rider whose phone has died.
 *
 * The window can't simply become a blocking wait at boot either: that would
 * delay every ordinary boot, and AGENTS.md #1 requires outputs restored from
 * persisted state in under 250ms after a watchdog or brownout reboot.
 *
 * So arming happens at boot (free) and watching happens on the existing ~10ms
 * app tick, for a bounded few seconds only. After that this disarms
 * permanently and every later tick is a single bool test — a stuck or shorted
 * button cannot wipe a bike's config mid-ride, which is exactly what a naive
 * always-on runtime watcher would allow.
 */

/** A press must BEGIN within this long after boot. Long enough to reach for a
 * button deliberately, short enough that the bike cannot be moving yet. */
#define FACTORY_RESET_WINDOW_MS 5000u
/** ...and then be held this long. Deliberately tedious: this wipes the bike. */
#define FACTORY_RESET_HOLD_MS 10000u

/* Configures BOARD_BOOT_PIN and arms the window. Call once from app_main(),
 * after output_hal_gpio_init() (the confirmation pattern drives the output
 * rails) and after nvs_flash_init() succeeds. Adds no delay to boot. */
void factory_reset_init(void);

/* Call every app tick with the same millisecond clock the rest of the loop
 * uses. Cheap and non-blocking except on the confirmed path.
 *
 * Releasing early cancels; a further press still counts while the window is
 * open. Once the window closes with no press in progress, this is inert until
 * the next boot.
 *
 * On confirmation it blocks: this board's LEDs passively mirror the 12 output
 * rails (hardware/PINOUT.md — there is no independently-controllable status
 * LED), so the "distinct LED pattern" AGENTS.md asks for is all 12 outputs
 * pulsing together, fast, well outside anything a lighting function produces.
 * It drives them through the raw output HAL, bypassing mc_output, and feeds
 * the task watchdog across the pattern. Then it erases the mc_cfg, mc_keys
 * and mc_lock namespaces plus the event log, and reboots — restarting rather
 * than continuing is what guarantees the rest of the firmware never keeps
 * serving the config and keystore that were just wiped. Does not return in
 * that case. */
void factory_reset_tick(uint32_t now_ms);
