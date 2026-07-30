#include "nvs_lock_hal.h"

#include <stdlib.h>

#include "nvs.h"

#define MC_LOCK_NAMESPACE "mc_lock"
#define MC_LOCK_KEY "lk"

/* Heap, never a local — see mc_config.c's mc_config_load(). nvs_lock_save()
 * in particular is reachable from the NimBLE host task (persist_lock_cb) as
 * well as from app_main()/app_task, so it must not assume a roomy stack. */
#define MC_LOCK_BUF_LEN 512

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

    if (required > MC_LOCK_BUF_LEN) {
        return ESP_ERR_INVALID_SIZE;
    }
    uint8_t *buf = malloc(MC_LOCK_BUF_LEN);
    if (buf == NULL) {
        return ESP_ERR_NO_MEM;
    }
    err = nvs_get_blob(s_handle, MC_LOCK_KEY, buf, &required);
    if (err != ESP_OK) {
        free(buf);
        return err;
    }
    if (mc_lock_deserialize(buf, required, out_config, out_locked_flag) != MC_LOCK_STORE_OK) {
        /* Corrupt lock blob: fail safe to disabled/unlocked rather than
         * trusting garbage — never guess into an immobilized boot. */
        free(buf);
        mc_lock_config_default(out_config);
        *out_locked_flag = false;
        return ESP_ERR_INVALID_STATE;
    }
    free(buf);
    return ESP_OK;
}

esp_err_t nvs_lock_save(const mc_lock_t *lock)
{
    if (!s_open) {
        return ESP_FAIL;
    }
    uint8_t *buf = malloc(MC_LOCK_BUF_LEN);
    if (buf == NULL) {
        return ESP_ERR_NO_MEM;
    }
    size_t len = 0;
    if (mc_lock_serialize(lock, buf, MC_LOCK_BUF_LEN, &len) != MC_LOCK_STORE_OK) {
        free(buf);
        return ESP_ERR_INVALID_SIZE;
    }
    esp_err_t err = nvs_set_blob(s_handle, MC_LOCK_KEY, buf, len);
    free(buf);
    if (err != ESP_OK) {
        return err;
    }
    return nvs_commit(s_handle);
}
