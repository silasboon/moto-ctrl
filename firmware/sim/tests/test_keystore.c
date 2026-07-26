#include "mc_keystore.h"

#include <assert.h>
#include <string.h>

/* A message + valid signature pair for a freshly generated key. */
typedef struct {
    uint8_t pk[MC_CRYPTO_PUBKEY_BYTES];
    uint8_t sk[MC_CRYPTO_SECRETKEY_BYTES];
    uint8_t msg[40];
    uint8_t sig[MC_CRYPTO_SIG_BYTES];
} test_key_t;

static void make_key(test_key_t *k, uint8_t seed)
{
    mc_crypto_keypair(k->pk, k->sk);
    memset(k->msg, seed, sizeof(k->msg));
    mc_crypto_sign(k->sig, k->msg, sizeof(k->msg), k->sk);
}

static void test_empty(void)
{
    mc_keystore_t ks;
    mc_keystore_init(&ks);
    assert(mc_keystore_is_empty(&ks));
    assert(mc_keystore_count(&ks) == 0);
}

static void test_add_find_remove(void)
{
    mc_keystore_t ks;
    mc_keystore_init(&ks);

    test_key_t k;
    make_key(&k, 0x01);

    int slot = mc_keystore_add(&ks, k.pk, "My Phone");
    assert(slot == 0);
    assert(!mc_keystore_is_empty(&ks));
    assert(mc_keystore_count(&ks) == 1);

    assert(mc_keystore_find_verifying(&ks, k.msg, sizeof(k.msg), k.sig) == 0);

    /* Wrong signature does not verify. */
    uint8_t bad_sig[MC_CRYPTO_SIG_BYTES];
    memcpy(bad_sig, k.sig, sizeof(bad_sig));
    bad_sig[0] ^= 0x01;
    assert(mc_keystore_find_verifying(&ks, k.msg, sizeof(k.msg), bad_sig) == -1);

    assert(mc_keystore_remove(&ks, slot));
    assert(mc_keystore_is_empty(&ks));
    assert(mc_keystore_find_verifying(&ks, k.msg, sizeof(k.msg), k.sig) == -1);
    /* Removing again is a no-op. */
    assert(!mc_keystore_remove(&ks, slot));
}

static void test_duplicate_add_is_idempotent(void)
{
    mc_keystore_t ks;
    mc_keystore_init(&ks);

    test_key_t k;
    make_key(&k, 0x02);

    int s1 = mc_keystore_add(&ks, k.pk, "First");
    int s2 = mc_keystore_add(&ks, k.pk, "Relabeled");
    assert(s1 == s2);
    assert(mc_keystore_count(&ks) == 1);

    char label[MC_KEY_LABEL_MAX];
    assert(mc_keystore_get(&ks, s1, NULL, label, sizeof(label)));
    assert(strcmp(label, "Relabeled") == 0);
}

static void test_capacity(void)
{
    mc_keystore_t ks;
    mc_keystore_init(&ks);

    test_key_t keys[MC_KEYSTORE_MAX_KEYS];
    for (int i = 0; i < MC_KEYSTORE_MAX_KEYS; i++) {
        make_key(&keys[i], (uint8_t)(0x10 + i));
        assert(mc_keystore_add(&ks, keys[i].pk, "k") == i);
    }
    assert(mc_keystore_count(&ks) == MC_KEYSTORE_MAX_KEYS);

    /* One more distinct key does not fit. */
    test_key_t extra;
    make_key(&extra, 0xEE);
    assert(mc_keystore_add(&ks, extra.pk, "overflow") == -1);

    /* Each enrolled key verifies against its own slot. */
    for (int i = 0; i < MC_KEYSTORE_MAX_KEYS; i++) {
        assert(mc_keystore_find_verifying(&ks, keys[i].msg, sizeof(keys[i].msg), keys[i].sig) == i);
    }

    /* Freeing a slot lets a new key enroll. */
    assert(mc_keystore_remove(&ks, 3));
    assert(mc_keystore_add(&ks, extra.pk, "now fits") == 3);
}

static void test_wipe(void)
{
    mc_keystore_t ks;
    mc_keystore_init(&ks);
    test_key_t a, b;
    make_key(&a, 0x21);
    make_key(&b, 0x22);
    mc_keystore_add(&ks, a.pk, "a");
    mc_keystore_add(&ks, b.pk, "b");
    assert(mc_keystore_count(&ks) == 2);

    mc_keystore_wipe(&ks);
    assert(mc_keystore_is_empty(&ks));
    assert(mc_keystore_find_verifying(&ks, a.msg, sizeof(a.msg), a.sig) == -1);
}

static void test_serialize_roundtrip(void)
{
    mc_keystore_t ks;
    mc_keystore_init(&ks);
    test_key_t a, b;
    make_key(&a, 0x31);
    make_key(&b, 0x32);
    mc_keystore_add(&ks, a.pk, "Alice");
    mc_keystore_add(&ks, b.pk, "Bob");

    uint8_t buf[1024];
    size_t len = 0;
    assert(mc_keystore_serialize(&ks, buf, sizeof(buf), &len) == MC_KEYSTORE_OK);

    mc_keystore_t restored;
    memset(&restored, 0xAA, sizeof(restored));
    assert(mc_keystore_deserialize(buf, len, &restored) == MC_KEYSTORE_OK);
    assert(memcmp(&ks, &restored, sizeof(ks)) == 0);

    /* Restored keystore still verifies signatures. */
    assert(mc_keystore_find_verifying(&restored, a.msg, sizeof(a.msg), a.sig) == 0);
    assert(mc_keystore_find_verifying(&restored, b.msg, sizeof(b.msg), b.sig) == 1);
}

static void test_deserialize_rejects_corrupt(void)
{
    mc_keystore_t ks;
    mc_keystore_init(&ks);
    uint8_t buf[1024];
    size_t len = 0;
    mc_keystore_serialize(&ks, buf, sizeof(buf), &len);
    buf[0] ^= 0xFF;
    mc_keystore_t out;
    assert(mc_keystore_deserialize(buf, len, &out) == MC_KEYSTORE_ERR_CORRUPT);
}

int main(void)
{
    test_empty();
    test_add_find_remove();
    test_duplicate_add_is_idempotent();
    test_capacity();
    test_wipe();
    test_serialize_roundtrip();
    test_deserialize_rejects_corrupt();
    return 0;
}
