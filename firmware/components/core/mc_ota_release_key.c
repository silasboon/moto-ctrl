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
 * This is a PLACEHOLDER. The corresponding private key was generated once
 * (via mc_crypto_keypair(), the same host-only primitive mc_lock's tests
 * use to stand in for the phone) purely to produce a mathematically valid
 * public key to compile against — the private key was never written to
 * disk or committed anywhere and cannot be recovered. That is intentional:
 * a placeholder key with no known private key is strictly safer than a
 * placeholder with one, since nothing can accidentally sign a "real"
 * looking image against it. Before this project ships a real OTA release:
 *
 *   1. The maintainer runs `tools/sign-firmware.py --gen-key <path>`
 *      ONCE, stores the resulting private key file OUTSIDE this repo
 *      (password manager / encrypted volume, never committed, never
 *      placed in a hosted CI secrets store — see tools/README.md), and
 *      pastes the tool's printed public-key C array over the constant
 *      below.
 *   2. Every release .bin is then signed locally with that private key via
 *      `tools/sign-firmware.py --input ... --key <path> --output
 *      release.mcota` before publishing.
 *
 * firmware/sim never uses this key at all — it embeds its own clearly
 * separate, fixed TEST keypair (firmware/sim/src/main.c) so ctest/itest/app
 * tests can synthesize validly-signed test images without this placeholder
 * (or a future real key) ever being involved.
 */
const uint8_t MC_OTA_RELEASE_PUBKEY[MC_CRYPTO_PUBKEY_BYTES] = {
    0xc7, 0x89, 0x65, 0x4d, 0x9e, 0xde, 0x8f, 0x2a,
    0x87, 0x45, 0xf2, 0x27, 0xe0, 0x8b, 0x18, 0xdd,
    0x7b, 0xfe, 0x1a, 0x53, 0xcf, 0x7b, 0x3e, 0xb9,
    0x79, 0x9c, 0x38, 0xb8, 0xdc, 0x20, 0x63, 0xfb,
};
