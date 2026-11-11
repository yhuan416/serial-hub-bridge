/*
 * Long-press recovery button (default GPIO0, 5 s):
 * resets WiFi provisioning and reboots into BLE/QR provisioning mode.
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t bridge_button_init(void);

#ifdef __cplusplus
}
#endif