/*
 * Device-side v1 session (binary push + capture). v0 NDJSON is not spoken.
 * Frame layout matches serial-hub-mcp/src/remote/v1_codec.js.
 */
#pragma once

#include "bridge.h"

#ifdef __cplusplus
extern "C" {
#endif

void bridge_v1_start(bridge_ctx_t *ctx);
void bridge_v1_session(bridge_ctx_t *ctx, int fd);

#ifdef __cplusplus
}
#endif
