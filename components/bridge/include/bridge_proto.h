/*
 * v0 NDJSON request dispatcher.
 *
 * `bridge_proto_handle` takes one complete JSON line and returns a heap
 * response string (without the trailing newline), or NULL when the line must
 * be ignored (e.g. malformed JSON, matching the host's fake bridge).
 */
#pragma once

#include <stdbool.h>

#include "bridge.h"

#ifdef __cplusplus
extern "C" {
#endif

char *bridge_proto_handle(bridge_ctx_t *ctx,
                          bool *authed,
                          const char *line,
                          bool *close_after);

/* Clear the op_id de-duplication cache (device restart / test hooks). */
void bridge_proto_reset(void);

#ifdef __cplusplus
}
#endif