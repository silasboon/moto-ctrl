#include "nvs_lock_hal.h"

#include "nvs.h"

#define MC_LOCK_NAMESPACE "mc_lock"
#define MC_LOCK_KEY "lk"

static nvs_handle_t s_handle;
static bool s_open = false;

esp_err_t nvs_lock_hal_init(void)
{
    esp_err_t err = nvs_open(MC_LOCK_NAMESPACE, NVS_READWRITE, &s_handle);
    if (err == ESP_OK) {
        s_open = true;
    }
    return err;
}

esp_err_t nvs_lock_load(mc_lock_config_t *out_config, bool *out_locked_flag)
{
    mc_lock_config_default(out_config);
    *out_locked_flag = false;
    if (!s_open) {
        return ESP_FAIL;
    }

    size_t required = 0;
    esp_err_t err = nvs_get_blob(s_handle, MC_LOCK_KEY, NULL, &required);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK; /* fresh device: disabled immobilizer, unlocked */
    }
    if (err != ESP_OK) {
        return err;
    }

    uint8_t buf[512];
    if (required > sizeof(buf)) {
        return ESP_ERR_INVALID_SIZE;
    }
    err = nvs_get_blob(s_handle, MC_LOCK_KEY, buf, &required);
    if (err != ESP_OK) {
        return err;
    }
    if (mc_lock_deserialize(buf, required, out_config, out_locked_flag) != MC_LOCK_STORE_OK) {
        /* Corrupt lock blob: fail safe to disabled/unlocked rather than
         * trusting garbage — never guess into an immobilized boot. */
        mc_lock_config_default(out_config);
        *out_locked_flag = false;
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
}

esp_err_t nvs_lock_save(const mc_lock_t *lock)
{
    if (!s_open) {
        return ESP_FAIL;
    }
    uint8_t buf[512];
    size_t len = 0;
    if (mc_lock_serialize(lock, buf, sizeof(buf), &len) != MC_LOCK_STORE_OK) {
        return ESP_ERR_INVALID_SIZE;
    }
    esp_err_t err = nvs_set_blob(s_handle, MC_LOCK_KEY, buf, len);
    if (err != ESP_OK) {
        return err;
    }
    return nvs_commit(s_handle);
}
