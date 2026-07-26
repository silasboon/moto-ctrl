#pragma once

/*
 * nvs_event_log_hal — mc_event_log_hal_t backed by a dedicated NVS
 * partition ("evtlog", firmware/partitions.csv), separate from the main
 * "nvs" partition config/keystore/lock/calib share — isolates the event
 * log's erase-cycle wear and gives it real headroom.
 */

#include "esp_err.h"

#include "mc_event_log.h"

/* Initializes (nvs_flash_init_partition, formatting on first boot/version
 * mismatch like the default partition) and opens the "evtlog" partition.
 * Call once at boot, after nvs_flash_init() for the default partition. */
esp_err_t nvs_event_log_hal_init(void);

/* Erases the entire "evtlog" partition (factory reset / ownership
 * transfer's full-wipe doctrine — docs/PROTOCOL.md §15). Closes any open
 * handle first; call nvs_event_log_hal_init() again afterward to resume
 * using it. */
esp_err_t nvs_event_log_hal_erase(void);

mc_event_log_hal_t nvs_event_log_hal_get(void);
