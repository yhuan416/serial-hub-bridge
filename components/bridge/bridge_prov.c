#include "bridge_prov.h"

#include <stdio.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/timers.h"
#include "network_provisioning/manager.h"
#include "network_provisioning/scheme_ble.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

#include "bridge_display.h"

#define TAG "bridge_prov"

#define RECONNECT_DELAY_MS 1000

static SemaphoreHandle_t s_lock;
static bool s_inited;
static bool s_provisioned;
static bool s_prov_active;
static bool s_connected;
static volatile bridge_prov_state_t s_state = BRIDGE_PROV_OFF;

static uint32_t s_ip; /* network order */
static int8_t s_rssi;
static char s_ssid[33];
static char s_ble_name[16];
static char s_qr_payload[160];
static TimerHandle_t s_reconnect_timer;

static void prov_lock(void)
{
    if (s_lock) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
}

static void prov_unlock(void)
{
    if (s_lock) {
        xSemaphoreGive(s_lock);
    }
}

static const char *state_name(bridge_prov_state_t st)
{
    switch (st) {
    case BRIDGE_PROV_IDLE:
        return "idle";
    case BRIDGE_PROV_PROVISIONING:
        return "provisioning";
    case BRIDGE_PROV_CONNECTING:
        return "connecting";
    case BRIDGE_PROV_CONNECTED:
        return "connected";
    case BRIDGE_PROV_OFF:
    default:
        return "off";
    }
}

const char *bridge_prov_state_name(bridge_prov_state_t st)
{
    return state_name(st);
}

static void refresh_display(void)
{
#if CONFIG_BRIDGE_LCD_ENABLE
    if (!bridge_display_ready()) {
        return;
    }
    char ip[16] = "-";
    if (s_ip) {
        snprintf(ip, sizeof(ip), "%u.%u.%u.%u", (unsigned)(s_ip & 0xFF),
                 (unsigned)((s_ip >> 8) & 0xFF), (unsigned)((s_ip >> 16) & 0xFF),
                 (unsigned)((s_ip >> 24) & 0xFF));
    }
    bridge_display_show_status(s_ssid, ip, s_connected, state_name(s_state));
#endif
}

static void build_qr(void)
{
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(s_ble_name, sizeof(s_ble_name), "%s%02X%02X%02X",
             CONFIG_BRIDGE_PROV_BLE_PREFIX, mac[3], mac[4], mac[5]);
    snprintf(s_qr_payload, sizeof(s_qr_payload),
             "{\"ver\":\"v1\",\"name\":\"%s\",\"pop\":\"%s\",\"transport\":\"ble\"}",
             s_ble_name, CONFIG_BRIDGE_PROV_POP);

    ESP_LOGI(TAG, "BLE name=%s", s_ble_name);
    ESP_LOGI(TAG, "QR payload: %s", s_qr_payload);
#if CONFIG_BRIDGE_LCD_ENABLE
    bridge_display_show_qr(s_qr_payload);
#endif
}

static void reconnect_cb(TimerHandle_t t)
{
    (void)t;
    esp_wifi_connect();
}

static void schedule_reconnect(void)
{
    if (!s_reconnect_timer) {
        s_reconnect_timer = xTimerCreate("wifi_rc", pdMS_TO_TICKS(RECONNECT_DELAY_MS),
                                         pdFALSE, NULL, reconnect_cb);
    }
    if (s_reconnect_timer) {
        xTimerStart(s_reconnect_timer, 0);
    }
}

static void on_prov_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;

    if (base == NETWORK_PROV_EVENT) {
        switch (id) {
        case NETWORK_PROV_START:
            ESP_LOGI(TAG, "provisioning started");
            break;
        case NETWORK_PROV_WIFI_CRED_RECV: {
            wifi_sta_config_t *cfg = (wifi_sta_config_t *)data;
            prov_lock();
            snprintf(s_ssid, sizeof(s_ssid), "%s", (const char *)cfg->ssid);
            prov_unlock();
            ESP_LOGI(TAG, "credentials received: %s", (const char *)cfg->ssid);
            break;
        }
        case NETWORK_PROV_WIFI_CRED_FAIL: {
            network_prov_wifi_sta_fail_reason_t *reason =
                (network_prov_wifi_sta_fail_reason_t *)data;
            ESP_LOGW(TAG, "provisioning failed: %s",
                     (*reason == NETWORK_PROV_WIFI_STA_AUTH_ERROR) ? "auth error"
                                                                   : "AP not found");
            break;
        }
        case NETWORK_PROV_WIFI_CRED_SUCCESS:
            ESP_LOGI(TAG, "provisioning credentials accepted");
            break;
        case NETWORK_PROV_END:
            ESP_LOGI(TAG, "provisioning ended, releasing BLE transport");
            prov_lock();
            s_prov_active = false;
            s_provisioned = true;
            if (s_state == BRIDGE_PROV_PROVISIONING) {
                s_state = BRIDGE_PROV_CONNECTING;
            }
            prov_unlock();
            network_prov_mgr_deinit();
            bridge_display_hide_qr();
            refresh_display();
            break;
        default:
            break;
        }
        return;
    }

    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
        return;
    }

    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        prov_lock();
        s_connected = false;
        s_ip = 0;
        s_rssi = 0;
        if (!s_prov_active) {
            s_state = s_provisioned ? BRIDGE_PROV_CONNECTING : BRIDGE_PROV_IDLE;
        }
        prov_unlock();
        if (!s_prov_active) {
            schedule_reconnect();
        }
        return;
    }

    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)data;
        wifi_config_t wc;
        wifi_ap_record_t ap;

        prov_lock();
        s_ip = ev->ip_info.ip.addr;
        s_connected = true;
        s_provisioned = true;
        s_state = BRIDGE_PROV_CONNECTED;
        if (esp_wifi_get_config(WIFI_IF_STA, &wc) == ESP_OK) {
            snprintf(s_ssid, sizeof(s_ssid), "%s", (const char *)wc.sta.ssid);
        }
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
            s_rssi = ap.rssi;
        }
        prov_unlock();

        if (s_reconnect_timer) {
            xTimerStop(s_reconnect_timer, 0);
        }
        bridge_display_hide_qr();
        refresh_display();
        ESP_LOGI(TAG, "connected to \"%s\", ip " IPSTR, s_ssid, IP2STR(&ev->ip_info.ip));
    }
}

