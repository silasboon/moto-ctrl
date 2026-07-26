#include "mc_persist.h"

#include <assert.h>

static void test_not_dirty_never_flushes(void)
{
    mc_persist_scheduler_t sched;
    mc_persist_init(&sched, 100);
    assert(mc_persist_should_flush(&sched, 0) == false);
    assert(mc_persist_should_flush(&sched, 100000) == false);
}

static void test_flush_gated_by_debounce_window(void)
{
    mc_persist_scheduler_t sched;
    mc_persist_init(&sched, 100);

    mc_persist_mark_dirty(&sched, 0);
    assert(mc_persist_should_flush(&sched, 50) == false);
    assert(mc_persist_should_flush(&sched, 99) == false);
    assert(mc_persist_should_flush(&sched, 100) == true);
}

static void test_mark_flushed_clears_dirty(void)
{
    mc_persist_scheduler_t sched;
    mc_persist_init(&sched, 100);

    mc_persist_mark_dirty(&sched, 0);
    assert(mc_persist_should_flush(&sched, 150) == true);
    mc_persist_mark_flushed(&sched);
    assert(mc_persist_should_flush(&sched, 150) == false);
    assert(mc_persist_should_flush(&sched, 999999) == false);
}

static void test_repeated_dirty_restarts_window(void)
{
    mc_persist_scheduler_t sched;
    mc_persist_init(&sched, 100);

    mc_persist_mark_dirty(&sched, 0);
    mc_persist_mark_dirty(&sched, 50); /* restarts the debounce window */
    assert(mc_persist_should_flush(&sched, 120) == false); /* only 70ms since last dirty */
    assert(mc_persist_should_flush(&sched, 150) == true);  /* 100ms since last dirty */
}

int main(void)
{
    test_not_dirty_never_flushes();
    test_flush_gated_by_debounce_window();
    test_mark_flushed_clears_dirty();
    test_repeated_dirty_restarts_window();
    return 0;
}
