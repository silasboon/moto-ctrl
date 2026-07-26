#pragma once

/*
 * mc_ota — firmware OTA update state machine (docs/PROTOCOL.md
 * §10). Portable C99, no ESP-IDF dependency — flash writes go through an
 * injected HAL (mc_ota_hal_t), exactly like mc_output/mc_diag separate
 * portable engine logic from the on-target GPIO/ADC HAL.
 *
 * State machine: IDLE -> RECEIVING -> COMMITTED -> (mc_ota_reboot() calls
 * hal.reboot(), which never returns on real hardware). ERROR is reachable
 * from RECEIVING/COMMITTED on any failure and is cleared only by
 * mc_ota_abort(). There is no separate VERIFYING/COMMITTING state —
 * signature verification happens synchronously inside mc_ota_begin() (before
 * any chunk is accepted) and hash-finalization happens synchronously inside
 * mc_ota_commit(), same shape as mc_session's config_commit().
 *
 * Trust model (AGENTS.md's "cryptographically sound" bar, applied here the
 * same way it's applied to phone-as-key): an image must be signed by the
 * project's release key (mc_ota_release_key.c, embedded at build time) in
 * ADDITION to arriving over an already-authenticated BLE session — either
 * alone is not sufficient. The signature covers a SHA-512 digest of the
 * image, not the raw bytes: this device has no PSRAM to buffer a
 * multi-hundred-KB image before hashing it, so mc_ota_begin() verifies the
 * signature over the 64-byte digest instantly (before accepting any chunk
 * bytes), then mc_ota_chunk()/mc_ota_commit() stream the image through
 * mc_crypto's incremental SHA-512 and confirm the transferred bytes match
 * what was signed.
 *
 * Safe-state gating: mc_ota_begin() and mc_ota_reboot() both take a
 * `safe_to_flash` bool computed by the caller (mc_session.c) from
 * !engine_running && !lv_cutoff_active (mc_output_engine_t /
 * mc_output_lv_cutoff_active) — mc_ota.c never includes mc_output.h/
 * mc_diag.h itself, mirroring how mc_lock_inputs_t is assembled by the
 * platform rather than mc_lock reaching into other modules' globals. This
 * extends AGENTS.md's existing doctrine (starter protection gates on
 * engine_running, low-voltage cutoff gates on battery voltage) to OTA:
 * flashing while riding or while the battery is critically low is refused.
 * mc_ota_reboot() re-checks at reboot time (not just at begin time) because
 * a COMMITTED image can sit safely in the inactive partition indefinitely —
 * nothing is lost if the bike started moving between commit and reboot; the
 * client just retries OTA_REBOOT later.
 *
 * Ride-safe note (AGENTS.md #1): the currently-running firmware image is
 * untouched throughout the entire transfer/flash-to-inactive-partition
 * process (that's what the A/B partition scheme is for) — only
 * mc_ota_reboot() ever switches which image boots. On real hardware, flash
 * writes must run on a dedicated task at lower priority than the
 * safety-critical tick loop (firmware/main/ota_hal.c), not inline on the
 * BLE callback or app_task — a design requirement this portable module
 * can't itself enforce, see docs/TESTING.md / HARDWARE_TESTING.md.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "mc_crypto.h"

typedef enum {
    MC_OTA_IDLE = 0,
    MC_OTA_RECEIVING,
    MC_OTA_COMMITTED, /* verified + flash-finalized; awaiting explicit mc_ota_reboot() */
    MC_OTA_ERROR,      /* last attempt failed; mc_ota_abort() to clear before retrying */
} mc_ota_state_t;

typedef enum {
    MC_OTA_OK = 0,
    MC_OTA_ERR_BAD_STATE,
    MC_OTA_ERR_BAD_SIZE,
    MC_OTA_ERR_BAD_SIGNATURE,
    MC_OTA_ERR_OUT_OF_ORDER_CHUNK,
    MC_OTA_ERR_OVERRUN,
    MC_OTA_ERR_HASH_MISMATCH,
    MC_OTA_ERR_FLASH,
    MC_OTA_ERR_UNSAFE_STATE,
} mc_ota_result_t;

