/*
 * Bridge HELLO token: compile-time override (CONFIG_BRIDGE_AUTH_TOKEN) or a
 * per-device token generated once and stored in NVS.
 *
 * v0 is plaintext; the token prevents accidental/opportunistic use, not a
 * network attacker. Use a trusted LAN or VPN/SSH tunnel for transport security.
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Call after nvs_flash_init(). Returns ESP_OK when a token is available. */
esp_err_t bridge_auth_init(void);
const char *bridge_auth_token(void);
bool bridge_auth_enabled(void);

#ifdef __cplusplus
}
#endif
