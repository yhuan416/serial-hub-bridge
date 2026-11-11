#include "bridge_event.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "sdkconfig.h"

#define TAG "event"

#ifndef CONFIG_BRIDGE_EVENT_ENTRIES
#define CONFIG_BRIDGE_EVENT_ENTRIES 64
#endif

static bridge_event_t s_events[CONFIG_BRIDGE_EVENT_ENTRIES];
static uint32_t s_head;
static uint32_t s_count;
static uint32_t s_seq;
static SemaphoreHandle_t s_lock;

esp_err_t bridge_event_init(void)
{
    if (s_lock) {
        return ESP_OK;
    }
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) {
        return ESP_ERR_NO_MEM;
    }
    s_head = 0;
    s_count = 0;
    s_seq = 0;
    return ESP_OK;
}

void bridge_event_push(uint8_t level, const char *code, const char *fmt, ...)
{
    char msg[BRIDGE_EVENT_MSG_MAX];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    if (level == BRIDGE_EVENT_ERROR) {
        ESP_LOGE(TAG, "%s: %s", code ? code : "?", msg);
    } else if (level == BRIDGE_EVENT_WARN) {
        ESP_LOGW(TAG, "%s: %s", code ? code : "?", msg);
    } else {
        ESP_LOGI(TAG, "%s: %s", code ? code : "?", msg);
    }

    if (!s_lock) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    uint32_t slot;
    if (s_count < CONFIG_BRIDGE_EVENT_ENTRIES) {
        slot = (s_head + s_count) % CONFIG_BRIDGE_EVENT_ENTRIES;
        s_count++;
    } else {
        slot = s_head;
        s_head = (s_head + 1) % CONFIG_BRIDGE_EVENT_ENTRIES;
    }
    bridge_event_t *e = &s_events[slot];
    memset(e, 0, sizeof(*e));
    e->seq = ++s_seq;
    e->ts_ms = esp_timer_get_time() / 1000;
    e->level = level;
    if (code) {
        strncpy(e->code, code, sizeof(e->code) - 1);
    }
    strncpy(e->msg, msg, sizeof(e->msg) - 1);
    xSemaphoreGive(s_lock);
}

uint32_t bridge_event_count(void)
{
    if (!s_lock) {
        return 0;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    uint32_t n = s_count;
    xSemaphoreGive(s_lock);
    return n;
}

uint32_t bridge_event_latest(void)
{
    if (!s_lock) {
        return 0;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    uint32_t n = s_seq;
    xSemaphoreGive(s_lock);
    return n;
}

bool bridge_event_earliest(uint32_t *seq)
{
    if (!s_lock || s_count == 0) {
        return false;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool ok = s_count > 0;
    if (ok && seq) {
        *seq = s_events[s_head].seq;
    }
    xSemaphoreGive(s_lock);
    return ok;
}

uint32_t bridge_event_query(uint32_t seq_from, uint32_t limit, bridge_event_t *out,
                            uint32_t out_cap)
{
    if (!s_lock || !out || out_cap == 0 || limit == 0) {
        return 0;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    uint32_t n = 0;
    for (uint32_t i = 0; i < s_count && n < out_cap && n < limit; i++) {
        const bridge_event_t *e = &s_events[(s_head + i) % CONFIG_BRIDGE_EVENT_ENTRIES];
        if (e->seq < seq_from) {
            continue;
        }
        out[n++] = *e;
    }
    xSemaphoreGive(s_lock);
    return n;
}