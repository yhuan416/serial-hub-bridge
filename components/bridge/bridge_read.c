#include "bridge_read.h"

#include <string.h>

static uint32_t u32_min(uint32_t a, uint32_t b)
{
    return a < b ? a : b;
}

void bridge_read_eval(const bridge_ring_t *ring,
                      const bridge_read_args_t *args,
                      uint64_t now_ms,
                      uint64_t started_ms,
                      bridge_read_result_t *out)
{
    memset(out, 0, sizeof(*out));

    const uint64_t from = args->has_from ? args->from_offset : ring->end;
    const uint32_t max_hold = args->max_hold_ms ? args->max_hold_ms : 10000;
    const uint32_t max_frame = args->max_frame ? args->max_frame : 65536;

    const uint32_t requested_wait = args->has_max_wait ? args->max_wait_ms : 5000;
    const uint32_t max_wait = u32_min(requested_wait, max_hold);
    const uint32_t idle = u32_min(args->has_idle ? args->idle_timeout_ms : 300, max_wait);
    const uint32_t budget = u32_min(args->has_budget ? args->budget_ms : max_wait, max_hold);
    const uint32_t limit = u32_min(max_wait, budget);

    uint32_t max_bytes = args->max_bytes ? args->max_bytes : max_frame;
    if (max_bytes > max_frame) {
        max_bytes = max_frame;
    }
    uint32_t min_bytes = args->min_bytes ? args->min_bytes : 1;

    const uint64_t clamp_from = from < ring->start ? ring->start : from;
    const uint64_t start = clamp_from > ring->end ? ring->end : clamp_from;
    const bool gap = from < ring->start;

    uint64_t view_end = ring->end;
    bool matched = false;
    if (args->match && args->match_len > 0) {
        uint64_t match_end = 0;
        if (bridge_ring_find(ring, from, args->match, args->match_len, &match_end)) {
            view_end = match_end < ring->end ? match_end : ring->end;
            matched = true;
        }
    }

    uint64_t avail = view_end > start ? view_end - start : 0;
    uint64_t n64 = avail < max_bytes ? avail : max_bytes;
    const size_t n = (size_t)n64;

    out->start = start;
    out->end = start + n;
    out->data_len = n;
    out->matched = matched;
    out->gap = gap;
    out->hold_clamped = requested_wait > max_hold;

    if (matched) {
        out->done = true;
        out->stopped_reason = "match";
        return;
    }
    if ((!args->match || args->match_len == 0) && n >= min_bytes) {
        out->done = true;
        out->stopped_reason = (n >= max_bytes) ? "max_bytes" : "min_bytes";
        return;
    }

    const uint64_t elapsed = now_ms >= started_ms ? now_ms - started_ms : 0;
    if (elapsed >= idle) {
        out->done = true;
        out->stopped_reason = "idle";
        return;
    }
    if (elapsed >= limit) {
        out->done = true;
        out->stopped_reason = elapsed >= budget ? "budget" : "idle";
        return;
    }

    uint32_t w_budget = (uint32_t)(limit - elapsed);
    uint32_t w_idle = (uint32_t)(idle - elapsed);
    uint32_t wait = u32_min(w_budget, w_idle);
    if (wait > 100) {
        wait = 100;
    }
    if (wait == 0) {
        wait = 1;
    }
    out->wait_ms = wait;
}