esp_err_t bridge_prov_init(void)
{
    if (s_inited) {
        return ESP_OK;
    }

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        return err;
    }

    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) {
        return ESP_ERR_NO_MEM;
    }

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    ESP_ERROR_CHECK(esp_event_handler_instance_register(NETWORK_PROV_EVENT, ESP_EVENT_ANY_ID,
                                                        on_prov_event, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        on_prov_event, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                        on_prov_event, NULL, NULL));

    wifi_init_config_t wifi_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wifi_cfg));

#if CONFIG_BRIDGE_LCD_ENABLE
    if (bridge_display_init() != ESP_OK) {
        ESP_LOGW(TAG, "LCD unavailable; QR only in the serial log");
    }
#endif

    network_prov_mgr_config_t mgr_cfg = {
        .scheme = network_prov_scheme_ble,
        .scheme_event_handler = NETWORK_PROV_SCHEME_BLE_EVENT_HANDLER_FREE_BTDM,
        .app_event_handler = NETWORK_PROV_EVENT_HANDLER_NONE,
        .network_prov_wifi_conn_cfg = {
            .wifi_conn_attempts = 0, /* infinite retries */
        },
    };
    ESP_ERROR_CHECK(network_prov_mgr_init(mgr_cfg));

    s_provisioned = false;
    network_prov_mgr_is_wifi_provisioned(&s_provisioned);

    if (s_provisioned) {
        s_state = BRIDGE_PROV_CONNECTING;
        ESP_LOGI(TAG, "already provisioned; starting STA");
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ESP_ERROR_CHECK(esp_wifi_start());
        refresh_display();
    } else {
        s_state = BRIDGE_PROV_PROVISIONING;
        s_prov_active = true;
        build_qr();
        err = network_prov_mgr_start_provisioning(NETWORK_PROV_SECURITY_1,
                                                  (const void *)CONFIG_BRIDGE_PROV_POP,
                                                  s_ble_name, NULL);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "BLE provisioning start failed: %s", esp_err_to_name(err));
            s_prov_active = false;
            s_state = BRIDGE_PROV_IDLE;
        } else {
            ESP_LOGI(TAG, "BLE provisioning active (service %s)", s_ble_name);
        }
    }

    s_inited = true;
    return ESP_OK;
}

bool bridge_prov_has_credentials(void)
{
    bool v;
    prov_lock();
    v = s_provisioned;
    prov_unlock();
    return v;
}

esp_err_t bridge_prov_clear_credentials(void)
{
    ESP_LOGI(TAG, "reset WiFi provisioning");
    esp_err_t err = network_prov_mgr_reset_wifi_provisioning();
    prov_lock();
    s_provisioned = false;
    s_connected = false;
    s_ip = 0;
    s_rssi = 0;
    s_state = BRIDGE_PROV_IDLE;
    prov_unlock();
    return err;
}

void bridge_prov_factory_reset(void)
{
    bridge_prov_clear_credentials();
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
}

void bridge_prov_get_status(bridge_prov_status_t *out)
{
    if (!out) {
        return;
    }
    prov_lock();
    out->state = s_state;
    out->ip = s_ip;
    out->rssi = s_rssi;
    memcpy(out->ssid, s_ssid, sizeof(out->ssid));
    out->provisioned = s_provisioned;
    memcpy(out->ble_name, s_ble_name, sizeof(out->ble_name));
    memcpy(out->qr_payload, s_qr_payload, sizeof(out->qr_payload));
    prov_unlock();
}