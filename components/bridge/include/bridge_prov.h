/*
 * WiFi provisioning over BLE, using ESP-IDF `network_provisioning`
 * (the IDF 6.x successor of wifi_provisioning).
 *
 * Flow (same as esp32_ble_scan_tools):
 *   - QR payload: {"ver":"v1","name":"PROV_XXXXXX","pop":"...","transport":"ble"}
 *   - BLE Security1 + PoP, default protocomm GATT service UUID
 *   - QR is shown on the 160x80 ST7789 LCD; long-press GPIO0 resets
 *   - credentials are stored by network_prov_mgr/wifi driver in NVS
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BRIDGE_PROV_OFF = 0,
    BRIDGE_PROV_IDLE,         /* not provisioned, BLE provisioning not running */
    BRIDGE_PROV_PROVISIONING, /* BLE provisioning active, QR shown */
    BRIDGE_PROV_CONNECTING,   /* provisioned, connecting to the AP */
    BRIDGE_PROV_CONNECTED,    /* got IP */
} bridge_prov_state_t;

typedef struct {
    bridge_prov_state_t state;
    uint32_t ip; /* network byte order, 0 when not connected */
    int8_t rssi;
    char ssid[33];
    bool provisioned;
    char ble_name[16];
    char qr_payload[160];
} bridge_prov_status_t;

/* NVS + netif + event loop + WiFi + network_prov_mgr. Idempotent. */
esp_err_t bridge_prov_init(void);

bool bridge_prov_has_credentials(void);

/* Erase provisioning state via network_prov_mgr_reset_wifi_provisioning(). */
esp_err_t bridge_prov_clear_credentials(void);

/* Clear provisioning state and reboot (button recovery). */
void bridge_prov_factory_reset(void);

void bridge_prov_get_status(bridge_prov_status_t *out);

const char *bridge_prov_state_name(bridge_prov_state_t state);

#ifdef __cplusplus
}
#endif