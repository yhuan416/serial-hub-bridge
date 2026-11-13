#include "bridge_port.h"
#include "bridge.h"
#include "bridge_event.h"

#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define TAG "bridge_port"

#define PORT_EV_RX BIT0
#define RX_TASK_STACK 4096
#define RX_TASK_PRIO 12
#define UART_EVT_QUEUE_LEN 20

struct bridge_port {
    bridge_port_config_t cfg;

    bridge_ring_t ring;
    SemaphoreHandle_t lock;
    EventGroupHandle_t events;
    QueueHandle_t uart_queue;
    TaskHandle_t rx_task;
    volatile bool running;

    uint64_t tx_offset;

    /* line log */
    bridge_log_meta_t *log_meta;
    char *log_text;
    uint32_t log_cap; /* entries */
    uint32_t log_count;
    uint32_t log_head;
    uint32_t log_seq;

    /* partial RX line */
    char *partial;
    uint32_t partial_len;
    uint64_t partial_off;

    int dtr;
    int rts;
    uint32_t last_drop_event_ms;
    uint32_t uart_overflow_events;
    uint64_t uart_overflow_bytes;
};

uint64_t bridge_now_ms(void)
{
    return (uint64_t)(esp_timer_get_time() / 1000);
}

/* Big buffers prefer PSRAM; fall back to internal RAM. */
static void *port_alloc(size_t n)
{
    void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) {
        p = heap_caps_malloc(n, MALLOC_CAP_8BIT);
    }
    return p;
}

static void *port_calloc(size_t n)
{
    void *p = port_alloc(n);
    if (p) {
        memset(p, 0, n);
    }
    return p;
}

static const char *parity_str(uart_parity_t p)
{
    switch (p) {
    case UART_PARITY_EVEN:
        return "even";
    case UART_PARITY_ODD:
        return "odd";
    default:
        return "none";
    }
}

static uart_parity_t parse_parity(const char *s)
{
    if (s && strcmp(s, "even") == 0) {
        return UART_PARITY_EVEN;
    }
    if (s && strcmp(s, "odd") == 0) {
        return UART_PARITY_ODD;
    }
    return UART_PARITY_DISABLE;
}

static uart_word_length_t map_data_bits(uint8_t n)
{
    switch (n) {
    case 5:
        return UART_DATA_5_BITS;
    case 6:
        return UART_DATA_6_BITS;
    case 7:
        return UART_DATA_7_BITS;
    default:
        return UART_DATA_8_BITS;
    }
}

static uart_stop_bits_t map_stop_bits(uint8_t n)
{
    return n == 2 ? UART_STOP_BITS_2 : UART_STOP_BITS_1;
}

static void port_ingest(bridge_port_t *p, const uint8_t *data, size_t len)
{
    if (!p || len == 0) {
        return;
    }
    xSemaphoreTake(p->lock, portMAX_DELAY);
    uint64_t dropped_before = p->ring.dropped;
    bridge_ring_append(&p->ring, data, len);
    if (p->ring.dropped > dropped_before) {
        uint32_t now = (uint32_t)bridge_now_ms();
        if (p->last_drop_event_ms == 0 || now - p->last_drop_event_ms >= 10000) {
            p->last_drop_event_ms = now;
            bridge_event_push(BRIDGE_EVENT_WARN, "ring.drop",
                              "dropped %llu bytes (cap %zu, total %llu)",
                              (unsigned long long)(p->ring.dropped - dropped_before),
                              p->ring.cap, (unsigned long long)p->ring.dropped);
        }
    }
    xSemaphoreGive(p->lock);
    xEventGroupSetBits(p->events, PORT_EV_RX);
}

