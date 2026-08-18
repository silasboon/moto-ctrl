#include "power_hal.h"

#include "esp_err.h"
#include "esp_log.h"
#include "esp_pm.h"

#include "ble_app.h"

static const char *TAG = "mc_power";

static esp_pm_lock_handle_t s_no_light_sleep;
static esp_pm_lock_handle_t s_cpu_freq_max;
static bool s_locks_held = true; /* power_hal_init() leaves both acquired */

void power_hal_init(void)
{
    esp_pm_config_t pm = {
        .max_freq_mhz = 160,
        /* 40MHz rather than the 10MHz XTAL/N floor: the BLE controller's
         * low-power clock is the main crystal, and dropping too far starves
         * it. 40MHz is the conventional safe minimum with BLE active. */
        .min_freq_mhz = 40,
        .light_sleep_enable = true,
    };
    esp_err_t err = esp_pm_configure(&pm);
    if (err != ESP_OK) {
        /* Not fatal: without PM the board simply behaves as it did before
         * any of this existed — full-rate loop, no sleep, higher draw. That
         * is strictly the safer failure direction, so log and carry on
         * rather than aborting a boot that is otherwise fine (ride-safe
         * failure: never let an error path take the bike down). */
        ESP_LOGE(TAG, "esp_pm_configure failed (%s); running without power management",
                 esp_err_to_name(err));
    }

    if (esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "mc_awake", &s_no_light_sleep) != ESP_OK ||
        esp_pm_lock_create(ESP_PM_CPU_FREQ_MAX, 0, "mc_cpu", &s_cpu_freq_max) != ESP_OK) {
        ESP_LOGE(TAG, "esp_pm_lock_create failed; staying awake permanently");
        s_no_light_sleep = NULL;
        s_cpu_freq_max = NULL;
        return;
    }

    /* Held from the outset: boot, output restore and the factory-reset
     * window all run before any policy decision is made. */
    esp_pm_lock_acquire(s_no_light_sleep);
    esp_pm_lock_acquire(s_cpu_freq_max);
    s_locks_held = true;
}

void power_hal_apply(const mc_power_profile_t *profile)
{
    ble_app_set_adv_interval(profile->adv == MC_POWER_ADV_SLOW);

    if (s_no_light_sleep == NULL || s_cpu_freq_max == NULL) {
        return; /* lock creation failed at init — permanently awake */
    }

    bool want_held = !profile->light_sleep_allowed;
    if (want_held == s_locks_held) {
        return;
    }
    if (want_held) {
        esp_pm_lock_acquire(s_no_light_sleep);
        esp_pm_lock_acquire(s_cpu_freq_max);
    } else {
        esp_pm_lock_release(s_no_light_sleep);
        esp_pm_lock_release(s_cpu_freq_max);
    }
    s_locks_held = want_held;
    ESP_LOGI(TAG, "%s (tick %ums, adv %s)", want_held ? "awake" : "low-power",
             (unsigned)profile->tick_interval_ms,
             (profile->adv == MC_POWER_ADV_SLOW) ? "slow" : "fast");
}
