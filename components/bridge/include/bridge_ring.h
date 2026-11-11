/*
 * Byte ring with absolute offsets (pure C, no ESP-IDF dependency).
 *
 * Semantics match the host's v0 device model:
 *   - `end`     : absolute offset of the next byte to be appended
 *                 (i.e. "bytes received so far", 0-based next position)
 *   - `start`   : absolute offset of the oldest byte still buffered
 *   - dropped   : total bytes evicted because the ring is full
 *
 * There is exactly one producer (the UART RX task) and readers may snapshot
 * while the producer appends. The caller is responsible for locking.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t *buf;
    size_t cap;
    size_t head;    /* index in buf of the oldest byte */
    size_t used;    /* bytes currently buffered */
    uint64_t start; /* absolute offset of buf[head] */
    uint64_t end;   /* absolute offset of the next append */
    uint64_t dropped;
    bool owns_buf;
} bridge_ring_t;

/* Allocate the backing store. Returns 0 on success, -1 on OOM/bad args. */
int bridge_ring_init(bridge_ring_t *ring, size_t cap);

/* Use a caller-provided backing store and take ownership of it. */
int bridge_ring_init_preallocated(bridge_ring_t *ring, uint8_t *buf, size_t cap);

/* Free the backing store and zero the struct. */
void bridge_ring_free(bridge_ring_t *ring);

/* Drop everything (absolute offsets restart at 0). */
void bridge_ring_reset(bridge_ring_t *ring);

/* Append bytes; on overflow the oldest bytes are evicted and counted. */
void bridge_ring_append(bridge_ring_t *ring, const uint8_t *data, size_t len);

/* Copy up to `cap` bytes starting at absolute offset `abs_start`.
 * Returns the number of bytes copied (0 if out of range). */
size_t bridge_ring_copy(const bridge_ring_t *ring, uint64_t abs_start, uint8_t *dst, size_t cap);

/* Find `needle` at or after `abs_from`. On success stores
 * (absolute offset just past the match) in *abs_end and returns true. */
bool bridge_ring_find(const bridge_ring_t *ring,
                      uint64_t abs_from,
                      const uint8_t *needle,
                      size_t nlen,
                      uint64_t *abs_end);

#ifdef __cplusplus
}
#endif