static void uart_rx_task(void *arg)
{
    bridge_port_t *p = (bridge_port_t *)arg;
    uint8_t buf[256];
    uart_event_t ev;

    for (;;) {
        if (!p->running) {
            break;
        }
        if (xQueueReceive(p->uart_queue, &ev, pdMS_TO_TICKS(500)) != pdTRUE) {
            continue;
        }

        switch (ev.type) {
        case UART_DATA: {
            size_t remaining = ev.size;
            while (remaining > 0) {
                size_t want = remaining < sizeof(buf) ? remaining : sizeof(buf);
                int n = uart_read_bytes(p->cfg.uart_num, buf, want, pdMS_TO_TICKS(20));
                if (n <= 0) {
                    break;
                }
                port_ingest(p, buf, (size_t)n);
                remaining -= (size_t)n;
            }
            break;
        }
        case UART_FIFO_OVF:
        case UART_BUFFER_FULL:
            /* Always make an overflow visible. ev.size is normally the number
             * of bytes still queued; a zero value means the driver could not
             * report an exact count (unknown, not necessarily zero loss). */
            p->uart_overflow_events++;
            if (ev.size > 0) {
                p->uart_overflow_bytes += ev.size;
            }
            ESP_LOGW(TAG, "UART%d RX overflow (%s, %u bytes%s)", (int)p->cfg.uart_num,
                     ev.type == UART_FIFO_OVF ? "fifo" : "buffer", (unsigned)ev.size,
                     ev.size ? "" : " unknown");
            bridge_event_push(BRIDGE_EVENT_WARN, "uart.overflow",
                              "%s overflow: %s%u bytes%s (events=%lu total=%llu)",
                              ev.type == UART_FIFO_OVF ? "fifo" : "buffer",
                              ev.size ? "" : "unknown ", (unsigned)ev.size,
                              ev.size ? "" : " (count unavailable)",
                              (unsigned long)p->uart_overflow_events,
                              (unsigned long long)p->uart_overflow_bytes);
            uart_flush_input(p->cfg.uart_num);
            xQueueReset(p->uart_queue);
            break;
        case UART_BREAK:
            ESP_LOGD(TAG, "UART%d break detected", (int)p->cfg.uart_num);
            break;
        case UART_PARITY_ERR:
        case UART_FRAME_ERR:
            ESP_LOGD(TAG, "UART%d error event %d", (int)p->cfg.uart_num, (int)ev.type);
            break;
        default:
            break;
        }
    }

    vTaskDelete(NULL);
}

