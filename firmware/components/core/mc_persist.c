#include "mc_persist.h"

#include <string.h>

void mc_persist_init(mc_persist_scheduler_t *sched, uint32_t debounce_ms)
{
    memset(sched, 0, sizeof(*sched));
    sched->debounce_ms = debounce_ms;
}

void mc_persist_mark_dirty(mc_persist_scheduler_t *sched, uint32_t now_ms)
{
    sched->dirty = true;
    sched->dirty_since_ms = now_ms;
}

bool mc_persist_should_flush(const mc_persist_scheduler_t *sched, uint32_t now_ms)
{
    if (!sched->dirty) {
        return false;
    }
    return mc_elapsed_at_least(now_ms, sched->dirty_since_ms, sched->debounce_ms);
}

void mc_persist_mark_flushed(mc_persist_scheduler_t *sched)
{
    sched->dirty = false;
}
