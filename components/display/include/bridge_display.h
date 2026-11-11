/*
 * 160x80 ST7789 status/QR display (Alientek ESP32-S3 board pins).
 * Mirrors the display used by esp32_ble_scan_tools.
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t bridge_display_init(void);
bool bridge_display_ready(void);

/* Draw a provisioning QR. Payload is the network_provisioning QR JSON. */
void bridge_display_show_qr(const char *payload);
void bridge_display_hide_qr(void);

/* Simple status screen (SSID/IP/BLE state). */
void bridge_display_show_status(const char *ssid, const char *ip,
                                bool connected, const char *status);

#ifdef __cplusplus
}
#endif