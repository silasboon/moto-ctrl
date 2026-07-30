#include "nvs_keystore_hal.h"

#include <stdlib.h>

#include "nvs.h"

#define MC_KS_NAMESPACE "mc_keys"
#define MC_KS_KEY "ks"

/* Serialisation staging buffer. Heap, never a local: nvs_keystore_load()
 * runs on the main task inside app_main(), where a 1KB local competed with
 * mc_config_load()'s buffer for CONFIG_ESP_MAIN_TASK_STACK_SIZE bytes of
 * stack — see the comment in mc_config.c's mc_config_load(). */
#define MC_KS_BUF_LEN 1024

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

    if (required > MC_KS_BUF_LEN) {
        return ESP_ERR_INVALID_SIZE;
    }
    uint8_t *buf = malloc(MC_KS_BUF_LEN);
    if (buf == NULL) {
        return ESP_ERR_NO_MEM;
    }
    err = nvs_get_blob(s_handle, MC_KS_KEY, buf, &required);
    if (err != ESP_OK) {
        free(buf);
        return err;
    }
    if (mc_keystore_deserialize(buf, required, out) != MC_KEYSTORE_OK) {
        /* Corrupt keystore: fail safe to empty rather than trusting garbage.
         * The rider can re-enroll (or use the button cheat-code fallback). */
        free(buf);
        mc_keystore_init(out);
        return ESP_ERR_INVALID_STATE;
    }
    free(buf);
    return ESP_OK;
}

esp_err_t nvs_keystore_save(const mc_keystore_t *ks)
{
    if (!s_open) {
        return ESP_FAIL;
    }
    uint8_t *buf = malloc(MC_KS_BUF_LEN);
    if (buf == NULL) {
        return ESP_ERR_NO_MEM;
    }
    size_t len = 0;
    if (mc_keystore_serialize(ks, buf, MC_KS_BUF_LEN, &len) != MC_KEYSTORE_OK) {
        free(buf);
        return ESP_ERR_INVALID_SIZE;
    }
    esp_err_t err = nvs_set_blob(s_handle, MC_KS_KEY, buf, len);
    free(buf);
    if (err != ESP_OK) {
        return err;
    }
    return nvs_commit(s_handle);
}
