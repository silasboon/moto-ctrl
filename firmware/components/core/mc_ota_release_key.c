#include <stdint.h>

#include "mc_crypto.h"

/*
 * MC_OTA_RELEASE_PUBKEY — the public half of the project's OTA release
 * signing key (docs/PROTOCOL.md §10, tools/sign-firmware.py). Real
 * firmware refuses to accept any OTA image whose signature doesn't verify
 * against this key (mc_ota_begin(), mc_ota.c) — in addition to, not
 * instead of, the BLE-session authentication that already gates the OTA
 * channel.
 *
 * This is the maintainer's real OTA release public key, generated via
 * `tools/sign-firmware.py gen-key`. The
 * matching private key lives outside this repo; every release .bin is
 * signed locally with it via `tools/sign-firmware.py sign --input ...
 * --key <path> --output release.mcota` before publishing.
 *
 * firmware/sim never uses this key at all — it embeds its own clearly
 * separate, fixed TEST keypair (firmware/sim/src/main.c) so ctest/itest/app
 * tests can synthesize validly-signed test images without this key ever
 * being involved.
 */
const uint8_t MC_OTA_RELEASE_PUBKEY[MC_CRYPTO_PUBKEY_BYTES] = {
    0x19, 0x59, 0xef, 0xf5, 0x14, 0x9c, 0x85, 0x56,
    0xe6, 0x51, 0x1a, 0x05, 0x74, 0x74, 0x03, 0x06,
    0xb8, 0xdb, 0x1c, 0x7f, 0xe8, 0x10, 0x2b, 0xa9,
    0xff, 0x96, 0x55, 0x33, 0x38, 0x4d, 0x61, 0x97,
};
