#include "nvs_calib_hal.h"

#include "nvs.h"

#define MC_CALIB_NAMESPACE "mc_calib"
#define MC_CALIB_KEY "cal"

static nvs_handle_t s_handle;
static bool s_open = false;

esp_err_t nvs_calib_hal_init(void)
{
    esp_err_t err = nvs_open(MC_CALIB_NAMESPACE, NVS_READWRITE, &s_handle);
    if (err == ESP_OK) {
        s_open = true;
    }
    return err;
}

esp_err_t nvs_calib_load(mc_diag_calib_t *out)
{
    mc_diag_calib_default(out);
    if (!s_open) {
        return ESP_FAIL;
    }

    size_t required = 0;
    esp_err_t err = nvs_get_blob(s_handle, MC_CALIB_KEY, NULL, &required);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK; /* never bench-calibrated: nominal defaults */
    }
    if (err != ESP_OK) {
        return err;
    }

    uint8_t buf[128];
    if (required > sizeof(buf)) {
        return ESP_ERR_INVALID_SIZE;
    }
    err = nvs_get_blob(s_handle, MC_CALIB_KEY, buf, &required);
    if (err != ESP_OK) {
        return err;
    }
    if (mc_diag_calib_deserialize(buf, required, out) != MC_DIAG_STORE_OK) {
        /* Corrupt calibration blob: fail safe to nominal defaults rather
         * than trusting garbage gain/offset values. */
        mc_diag_calib_default(out);
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
}

esp_err_t nvs_calib_save(const mc_diag_calib_t *calib)
{
    if (!s_open) {
        return ESP_FAIL;
    }
    uint8_t buf[128];
    size_t len = 0;
    if (mc_diag_calib_serialize(calib, buf, sizeof(buf), &len) != MC_DIAG_STORE_OK) {
        return ESP_ERR_INVALID_SIZE;
    }
    esp_err_t err = nvs_set_blob(s_handle, MC_CALIB_KEY, buf, len);
    if (err != ESP_OK) {
        return err;
    }
    return nvs_commit(s_handle);
}
