#include "bridge_net.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"

#include "bridge_button.h"
#include "bridge_v1.h"
#include "bridge_prov.h"
#include "bridge_auth.h"

#define TAG "bridge_net"

#define MIN_INTERNAL_HEAP (24 * 1024)

/* Session buffers are large and long-lived; keep them out of internal DRAM
 * so Wi-Fi/PHY timer allocations still succeed under log/export load. */
static void *net_alloc(size_t n)
{
    void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) {
        p = malloc(n);
    }
    return p;
}

typedef struct {
    bridge_ctx_t ctx;
    int fd;
} session_arg_t;

static void session_task(void *arg)
{
    session_arg_t *a = (session_arg_t *)arg;
    if (!a) {
        vTaskDelete(NULL);
        return;
    }
    bridge_v1_session(&a->ctx, a->fd);
    shutdown(a->fd, SHUT_RDWR);
    close(a->fd);
    if (a->ctx.scratch) free(a->ctx.scratch);
    free(a);
    ESP_LOGI(TAG, "client session ended");
    vTaskDelete(NULL);
}

static void serve_forever(bridge_ctx_t *ctx, uint16_t port)
{
    int srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) {
        ESP_LOGE(TAG, "socket() failed: errno=%d", errno);
        return;
    }

    int yes = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);

    if (bind(srv, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        ESP_LOGE(TAG, "bind(:%u) failed: errno=%d", (unsigned)port, errno);
        close(srv);
        return;
    }
    if (listen(srv, 4) != 0) {
        ESP_LOGE(TAG, "listen() failed: errno=%d", errno);
        close(srv);
        return;
    }

    ESP_LOGI(TAG, "listening on tcp://0.0.0.0:%u", (unsigned)port);

    for (;;) {
        struct sockaddr_in peer;
        socklen_t plen = sizeof(peer);
        int fd = accept(srv, (struct sockaddr *)&peer, &plen);
        if (fd < 0) {
            ESP_LOGW(TAG, "accept() failed: errno=%d", errno);
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        int nodelay = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

        const size_t int_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        const size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        ESP_LOGI(TAG, "client %s:%u connected (heap internal=%u psram=%u)",
                 inet_ntoa(peer.sin_addr), (unsigned)ntohs(peer.sin_port),
                 (unsigned)int_free, (unsigned)psram_free);
        if (int_free < MIN_INTERNAL_HEAP) {
            ESP_LOGE(TAG, "rejecting session: internal heap too low (%u < %u)",
                     (unsigned)int_free, (unsigned)MIN_INTERNAL_HEAP);
            close(fd);
            continue;
        }
        session_arg_t *a = (session_arg_t *)calloc(1, sizeof(*a));
        if (!a) {
            close(fd);
            continue;
        }
        a->ctx = *ctx;
        a->ctx.port = ctx->ports[0];
        a->ctx.port_index = 0;
        a->ctx.scratch = (uint8_t *)net_alloc(a->ctx.scratch_cap ? a->ctx.scratch_cap : 32768);
        if (!a->ctx.scratch) {
            free(a);
            close(fd);
            continue;
        }
        a->ctx.scratch_cap = a->ctx.scratch_cap ? a->ctx.scratch_cap : 32768;
        a->fd = fd;
        if (xTaskCreate(session_task, "tcp_sess", 8192, a, 8, NULL) != pdPASS) {
            free(a->ctx.scratch);
            free(a);
            close(fd);
            continue;
        }
    }
}

void bridge_net_run(bridge_ctx_t *ctx, const bridge_net_config_t *cfg)
{
    if (!ctx || !cfg) {
        return;
    }

    esp_err_t err = bridge_prov_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "provisioning init failed: %s", esp_err_to_name(err));
        return;
    }

    ctx->token = bridge_auth_token();
    ESP_LOGI(TAG, "auth=%s", ctx->token ? "on" : "off");

    bridge_button_init();
    bridge_v1_start(ctx);
    serve_forever(ctx, cfg->tcp_port);
}