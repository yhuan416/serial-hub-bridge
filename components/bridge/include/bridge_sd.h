/*
 * Device-authoritative SD log: raw bytes + fixed-size line index.
 *
 * Layout (current segment):
 *   /sdcard/shb/raw-<epoch>-<rand>.bin   raw RX/TX bytes, append-only
 *   /sdcard/shb/idx-<epoch>-<rand>.bin   sd_idx_rec_t, one per line/TX write
 *
 * The RAM ring stays the live window for READ; SD is the durable history used
 * by LOG_STATS / LOG_EXPORT after the agent comes back.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct bridge_port bridge_port_t;

#define BRIDGE_SD_MAX_SEGMENTS 16

typedef struct {
    char id[40];
    uint64_t raw_bytes;
    uint32_t index_count;
    uint32_t first_seq;
    uint32_t latest_seq;
    uint32_t latest_rx;
    uint32_t latest_tx;
    uint32_t acked_rx;
    uint32_t acked_tx;
    int64_t epoch_base_ms;
    int64_t mtime_s;
    bool active;
    bool acked;
    bool time_synced;
} bridge_sd_segment_t;

/* Packed 20-byte index record. `offset` is the per-boot RX/TX absolute offset
 * of the line start; `len` is the raw span (including CR/LF) or the TX length. */
typedef struct __attribute__((packed)) {
    uint64_t offset;
    uint32_t seq;
    uint32_t tick_ms; /* uptime ms */
    uint16_t len;
    uint8_t dir; /* 0=rx, 1=tx */
    uint8_t reserved;
} sd_idx_rec_t;

typedef struct {
    bool mounted;
    uint64_t capacity_bytes;
    uint64_t used_bytes;
    uint64_t free_bytes;
    uint64_t raw_bytes;   /* current segment */
    uint32_t index_count; /* current segment */
    uint32_t write_errors;
    bool broken;
    uint32_t earliest_seq;      /* current segment */
    uint32_t latest_seq;        /* current segment */
    uint32_t global_latest_seq; /* max latest_seq across segments */
    int64_t epoch_base_ms;      /* active segment tick -> epoch anchor */
    bool time_synced;
    uint64_t queue_bytes;   /* SD submit queue capacity */
    uint64_t queue_used;
    uint32_t queue_dropped;
    uint64_t queue_dropped_bytes;
    char segment[40];
    size_t segment_count;
    bridge_sd_segment_t segments[BRIDGE_SD_MAX_SEGMENTS];
} bridge_sd_status_t;

typedef struct {
    const char *segment; /* NULL/empty = active segment */
    uint8_t dir; /* 0=rx, 1=tx; 0xff=any (not supported for export) */
    bool has_seq_from;
    uint32_t seq_from;
    bool has_seq_to;
    uint32_t seq_to;
    uint64_t cursor;     /* raw offset, 0 = start */
    uint32_t max_bytes;  /* clamp to BRIDGE_SD_EXPORT_CHUNK */
} bridge_sd_export_req_t;

typedef struct {
    uint8_t *chunk; /* raw bytes */
    size_t chunk_len;
    uint8_t *index; /* sd_idx_rec_t[] with offset relative to chunk start */
    size_t index_len;
    char segment[40];
    int64_t time_base_ms;
    bool time_synced;
    bool eof;
    uint64_t next_cursor;
} bridge_sd_export_t;

esp_err_t bridge_sd_init(bool spi_bus_ready, bridge_port_t *port);
bool bridge_sd_ready(void);
uint32_t bridge_sd_latest_seq(void);
void bridge_sd_sync(void);

/* Enqueue one atomic raw+index record for the SD writer task.
 * Called from the UART path; never performs FAT I/O and never blocks on SD.
 * raw_len == len + 1 means "append LF" (RX line); raw_len == len means exact.
 * Returns false when the PSRAM queue is full (counted as queue_dropped). */
bool bridge_sd_submit(uint32_t seq, int64_t tick_ms, uint8_t dir,
                      const uint8_t *data, uint16_t len, uint16_t raw_len);

void bridge_sd_get_status(bridge_sd_status_t *out);

/* Export one page. Caller frees `out` with bridge_sd_export_free(). */
esp_err_t bridge_sd_export(const bridge_sd_export_req_t *req, bridge_sd_export_t *out);
void bridge_sd_export_free(bridge_sd_export_t *out);
/* segment=NULL/empty clears all segments; returns whether the active segment was cleared. */
esp_err_t bridge_sd_clear(const char *segment, bool *active_cleared);

/* Persist host archive watermarks for retention; dir 0=rx, 1=tx. */
esp_err_t bridge_sd_ack(const char *segment, uint8_t dir, uint32_t seq);

#ifdef __cplusplus
}
#endif