/* Flash-write HAL, injected so this module stays portable. On real hardware
 * (firmware/main/ota_hal.c) these wrap esp_ota_begin/write/end/
 * set_boot_partition and run on a dedicated FreeRTOS task; the simulator
 * (firmware/sim/src/main.c) backs them with an in-memory buffer. */
typedef struct {
    bool (*flash_begin)(uint32_t image_size, void *ctx);
    bool (*flash_write)(uint32_t offset, const uint8_t *data, size_t len, void *ctx);
    bool (*flash_finalize)(void *ctx); /* esp_ota_end() + esp_ota_set_boot_partition() on target */
    void (*flash_abort)(void *ctx);    /* best-effort cleanup; safe to call from IDLE too */
    void (*reboot)(void *ctx);         /* esp_restart(); never returns on real hardware */
    void *ctx;
} mc_ota_hal_t;

typedef struct {
    mc_ota_hal_t hal;
    const uint8_t *release_pubkey; /* MC_CRYPTO_PUBKEY_BYTES; not owned, must outlive this struct */

    mc_ota_state_t state;
    uint32_t image_size;
    uint32_t bytes_received;
    uint8_t declared_sha512[MC_CRYPTO_HASH_BYTES];
    mc_crypto_hash_ctx_t running_hash;
} mc_ota_t;

void mc_ota_init(mc_ota_t *ota, mc_ota_hal_t hal, const uint8_t release_pubkey[MC_CRYPTO_PUBKEY_BYTES]);

/* Verifies the signature over `sha512` (not the image itself — see header
 * comment), checks image_size against MC_OTA_MAX_IMAGE_SIZE, requires
 * state==IDLE and safe_to_flash, then calls hal.flash_begin() and resets
 * the running hash. On any rejection, no flash write occurs and state stays
 * IDLE (or unchanged on a bad-state call). */
mc_ota_result_t mc_ota_begin(mc_ota_t *ota, uint32_t image_size,
                             const uint8_t sha512[MC_CRYPTO_HASH_BYTES],
                             const uint8_t signature[MC_CRYPTO_SIG_BYTES],
                             bool safe_to_flash);

/* Requires state==RECEIVING and offset == bytes_received exactly (chunks
 * must arrive in order — this module streams straight to flash via the HAL
 * and folds each chunk into the running hash, it never buffers the image).
 * Any violation moves to ERROR; the client must mc_ota_abort() and restart. */
mc_ota_result_t mc_ota_chunk(mc_ota_t *ota, uint32_t offset, const uint8_t *data, size_t len);

/* Requires state==RECEIVING and bytes_received == image_size. Finalizes the
 * running hash and compares it against the declared digest from
 * mc_ota_begin() (catches transport corruption/truncation — the signature
 * check already happened at begin time). On match, calls
 * hal.flash_finalize() and moves to COMMITTED. Does NOT reboot. */
mc_ota_result_t mc_ota_commit(mc_ota_t *ota);

/* Idempotent cancel from any state (including IDLE/ERROR — always safe to
 * call). Calls hal.flash_abort() and returns to IDLE. */
void mc_ota_abort(mc_ota_t *ota);

/* Requires state==COMMITTED and safe_to_flash (re-checked here — see header
 * comment on why this is a second, later check). On success calls
 * hal.reboot() and never returns on real hardware; on rejection COMMITTED
 * is left intact so the client can retry later. */
mc_ota_result_t mc_ota_reboot(mc_ota_t *ota, bool safe_to_flash);

static inline mc_ota_state_t mc_ota_get_state(const mc_ota_t *ota)
{
    return ota->state;
}
static inline uint32_t mc_ota_get_bytes_received(const mc_ota_t *ota)
{
    return ota->bytes_received;
}
static inline uint32_t mc_ota_get_image_size(const mc_ota_t *ota)
{
    return ota->image_size;
}
