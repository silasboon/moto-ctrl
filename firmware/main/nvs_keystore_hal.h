#pragma once

#include "esp_err.h"
#include "mc_keystore.h"

/* Persists the enrolled-key store in its own NVS namespace, separate from
 * the config blob (mc_keystore.h: keys are never part of the exportable
 * config). Call after nvs_flash_init(). */
esp_err_t nvs_keystore_hal_init(void);

/* Loads the keystore. If nothing is stored yet, fills `out` with an empty
 * keystore and returns ESP_OK. */
esp_err_t nvs_keystore_load(mc_keystore_t *out);

/* Serializes and writes the keystore. */
esp_err_t nvs_keystore_save(const mc_keystore_t *ks);
