#pragma once

/*
 * mc_protocol — the wire constants shared by every transport (NimBLE GATT
 * on the device, the WebSocket sim server, and any third-party client).
 * The full human-readable spec is docs/PROTOCOL.md; this header is the
 * authoritative source those constants are documented from.
 *
 * A "channel" identifies a logical stream that maps 1:1 to a BLE GATT
 * characteristic on the device and to the first byte of each WebSocket
 * binary message in the simulator. Within a channel, the first payload
 * byte is an opcode. Device->client opcodes have bit 0x80 set by
 * convention; client->device opcodes do not.
 */

#include <string.h>

#include "mc_types.h"

typedef enum {
    MC_CH_STATUS = 0,
    MC_CH_AUTH = 1,
    MC_CH_COMMAND = 2,
    MC_CH_CONFIG = 3,
    MC_CH_OTA = 4,
    MC_CH_COUNT,
} mc_channel_t;

/* --- STATUS channel --- */
#define MC_OP_STATUS_GET 0x01     /* client->device: request a snapshot */
#define MC_OP_STATUS 0x81         /* device->client: [16-byte mc_status wire] */

/* --- AUTH channel --- */
#define MC_OP_AUTH_BEGIN 0x01     /* client->device: request a challenge */
#define MC_OP_AUTH_CHALLENGE 0x81 /* device->client: [nonce:32] */
#define MC_OP_AUTH_RESPONSE 0x02  /* client->device: [sig:64] over the auth message */
#define MC_OP_AUTH_RESULT 0x82    /* device->client: [result:1][slot:1] */
#define MC_OP_ENROLL 0x10         /* client->device: [pubkey:32][label:utf8...] */
#define MC_OP_ENROLL_RESULT 0x90  /* device->client: [result:1][slot:1] */
#define MC_OP_KEY_LIST 0x11       /* client->device: (authed) list enrolled keys */
#define MC_OP_KEY_LIST_RESULT 0x91/* device->client: [count:1]{[slot:1][len:1][label]} */
#define MC_OP_KEY_REVOKE 0x12     /* client->device: (authed) [slot:1] */
#define MC_OP_KEY_REVOKE_RESULT 0x92 /* device->client: [result:1][slot:1] */

/* --- COMMAND channel (all require an authenticated session) ---
 *
 * The lock/immobilizer ops (docs/PROTOCOL.md §11) live on this
 * channel rather than a new one — they're commands like SET_OUTPUT, just
 * routed to mc_lock instead of mc_output. Simple ops share the existing
 * COMMAND_RESULT [req_opcode:1][result:1] reply; the two ops that return a
 * payload (LOCK_GET_CONFIG, CHEATCODE_TEST) get their own response opcode.
 */
#define MC_OP_SET_OUTPUT 0x01     /* client->device: [channel:1][on:1] */
#define MC_OP_COMMAND_RESULT 0x81 /* device->client: [req_opcode:1][result:1] */

#define MC_OP_LOCK 0x02                /* client->device: (no payload) lock now if safe */
#define MC_OP_UNLOCK 0x03              /* client->device: (no payload) explicit phone-as-key unlock */
#define MC_OP_LOCK_GET_CONFIG 0x04     /* client->device: (no payload) */
/* device->client: [immobilizer_enabled:1][methods_mask:1][ignition_switch_input:1 (0xFF=none)]
 *                 [auto_lock_grace_ms:u16le][cheatcode_window_ms:u16le][cheatcode_set:1][cheatcode_len:1]
 * Never includes the cheat-code salt/hash — the device is the only party
 * that ever needs them. */
#define MC_OP_LOCK_CONFIG 0x84
/* client->device: [immobilizer_enabled:1][methods_mask:1][ignition_switch_input:1]
 *                 [auto_lock_grace_ms:u16le][cheatcode_window_ms:u16le] */
#define MC_OP_LOCK_SET_CONFIG 0x05
#define MC_OP_CHEATCODE_SET 0x06       /* client->device: [len:1][buttons:len] (len 4-10, buttons 0-7) */
#define MC_OP_CHEATCODE_CLEAR 0x07     /* client->device: (no payload); REJECTED while immobilizer is enabled */
#define MC_OP_CHEATCODE_TEST 0x08      /* client->device: [len:1][buttons:len] — practice only, no side effects */
#define MC_OP_CHEATCODE_TEST_RESULT 0x88 /* device->client: [result:1][match:1] */
#define MC_OP_TRANSFER_OWNERSHIP 0x09  /* client->device: (no payload) wipe keys + lock config */

/* Method bits for MC_OP_LOCK_(GET|SET)_CONFIG's methods_mask. The button
 * cheat-code is not a bit here — it is always active whenever
 * immobilizer_enabled is set (AGENTS.md #3: always-available fallback). */
#define MC_LOCK_METHOD_PHONE (1u << 0)
#define MC_LOCK_METHOD_IGNITION_SWITCH (1u << 1)

