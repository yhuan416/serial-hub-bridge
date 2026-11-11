/*
 * Host-side unit tests for the pure v0 data-plane logic.
 *
 * Build & run:  ./test/run.sh
 *
 * These do not need ESP-IDF; they validate the same decisions the host's
 * fake bridge makes, so firmware and host cannot drift silently.
 */

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "bridge_read.h"
#include "bridge_ring.h"

static int g_failures = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);  \
            g_failures++;                                                    \
        }                                                                    \
    } while (0)

#define CHECK_STR(a, b)                                                      \
    do {                                                                     \
        const char *_a = (a);                                                \
        const char *_b = (b);                                                \
        if (!_a || !_b || strcmp(_a, _b) != 0) {                             \
            fprintf(stderr, "FAIL %s:%d: \"%s\" != \"%s\"\n", __FILE__,       \
                    __LINE__, _a ? _a : "(null)", _b ? _b : "(null)");       \
            g_failures++;                                                    \
        }                                                                    \
    } while (0)

static bridge_read_args_t read_args(void)
{
    bridge_read_args_t a;
    memset(&a, 0, sizeof(a));
    a.min_bytes = 1;
    a.max_frame = 65536;
    a.max_hold_ms = 10000;
    return a;
}

/* Run eval at `started` with zero elapsed; if not done, report wait. */
static bridge_read_result_t eval0(const bridge_ring_t *r, bridge_read_args_t *a)
{
    bridge_read_result_t out;
    bridge_read_eval(r, a, 0, 0, &out);
    return out;
}

static void test_append_copy_basic(void)
{
    bridge_ring_t r;
    uint8_t out[32];

    CHECK(bridge_ring_init(&r, 8) == 0);

    bridge_ring_append(&r, (const uint8_t *)"abcd", 4);
    CHECK(r.start == 0 && r.end == 4 && r.used == 4);
    CHECK(bridge_ring_copy(&r, 0, out, sizeof(out)) == 4);
    CHECK(memcmp(out, "abcd", 4) == 0);

    /* second append wraps (tail=4, first=4, rest wraps to 0) */
    bridge_ring_append(&r, (const uint8_t *)"efgh", 4);
    CHECK(r.start == 0 && r.end == 8 && r.used == 8);
    CHECK(bridge_ring_copy(&r, 0, out, sizeof(out)) == 8);
    CHECK(memcmp(out, "abcdefgh", 8) == 0);
    CHECK(bridge_ring_copy(&r, 5, out, sizeof(out)) == 3);
    CHECK(memcmp(out, "fgh", 3) == 0);

    bridge_ring_free(&r);
}

static void test_overflow_evicts_oldest(void)
{
    bridge_ring_t r;
    uint8_t out[16];

    CHECK(bridge_ring_init(&r, 8) == 0);
    bridge_ring_append(&r, (const uint8_t *)"abcdefgh", 8);
    bridge_ring_append(&r, (const uint8_t *)"ij", 2);

    CHECK(r.end == 10);
    CHECK(r.used == 8);
    CHECK(r.start == 2);
    CHECK(r.dropped == 2);
    CHECK(bridge_ring_copy(&r, 0, out, sizeof(out)) == 8);
    CHECK(memcmp(out, "cdefghij", 8) == 0);

    /* a single append larger than the ring keeps only its tail */
    bridge_ring_append(&r, (const uint8_t *)"0123456789AB", 12);
    CHECK(r.end == 22);
    CHECK(r.used == 8);
    CHECK(r.start == 14);
    CHECK(bridge_ring_copy(&r, 14, out, sizeof(out)) == 8);
    CHECK(memcmp(out, "456789AB", 8) == 0);

    bridge_ring_free(&r);
}

static void test_read_no_data_wait_zero_is_idle(void)
{
    bridge_ring_t r;
    bridge_read_args_t a = read_args();
    bridge_read_result_t res;

    CHECK(bridge_ring_init(&r, 64) == 0);
    a.has_max_wait = true;
    a.max_wait_ms = 0;
    a.has_budget = true;
    a.budget_ms = 0;

    res = eval0(&r, &a);
    CHECK(res.done);
    CHECK_STR(res.stopped_reason, "idle");
    CHECK(res.data_len == 0);
    CHECK(res.start == 0 && res.end == 0);

    bridge_ring_free(&r);
}

static void test_read_returns_available_bytes(void)
{
    bridge_ring_t r;
    bridge_read_args_t a = read_args();
    bridge_read_result_t res;

    CHECK(bridge_ring_init(&r, 64) == 0);
    bridge_ring_append(&r, (const uint8_t *)"hello", 5);

    a.has_from = true;
    a.from_offset = 0;
    a.max_bytes = 64;

    res = eval0(&r, &a);
    CHECK(res.done);
    CHECK_STR(res.stopped_reason, "min_bytes");
    CHECK(res.start == 0 && res.end == 5 && res.data_len == 5);
    CHECK(!res.matched);

    bridge_ring_free(&r);
}

static void test_read_max_bytes_stop(void)
{
    bridge_ring_t r;
    bridge_read_args_t a = read_args();
    bridge_read_result_t res;

    CHECK(bridge_ring_init(&r, 64) == 0);
    bridge_ring_append(&r, (const uint8_t *)"0123456789", 10);

    a.has_from = true;
    a.from_offset = 0;
    a.max_bytes = 4;

    res = eval0(&r, &a);
    CHECK(res.done);
    CHECK_STR(res.stopped_reason, "max_bytes");
    CHECK(res.start == 0 && res.end == 4);

    bridge_ring_free(&r);
}

