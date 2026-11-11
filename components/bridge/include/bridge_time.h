/*
 * Device wall-clock (SNTP) alongside the monotonic uptime tick.
 *
 * tick_ms:   always available, used for offsets/timeouts.
 * epoch_ms:  only meaningful after SNTP sync; logs use tick + a per-segment
 *            time base to recover wall-clock timestamps.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void bridge_time_init(void);
/* Start SNTP once Wi-Fi has an IP. Idempotent. */
void bridge_time_start(void);

bool bridge_time_synced(void);
uint64_t bridge_time_epoch_ms(void);
uint64_t bridge_time_tick_ms(void);

/* epoch_ms - tick_ms; 0 when not synced. Stable for this boot after first sync. */
int64_t bridge_time_base_ms(void);

#ifdef __cplusplus
}
#endif
