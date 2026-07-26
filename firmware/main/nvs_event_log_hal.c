#include "nvs_event_log_hal.h"

#include <stdio.h>

#include "nvs.h"
#include "nvs_flash.h"

#define MC_EVTLOG_PARTITION "evtlog"
#define MC_EVTLOG_NAMESPACE "mc_evtlog"
#define MC_EVTLOG_LAST_SEQ_KEY "last_seq"

static nvs_handle_t s_handle;
static bool s_open = false;

esp_err_t nvs_event_log_hal_init(void)
{
    esp_err_t err = nvs_flash_init_partition(MC_EVTLOG_PARTITION);
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        esp_err_t erase_err = nvs_flash_erase_partition(MC_EVTLOG_PARTITION);
        if (erase_err != ESP_OK) {
            return erase_err;
        }
        err = nvs_flash_init_partition(MC_EVTLOG_PARTITION);
    }
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_open_from_partition(MC_EVTLOG_PARTITION, MC_EVTLOG_NAMESPACE, NVS_READWRITE, &s_handle);
    if (err == ESP_OK) {
        s_open = true;
    }
    return err;
}

esp_err_t nvs_event_log_hal_erase(void)
{
    if (s_open) {
        nvs_close(s_handle);
        s_open = false;
    }
    return nvs_flash_erase_partition(MC_EVTLOG_PARTITION);
}

/* Fixed-width hex slot key ("s0000".."s03ff" for MC_EVENT_LOG_SLOT_COUNT=
 * 1024) — well under NVS's 15-char key limit. */
static void slot_key(uint16_t slot, char out[8])
{
    snprintf(out, 8, "s%04x", slot);
}

static bool hal_read_slot(uint16_t slot, mc_event_record_t *out, void *ctx)
{
    (void)ctx;
    if (!s_open) {
        return false;
    }
    char key[8];
    slot_key(slot, key);
    size_t len = sizeof(*out);
    esp_err_t err = nvs_get_blob(s_handle, key, out, &len);
    return err == ESP_OK && len == sizeof(*out);
}

static bool hal_write_slot(uint16_t slot, const mc_event_record_t *rec, void *ctx)
{
    (void)ctx;
    if (!s_open) {
        return false;
    }
    char key[8];
    slot_key(slot, key);
    if (nvs_set_blob(s_handle, key, rec, sizeof(*rec)) != ESP_OK) {
        return false;
    }
    return nvs_commit(s_handle) == ESP_OK;
}

static uint32_t hal_get_last_seq(void *ctx)
{
    (void)ctx;
    if (!s_open) {
        return 0;
    }
    uint32_t seq = 0;
    /* Absent key (fresh/erased partition) leaves seq at 0 -- an empty log,
     * same as every other "nothing persisted yet" fallback in this
     * codebase. */
    nvs_get_u32(s_handle, MC_EVTLOG_LAST_SEQ_KEY, &seq);
    return seq;
}

static bool hal_set_last_seq(uint32_t seq, void *ctx)
{
    (void)ctx;
    if (!s_open) {
        return false;
    }
    if (nvs_set_u32(s_handle, MC_EVTLOG_LAST_SEQ_KEY, seq) != ESP_OK) {
        return false;
    }
    return nvs_commit(s_handle) == ESP_OK;
}

mc_event_log_hal_t nvs_event_log_hal_get(void)
{
    mc_event_log_hal_t hal = {
        .read_slot = hal_read_slot,
        .write_slot = hal_write_slot,
        .get_last_seq = hal_get_last_seq,
        .set_last_seq = hal_set_last_seq,
        .ctx = NULL,
    };
    return hal;
}
