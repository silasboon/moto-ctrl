#pragma once

#include "esp_err.h"
#include "mc_config.h"

/* Opens the NVS namespace used for the persisted mc_config_t blob. Call
 * after nvs_flash_init() succeeds. */
esp_err_t nvs_config_hal_init(void);

mc_config_store_hal_t nvs_config_hal_get(void);