/* --- Diagnostics ops (docs/PROTOCOL.md §12). Same channel, same
 * authed gate, same "dedicated response opcode only where there's a
 * payload, MC_OP_COMMAND_RESULT otherwise" convention as the lock ops. Every
 * dedicated response below leads with [result:1] (MC_RESULT_REJECTED with a
 * zeroed remainder if the device has no mc_diag attached), matching
 * MC_OP_CHEATCODE_TEST_RESULT's [result:1][...] shape rather than
 * MC_OP_LOCK_CONFIG's bare-payload one, since diag responses need to signal
 * "not available" without a separate error path.
 */
#define MC_OP_DIAG_GET 0x0A         /* client->device: (no payload) */
/* device->client: [result:1] then MC_OUTPUT_COUNT * current_ma:u16le, then
 * MC_OUTPUT_COUNT * fault:1 (mc_diag_fault_t) */
#define MC_OP_DIAG_RESULT 0x8A
#define MC_OP_DIAG_GET_CONFIG 0x0B  /* client->device: (no payload) */
/* device->client: [result:1] then MC_OUTPUT_COUNT *
 * [open_load_ma:u16le][overcurrent_ma:u16le], then
 * [lv_cutoff_mv:u16le][lv_cutoff_hysteresis_mv:u16le]
 * [engine_run_mv:u16le][engine_run_hysteresis_mv:u16le] */
#define MC_OP_DIAG_CONFIG 0x8B
/* client->device: same shape as MC_OP_DIAG_CONFIG's payload, minus the
 * leading result byte (MC_OUTPUT_COUNT*4 + 8 bytes) */
#define MC_OP_DIAG_SET_CONFIG 0x0C
#define MC_OP_DIAG_GET_CALIB 0x0D   /* client->device: (no payload) */
/* device->client: [result:1][is_gain:f32le][is_offset_mv:i16le]
 * [kilis:f32le][vbat_gain:f32le][vbat_offset_mv:i16le] */
#define MC_OP_DIAG_CALIB 0x8D
/* client->device: [is_gain:f32le][is_offset_mv:i16le][kilis:f32le]
 * [vbat_gain:f32le][vbat_offset_mv:i16le] (16 bytes, no leading result byte) */
#define MC_OP_DIAG_SET_CALIB 0x0E
#define MC_OP_DIAG_LEARN 0x0F       /* client->device: [channel:1] (0xFF = every energized channel) */

/* --- Flashers/PWM (docs/PROTOCOL.md §13). One dedicated opcode — plain
 * turn-signal control already works over MC_OP_SET_OUTPUT (mutual
 * exclusion + auto-cancel are embedded in mc_output_set() itself), and
 * dimming duty / auto-cancel timing / brake-pulse timing / the brake-switch
 * input assignment all ride the CONFIG channel's existing JSON, same as
 * starter_interlock_input always has — no wire change needed for those. */
#define MC_OP_HAZARD_PRESS 0x10     /* client->device: (no payload). Replies
                                      * MC_OP_COMMAND_RESULT — REJECTED if
                                      * neither a TURN_L nor TURN_R channel
                                      * is configured. */

/* --- Event log (docs/PROTOCOL.md §15). Security/safety-relevant
 * events only (lock transitions, key enroll/revoke/transfer, factory
 * reset, cheat-code lockout, OTA begin/success/failure, low-voltage
 * cutoff enter/exit) — not routine output toggles or diagnostics faults,
 * which are already visible live via STATUS/DIAG. Unrelated to the
 * simulator-only SIM_OP_GET_LOG/SIM_OP_LOG_ENTRY debug trace
 * (firmware/sim/src/sim_protocol.h) — that channel never reaches real
 * hardware or this app; this one does. */
#define MC_OP_EVENT_LOG_GET 0x11    /* client->device: [since_seq:u32le] (0 = oldest available) */
/* device->client, one or more frames: [index:u16le][total:u16le][count:1]
 * then count * 12-byte mc_event_record_t entries, oldest-first. An empty
 * log is a single frame with index=0,total=0,count=0 (same empty-result
 * shape MC_OP_CONFIG_CHUNK uses). REJECTED (via MC_OP_COMMAND_RESULT
 * instead) if the device has no event log attached. */
#define MC_OP_EVENT_LOG_CHUNK 0x91
#define MC_PROTOCOL_EVENT_LOG_CHUNK_RECORDS 10 /* records per EVENT_LOG_CHUNK frame */

/* --- CONFIG channel (all require an authenticated session) --- */
#define MC_OP_CONFIG_READ 0x01        /* client->device: request full config as chunks */
#define MC_OP_CONFIG_CHUNK 0x81       /* device->client: [offset:u16le][total:u16le][bytes] */
#define MC_OP_CONFIG_WRITE_BEGIN 0x02 /* client->device: [total:u16le] */
#define MC_OP_CONFIG_WRITE_CHUNK 0x03 /* client->device: [offset:u16le][bytes] */
#define MC_OP_CONFIG_WRITE_COMMIT 0x04/* client->device: apply staged config */
#define MC_OP_CONFIG_WRITE_RESULT 0x82/* device->client: [result:1] */

