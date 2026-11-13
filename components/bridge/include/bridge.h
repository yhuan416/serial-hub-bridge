/*
 * Shared state for the serial bridge.
 *
 * Contract: serial-hub-mcp/docs/remote-link-protocol-v1.md
 * Binary frames. The device keeps only unacked send-cache bytes.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "bridge_port.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BRIDGE_PROTO_VERSION "v1"
#define BRIDGE_FW_VERSION "0.2.0"
#define BRIDGE_PORT_ID "uart0"
#define BRIDGE_PORT_KIND "uart"
#define BRIDGE_MAX_PORTS 2

typedef struct bridge_ctx {
    bridge_port_t *ports[BRIDGE_MAX_PORTS];
    const char *port_ids[BRIDGE_MAX_PORTS];
    int port_count;
    int port_index;
    bridge_port_t *port; /* current/default = ports[0] */

    /* NULL or empty => HELLO token auth disabled (caps.auth=false). */
    const char *token;

    uint32_t max_frame;
    uint32_t max_hold_ms;

    /* Reused by READ/TRANSACT to stage at most max_frame response bytes. */
    uint8_t *scratch;
    size_t scratch_cap;
} bridge_ctx_t;

/* Milliseconds since boot (monotonic; used for v0 tick timing). */
uint64_t bridge_now_ms(void);

#ifdef __cplusplus
}
#endif