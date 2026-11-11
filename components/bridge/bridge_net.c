#include "bridge_net.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"

#include "bridge_button.h"
#include "bridge_proto.h"
#include "bridge_prov.h"

#define TAG "bridge_net"

#define RECV_CHUNK 512

static bool send_all(int fd, const char *buf, size_t len)
{
    size_t sent = 0;
    while (sent < len) {
        int n = send(fd, buf + sent, len - sent, 0);
        if (n <= 0) {
            return false;
        }
        sent += (size_t)n;
    }
    return true;
}

static void send_frame(int fd, const char *json)
{
    if (!json) {
        return;
    }
    if (send_all(fd, json, strlen(json))) {
        send_all(fd, "\n", 1);
    }
}

static void frame_too_large(int fd, uint32_t max_frame)
{
    char buf[192];
    snprintf(buf, sizeof(buf),
             "{\"id\":null,\"ok\":false,\"error\":{\"code\":\"frame_too_large\","
             "\"message\":\"frame exceeds max_frame=%u\"}}",
             (unsigned)max_frame);
    send_frame(fd, buf);
}

static void session_run(bridge_ctx_t *ctx, int fd)
{
    size_t cap = (size_t)ctx->max_frame + 1;
    char *line = (char *)malloc(cap);
    if (!line) {
        ESP_LOGE(TAG, "OOM for line buffer");
        return;
    }

    size_t used = 0;
    bool authed = !(ctx->token && ctx->token[0]);
    bool close_after = false;
    char tmp[RECV_CHUNK];

    for (;;) {
        int n = recv(fd, tmp, sizeof(tmp), 0);
        if (n <= 0) {
            break;
        }

        for (int i = 0; i < n; i++) {
            char c = tmp[i];
            if (c == '\n') {
                while (used > 0 && (line[used - 1] == '\r' || line[used - 1] == ' ' ||
                                    line[used - 1] == '\t')) {
                    used--;
                }
                size_t start = 0;
                while (start < used && (line[start] == ' ' || line[start] == '\t')) {
                    start++;
                }
                line[used] = '\0';
                if (used > start) {
                    char *resp = bridge_proto_handle(ctx, &authed, line + start, &close_after);
                    if (resp) {
                        send_frame(fd, resp);
                        free(resp);
                    }
                }
                used = 0;
                if (close_after) {
                    goto done;
                }
            } else {
                if (used >= cap - 1) {
                    frame_too_large(fd, ctx->max_frame);
                    goto done;
                }
                line[used++] = c;
            }
        }
    }

done:
    free(line);
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
    if (listen(srv, 1) != 0) {
        ESP_LOGE(TAG, "listen() failed: errno=%d", errno);
        close(srv);
        return;
    }

    ESP_LOGI(TAG, "listening on tcp://0.0.0.0:%u (proto %s)", (unsigned)port, BRIDGE_PROTO_VERSION);

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

        ESP_LOGI(TAG, "client %s:%u connected", inet_ntoa(peer.sin_addr),
                 (unsigned)ntohs(peer.sin_port));
        session_run(ctx, fd);
        shutdown(fd, SHUT_RDWR);
        close(fd);
        ESP_LOGI(TAG, "client disconnected");
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

    bridge_button_init();
    serve_forever(ctx, cfg->tcp_port);
}