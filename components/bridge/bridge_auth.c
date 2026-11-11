#include "bridge_auth.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_random.h"
#include "nvs.h"
#include "sdkconfig.h"

#define TAG "bridge_auth"
#define NVS_NS "shb"
#define NVS_KEY "token"
#define TOKEN_HEX_LEN 32

static char s_token[TOKEN_HEX_LEN + 1];
static bool s_enabled;

static void make_token(char out[TOKEN_HEX_LEN + 1])
{
    uint8_t raw[TOKEN_HEX_LEN / 2];
    esp_fill_random(raw, sizeof(raw));
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < sizeof(raw); i++) {
        out[i * 2] = hex[raw[i] >> 4];
        out[i * 2 + 1] = hex[raw[i] & 0x0f];
    }
    out[TOKEN_HEX_LEN] = '\0';
}

esp_err_t bridge_auth_init(void)
{
    if (s_enabled) {
        return ESP_OK;
    }
    if (CONFIG_BRIDGE_AUTH_TOKEN[0]) {
        strncpy(s_token, CONFIG_BRIDGE_AUTH_TOKEN, sizeof(s_token) - 1);
        s_token[sizeof(s_token) - 1] = '\0';
        s_enabled = true;
        ESP_LOGI(TAG, "auth token from Kconfig: %s", s_token);
        return ESP_OK;
    }

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open: %s", esp_err_to_name(err));
        return err;
    }

    size_t len = sizeof(s_token);
    err = nvs_get_str(h, NVS_KEY, s_token, &len);
    if (err != ESP_OK || len <= 1) {
        make_token(s_token);
        err = nvs_set_str(h, NVS_KEY, s_token);
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "persist token: %s", esp_err_to_name(err));
        } else {
            ESP_LOGI(TAG, "generated auth token: %s", s_token);
        }
    } else {
        ESP_LOGI(TAG, "auth token from NVS: %s", s_token);
    }
    nvs_close(h);

    if (err == ESP_OK || s_token[0]) {
        s_enabled = true;
        return ESP_OK;
    }
    return err;
}

const char *bridge_auth_token(void)
{
    return s_enabled ? s_token : NULL;
}

bool bridge_auth_enabled(void)
{
    return s_enabled;
}