bridge_port_t *bridge_port_init(const bridge_port_config_t *cfg)
{
    if (!cfg) {
        return NULL;
    }

    bridge_port_t *p = (bridge_port_t *)calloc(1, sizeof(bridge_port_t));
    if (!p) {
        ESP_LOGE(TAG, "OOM for port");
        return NULL;
    }
    p->cfg = *cfg;
    p->dtr = -1;
    p->rts = -1;
    p->tx_offset = 0;

    size_t ring_bytes = cfg->ring_bytes ? cfg->ring_bytes : (128 * 1024);
    uint8_t *ring_buf = (uint8_t *)port_alloc(ring_bytes);
    if (ring_buf) {
        bridge_ring_init_preallocated(&p->ring, ring_buf, ring_bytes);
    } else if (bridge_ring_init(&p->ring, ring_bytes) != 0) {
        ESP_LOGE(TAG, "ring init failed (%u bytes)", (unsigned)ring_bytes);
        free(p);
        return NULL;
    }

    p->lock = xSemaphoreCreateMutex();
    p->events = xEventGroupCreate();
    if (!p->lock || !p->events) {
        ESP_LOGE(TAG, "sync primitives failed");
        goto fail;
    }

    p->log_cap = cfg->log_entries;
    if (p->log_cap > 0 && cfg->log_line_max > 0) {
        p->log_meta = (bridge_log_meta_t *)port_calloc(p->log_cap * sizeof(bridge_log_meta_t));
        p->log_text = (char *)port_alloc((size_t)p->log_cap * cfg->log_line_max);
        if (!p->log_meta || !p->log_text) {
            ESP_LOGW(TAG, "log disabled (OOM)");
            free(p->log_meta);
            free(p->log_text);
            p->log_meta = NULL;
            p->log_text = NULL;
            p->log_cap = 0;
        }
    }
    if (cfg->log_partial_max > 0) {
        p->partial = (char *)port_alloc(cfg->log_partial_max);
        if (!p->partial) {
            ESP_LOGW(TAG, "RX line log disabled (OOM)");
        }
    }
    p->partial_off = 0;

    if (cfg->dtr_gpio >= 0) {
        gpio_config_t io = {
            .pin_bit_mask = 1ULL << (uint32_t)cfg->dtr_gpio,
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        gpio_config(&io);
        gpio_set_level((gpio_num_t)cfg->dtr_gpio, 1); /* deasserted (active low) */
    }

    uart_config_t uc = {
        .baud_rate = cfg->baud_rate,
        .data_bits = map_data_bits(cfg->data_bits),
        .parity = parse_parity(cfg->parity),
        .stop_bits = map_stop_bits(cfg->stop_bits),
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    int rx_buf = cfg->uart_rx_buf > 0 ? cfg->uart_rx_buf : 8192;
    esp_err_t err = uart_driver_install(cfg->uart_num, rx_buf, rx_buf, UART_EVT_QUEUE_LEN,
                                        &p->uart_queue, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_driver_install(%d) failed: %s", (int)cfg->uart_num, esp_err_to_name(err));
        goto fail;
    }
    p->running = true;

    err = uart_param_config(cfg->uart_num, &uc);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_param_config failed: %s", esp_err_to_name(err));
        goto fail;
    }

    err = uart_set_pin(cfg->uart_num, cfg->tx_pin, cfg->rx_pin,
                       cfg->rts_pin >= 0 ? cfg->rts_pin : UART_PIN_NO_CHANGE,
                       cfg->cts_pin >= 0 ? cfg->cts_pin : UART_PIN_NO_CHANGE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_set_pin failed: %s", esp_err_to_name(err));
        goto fail;
    }
    if (cfg->rx_pin >= 0) {
        gpio_set_pull_mode((gpio_num_t)cfg->rx_pin, GPIO_PULLUP_ONLY);
    }

    if (xTaskCreate(uart_rx_task, "bridge_uart_rx", RX_TASK_STACK, p, RX_TASK_PRIO, &p->rx_task) != pdPASS) {
        ESP_LOGE(TAG, "RX task create failed");
        goto fail;
    }

    ESP_LOGI(TAG, "uart%d ready TX=%d RX=%d RTS=%d CTS=%d DTR=%d baud=%d ring=%uKB log=%ux%uB",
             (int)cfg->uart_num, cfg->tx_pin, cfg->rx_pin, cfg->rts_pin, cfg->cts_pin,
             cfg->dtr_gpio, cfg->baud_rate, (unsigned)(cfg->ring_bytes / 1024),
             (unsigned)p->log_cap, (unsigned)cfg->log_line_max);
    return p;

fail:
    if (p->uart_queue) {
        uart_driver_delete(cfg->uart_num);
    }
    if (p->events) {
        vEventGroupDelete(p->events);
    }
    if (p->lock) {
        vSemaphoreDelete(p->lock);
    }
    free(p->partial);
    free(p->log_text);
    free(p->log_meta);
    bridge_ring_free(&p->ring);
    free(p);
    return NULL;
}

void bridge_port_deinit(bridge_port_t *p)
{
    if (!p) {
        return;
    }
    p->running = false;
    if (p->rx_task) {
        vTaskDelay(pdMS_TO_TICKS(20));
        vTaskDelete(p->rx_task);
    }
    uart_driver_delete(p->cfg.uart_num);
    vEventGroupDelete(p->events);
    vSemaphoreDelete(p->lock);
    free(p->partial);
    free(p->log_text);
    free(p->log_meta);
    bridge_ring_free(&p->ring);
    free(p);
}

const bridge_port_config_t *bridge_port_config(const bridge_port_t *p)
{
    return p ? &p->cfg : NULL;
}

bool bridge_port_can_set_lines(const bridge_port_t *p)
{
    return p && (p->cfg.rts_pin >= 0 || p->cfg.dtr_gpio >= 0);
}

esp_err_t bridge_port_configure(bridge_port_t *p,
                                int baud_rate,
                                uint8_t data_bits,
                                uint8_t stop_bits,
                                const char *parity,
                                const char *flow_control)
{
    if (!p) {
        return ESP_ERR_INVALID_ARG;
    }
    bool want_rtscts = flow_control && strcmp(flow_control, "rtscts") == 0;
    if (want_rtscts && (p->cfg.rts_pin < 0 || p->cfg.cts_pin < 0)) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    esp_err_t err;

    err = uart_set_baudrate(p->cfg.uart_num, (uint32_t)baud_rate);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_set_baudrate(%d): %s", baud_rate, esp_err_to_name(err));
        return err;
    }
    err = uart_set_word_length(p->cfg.uart_num, map_data_bits(data_bits));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_set_word_length(%u): %s", (unsigned)data_bits, esp_err_to_name(err));
        return err;
    }
    err = uart_set_parity(p->cfg.uart_num, parse_parity(parity));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_set_parity(%s): %s", parity ? parity : "?", esp_err_to_name(err));
        return err;
    }
    err = uart_set_stop_bits(p->cfg.uart_num, map_stop_bits(stop_bits));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_set_stop_bits(%u): %s", (unsigned)stop_bits, esp_err_to_name(err));
        return err;
    }
    err = uart_set_hw_flow_ctrl(p->cfg.uart_num,
                                want_rtscts ? UART_HW_FLOWCTRL_CTS_RTS : UART_HW_FLOWCTRL_DISABLE,
                                want_rtscts ? 127 : 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_set_hw_flow_ctrl(%s): %s", flow_control ? flow_control : "?",
                 esp_err_to_name(err));
        return err;
    }

    p->cfg.baud_rate = baud_rate;
    p->cfg.data_bits = data_bits;
    p->cfg.stop_bits = stop_bits;
    strncpy(p->cfg.parity, parity ? parity : "none", sizeof(p->cfg.parity) - 1);
    p->cfg.parity[sizeof(p->cfg.parity) - 1] = '\0';
    strncpy(p->cfg.flow_control, flow_control ? flow_control : "none",
            sizeof(p->cfg.flow_control) - 1);
    p->cfg.flow_control[sizeof(p->cfg.flow_control) - 1] = '\0';
    ESP_LOGI(TAG, "uart%d config %d %u-%s-%u flow=%s",
             (int)p->cfg.uart_num, baud_rate, (unsigned)data_bits, parity_str(parse_parity(parity)),
             (unsigned)stop_bits, flow_control ? flow_control : "none");
    return ESP_OK;
}

