#include "mc_keystore.h"

#include <string.h>

#define MC_KEYSTORE_MAGIC 0x314B434Du /* "MCK1" little-endian */
#define MC_KEYSTORE_HEADER_LEN 4u
#define MC_KEYSTORE_SCHEMA_VERSION 1

void mc_keystore_init(mc_keystore_t *ks)
{
    memset(ks, 0, sizeof(*ks));
}

int mc_keystore_count(const mc_keystore_t *ks)
{
    int n = 0;
    for (int i = 0; i < MC_KEYSTORE_MAX_KEYS; i++) {
        if (ks->slots[i].in_use) {
            n++;
        }
    }
    return n;
}

static int find_pubkey(const mc_keystore_t *ks, const uint8_t pubkey[MC_CRYPTO_PUBKEY_BYTES])
{
    for (int i = 0; i < MC_KEYSTORE_MAX_KEYS; i++) {
        if (ks->slots[i].in_use &&
            memcmp(ks->slots[i].pubkey, pubkey, MC_CRYPTO_PUBKEY_BYTES) == 0) {
            return i;
        }
    }
    return -1;
}

static void set_label(mc_key_slot_t *slot, const char *label)
{
    if (label == NULL) {
        slot->label[0] = '\0';
        return;
    }
    strncpy(slot->label, label, MC_KEY_LABEL_MAX - 1);
    slot->label[MC_KEY_LABEL_MAX - 1] = '\0';
}

int mc_keystore_add(mc_keystore_t *ks, const uint8_t pubkey[MC_CRYPTO_PUBKEY_BYTES], const char *label)
{
    int existing = find_pubkey(ks, pubkey);
    if (existing >= 0) {
        set_label(&ks->slots[existing], label);
        return existing;
    }

    for (int i = 0; i < MC_KEYSTORE_MAX_KEYS; i++) {
        if (!ks->slots[i].in_use) {
            ks->slots[i].in_use = true;
            memcpy(ks->slots[i].pubkey, pubkey, MC_CRYPTO_PUBKEY_BYTES);
            set_label(&ks->slots[i], label);
            return i;
        }
    }
    return -1; /* full */
}

bool mc_keystore_remove(mc_keystore_t *ks, int slot)
{
    if (slot < 0 || slot >= MC_KEYSTORE_MAX_KEYS || !ks->slots[slot].in_use) {
        return false;
    }
    memset(&ks->slots[slot], 0, sizeof(ks->slots[slot]));
    return true;
}

void mc_keystore_wipe(mc_keystore_t *ks)
{
    memset(ks, 0, sizeof(*ks));
}

int mc_keystore_find_verifying(const mc_keystore_t *ks,
                               const uint8_t *msg, size_t msg_len,
                               const uint8_t sig[MC_CRYPTO_SIG_BYTES])
{
    for (int i = 0; i < MC_KEYSTORE_MAX_KEYS; i++) {
        if (ks->slots[i].in_use &&
            mc_crypto_verify(ks->slots[i].pubkey, msg, msg_len, sig)) {
            return i;
        }
    }
    return -1;
}

bool mc_keystore_get(const mc_keystore_t *ks, int slot,
                     uint8_t out_pubkey[MC_CRYPTO_PUBKEY_BYTES], char *out_label, size_t label_cap)
{
    if (slot < 0 || slot >= MC_KEYSTORE_MAX_KEYS || !ks->slots[slot].in_use) {
        return false;
    }
    if (out_pubkey != NULL) {
        memcpy(out_pubkey, ks->slots[slot].pubkey, MC_CRYPTO_PUBKEY_BYTES);
    }
    if (out_label != NULL && label_cap > 0) {
        strncpy(out_label, ks->slots[slot].label, label_cap - 1);
        out_label[label_cap - 1] = '\0';
    }
    return true;
}

mc_keystore_result_t mc_keystore_serialize(const mc_keystore_t *ks, uint8_t *buf, size_t buf_len, size_t *out_len)
{
    size_t total = MC_KEYSTORE_HEADER_LEN + sizeof(uint16_t) + sizeof(*ks);
    if (buf_len < total) {
        return MC_KEYSTORE_ERR_BUFFER_TOO_SMALL;
    }

    uint32_t magic = MC_KEYSTORE_MAGIC;
    uint16_t version = MC_KEYSTORE_SCHEMA_VERSION;
    memcpy(buf, &magic, sizeof(magic));
    memcpy(buf + MC_KEYSTORE_HEADER_LEN, &version, sizeof(version));
    memcpy(buf + MC_KEYSTORE_HEADER_LEN + sizeof(version), ks, sizeof(*ks));

    *out_len = total;
    return MC_KEYSTORE_OK;
}

mc_keystore_result_t mc_keystore_deserialize(const uint8_t *buf, size_t len, mc_keystore_t *out)
{
    if (len < MC_KEYSTORE_HEADER_LEN + sizeof(uint16_t)) {
        return MC_KEYSTORE_ERR_CORRUPT;
    }

    uint32_t magic;
    memcpy(&magic, buf, sizeof(magic));
    if (magic != MC_KEYSTORE_MAGIC) {
        return MC_KEYSTORE_ERR_CORRUPT;
    }

    uint16_t version;
    memcpy(&version, buf + MC_KEYSTORE_HEADER_LEN, sizeof(version));
    if (version > MC_KEYSTORE_SCHEMA_VERSION) {
        return MC_KEYSTORE_ERR_FUTURE_VERSION;
    }

    if (len != MC_KEYSTORE_HEADER_LEN + sizeof(version) + sizeof(*out)) {
        return MC_KEYSTORE_ERR_CORRUPT;
    }
    memcpy(out, buf + MC_KEYSTORE_HEADER_LEN + sizeof(version), sizeof(*out));
    return MC_KEYSTORE_OK;
}