/* --- OTA channel (docs/PROTOCOL.md §10). Requires an authenticated
 * session (unlike the pre-Phase-8 stub, which answered every frame
 * unconditionally). Images must be signed by the project's release key
 * (mc_ota_release_key.c) — this is in addition to, not instead of, the
 * session auth that gates the channel itself. BEGIN/REBOOT are rejected
 * unless the device is in a safe state (!engine_running &&
 * !lv_cutoff_active), mirroring how starter/low-voltage protection already
 * gate other risky operations. */
#define MC_OP_OTA_BEGIN  0x01   /* client->device: [image_size:u32le][sha512:64][signature:64] (132 bytes) */
#define MC_OP_OTA_CHUNK  0x02   /* client->device: [offset:u32le][data:bytes]; offset must equal bytes received so far */
#define MC_OP_OTA_COMMIT 0x03   /* client->device: (no payload) — verify hash, finalize into COMMITTED (does not reboot) */
#define MC_OP_OTA_ABORT  0x04   /* client->device: (no payload) — idempotent cancel/cleanup from any state */
#define MC_OP_OTA_REBOOT 0x05   /* client->device: (no payload) — apply a COMMITTED image now; re-checks safe-state */
#define MC_OP_OTA_STATUS 0x06   /* client->device: (no payload) — poll progress / resume after reconnect */
/* device->client: [result:1][state:1][bytes_received:u32le][image_size:u32le]
 * (10 bytes) — leads with a result byte like MC_OP_DIAG_RESULT etc., so a
 * device with no mc_ota attached (app->ota == NULL) can still reply with a
 * well-formed, zeroed frame carrying MC_RESULT_REJECTED. */
#define MC_OP_OTA_STATUS_RESULT 0x86
#define MC_OP_OTA_RESULT 0x8F         /* device->client: [result:1] — reply for BEGIN/CHUNK/COMMIT/ABORT/REBOOT */

#define MC_OTA_MAX_IMAGE_SIZE 0x180000 /* must track partitions.csv's ota_0/ota_1 size */

/* Generic result codes carried in *_RESULT payloads. */
typedef enum {
    MC_RESULT_OK = 0,
    MC_RESULT_UNAUTHENTICATED = 1, /* session has not completed challenge-response */
    MC_RESULT_BAD_REQUEST = 2,     /* malformed / truncated payload */
    MC_RESULT_REJECTED = 3,        /* semantically refused (e.g. starter over BLE) */
    MC_RESULT_ENROLL_DENIED = 4,   /* enrollment not permitted in current state */
    MC_RESULT_KEYSTORE_FULL = 5,
    MC_RESULT_NOT_FOUND = 6,       /* referenced key slot not present */
    MC_RESULT_NOT_IMPLEMENTED = 7, /* unused (OTA is implemented); kept for wire stability */
    MC_RESULT_INTERNAL = 8,
} mc_result_t;

/* Data bytes per CONFIG_CHUNK. A client reading config must negotiate an
 * ATT MTU large enough to carry one chunk frame (this many data bytes plus
 * the channel byte, opcode, and 4-byte chunk header). The WebSocket sim
 * has no such limit. */
#define MC_PROTOCOL_CONFIG_CHUNK 128

/* Domain-separation prefix for the signed auth message. The message a
 * client signs is exactly these bytes (no trailing NUL) followed by the
 * 32-byte challenge nonce. Documented in docs/PROTOCOL.md. */
#define MC_AUTH_CONTEXT "moto-ctrl-auth-v1"
#define MC_AUTH_CONTEXT_LEN 17

/* --- little-endian helpers shared by transports/clients --- */
static inline void mc_put_u16le(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
}
static inline uint16_t mc_get_u16le(const uint8_t *p)
{
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}
static inline void mc_put_i16le(uint8_t *p, int16_t v)
{
    mc_put_u16le(p, (uint16_t)v);
}
static inline int16_t mc_get_i16le(const uint8_t *p)
{
    return (int16_t)mc_get_u16le(p);
}
/* OTA's image_size/chunk-offset/bytes_received fields need 32
 * bits (a firmware image can exceed 65535 bytes). */
static inline void mc_put_u32le(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}
static inline uint32_t mc_get_u32le(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
/* IEEE754 binary32, little-endian — standard on every target this project
 * runs on (ESP32-S3, x86/ARM hosts) and readable from JS via
 * DataView.getFloat32(offset, true). Used by the diagnostics
 * calibration wire format (mc_diag_calib_t's float fields). */
static inline void mc_put_f32le(uint8_t *p, float v)
{
    uint32_t bits;
    memcpy(&bits, &v, sizeof(bits));
    mc_put_u16le(p, (uint16_t)(bits & 0xFFFFu));
    mc_put_u16le(p + 2, (uint16_t)(bits >> 16));
}
static inline float mc_get_f32le(const uint8_t *p)
{
    uint32_t bits = (uint32_t)mc_get_u16le(p) | ((uint32_t)mc_get_u16le(p + 2) << 16);
    float v;
    memcpy(&v, &bits, sizeof(v));
    return v;
}
