#include "bridge_ring.h"

#include <stdlib.h>
#include <string.h>

int bridge_ring_init(bridge_ring_t *ring, size_t cap)
{
    if (!ring || cap == 0) {
        return -1;
    }
    memset(ring, 0, sizeof(*ring));
    ring->buf = (uint8_t *)malloc(cap);
    if (!ring->buf) {
        return -1;
    }
    ring->cap = cap;
    ring->owns_buf = true;
    return 0;
}

int bridge_ring_init_preallocated(bridge_ring_t *ring, uint8_t *buf, size_t cap)
{
    if (!ring || !buf || cap == 0) {
        return -1;
    }
    memset(ring, 0, sizeof(*ring));
    ring->buf = buf;
    ring->cap = cap;
    ring->owns_buf = true; /* the ring frees buf on bridge_ring_free() */
    return 0;
}

void bridge_ring_free(bridge_ring_t *ring)
{
    if (!ring) {
        return;
    }
    if (ring->owns_buf) {
        free(ring->buf);
    }
    memset(ring, 0, sizeof(*ring));
}

void bridge_ring_reset(bridge_ring_t *ring)
{
    if (!ring) {
        return;
    }
    ring->head = 0;
    ring->used = 0;
    ring->start = 0;
    ring->end = 0;
    ring->dropped = 0;
}

void bridge_ring_append(bridge_ring_t *ring, const uint8_t *data, size_t len)
{
    if (!ring || !data || len == 0 || ring->cap == 0) {
        return;
    }

    if (len >= ring->cap) {
        /* The new data alone is larger than the ring: keep only its tail. */
        ring->dropped += ring->used + (len - ring->cap);
        memcpy(ring->buf, data + (len - ring->cap), ring->cap);
        ring->head = 0;
        ring->used = ring->cap;
    } else {
        if (ring->used + len > ring->cap) {
            size_t drop = ring->used + len - ring->cap;
            ring->head = (ring->head + drop) % ring->cap;
            ring->used -= drop;
            ring->dropped += drop;
        }
        size_t tail = (ring->head + ring->used) % ring->cap;
        size_t first = ring->cap - tail;
        if (first > len) {
            first = len;
        }
        memcpy(ring->buf + tail, data, first);
        if (len > first) {
            memcpy(ring->buf, data + first, len - first);
        }
        ring->used += len;
    }

    ring->end += len;
    ring->start = ring->end - ring->used;
}

size_t bridge_ring_copy(const bridge_ring_t *ring, uint64_t abs_start, uint8_t *dst, size_t cap)
{
    if (!ring || !dst || cap == 0) {
        return 0;
    }
    if (abs_start < ring->start) {
        abs_start = ring->start;
    }
    if (abs_start >= ring->end) {
        return 0;
    }

    size_t avail = (size_t)(ring->end - abs_start);
    size_t n = avail < cap ? avail : cap;
    size_t idx = (size_t)((ring->head + (size_t)(abs_start - ring->start)) % ring->cap);
    size_t first = ring->cap - idx;
    if (first > n) {
        first = n;
    }
    memcpy(dst, ring->buf + idx, first);
    if (n > first) {
        memcpy(dst + first, ring->buf, n - first);
    }
    return n;
}

bool bridge_ring_find(const bridge_ring_t *ring,
                      uint64_t abs_from,
                      const uint8_t *needle,
                      size_t nlen,
                      uint64_t *abs_end)
{
    if (!ring || !needle || nlen == 0) {
        return false;
    }
    if (abs_from < ring->start) {
        abs_from = ring->start;
    }
    if (abs_from >= ring->end) {
        return false;
    }

    size_t idx = (size_t)((ring->head + (size_t)(abs_from - ring->start)) % ring->cap);
    for (uint64_t abs = abs_from; abs + nlen <= ring->end; abs++) {
        size_t i = idx;
        size_t k = 0;
        while (k < nlen) {
            if (ring->buf[i] != needle[k]) {
                break;
            }
            i++;
            if (i == ring->cap) {
                i = 0;
            }
            k++;
        }
        if (k == nlen) {
            if (abs_end) {
                *abs_end = abs + nlen;
            }
            return true;
        }
        idx++;
        if (idx == ring->cap) {
            idx = 0;
        }
    }
    return false;
}