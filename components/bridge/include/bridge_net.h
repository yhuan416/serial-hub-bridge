/*
 * WiFi STA + TCP server + NDJSON session loop for the v0 bridge.
 */
#pragma once

#include <stdint.h>

#include "bridge.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint16_t tcp_port;
} bridge_net_config_t;

/* Connect WiFi (never returns on failure loop) and serve clients forever. */
void bridge_net_run(bridge_ctx_t *ctx, const bridge_net_config_t *cfg);

#ifdef __cplusplus
}
#endif