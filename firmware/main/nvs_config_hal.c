#include "nvs_config_hal.h"

#include "nvs.h"

#define MC_NVS_NAMESPACE "mc_cfg"
#define MC_NVS_KEY "cfg"

static nvs_handle_t s_handle;
static bool s_open = false;

esp_err_t nvs_config_hal_init(void)
{
    esp_err_t err = nvs_open(MC_NVS_NAMESPACE, NVS_READWRITE, &s_handle);
    if (err == ESP_OK) {
        s_open = true;
    }
    return err;
}

static mc_config_result_t hal_load(uint8_t *buf, size_t buf_len, size_t *out_len, void *ctx)
{
    (void)ctx;
    if (!s_open) {
        return MC_CONFIG_ERR_STORE_READ;
    }

    size_t required = 0;
    esp_err_t err = nvs_get_blob(s_handle, MC_NVS_KEY, NULL, &required);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return MC_CONFIG_ERR_NOT_FOUND;
    }
    if (err != ESP_OK) {
        return MC_CONFIG_ERR_STORE_READ;
    }
    if (required > buf_len) {
        return MC_CONFIG_ERR_BUFFER_TOO_SMALL;
    }

    err = nvs_get_blob(s_handle, MC_NVS_KEY, buf, &required);
    if (err != ESP_OK) {
        return MC_CONFIG_ERR_STORE_READ;
    }
    *out_len = required;
    return MC_CONFIG_OK;
}

static mc_config_result_t hal_save(const uint8_t *buf, size_t len, void *ctx)
{
    (void)ctx;
    if (!s_open) {
        return MC_CONFIG_ERR_STORE_WRITE;
    }

    if (nvs_set_blob(s_handle, MC_NVS_KEY, buf, len) != ESP_OK) {
        return MC_CONFIG_ERR_STORE_WRITE;
    }
    if (nvs_commit(s_handle) != ESP_OK) {
        return MC_CONFIG_ERR_STORE_WRITE;
    }
    return MC_CONFIG_OK;
}

mc_config_store_hal_t nvs_config_hal_get(void)
{
    mc_config_store_hal_t hal = { .load = hal_load, .save = hal_save, .ctx = NULL };
    return hal;
}
