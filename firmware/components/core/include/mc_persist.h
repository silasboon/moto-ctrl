#pragma once

/*
 * mc_persist — debounced-write scheduler for config persistence.
 *
 * NVS flash has a limited erase-cycle budget, and mc_output_set() /
 * config changes can happen far more often than they need to be
 * committed to flash (AGENTS.md: "state persistence strategy that
 * respects NVS flash wear (debounced writes)"). Mark the store dirty on
 * every change; a caller (firmware/main's config task) polls
 * mc_persist_should_flush() at its own cadence and only then calls
 * mc_config_save() and mc_persist_mark_flushed().
 *
 * This does not affect the boot-time restore path (mc_output_restore_
 * from_config) — that always applies whatever was last actually flushed,
 * which is the persisted state AGENTS.md safety requirement #1 requires
 * be restored within 250ms of a reboot.
 */

#include "mc_types.h"

typedef struct {
    uint32_t debounce_ms;
    bool dirty;
    uint32_t dirty_since_ms;
} mc_persist_scheduler_t;

void mc_persist_init(mc_persist_scheduler_t *sched, uint32_t debounce_ms);

/* Call whenever persisted state changes. Restarts the debounce window. */
void mc_persist_mark_dirty(mc_persist_scheduler_t *sched, uint32_t now_ms);

/* True once the debounce window has elapsed since the last mark_dirty
 * with no flush yet. The caller should save the config and then call
 * mc_persist_mark_flushed(). */
bool mc_persist_should_flush(const mc_persist_scheduler_t *sched, uint32_t now_ms);

void mc_persist_mark_flushed(mc_persist_scheduler_t *sched);
