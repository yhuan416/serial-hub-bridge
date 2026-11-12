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
#include "bridge_time.h"

static const char *TAG = "app";

void app_main(void)
{
    ESP_LOGI(TAG, "serial-hub-bridge %s (proto %s)", BRIDGE_FW_VERSION, BRIDGE_PROTO_VERSION);
    bridge_proto_reset();
    bridge_time_init();
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
    const int port_count = CONFIG_BRIDGE_PORT_COUNT;
    bridge_port_t *ports[BRIDGE_MAX_PORTS] = {0};
    pc.ring_bytes = CONFIG_BRIDGE_RING_BYTES / (port_count > 0 ? port_count : 1);
    pc.sd_port = 0;
    ports[0] = bridge_port_init(&pc);
    if (!ports[0]) {
        ESP_LOGE(TAG, "failed to init UART bridge port 0");
        return;
    }
    if (port_count > 1) {
        bridge_port_config_t pc1 = pc;
        pc1.uart_num = (uart_port_t)CONFIG_BRIDGE_UART2_NUM;
        pc1.tx_pin = CONFIG_BRIDGE_UART2_TX_GPIO;
        pc1.rx_pin = CONFIG_BRIDGE_UART2_RX_GPIO;
        pc1.baud_rate = CONFIG_BRIDGE_UART2_BAUD;
        pc1.sd_port = 1;
        ports[1] = bridge_port_init(&pc1);
        if (!ports[1]) {
            ESP_LOGW(TAG, "UART2 bridge port init failed; continuing single-port");
        }
    }

    const int actual_ports = ports[1] ? 2 : 1;
    if (bridge_sd_init(bridge_display_ready(), ports, actual_ports) != ESP_OK) {
        ESP_LOGW(TAG, "SD log unavailable; RAM-only mode");
    }

    static bridge_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.port_count = actual_ports;
    ctx.ports[0] = ports[0];
    ctx.ports[1] = ports[1];
    ctx.port_ids[0] = "uart1";
    ctx.port_ids[1] = "uart2";
    ctx.port = ports[0];
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