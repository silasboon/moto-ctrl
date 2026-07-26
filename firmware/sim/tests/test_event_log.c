/*
 * mc_event_log — the persisted security/safety event ring buffer.
 */
#include "mc_event_log.h"

#include <assert.h>
#include <string.h>

/* --- fake HAL: an array of slots standing in for the "evtlog" NVS partition --- */

typedef struct {
    mc_event_record_t records[MC_EVENT_LOG_SLOT_COUNT];
    bool present[MC_EVENT_LOG_SLOT_COUNT];
    uint32_t last_seq;
    bool write_should_fail;
    int write_calls;
} fake_store_t;

static bool fake_read(uint16_t slot, mc_event_record_t *out, void *ctx)
{
    fake_store_t *s = (fake_store_t *)ctx;
    if (slot >= MC_EVENT_LOG_SLOT_COUNT || !s->present[slot]) {
        return false;
    }
    *out = s->records[slot];
    return true;
}
static bool fake_write(uint16_t slot, const mc_event_record_t *rec, void *ctx)
{
    fake_store_t *s = (fake_store_t *)ctx;
    s->write_calls++;
    if (s->write_should_fail) {
        return false;
    }
    s->records[slot] = *rec;
    s->present[slot] = true;
    return true;
}
static uint32_t fake_get_last_seq(void *ctx) { return ((fake_store_t *)ctx)->last_seq; }
static bool fake_set_last_seq(uint32_t seq, void *ctx)
{
    ((fake_store_t *)ctx)->last_seq = seq;
    return true;
}

static mc_event_log_hal_t make_hal(fake_store_t *store)
{
    mc_event_log_hal_t hal = {
        .read_slot = fake_read,
        .write_slot = fake_write,
        .get_last_seq = fake_get_last_seq,
        .set_last_seq = fake_set_last_seq,
        .ctx = store,
    };
    return hal;
}

/* --- basic append/read --- */

static void test_empty_log_reads_nothing(void)
{
    fake_store_t store = {0};
    mc_event_log_t log;
    mc_event_log_init(&log, make_hal(&store));

    assert(mc_event_log_last_seq(&log) == 0);
    assert(mc_event_log_count_since(&log, 0) == 0);
    mc_event_record_t out[4];
    assert(mc_event_log_read(&log, 0, out, 4) == 0);
}

static void test_append_read_round_trip(void)
{
    fake_store_t store = {0};
    mc_event_log_t log;
    mc_event_log_init(&log, make_hal(&store));

    mc_event_log_append(&log, MC_EVT_LOCK_ENGAGED, 0, 0, 1000);
    mc_event_log_append(&log, MC_EVT_KEY_ENROLLED, 3, 0, 2000);
    mc_event_log_append(&log, MC_EVT_LV_CUTOFF_ENTER, 0, 0, 3000);

    assert(mc_event_log_last_seq(&log) == 3);
    assert(mc_event_log_count_since(&log, 0) == 3);

    mc_event_record_t out[8];
    size_t n = mc_event_log_read(&log, 0, out, 8);
    assert(n == 3);
    assert(out[0].seq == 1 && out[0].type == MC_EVT_LOCK_ENGAGED && out[0].uptime_ms == 1000);
    assert(out[1].seq == 2 && out[1].type == MC_EVT_KEY_ENROLLED && out[1].arg0 == 3 && out[1].uptime_ms == 2000);
    assert(out[2].seq == 3 && out[2].type == MC_EVT_LV_CUTOFF_ENTER && out[2].uptime_ms == 3000);
}

static void test_read_since_seq_filters_correctly(void)
{
    fake_store_t store = {0};
    mc_event_log_t log;
    mc_event_log_init(&log, make_hal(&store));
    for (int i = 0; i < 5; i++) {
        mc_event_log_append(&log, MC_EVT_LOCK_ENGAGED, (uint8_t)i, 0, (uint32_t)i);
    }

    assert(mc_event_log_count_since(&log, 2) == 3); /* seqs 3,4,5 */
    mc_event_record_t out[8];
    size_t n = mc_event_log_read(&log, 2, out, 8);
    assert(n == 3);
    assert(out[0].seq == 3 && out[1].seq == 4 && out[2].seq == 5);

    /* since_seq == last_seq: nothing new. */
    assert(mc_event_log_count_since(&log, 5) == 0);
    assert(mc_event_log_read(&log, 5, out, 8) == 0);
}

