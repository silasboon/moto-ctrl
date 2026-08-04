#include "mc_status.h"

#include <string.h>

static void put_u16le(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
}

static uint16_t get_u16le(const uint8_t *p)
{
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static void put_u32le(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}

static uint32_t get_u32le(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

void mc_status_init(mc_status_t *out)
{
    memset(out, 0, sizeof(*out));
    out->fw_major = MC_FW_VERSION_MAJOR;
    out->fw_minor = MC_FW_VERSION_MINOR;
    out->fw_patch = MC_FW_VERSION_PATCH;
    out->lock_state = MC_LOCK_UNKNOWN;
}

bool mc_status_serialize(const mc_status_t *st, uint8_t *buf, size_t buf_len)
{
    if (buf_len < MC_STATUS_WIRE_LEN) {
        return false;
    }
    buf[0] = st->fw_major;
    buf[1] = st->fw_minor;
    buf[2] = st->fw_patch;
    buf[3] = st->lock_state;
    put_u32le(&buf[4], st->uptime_ms);
    put_u16le(&buf[8], st->battery_mv);
    put_u16le(&buf[10], st->output_state_mask);
    put_u16le(&buf[12], st->output_fault_mask);
    buf[14] = (uint8_t)st->rssi_dbm;
    buf[15] = (uint8_t)((st->cheatcode_backoff ? 0x01 : 0) |
                        (st->lv_cutoff_active ? 0x02 : 0) |
                        (st->hazard_active ? 0x04 : 0)); /* bits 0-2; bits 3-7 reserved */
    return true;
}

bool mc_status_deserialize(const uint8_t *buf, size_t len, mc_status_t *out)
{
    if (len < MC_STATUS_WIRE_LEN) {
        return false;
    }
    out->fw_major = buf[0];
    out->fw_minor = buf[1];
    out->fw_patch = buf[2];
    out->lock_state = buf[3];
    out->uptime_ms = get_u32le(&buf[4]);
    out->battery_mv = get_u16le(&buf[8]);
    out->output_state_mask = get_u16le(&buf[10]);
    out->output_fault_mask = get_u16le(&buf[12]);
    out->rssi_dbm = (int8_t)buf[14];
    out->cheatcode_backoff = (buf[15] & 0x01) != 0;
    out->lv_cutoff_active = (buf[15] & 0x02) != 0;
    out->hazard_active = (buf[15] & 0x04) != 0;
    return true;
}
