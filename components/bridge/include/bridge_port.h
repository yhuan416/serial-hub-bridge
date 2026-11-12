/*
 * One logical bridged UART port: UART driver + absolute-offset RX ring +
 * bounded line index for the LOG_* RPCs.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "driver/uart.h"
#include "bridge_read.h"
#include "bridge_ring.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uart_port_t uart_num;
    int tx_pin;
    int rx_pin;
    int rts_pin; /* <0 => no RTS output (flow control / set_lines unsupported) */
    int cts_pin; /* <0 => no CTS input */
    int dtr_gpio; /* <0 => no manual DTR output */

    int baud_rate;
    uint8_t data_bits; /* 5..8 */
    uint8_t stop_bits; /* 1..2 */
    char parity[8];       /* "none" | "even" | "odd" */
    char flow_control[8]; /* "none" | "rtscts" */

    size_t ring_bytes;
    int uart_rx_buf;

    uint32_t log_entries;
    uint32_t log_line_max;
    uint32_t log_partial_max;
    uint8_t sd_port; /* SD log namespace index (0/1) */
} bridge_port_config_t;

typedef struct bridge_port bridge_port_t;

typedef struct {
    uint64_t ring_start_offset;
    uint64_t ring_end_offset;
    size_t ring_used;
    size_t ring_capacity;
    uint64_t ring_dropped_bytes;
    uint64_t rx_bytes;          /* absolute RX bytes delivered by UART */
    uint64_t tx_bytes;          /* absolute TX bytes accepted */
    uint64_t uart_overflow_bytes;
    uint32_t uart_overflow_events;
} bridge_port_stat_t;

typedef struct {
    uint32_t seq;
    int64_t ts_ms;
    uint8_t dir; /* 0=rx, 1=tx */
    uint64_t offset;
    uint32_t bytes;    /* full logical line length */
    uint32_t text_len; /* stored line length (<= log_line_max) */
    bool truncated;
    bool ts_synced;
} bridge_log_meta_t;

bridge_port_t *bridge_port_init(const bridge_port_config_t *cfg);
void bridge_port_deinit(bridge_port_t *p);

const bridge_port_config_t *bridge_port_config(const bridge_port_t *p);
bool bridge_port_can_set_lines(const bridge_port_t *p);

/* Apply line coding. Any field is validated by the caller; returns ESP_ERR_*. */
esp_err_t bridge_port_configure(bridge_port_t *p,
                                int baud_rate,
                                uint8_t data_bits,
                                uint8_t stop_bits,
                                const char *parity,
                                const char *flow_control);

/* Write to UART. Returns bytes accepted, fills TX bookkeeping. */
int bridge_port_write(bridge_port_t *p,
                      const uint8_t *data,
                      size_t len,
                      uint64_t *tx_offset,
                      uint32_t *tx_log_seq);

/* Absolute RX offset (next byte position). */
uint64_t bridge_port_rx_offset(bridge_port_t *p);

void bridge_port_stat(bridge_port_t *p, bridge_port_stat_t *out);

/* Evaluate+copy one READ pass under the port lock.
 * Returns true when the result is final (out->done), copying data into `out`
 * (never more than `out_cap`, which must be >= requested max_bytes). */
bool bridge_port_read(bridge_port_t *p,
                      const bridge_read_args_t *args,
                      uint64_t started_ms,
                      bridge_read_result_t *res,
                      uint8_t *out,
                      size_t out_cap);

/* Wait for new RX data or the timeout. */
void bridge_port_wait(bridge_port_t *p, uint32_t ms);

esp_err_t bridge_port_set_lines(bridge_port_t *p, int dtr, int rts, int *out_dtr, int *out_rts);
esp_err_t bridge_port_send_break(bridge_port_t *p, uint32_t duration_ms);

/* ── bounded line log (RAM; the device-authoritative log is a later step) ── */

uint32_t bridge_port_log_count(bridge_port_t *p);
bool bridge_port_log_earliest_seq(bridge_port_t *p, uint32_t *seq);
uint32_t bridge_port_log_latest_seq(bridge_port_t *p);
uint32_t bridge_port_log_used_bytes(bridge_port_t *p);
uint32_t bridge_port_log_capacity_bytes(bridge_port_t *p);

/* index 0 = oldest retained entry. Copies text into `text` (NUL-terminated). */
bool bridge_port_log_get(bridge_port_t *p,
                         uint32_t index,
                         bridge_log_meta_t *meta,
                         char *text,
                         size_t text_cap);

void bridge_port_log_clear(bridge_port_t *p, bool reset_seq);
void bridge_port_log_set_seq(bridge_port_t *p, uint32_t seq);

/* Flush an unterminated RX line as its own log entry (called periodically and
 * before export) so the SD tail does not wait forever for a newline. */
void bridge_port_log_flush_partial(bridge_port_t *p);

#ifdef __cplusplus
}
#endif