#pragma once

#include "esp_err.h"
#include "mc_lock.h"

/* Persists {lock config, locked_flag} in its own NVS namespace, separate
 * from mc_config and mc_keystore — same rationale as the keystore: the
 * cheat-code hash is security-sensitive and must never ride the exportable
 * JSON config backup (AGENTS.md #4 doctrine, applied to mc_lock — see
 * mc_lock.h). Call after nvs_flash_init(). */
esp_err_t nvs_lock_hal_init(void);

/* Loads the persisted lock config + locked_flag. If nothing is stored yet,
 * fills `out_config` with defaults (immobilizer disabled) and
 * `out_locked_flag` with false, returning ESP_OK — a fresh/factory-reset
 * device boots unlocked and unconfigured. */
esp_err_t nvs_lock_load(mc_lock_config_t *out_config, bool *out_locked_flag);

/* Serializes and writes {lock->config, lock->locked_flag}. Lock/cheat-code
 * changes persist immediately (mc_lock.h) rather than through the debounced
 * mc_persist scheduler used for config/keystore — they're rare and
 * security-relevant enough that the extra flash write is worth it. */
esp_err_t nvs_lock_save(const mc_lock_t *lock);