static void test_read_match_across_wrap(void)
{
    bridge_ring_t r;
    bridge_read_args_t a = read_args();
    bridge_read_result_t res;
    const uint8_t needle[] = {'O', 'K'};

    CHECK(bridge_ring_init(&r, 8) == 0);
    /* force wrap so "OK" is split across the physical end */
    bridge_ring_append(&r, (const uint8_t *)"abcdef", 6);
    bridge_ring_append(&r, (const uint8_t *)"xxxx", 4); /* drop "ab" */
    bridge_ring_append(&r, (const uint8_t *)"..OK", 4); /* end 14, start 6 */

    a.has_from = true;
    a.from_offset = 6;
    a.match = needle;
    a.match_len = sizeof(needle);
    a.max_bytes = 64;

    res = eval0(&r, &a);
    CHECK(res.done);
    CHECK_STR(res.stopped_reason, "match");
    CHECK(res.matched);
    CHECK(res.start == 6 && res.end == 14);
    CHECK(res.data_len == 8);

    bridge_ring_free(&r);
}

static void test_read_gap_and_clamp(void)
{
    bridge_ring_t r;
    bridge_read_args_t a = read_args();
    bridge_read_result_t res;

    CHECK(bridge_ring_init(&r, 8) == 0);
    bridge_ring_append(&r, (const uint8_t *)"abcdefghij", 10); /* start=2, end=10 */

    a.has_from = true;
    a.from_offset = 0;
    a.max_bytes = 64;

    res = eval0(&r, &a);
    CHECK(res.done);
    CHECK(res.gap);
    CHECK(res.start == 2);
    CHECK(res.data_len == 8);

    /* from beyond end: clamp to end; a zero wait returns an empty idle view */
    a.from_offset = 99;
    a.has_max_wait = true;
    a.max_wait_ms = 0;
    res = eval0(&r, &a);
    CHECK(res.done);
    CHECK(!res.gap);
    CHECK(res.start == 10 && res.end == 10);

    bridge_ring_free(&r);
}

static void test_read_budget_stop(void)
{
    bridge_ring_t r;
    bridge_read_args_t a = read_args();
    bridge_read_result_t res;

    CHECK(bridge_ring_init(&r, 64) == 0);

    a.has_from = true;
    a.from_offset = 0;
    a.has_max_wait = true;
    a.max_wait_ms = 2000;
    a.has_budget = true;
    a.budget_ms = 1000;
    a.has_idle = true;
    a.idle_timeout_ms = 2000;
    a.max_bytes = 64;

    /* no data, no match: not done yet, asks for a short wait */
    res = eval0(&r, &a);
    CHECK(!res.done);
    CHECK(res.wait_ms == 100);

    /* same evaluation after the budget expired */
    bridge_read_eval(&r, &a, 1000, 0, &res);
    CHECK(res.done);
    CHECK_STR(res.stopped_reason, "budget");

    bridge_ring_free(&r);
}

static void test_transact_barrier(void)
{
    bridge_ring_t r;
    bridge_read_args_t a = read_args();
    bridge_read_result_t res;

    CHECK(bridge_ring_init(&r, 64) == 0);
    bridge_ring_append(&r, (const uint8_t *)"NOISE", 5);

    /* host writes PING, device replies PONG: bytes after the barrier only */
    const uint64_t barrier = r.end; /* 5 */
    bridge_ring_append(&r, (const uint8_t *)"PONG", 4);

    a.has_from = true;
    a.from_offset = barrier;
    a.match = (const uint8_t *)"PONG";
    a.match_len = 4;
    a.max_bytes = 64;

    res = eval0(&r, &a);
    CHECK(res.done);
    CHECK(res.matched);
    CHECK(res.start == 5 && res.end == 9);
    CHECK(res.data_len == 4);

    bridge_ring_free(&r);
}

static void test_find_across_wrap(void)
{
    bridge_ring_t r;
    uint64_t end = 0;

    CHECK(bridge_ring_init(&r, 5) == 0);
    bridge_ring_append(&r, (const uint8_t *)"abcde", 5);
    bridge_ring_append(&r, (const uint8_t *)"fghij", 5); /* start=5, ring: fghij */

    CHECK(bridge_ring_find(&r, 5, (const uint8_t *)"fgh", 3, &end));
    CHECK(end == 8);
    CHECK(bridge_ring_find(&r, 5, (const uint8_t *)"hij", 3, &end));
    CHECK(end == 10);
    CHECK(!bridge_ring_find(&r, 5, (const uint8_t *)"abc", 3, &end));

    bridge_ring_free(&r);
}

int main(void)
{
    test_append_copy_basic();
    test_overflow_evicts_oldest();
    test_read_no_data_wait_zero_is_idle();
    test_read_returns_available_bytes();
    test_read_max_bytes_stop();
    test_read_match_across_wrap();
    test_read_gap_and_clamp();
    test_read_budget_stop();
    test_transact_barrier();
    test_find_across_wrap();

    if (g_failures) {
        fprintf(stderr, "\n%d check(s) failed\n", g_failures);
        return 1;
    }
    printf("ring/read: all checks passed\n");
    return 0;
}