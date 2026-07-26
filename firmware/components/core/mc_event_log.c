#include "mc_event_log.h"

void mc_event_log_init(mc_event_log_t *log, mc_event_log_hal_t hal)
{
    log->hal = hal;
    log->last_seq = (hal.get_last_seq != NULL) ? hal.get_last_seq(hal.ctx) : 0;
}

void mc_event_log_append(mc_event_log_t *log, mc_event_type_t type, uint8_t arg0, uint8_t arg1, uint32_t now_ms)
{
    uint32_t next_seq = log->last_seq + 1;
    uint16_t slot = (uint16_t)((next_seq - 1) % MC_EVENT_LOG_SLOT_COUNT);

    mc_event_record_t rec;
    rec.seq = next_seq;
    rec.uptime_ms = now_ms;
    rec.type = (uint8_t)type;
    rec.arg0 = arg0;
    rec.arg1 = arg1;
    rec.reserved = 0;

    /* A write (or cursor-persist) failure is swallowed: the in-RAM cursor
     * still advances, so the log keeps working for every future append even
     * if this one record silently failed to persist — logging must never
     * become a fatal path for the operation it's logging. */
    if (log->hal.write_slot != NULL) {
        log->hal.write_slot(slot, &rec, log->hal.ctx);
    }
    log->last_seq = next_seq;
    if (log->hal.set_last_seq != NULL) {
        log->hal.set_last_seq(next_seq, log->hal.ctx);
    }
}

/* Shared by mc_event_log_read() and mc_event_log_count_since(): the
 * effective starting seq (exclusive) once both `since_seq` and ring-buffer
 * eviction are accounted for. */
static uint32_t effective_start(const mc_event_log_t *log, uint32_t since_seq)
{
    uint32_t oldest_available = (log->last_seq > MC_EVENT_LOG_SLOT_COUNT)
                                     ? (log->last_seq - MC_EVENT_LOG_SLOT_COUNT)
                                     : 0;
    return (since_seq > oldest_available) ? since_seq : oldest_available;
}

size_t mc_event_log_count_since(const mc_event_log_t *log, uint32_t since_seq)
{
    if (log->last_seq == 0) {
        return 0;
    }
    uint32_t start = effective_start(log, since_seq);
    return (log->last_seq > start) ? (size_t)(log->last_seq - start) : 0;
}

size_t mc_event_log_read(const mc_event_log_t *log, uint32_t since_seq, mc_event_record_t *out, size_t max_count)
{
    if (log->last_seq == 0) {
        return 0;
    }

    uint32_t start = effective_start(log, since_seq);

    size_t count = 0;
    for (uint32_t seq = start + 1; seq <= log->last_seq && count < max_count; seq++) {
        uint16_t slot = (uint16_t)((seq - 1) % MC_EVENT_LOG_SLOT_COUNT);
        mc_event_record_t rec;
        if (log->hal.read_slot == NULL || !log->hal.read_slot(slot, &rec, log->hal.ctx)) {
            continue; /* never written / HAL read failure: tolerate the gap */
        }
        if (rec.seq != seq) {
            continue; /* slot holds a different generation's record (corruption or a lapped write): skip */
        }
        out[count++] = rec;
    }
    return count;
}

void mc_event_log_clear(mc_event_log_t *log)
{
    log->last_seq = 0;
    if (log->hal.set_last_seq != NULL) {
        log->hal.set_last_seq(0, log->hal.ctx);
    }
}
