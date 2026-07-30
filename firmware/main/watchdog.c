#include "watchdog.h"

#include "esp_err.h"
#include "esp_task_wdt.h"
#include "sdkconfig.h"

#define MC_WDT_TIMEOUT_MS 5000

/* Idle tasks stay subscribed, matching CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_*.
 * A starved idle task means some higher-priority task is spinning and never
 * yielding, which on this board means outputs stop being ticked — exactly
 * the condition the watchdog exists to reboot us out of (AGENTS.md #1). */
#if CONFIG_FREERTOS_UNICORE
#define MC_WDT_IDLE_CORE_MASK 0x1
#else
#define MC_WDT_IDLE_CORE_MASK 0x3
#endif

void mc_watchdog_init(void)
{
    esp_task_wdt_config_t config = {
        .timeout_ms = MC_WDT_TIMEOUT_MS,
        .idle_core_mask = MC_WDT_IDLE_CORE_MASK,
        .trigger_panic = true,
    };
    /* CONFIG_ESP_TASK_WDT_INIT=y means the startup code already brought the
     * TWDT up from Kconfig values, so esp_task_wdt_init() here returns
     * ESP_ERR_INVALID_STATE and silently does nothing — this used to leave
     * MC_WDT_TIMEOUT_MS/trigger_panic unapplied. Reconfigure instead so the
     * values above are the ones actually in force either way. */
    esp_err_t err = esp_task_wdt_init(&config);
    if (err == ESP_ERR_INVALID_STATE) {
        err = esp_task_wdt_reconfigure(&config);
    }
    ESP_ERROR_CHECK_WITHOUT_ABORT(err);
}

void mc_watchdog_feed(void)
{
    esp_task_wdt_reset();
}
