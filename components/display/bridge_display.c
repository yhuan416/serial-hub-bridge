#include "bridge_display.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "qrcodegen.h"
#include "sdkconfig.h"

#include "spilcd.h"

#define TAG "display"

#define FB_W 160
#define FB_H 80

/* Implemented in spilcd.c */
void display_draw_bitmap(int x, int y, int w, int h, const uint16_t *data);

static bool s_ready;

esp_err_t bridge_display_init(void)
{
    if (s_ready) {
        return ESP_OK;
    }
    esp_err_t err = spilcd_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "spilcd_init failed: %s", esp_err_to_name(err));
        return err;
    }
    s_ready = true;
    spilcd_clear(BLACK);
    ESP_LOGI(TAG, "160x80 ST7789 ready");
    return ESP_OK;
}

bool bridge_display_ready(void)
{
    return s_ready;
}

void bridge_display_show_qr(const char *payload)
{
    if (!s_ready || !payload) {
        return;
    }

    uint16_t *fb = (uint16_t *)heap_caps_malloc(FB_W * FB_H * sizeof(uint16_t), MALLOC_CAP_DMA);
    uint8_t *qr = (uint8_t *)malloc(qrcodegen_BUFFER_LEN_MAX);
    uint8_t *tmp = (uint8_t *)malloc(qrcodegen_BUFFER_LEN_MAX);
    if (!fb || !qr || !tmp) {
        ESP_LOGE(TAG, "OOM for QR framebuffer");
        free(fb);
        free(qr);
        free(tmp);
        return;
    }

    for (int i = 0; i < FB_W * FB_H; i++) {
        fb[i] = WHITE;
    }

    if (!qrcodegen_encodeText(payload, tmp, qr, qrcodegen_Ecc_LOW,
                              qrcodegen_VERSION_MIN, qrcodegen_VERSION_MAX,
                              qrcodegen_Mask_AUTO, true)) {
        ESP_LOGW(TAG, "QR encode failed (payload %u bytes)", (unsigned)strlen(payload));
        free(fb);
        free(qr);
        free(tmp);
        return;
    }

    int size = qrcodegen_getSize(qr);
    /* The framebuffer is white, so the surrounding white pixels are the
     * quiet zone. Use the largest integer scale that fits the 80 px height
     * (33 modules -> 2, i.e. 66x66 instead of the previous 33x33). */
    int scale = FB_H / size;
    if (scale < 1) {
        scale = 1;
    }
    int px = size * scale;
    int ox = (FB_W - px) / 2;
    int oy = (FB_H - px) / 2;
    if (ox < 0) {
        ox = 0;
    }
    if (oy < 0) {
        oy = 0;
    }

    for (int y = 0; y < size; y++) {
        for (int x = 0; x < size; x++) {
            if (!qrcodegen_getModule(qr, x, y)) {
                continue;
            }
            int bx = ox + x * scale;
            int by = oy + y * scale;
            for (int dy = 0; dy < scale; dy++) {
                for (int dx = 0; dx < scale; dx++) {
                    int px_x = bx + dx;
                    int px_y = by + dy;
                    if (px_x >= 0 && px_x < FB_W && px_y >= 0 && px_y < FB_H) {
                        fb[px_y * FB_W + px_x] = BLACK;
                    }
                }
            }
        }
    }

    display_draw_bitmap(0, 0, FB_W, FB_H, fb);
    free(fb);
    free(qr);
    free(tmp);
    ESP_LOGI(TAG, "QR displayed (%d modules, scale %d, %dpx)", size, scale, px);
}

void bridge_display_hide_qr(void)
{
    if (s_ready) {
        spilcd_clear(BLACK);
    }
}

void bridge_display_show_status(const char *ssid, const char *ip, bool connected, const char *status)
{
    if (!s_ready) {
        return;
    }

    char line[40];
    spilcd_clear(BLACK);
    spilcd_show_string(0, 0, FB_W, 16, 12, "serial-hub-bridge", CYAN);

    if (connected) {
        snprintf(line, sizeof(line), "SSID:%s", ssid ? ssid : "-");
        spilcd_show_string(0, 14, FB_W, 16, 12, line, MAGENTA);
        snprintf(line, sizeof(line), "%s", ip ? ip : "-");
        spilcd_show_string(0, 28, FB_W, 16, 12, line, WHITE);
    } else {
        spilcd_show_string(0, 14, FB_W, 16, 12, "WiFi:Disconnected", RED);
        spilcd_show_string(0, 28, FB_W, 16, 12, "No IP", GRAY);
    }

    snprintf(line, sizeof(line), "%s", status ? status : "");
    spilcd_show_string(0, 42, FB_W, 16, 12, line, YELLOW);

    /* Bottom-right wiring hint: keep it out of the status text's way and
     * leave the last 12 px row fully visible (y=64..75). */
    snprintf(line, sizeof(line), "U1 TX%d RX%d",
             CONFIG_BRIDGE_UART_TX_GPIO, CONFIG_BRIDGE_UART_RX_GPIO);
    spilcd_show_string(82, 52, 78, 12, 12, line, GREEN);
#if CONFIG_BRIDGE_PORT_COUNT > 1
    snprintf(line, sizeof(line), "U2 TX%d RX%d",
             CONFIG_BRIDGE_UART2_TX_GPIO, CONFIG_BRIDGE_UART2_RX_GPIO);
    spilcd_show_string(82, 66, 78, 12, 12, line, CYAN);
#else
    spilcd_show_string(82, 66, 78, 12, 12, "U2 off", GRAY);
#endif
}