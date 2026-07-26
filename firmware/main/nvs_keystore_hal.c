#include "nvs_keystore_hal.h"

#include "nvs.h"

#define MC_KS_NAMESPACE "mc_keys"
#define MC_KS_KEY "ks"

static nvs_handle_t s_handle;
static bool s_open = false;

esp_err_t nvs_keystore_hal_init(void)
{
    esp_err_t err = nvs_open(MC_KS_NAMESPACE, NVS_READWRITE, &s_handle);
    if (err == ESP_OK) {
        s_open = true;
    }
    return err;
}

esp_err_t nvs_keystore_load(mc_keystore_t *out)
{
    mc_keystore_init(out);
    if (!s_open) {
        return ESP_FAIL;
    }

    size_t required = 0;
    esp_err_t err = nvs_get_blob(s_handle, MC_KS_KEY, NULL, &required);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK; /* empty keystore is fine */
    }
    if (err != ESP_OK) {
        return err;
    }

    uint8_t buf[1024];
    if (required > sizeof(buf)) {
        return ESP_ERR_INVALID_SIZE;
    }
    err = nvs_get_blob(s_handle, MC_KS_KEY, buf, &required);
    if (err != ESP_OK) {
        return err;
    }
    if (mc_keystore_deserialize(buf, required, out) != MC_KEYSTORE_OK) {
        /* Corrupt keystore: fail safe to empty rather than trusting garbage.
         * The rider can re-enroll (or use the button cheat-code fallback). */
        mc_keystore_init(out);
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
}

esp_err_t nvs_keystore_save(const mc_keystore_t *ks)
{
    if (!s_open) {
        return ESP_FAIL;
    }
    uint8_t buf[1024];
    size_t len = 0;
    if (mc_keystore_serialize(ks, buf, sizeof(buf), &len) != MC_KEYSTORE_OK) {
        return ESP_ERR_INVALID_SIZE;
    }
    esp_err_t err = nvs_set_blob(s_handle, MC_KS_KEY, buf, len);
    if (err != ESP_OK) {
        return err;
    }
    return nvs_commit(s_handle);
}