int bridge_port_write(bridge_port_t *p,
                      const uint8_t *data,
                      size_t len,
                      uint64_t *tx_offset,
                      uint32_t *tx_log_seq)
{
    if (!p || !data || len == 0) {
        return 0;
    }
    int n = uart_write_bytes(p->cfg.uart_num, data, (size_t)len);
    if (n < 0) {
        return -1;
    }

    xSemaphoreTake(p->lock, portMAX_DELAY);
    p->tx_offset += len;
    uint64_t new_off = p->tx_offset;
    xSemaphoreGive(p->lock);

    if (tx_offset) {
        *tx_offset = new_off;
    }
    if (tx_log_seq) {
        *tx_log_seq = 0;
    }
    return n;
}

uint64_t bridge_port_rx_offset(bridge_port_t *p)
{
    if (!p) {
        return 0;
    }
    xSemaphoreTake(p->lock, portMAX_DELAY);
    uint64_t v = p->ring.end;
    xSemaphoreGive(p->lock);
    return v;
}

void bridge_port_stat(bridge_port_t *p, bridge_port_stat_t *out)
{
    if (!p || !out) {
        return;
    }
    xSemaphoreTake(p->lock, portMAX_DELAY);
    out->ring_start_offset = p->ring.start;
    out->ring_end_offset = p->ring.end;
    out->ring_used = p->ring.used;
    out->ring_capacity = p->ring.cap;
    out->ring_dropped_bytes = p->ring.dropped;
    out->rx_bytes = p->ring.end;
    out->tx_bytes = p->tx_offset;
    out->uart_overflow_bytes = p->uart_overflow_bytes;
    out->uart_overflow_events = p->uart_overflow_events;
    xSemaphoreGive(p->lock);
}

bool bridge_port_read(bridge_port_t *p,
                      const bridge_read_args_t *args,
                      uint64_t started_ms,
                      bridge_read_result_t *res,
                      uint8_t *out,
                      size_t out_cap)
{
    if (!p || !args || !res) {
        return false;
    }

    xSemaphoreTake(p->lock, portMAX_DELAY);
    bridge_read_eval(&p->ring, args, bridge_now_ms(), started_ms, res);
    if (res->done) {
        size_t want = res->data_len < out_cap ? res->data_len : out_cap;
        size_t n = bridge_ring_copy(&p->ring, res->start, out, want);
        res->data_len = n;
        res->end = res->start + n;
    }
    xSemaphoreGive(p->lock);
    return res->done;
}

void bridge_port_wait(bridge_port_t *p, uint32_t ms)
{
    if (!p) {
        return;
    }
    TickType_t ticks = ms == 0 ? 0 : pdMS_TO_TICKS(ms);
    xEventGroupWaitBits(p->events, PORT_EV_RX, pdTRUE, pdTRUE, ticks);
}

esp_err_t bridge_port_set_lines(bridge_port_t *p, int dtr, int rts, int *out_dtr, int *out_rts)
{
    if (!p) {
        return ESP_ERR_INVALID_ARG;
    }
    if (dtr >= 0 && p->cfg.dtr_gpio < 0) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (rts >= 0 && p->cfg.rts_pin < 0) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    if (dtr >= 0) {
        gpio_set_level((gpio_num_t)p->cfg.dtr_gpio, dtr ? 0 : 1); /* active low */
        p->dtr = dtr;
    }
    if (rts >= 0) {
        uart_set_rts(p->cfg.uart_num, rts ? 0 : 1); /* active low */
        p->rts = rts;
    }

    if (out_dtr) {
        *out_dtr = p->dtr;
    }
    if (out_rts) {
        *out_rts = p->rts;
    }
    return ESP_OK;
}

