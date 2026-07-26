#include "watchdog.h"

#include "esp_task_wdt.h"

#define MC_WDT_TIMEOUT_MS 5000

void mc_watchdog_init(void)
{
    esp_task_wdt_config_t config = {
        .timeout_ms = MC_WDT_TIMEOUT_MS,
        .idle_core_mask = 0,
        .trigger_panic = true,
    };
    esp_task_wdt_init(&config);
}

void mc_watchdog_feed(void)
{
    esp_task_wdt_reset();
}
