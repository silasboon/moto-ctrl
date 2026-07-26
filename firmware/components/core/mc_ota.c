#include "mc_ota.h"

#include <string.h>

#include "mc_protocol.h" /* MC_OTA_MAX_IMAGE_SIZE */

void mc_ota_init(mc_ota_t *ota, mc_ota_hal_t hal, const uint8_t release_pubkey[MC_CRYPTO_PUBKEY_BYTES])
{
    memset(ota, 0, sizeof(*ota));
    ota->hal = hal;
    ota->release_pubkey = release_pubkey;
    ota->state = MC_OTA_IDLE;
}

mc_ota_result_t mc_ota_begin(mc_ota_t *ota, uint32_t image_size,
                             const uint8_t sha512[MC_CRYPTO_HASH_BYTES],
                             const uint8_t signature[MC_CRYPTO_SIG_BYTES],
                             bool safe_to_flash)
{
    if (ota->state != MC_OTA_IDLE) {
        return MC_OTA_ERR_BAD_STATE;
    }
    if (image_size == 0 || image_size > MC_OTA_MAX_IMAGE_SIZE) {
        return MC_OTA_ERR_BAD_SIZE;
    }
    if (!safe_to_flash) {
        return MC_OTA_ERR_UNSAFE_STATE;
    }
    if (!mc_crypto_verify(ota->release_pubkey, sha512, MC_CRYPTO_HASH_BYTES, signature)) {
        return MC_OTA_ERR_BAD_SIGNATURE;
    }
    if (ota->hal.flash_begin != NULL && !ota->hal.flash_begin(image_size, ota->hal.ctx)) {
        return MC_OTA_ERR_FLASH;
    }

    memcpy(ota->declared_sha512, sha512, MC_CRYPTO_HASH_BYTES);
    ota->image_size = image_size;
    ota->bytes_received = 0;
    mc_crypto_hash_sha512_init(&ota->running_hash);
    ota->state = MC_OTA_RECEIVING;
    return MC_OTA_OK;
}

mc_ota_result_t mc_ota_chunk(mc_ota_t *ota, uint32_t offset, const uint8_t *data, size_t len)
{
    if (ota->state != MC_OTA_RECEIVING) {
        return MC_OTA_ERR_BAD_STATE;
    }
    if (offset != ota->bytes_received) {
        ota->state = MC_OTA_ERROR;
        return MC_OTA_ERR_OUT_OF_ORDER_CHUNK;
    }
    if ((uint64_t)ota->bytes_received + (uint64_t)len > (uint64_t)ota->image_size) {
        ota->state = MC_OTA_ERROR;
        return MC_OTA_ERR_OVERRUN;
    }
    if (ota->hal.flash_write != NULL && !ota->hal.flash_write(offset, data, len, ota->hal.ctx)) {
        ota->state = MC_OTA_ERROR;
        return MC_OTA_ERR_FLASH;
    }

    mc_crypto_hash_sha512_update(&ota->running_hash, data, len);
    ota->bytes_received += (uint32_t)len;
    return MC_OTA_OK;
}

mc_ota_result_t mc_ota_commit(mc_ota_t *ota)
{
    if (ota->state != MC_OTA_RECEIVING || ota->bytes_received != ota->image_size) {
        return MC_OTA_ERR_BAD_STATE;
    }

    uint8_t computed[MC_CRYPTO_HASH_BYTES];
    /* Finalizing consumes the running hash context; whether commit succeeds
     * or fails, this transfer attempt is over (abort() is required either
     * way before a new begin()), so there's no need to preserve it. */
    mc_crypto_hash_sha512_final(&ota->running_hash, computed);
    if (memcmp(computed, ota->declared_sha512, MC_CRYPTO_HASH_BYTES) != 0) {
        ota->state = MC_OTA_ERROR;
        return MC_OTA_ERR_HASH_MISMATCH;
    }
    if (ota->hal.flash_finalize != NULL && !ota->hal.flash_finalize(ota->hal.ctx)) {
        ota->state = MC_OTA_ERROR;
        return MC_OTA_ERR_FLASH;
    }

    ota->state = MC_OTA_COMMITTED;
    return MC_OTA_OK;
}

void mc_ota_abort(mc_ota_t *ota)
{
    if (ota->hal.flash_abort != NULL) {
        ota->hal.flash_abort(ota->hal.ctx);
    }
    ota->state = MC_OTA_IDLE;
    ota->image_size = 0;
    ota->bytes_received = 0;
}

mc_ota_result_t mc_ota_reboot(mc_ota_t *ota, bool safe_to_flash)
{
    if (ota->state != MC_OTA_COMMITTED) {
        return MC_OTA_ERR_BAD_STATE;
    }
    if (!safe_to_flash) {
        return MC_OTA_ERR_UNSAFE_STATE; /* COMMITTED is left intact; caller can retry later */
    }
    if (ota->hal.reboot != NULL) {
        ota->hal.reboot(ota->hal.ctx); /* never returns on real hardware */
    }
    return MC_OTA_OK;
}