esp_err_t bridge_port_send_break(bridge_port_t *p, uint32_t duration_ms)
{
    if (!p) {
        return ESP_ERR_INVALID_ARG;
    }
    if (duration_ms == 0) {
        duration_ms = 300;
    }
    if (duration_ms > 10000) {
        duration_ms = 10000;
    }

    uint32_t baud = p->cfg.baud_rate > 0 ? (uint32_t)p->cfg.baud_rate : 115200;
    uint32_t bits = (baud / 1000) * duration_ms + ((baud % 1000) * duration_ms) / 1000;
    if (bits == 0) {
        bits = 1;
    }
    int r = uart_write_bytes_with_break(p->cfg.uart_num, NULL, 0, (int)bits);
    return r < 0 ? ESP_FAIL : ESP_OK;
}

/* ── log accessors ─────────────────────────────────────────────────────── */

uint32_t bridge_port_log_count(bridge_port_t *p)
{
    if (!p) {
        return 0;
    }
    xSemaphoreTake(p->lock, portMAX_DELAY);
    uint32_t n = p->log_count;
    xSemaphoreGive(p->lock);
    return n;
}

bool bridge_port_log_earliest_seq(bridge_port_t *p, uint32_t *seq)
{
    if (!p) {
        return false;
    }
    xSemaphoreTake(p->lock, portMAX_DELAY);
    bool ok = p->log_count > 0 && p->log_meta != NULL;
    if (ok && seq) {
        *seq = p->log_meta[p->log_head].seq;
    }
    xSemaphoreGive(p->lock);
    return ok;
}

uint32_t bridge_port_log_latest_seq(bridge_port_t *p)
{
    if (!p) {
        return 0;
    }
    xSemaphoreTake(p->lock, portMAX_DELAY);
    uint32_t n = p->log_seq;
    xSemaphoreGive(p->lock);
    return n;
}

uint32_t bridge_port_log_used_bytes(bridge_port_t *p)
{
    if (!p || !p->log_meta) {
        return 0;
    }
    uint32_t total = 0;
    xSemaphoreTake(p->lock, portMAX_DELAY);
    for (uint32_t i = 0; i < p->log_count; i++) {
        total += p->log_meta[(p->log_head + i) % p->log_cap].text_len;
    }
    xSemaphoreGive(p->lock);
    return total;
}

uint32_t bridge_port_log_capacity_bytes(bridge_port_t *p)
{
    if (!p || !p->log_meta) {
        return 0;
    }
    return p->log_cap * p->cfg.log_line_max;
}

bool bridge_port_log_get(bridge_port_t *p,
                         uint32_t index,
                         bridge_log_meta_t *meta,
                         char *text,
                         size_t text_cap)
{
    if (!p || !p->log_meta) {
        return false;
    }
    xSemaphoreTake(p->lock, portMAX_DELAY);
    bool ok = index < p->log_count;
    if (ok) {
        uint32_t slot = (p->log_head + index) % p->log_cap;
        bridge_log_meta_t m = p->log_meta[slot];
        if (text && text_cap > 0) {
            uint32_t n = m.text_len;
            if (n > text_cap - 1) {
                n = (uint32_t)(text_cap - 1);
            }
            memcpy(text, p->log_text + (size_t)slot * p->cfg.log_line_max, n);
            text[n] = '\0';
        }
        if (meta) {
            *meta = m;
        }
    }
    xSemaphoreGive(p->lock);
    return ok;
}

void bridge_port_log_clear(bridge_port_t *p, bool reset_seq)
{
    if (!p) {
        return;
    }
    xSemaphoreTake(p->lock, portMAX_DELAY);
    p->log_count = 0;
    p->log_head = 0;
    if (reset_seq) {
        p->log_seq = 0;
    }
    xSemaphoreGive(p->lock);
}

void bridge_port_log_set_seq(bridge_port_t *p, uint32_t seq)
{
    if (!p) {
        return;
    }
    xSemaphoreTake(p->lock, portMAX_DELAY);
    if (seq > p->log_seq) {
        p->log_seq = seq;
    }
    xSemaphoreGive(p->lock);
}

void bridge_port_log_flush_partial(bridge_port_t *p)
{
    if (!p || !p->partial || p->partial_len == 0) {
        return;
    }
    xSemaphoreTake(p->lock, portMAX_DELAY);
    if (p->partial_len > 0) {
        p->partial_len = 0;
        p->partial_off = p->ring.end;
    }
    xSemaphoreGive(p->lock);
}