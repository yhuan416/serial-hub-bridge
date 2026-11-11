/*
 * Control-plane event log (bounded RAM ring, independent of the data log).
 *
 * Examples: SD mount/format/remount, write/fsync errors, ring drops.
 * Events are also emitted to the serial console via ESP_LOG; EVENT_QUERY
 * exposes them to the host without touching the UART data path.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BRIDGE_EVENT_CODE_MAX 16
#define BRIDGE_EVENT_MSG_MAX 96

typedef enum {
    BRIDGE_EVENT_INFO = 0,
    BRIDGE_EVENT_WARN = 1,
    BRIDGE_EVENT_ERROR = 2,
} bridge_event_level_t;

typedef struct {
    uint32_t seq;
    int64_t ts_ms;
    uint8_t level;
    uint8_t reserved[3];
    char code[BRIDGE_EVENT_CODE_MAX];
    char msg[BRIDGE_EVENT_MSG_MAX];
} bridge_event_t;

esp_err_t bridge_event_init(void);

/* Format a message (printf-style) and append it to the ring + serial log. */
void bridge_event_push(uint8_t level, const char *code, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

uint32_t bridge_event_count(void);
uint32_t bridge_event_latest(void);
bool bridge_event_earliest(uint32_t *seq);

/* Copy up to `limit` events with seq >= seq_from (oldest first).
 * Returns the number of events copied. */
uint32_t bridge_event_query(uint32_t seq_from, uint32_t limit, bridge_event_t *out,
                            uint32_t out_cap);

#ifdef __cplusplus
}
#endif