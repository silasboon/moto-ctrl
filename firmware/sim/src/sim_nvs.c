#include "sim_nvs.h"

#include <string.h>

void sim_nvs_init(sim_nvs_t *nvs)
{
    memset(nvs, 0, sizeof(*nvs));
}

mc_config_result_t sim_nvs_config_load(uint8_t *buf, size_t buf_len, size_t *out_len, void *ctx)
{
    sim_nvs_t *nvs = (sim_nvs_t *)ctx;
    if (!nvs->config.present) {
        return MC_CONFIG_ERR_NOT_FOUND;
    }
    if (nvs->config.len > buf_len) {
        return MC_CONFIG_ERR_BUFFER_TOO_SMALL;
    }
    memcpy(buf, nvs->config.buf, nvs->config.len);
    *out_len = nvs->config.len;
    return MC_CONFIG_OK;
}

mc_config_result_t sim_nvs_config_save(const uint8_t *buf, size_t len, void *ctx)
{
    sim_nvs_t *nvs = (sim_nvs_t *)ctx;
    if (len > SIM_NVS_BUF_MAX) {
        return MC_CONFIG_ERR_STORE_WRITE;
    }
    memcpy(nvs->config.buf, buf, len);
    nvs->config.len = len;
    nvs->config.present = true;
    return MC_CONFIG_OK;
}

bool sim_nvs_keystore_load(sim_nvs_t *nvs, mc_keystore_t *out)
{
    if (!nvs->keystore.present) {
        return false;
    }
    return mc_keystore_deserialize(nvs->keystore.buf, nvs->keystore.len, out) == MC_KEYSTORE_OK;
}

bool sim_nvs_keystore_save(sim_nvs_t *nvs, const mc_keystore_t *ks)
{
    size_t out_len = 0;
    mc_keystore_result_t r = mc_keystore_serialize(ks, nvs->keystore.buf, SIM_NVS_BUF_MAX, &out_len);
    if (r != MC_KEYSTORE_OK) {
        return false;
    }
    nvs->keystore.len = out_len;
    nvs->keystore.present = true;
    return true;
}

bool sim_nvs_lock_load(sim_nvs_t *nvs, mc_lock_config_t *out_config, bool *out_locked_flag)
{
    mc_lock_config_default(out_config);
    *out_locked_flag = false;
    if (!nvs->lock.present) {
        return true; /* fresh device: disabled immobilizer, unlocked */
    }
    if (mc_lock_deserialize(nvs->lock.buf, nvs->lock.len, out_config, out_locked_flag) != MC_LOCK_STORE_OK) {
        mc_lock_config_default(out_config);
        *out_locked_flag = false;
        return false;
    }
    return true;
}

bool sim_nvs_lock_save(sim_nvs_t *nvs, const mc_lock_t *lock)
{
    size_t out_len = 0;
    mc_lock_store_result_t r = mc_lock_serialize(lock, nvs->lock.buf, SIM_NVS_BUF_MAX, &out_len);
    if (r != MC_LOCK_STORE_OK) {
        return false;
    }
    nvs->lock.len = out_len;
    nvs->lock.present = true;
    return true;
}

bool sim_nvs_calib_load(sim_nvs_t *nvs, mc_diag_calib_t *out)
{
    /* Identity default -- see sim_nvs.h for why this differs deliberately
     * from mc_diag_calib_default(). */
    memset(out, 0, sizeof(*out));
    out->is_gain = 1.0f;
    out->is_offset_mv = 0;
    out->kilis = 2000.0f;
    out->vbat_gain = 1.0f;
    out->vbat_offset_mv = 0;

    if (!nvs->calib.present) {
        return true;
    }
    mc_diag_calib_t loaded;
    if (mc_diag_calib_deserialize(nvs->calib.buf, nvs->calib.len, &loaded) != MC_DIAG_STORE_OK) {
        return false; /* corrupt: caller keeps the identity fallback already written to *out */
    }
    *out = loaded;
    return true;
}

bool sim_nvs_calib_save(sim_nvs_t *nvs, const mc_diag_calib_t *calib)
{
    size_t out_len = 0;
    mc_diag_store_result_t r = mc_diag_calib_serialize(calib, nvs->calib.buf, SIM_NVS_BUF_MAX, &out_len);
    if (r != MC_DIAG_STORE_OK) {
        return false;
    }
    nvs->calib.len = out_len;
    nvs->calib.present = true;
    return true;
}

bool sim_nvs_evtlog_read_slot(uint16_t slot, mc_event_record_t *out, void *ctx)
{
    sim_nvs_t *nvs = (sim_nvs_t *)ctx;
    if (slot >= MC_EVENT_LOG_SLOT_COUNT || !nvs->evtlog.record_present[slot]) {
        return false;
    }
    *out = nvs->evtlog.records[slot];
    return true;
}

bool sim_nvs_evtlog_write_slot(uint16_t slot, const mc_event_record_t *rec, void *ctx)
{
    sim_nvs_t *nvs = (sim_nvs_t *)ctx;
    if (slot >= MC_EVENT_LOG_SLOT_COUNT) {
        return false;
    }
    nvs->evtlog.records[slot] = *rec;
    nvs->evtlog.record_present[slot] = true;
    return true;
}

uint32_t sim_nvs_evtlog_get_last_seq(void *ctx)
{
    sim_nvs_t *nvs = (sim_nvs_t *)ctx;
    return nvs->evtlog.last_seq;
}

bool sim_nvs_evtlog_set_last_seq(uint32_t seq, void *ctx)
{
    sim_nvs_t *nvs = (sim_nvs_t *)ctx;
    nvs->evtlog.last_seq = seq;
    return true;
}

void sim_nvs_evtlog_erase(sim_nvs_t *nvs)
{
    memset(&nvs->evtlog, 0, sizeof(nvs->evtlog));
}

void sim_nvs_corrupt(sim_nvs_slot_t *slot)
{
    if (!slot->present || slot->len == 0) {
        return;
    }
    /* Flip a handful of bytes spread through the blob — enough to break the
     * magic/length check or the payload without being a no-op on a lucky
     * XOR. Mirrors the bit-rot-style corruption firmware's unit tests use
     * (test_config.c: buf[0] ^= 0xFF). */
    for (size_t i = 0; i < slot->len; i += (slot->len / 4) + 1) {
        slot->buf[i] ^= 0xFF;
    }
}

void sim_nvs_erase(sim_nvs_slot_t *slot)
{
    slot->present = false;
    slot->len = 0;
}