static void test_read_respects_max_count(void)
{
    fake_store_t store = {0};
    mc_event_log_t log;
    mc_event_log_init(&log, make_hal(&store));
    for (int i = 0; i < 10; i++) {
        mc_event_log_append(&log, MC_EVT_LOCK_ENGAGED, 0, 0, 0);
    }

    mc_event_record_t out[3];
    size_t n = mc_event_log_read(&log, 0, out, 3);
    assert(n == 3);
    assert(out[0].seq == 1 && out[1].seq == 2 && out[2].seq == 3); /* oldest-first */
}

/* --- ring wraparound --- */

static void test_wraparound_evicts_oldest(void)
{
    fake_store_t store = {0};
    mc_event_log_t log;
    mc_event_log_init(&log, make_hal(&store));

    /* Fill past capacity by 50 -- the oldest 50 records are evicted. */
    uint32_t total = MC_EVENT_LOG_SLOT_COUNT + 50;
    for (uint32_t i = 0; i < total; i++) {
        mc_event_log_append(&log, MC_EVT_LOCK_ENGAGED, (uint8_t)(i % 256), 0, i);
    }
    assert(mc_event_log_last_seq(&log) == total);

    /* Asking from the very beginning must silently skip evicted seqs and
     * return only what's still available (exactly SLOT_COUNT records). */
    assert(mc_event_log_count_since(&log, 0) == MC_EVENT_LOG_SLOT_COUNT);

    mc_event_record_t out[MC_EVENT_LOG_SLOT_COUNT];
    size_t n = mc_event_log_read(&log, 0, out, MC_EVENT_LOG_SLOT_COUNT);
    assert(n == MC_EVENT_LOG_SLOT_COUNT);
    /* Oldest surviving record is seq 51 (1..50 were evicted), newest is
     * seq total, and they're contiguous/oldest-first. */
    assert(out[0].seq == 51);
    assert(out[n - 1].seq == total);
    for (size_t i = 1; i < n; i++) {
        assert(out[i].seq == out[i - 1].seq + 1);
    }
}

static void test_wraparound_slot_reuse_does_not_corrupt_newer_record(void)
{
    /* Directly exercises the seq-stamped-in-slot corruption check: once
     * slot 0 has been reused by seq (SLOT_COUNT+1), reading seq 1 (which
     * used to live in slot 0) must not return the stale/wrong record. */
    fake_store_t store = {0};
    mc_event_log_t log;
    mc_event_log_init(&log, make_hal(&store));

    for (uint32_t i = 0; i < MC_EVENT_LOG_SLOT_COUNT + 1; i++) {
        mc_event_log_append(&log, MC_EVT_LOCK_ENGAGED, 0, 0, i);
    }
    /* seq 1 is evicted (count_since(0) excludes it); even if asked for
     * directly it must not surface a stale record from the reused slot. */
    mc_event_record_t out[4];
    size_t n = mc_event_log_read(&log, 0, out, 4);
    assert(n >= 1);
    assert(out[0].seq == 2); /* seq 1 evicted, seq 2 is the oldest survivor */
}

/* --- clear --- */

static void test_clear_then_append_leaves_one_record(void)
{
    fake_store_t store = {0};
    mc_event_log_t log;
    mc_event_log_init(&log, make_hal(&store));
    mc_event_log_append(&log, MC_EVT_KEY_ENROLLED, 0, 0, 100);
    mc_event_log_append(&log, MC_EVT_KEY_ENROLLED, 1, 0, 200);
    assert(mc_event_log_count_since(&log, 0) == 2);

    mc_event_log_clear(&log);
    assert(mc_event_log_last_seq(&log) == 0);
    assert(mc_event_log_count_since(&log, 0) == 0);

    mc_event_log_append(&log, MC_EVT_OWNERSHIP_TRANSFERRED, 0, 0, 300);
    assert(mc_event_log_count_since(&log, 0) == 1);

    mc_event_record_t out[4];
    size_t n = mc_event_log_read(&log, 0, out, 4);
    assert(n == 1);
    assert(out[0].type == MC_EVT_OWNERSHIP_TRANSFERRED);
    assert(out[0].seq == 1); /* cursor restarted from 1 */
}

