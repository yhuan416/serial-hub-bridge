#include "bridge_time.h"

#include <sys/time.h>
#include <time.h>

#include "esp_log.h"
#include "esp_sntp.h"
#include "esp_timer.h"
#include "sdkconfig.h"

#define TAG "bridge_time"

static bool s_started;
static bool s_synced;

static void time_sync_cb(struct timeval *tv)
{
    (void)tv;
    s_synced = true;
    ESP_LOGI(TAG, "SNTP synchronized");
}

void bridge_time_init(void)
{
#if CONFIG_BRIDGE_NTP_ENABLE
    sntp_set_time_sync_notification_cb(time_sync_cb);
    sntp_set_sync_mode(SNTP_SYNC_MODE_IMMED);
#endif
}

void bridge_time_start(void)
{
#if CONFIG_BRIDGE_NTP_ENABLE
    if (s_started) {
        return;
    }
    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, CONFIG_BRIDGE_NTP_SERVER);
    esp_sntp_init();
    s_started = true;
    ESP_LOGI(TAG, "SNTP started: %s", CONFIG_BRIDGE_NTP_SERVER);
#endif
}

bool bridge_time_synced(void)
{
    if (!s_synced) {
        time_t now = time(NULL);
        if (now > 1600000000) {
            s_synced = true;
        }
    }
    return s_synced;
}

uint64_t bridge_time_tick_ms(void)
{
    return (uint64_t)(esp_timer_get_time() / 1000);
}

uint64_t bridge_time_epoch_ms(void)
{
    if (!bridge_time_synced()) {
        return 0;
    }
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000ULL + (uint64_t)tv.tv_usec / 1000ULL;
}

int64_t bridge_time_base_ms(void)
{
    if (!bridge_time_synced()) {
        return 0;
    }
    uint64_t epoch = bridge_time_epoch_ms();
    uint64_t tick = bridge_time_tick_ms();
    return epoch > tick ? (int64_t)(epoch - tick) : 0;
}
