#pragma once

#include "esp_err.h"
#include "mc_diag.h"

/* Persists board calibration (mc_diag_calib_t) in its own NVS namespace,
 * separate from mc_config, mc_keystore, and mc_lock — these constants
 * describe THIS physical board's analog sense lines, not an owner's
 * settings, so they deliberately do NOT ride the exportable JSON config
 * backup (applying board A's calibration to board B would silently
 * misreport board B's real current/voltage) and are NOT wiped by factory
 * reset / ownership transfer (see firmware/main/factory_reset.c, which does
 * not erase this namespace — the next owner keeps the same physical board
 * and the same correct calibration). Call after nvs_flash_init(). */
esp_err_t nvs_calib_hal_init(void);

/* Loads the persisted calibration. If nothing is stored yet (a fresh board,
 * never bench-calibrated), fills `out` with mc_diag_calib_default()'s
 * nominal/uncalibrated values and returns ESP_OK. */
esp_err_t nvs_calib_load(mc_diag_calib_t *out);

esp_err_t nvs_calib_save(const mc_diag_calib_t *calib);