/* --- persistence resume --- */

static void test_init_resumes_from_hal_last_seq(void)
{
    /* Simulates a reboot: a fresh mc_event_log_t bound to a HAL whose
     * store already has history picks up the cursor, not zero. */
    fake_store_t store = {0};
    mc_event_log_t log1;
    mc_event_log_init(&log1, make_hal(&store));
    mc_event_log_append(&log1, MC_EVT_LOCK_ENGAGED, 0, 0, 10);
    mc_event_log_append(&log1, MC_EVT_LOCK_RELEASED, 0, 0, 20);

    mc_event_log_t log2; /* fresh struct, same backing store */
    mc_event_log_init(&log2, make_hal(&store));
    assert(mc_event_log_last_seq(&log2) == 2);

    mc_event_log_append(&log2, MC_EVT_FACTORY_RESET, 0, 0, 30);
    assert(mc_event_log_last_seq(&log2) == 3);
    mc_event_record_t out[4];
    assert(mc_event_log_read(&log2, 0, out, 4) == 3);
}

/* --- HAL failure tolerance --- */

static void test_write_failure_does_not_block_future_appends(void)
{
    fake_store_t store = {0};
    store.write_should_fail = true;
    mc_event_log_t log;
    mc_event_log_init(&log, make_hal(&store));

    /* Append "succeeds" (no crash, no error return -- void function) even
     * though the underlying write failed; logging must never become a
     * fatal path for the operation it's logging. */
    mc_event_log_append(&log, MC_EVT_LOCK_ENGAGED, 0, 0, 100);
    assert(mc_event_log_last_seq(&log) == 1); /* in-RAM cursor still advances */
    assert(store.write_calls == 1);

    /* A later successful write still works normally. */
    store.write_should_fail = false;
    mc_event_log_append(&log, MC_EVT_LOCK_RELEASED, 0, 0, 200);
    assert(mc_event_log_last_seq(&log) == 2);

    mc_event_record_t out[4];
    size_t n = mc_event_log_read(&log, 0, out, 4);
    /* seq 1's write failed (never actually stored) -- reading tolerates the
     * gap and returns only what's really there (seq 2). */
    assert(n == 1);
    assert(out[0].seq == 2);
}

static void test_corrupt_slot_is_skipped_not_returned(void)
{
    /* A slot whose stored record's own seq doesn't match what's expected
     * (bit-rot / a lapped write in a pathological scenario) must be
     * silently skipped, not surfaced as a wrong record. */
    fake_store_t store = {0};
    mc_event_log_t log;
    mc_event_log_init(&log, make_hal(&store));
    mc_event_log_append(&log, MC_EVT_LOCK_ENGAGED, 0, 0, 100);
    mc_event_log_append(&log, MC_EVT_LOCK_RELEASED, 0, 0, 200);

    /* Corrupt slot 0's stored seq (belongs to seq 1) directly in the fake
     * store, simulating bit-rot. */
    store.records[0].seq = 99;

    mc_event_record_t out[4];
    size_t n = mc_event_log_read(&log, 0, out, 4);
    assert(n == 1); /* seq 1 skipped as corrupt; only seq 2 returned */
    assert(out[0].seq == 2);
}

int main(void)
{
    test_empty_log_reads_nothing();
    test_append_read_round_trip();
    test_read_since_seq_filters_correctly();
    test_read_respects_max_count();
    test_wraparound_evicts_oldest();
    test_wraparound_slot_reuse_does_not_corrupt_newer_record();
    test_clear_then_append_leaves_one_record();
    test_init_resumes_from_hal_last_seq();
    test_write_failure_does_not_block_future_appends();
    test_corrupt_slot_is_skipped_not_returned();
    return 0;
}
