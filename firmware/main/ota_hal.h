#pragma once

/*
 * ota_hal — mc_ota_hal_t backed by real esp_ota_* flash writes, run on a
 * dedicated low-priority FreeRTOS task so an in-progress OTA
 * transfer can never starve app_task's safety-critical 10ms tick loop or
 * the watchdog feed — see ota_hal.c's header comment.
 */

#include "mc_ota.h"

/* Starts the dedicated OTA flash-write task. Call once at boot. */
void ota_hal_init(void);

mc_ota_hal_t ota_hal_get(void);
