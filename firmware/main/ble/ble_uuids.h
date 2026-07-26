#pragma once

#include "host/ble_uuid.h"

/*
 * MOTO-CTRL 128-bit GATT UUIDs.
 *
 * Base: 5a4f00XX-9b1e-4f8a-9c2d-1a2b3c4d5e6f, where XX distinguishes each
 * service/characteristic. NimBLE stores UUID bytes little-endian (reversed
 * from the string form); only array index 12 (the XX byte) varies, so the
 * macro fixes every other byte. These values are documented for third-party
 * clients in docs/PROTOCOL.md — keep the two in sync.
 */
#define MC_UUID128(xx) \
    BLE_UUID128_INIT(0x6f, 0x5e, 0x4d, 0x3c, 0x2b, 0x1a, 0x2d, 0x9c, \
                     0x8a, 0x4f, 0x1e, 0x9b, (xx), 0x00, 0x4f, 0x5a)

/* Status service (0x10): status characteristic (0x11) — read/write/notify. */
#define MC_UUID_SVC_STATUS   MC_UUID128(0x10)
#define MC_UUID_CHR_STATUS   MC_UUID128(0x11)

/* Control service (0x20): auth char (0x21) + command char (0x22). */
#define MC_UUID_SVC_CONTROL  MC_UUID128(0x20)
#define MC_UUID_CHR_AUTH     MC_UUID128(0x21)
#define MC_UUID_CHR_COMMAND  MC_UUID128(0x22)

/* Config service (0x30): config characteristic (0x31) — chunked JSON. */
#define MC_UUID_SVC_CONFIG   MC_UUID128(0x30)
#define MC_UUID_CHR_CONFIG   MC_UUID128(0x31)

/* OTA service (0x40). */
#define MC_UUID_SVC_OTA      MC_UUID128(0x40)
#define MC_UUID_CHR_OTA      MC_UUID128(0x41)
