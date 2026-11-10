/* SPDX-License-Identifier: MIT */
#ifndef HV_WINDOWS_TICK_DEADLINE_H
#define HV_WINDOWS_TICK_DEADLINE_H

#include <stdbool.h>
#include <stdint.h>

/* CPU-local state; intervals and elapsed delays stay below the signed half-range. */
struct hv_tick_deadline {
    uint64_t next;
    uint64_t interval;
    bool valid;
};

static inline void hv_tick_deadline_init(struct hv_tick_deadline *state, uint64_t now,
                                         uint64_t interval)
{
    if (!state->valid) {
        state->next = now + interval;
        state->valid = true;
    }
    /* A role/interval change must not postpone the already armed deadline. */
    state->interval = interval;
}

static inline bool hv_tick_deadline_due(const struct hv_tick_deadline *state, uint64_t now)
{
    return state->valid && (int64_t)(now - state->next) >= 0;
}

static inline uint64_t hv_tick_deadline_delay(const struct hv_tick_deadline *state,
                                             uint64_t now, uint64_t limit)
{
    if (hv_tick_deadline_due(state, now))
        return 1;
    uint64_t remaining = state->next - now;
    return remaining < limit ? remaining : limit;
}

static inline void hv_tick_deadline_complete(struct hv_tick_deadline *state, uint64_t now)
{
    if (hv_tick_deadline_due(state, now)) {
        /* Skip elapsed periods, retaining phase without a catch-up interrupt storm. */
        uint64_t periods = (now - state->next) / state->interval + 1;
        state->next += periods * state->interval;
    }
}

#endif
