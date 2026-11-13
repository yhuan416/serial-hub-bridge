/*
 * Byte-window helper (pure C, no ESP-IDF dependency).
 *
 * The v1 pump uses this to copy UART-ring bytes into the send cache.
 * It is not a host-visible READ RPC.
 *
 *   stop priority: match > max_bytes/min_bytes > idle > budget
 *
 * `bridge_read_eval` performs one evaluation pass. The caller (net layer)
 * loops:
 *
 *   started = now_ms();
 *   for (;;) {
 *       bridge_read_eval(ring, &args, now_ms(), started, &res);
 *       if (res.done) break;
 *       wait(res.wait_ms);
 *   }
 *
 * Keeping it pure lets us unit-test the exact semantics on the host with gcc.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "bridge_ring.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool has_from;
    uint64_t from_offset;

    const uint8_t *match; /* optional */
    size_t match_len;

    uint32_t min_bytes; /* 0 -> 1 */
    uint32_t max_bytes; /* 0 -> max_frame */

    uint32_t max_frame;
    uint32_t max_hold_ms;

    bool has_max_wait;
    uint32_t max_wait_ms; /* default 5000 */

    bool has_idle;
    uint32_t idle_timeout_ms; /* default 300 */

    bool has_budget;
    uint32_t budget_ms; /* default max_wait */
} bridge_read_args_t;

typedef struct {
    bool done;

    uint64_t start;
    uint64_t end;
    size_t data_len; /* end - start */

    bool matched;
    bool gap;
    bool hold_clamped;
    const char *stopped_reason; /* static string: match|max_bytes|min_bytes|idle|budget */

    /* Valid when !done: how long the caller may sleep before re-evaluating. */
    uint32_t wait_ms;
} bridge_read_result_t;

void bridge_read_eval(const bridge_ring_t *ring,
                      const bridge_read_args_t *args,
                      uint64_t now_ms,
                      uint64_t started_ms,
                      bridge_read_result_t *out);

#ifdef __cplusplus
}
#endif