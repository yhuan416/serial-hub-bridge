/*
 * serial-hub-bridge — ESP32-S3 wireless serial bridge (v0 device side).
 *
 * UART <-> TCP/NDJSON RPC, implementing
 * serial-hub-mcp/docs/remote-link-protocol.md (proto v0).
 */

#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "sdkconfig.h"

#include "bridge.h"
#include "bridge_display.h"
#include "bridge_event.h"
#include "bridge_net.h"
#include "bridge_port.h"
#include "bridge_proto.h"
#include "bridge_sd.h"

static const char *TAG = "app";

void app_main(void)
{
    ESP_LOGI(TAG, "serial-hub-bridge %s (proto %s)", BRIDGE_FW_VERSION, BRIDGE_PROTO_VERSION);
    bridge_proto_reset();
    if (bridge_event_init() != ESP_OK) {
        ESP_LOGW(TAG, "event log init failed");
    }

    bridge_port_config_t pc = {
        .uart_num = (uart_port_t)CONFIG_BRIDGE_UART_NUM,
        .tx_pin = CONFIG_BRIDGE_UART_TX_GPIO,
        .rx_pin = CONFIG_BRIDGE_UART_RX_GPIO,
        .rts_pin = CONFIG_BRIDGE_UART_RTS_GPIO,
        .cts_pin = CONFIG_BRIDGE_UART_CTS_GPIO,
        .dtr_gpio = CONFIG_BRIDGE_UART_DTR_GPIO,
        .baud_rate = CONFIG_BRIDGE_UART_BAUD,
        .data_bits = CONFIG_BRIDGE_UART_DATA_BITS,
        .stop_bits = CONFIG_BRIDGE_UART_STOP_BITS,
        .parity = "none",
        .flow_control = "none",
        .ring_bytes = CONFIG_BRIDGE_RING_BYTES,
        .uart_rx_buf = CONFIG_BRIDGE_UART_RX_BUF,
        .log_entries = CONFIG_BRIDGE_LOG_ENTRIES,
        .log_line_max = CONFIG_BRIDGE_LOG_LINE_MAX,
        .log_partial_max = CONFIG_BRIDGE_LOG_PARTIAL_MAX,
    };

#if CONFIG_BRIDGE_LCD_ENABLE
    if (bridge_display_init() != ESP_OK) {
        ESP_LOGW(TAG, "LCD init failed; QR only in the serial log");
    }
#endif
    bridge_port_t *port = bridge_port_init(&pc);
    if (!port) {
        ESP_LOGE(TAG, "failed to init UART bridge port");
        return;
    }

    if (bridge_sd_init(bridge_display_ready(), port) != ESP_OK) {
        ESP_LOGW(TAG, "SD log unavailable; RAM-only mode");
    }

    static bridge_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.port = port;
    ctx.token = CONFIG_BRIDGE_AUTH_TOKEN[0] ? CONFIG_BRIDGE_AUTH_TOKEN : NULL;
    ctx.max_frame = CONFIG_BRIDGE_MAX_FRAME;
    ctx.max_hold_ms = CONFIG_BRIDGE_MAX_HOLD_MS;
    ctx.scratch_cap = CONFIG_BRIDGE_MAX_FRAME;
    ctx.scratch = (uint8_t *)heap_caps_malloc(ctx.scratch_cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!ctx.scratch) {
        ctx.scratch = (uint8_t *)heap_caps_malloc(ctx.scratch_cap, MALLOC_CAP_8BIT);
    }
    if (!ctx.scratch) {
        ESP_LOGE(TAG, "failed to allocate %u-byte scratch buffer", (unsigned)ctx.scratch_cap);
        return;
    }

    bridge_net_config_t nc = {
        .tcp_port = CONFIG_BRIDGE_TCP_PORT,
    };

    ESP_LOGI(TAG, "auth=%s max_frame=%u max_hold_ms=%u",
             ctx.token ? "on" : "off", (unsigned)ctx.max_frame, (unsigned)ctx.max_hold_ms);

    bridge_net_run(&ctx, &nc);
}