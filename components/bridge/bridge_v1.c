#include "bridge_v1.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "nvs.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "mbedtls/base64.h"

#include "bridge_port.h"
#include "bridge_sd.h"
#include "sdkconfig.h"

#define TAG "bridge_v1"

#define HDR 10
#define DATA_HDR 30
#define ACK_LEN 21
#define T_HELLO 1
#define T_HELLO_ACK 2
#define T_CONTROL 3
#define T_DATA 4
#define T_ACK 5
#define T_EVENT 6
#define T_ERROR 7
#define FLAG_GAP 1
#define FLAG_BATCH 4

#define CAP_DEFAULT (256 * 1024)
#define CAP_MIN (32 * 1024)
#define CAP_MAX (2 * 1024 * 1024)
#define NVS_NS "shb"
#define NVS_CAP "cap_bytes"
#define SPILL_CHUNK (32 * 1024)
#define SPILL_MAX 48
#define BATCH_MAX 4096
#define OUTQ 6
#define LINGER_MS 60000
#define WIN_DEFAULT (32 * 1024)
#define WIN_MAX (64 * 1024)
#define LEASE_DEFAULT 30000
#define MAX_CONNS 2
#define MAX_SESS 4

typedef struct v1_conn v1_conn_t;

typedef struct {
    bool used;
    char id[16];
    int slot;
    int port_index;
    uint64_t sent;
    uint64_t acked;
    uint32_t window;
    uint32_t lease_ms;
    bool writer;
    bool paused;
    uint64_t last_ms;
    v1_conn_t *conn;
} v1_sess_t;

typedef struct {
    bool live;
    uint64_t id;
    int port_index;
    uint8_t *bytes;
    size_t cap_bytes;
    uint64_t oldest;
    uint64_t ram_base;
    uint64_t newest;
    struct {
        uint64_t off;
        uint32_t len;
    } spill[SPILL_MAX];
    int spill_n;
    uint8_t *stage;
    uint32_t stage_len;
    uint64_t stage_off;
    bool stage_busy;
    uint64_t uart_cursor;
    bool cursor_set;
    bool uart_paused;
    int sessions;
    uint64_t linger_at;
    uint64_t linger_ack;
    v1_sess_t *writer;
} v1_cap_t;

struct v1_conn {
    bool used;
    int fd;
    bridge_ctx_t *ctx;
    bool authed;
    v1_sess_t sess[MAX_SESS];
    uint8_t *rx;
    size_t rx_cap;
    size_t rx_len;
    struct {
        uint8_t *p;
        uint16_t n;
        uint16_t off;
    } outq[OUTQ];
    uint8_t oq_r;
    uint8_t oq_n;
};

static v1_cap_t s_cap[BRIDGE_MAX_PORTS];
static v1_conn_t s_conn[MAX_CONNS];
static SemaphoreHandle_t s_mu;
static bridge_ctx_t *s_ctx;
static size_t s_cap_bytes = CAP_DEFAULT;

static size_t clamp_cap(uint32_t n)
{
    if (n < CAP_MIN) {
        n = CAP_MIN;
    }
    if (n > CAP_MAX) {
        n = CAP_MAX;
    }
    n &= ~0xfffu;
    if (n < CAP_MIN) {
        n = CAP_MIN;
    }
    return n;
}

static size_t cap_size(const v1_cap_t *cap)
{
    return cap && cap->cap_bytes ? cap->cap_bytes : s_cap_bytes;
}

static void load_cap_nvs(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    uint32_t v = 0;
    if (nvs_get_u32(h, NVS_CAP, &v) == ESP_OK && v) {
        s_cap_bytes = clamp_cap(v);
    }
    nvs_close(h);
}

static bool save_cap_nvs(size_t n)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return false;
    }
    esp_err_t err = nvs_set_u32(h, NVS_CAP, (uint32_t)n);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err == ESP_OK;
}

static uint64_t now_ms(void)
{
    return (uint64_t)(esp_timer_get_time() / 1000);
}

static uint64_t new_id(void)
{
    uint64_t id = 0;
    esp_fill_random(&id, sizeof(id));
    id = (id & ((1ULL << 52) - 1)) | 1;
    return id;
}

static void hex12(char out[16])
{
    uint8_t b[6];
    esp_fill_random(b, sizeof(b));
    static const char *hexd = "0123456789abcdef";
    for (int i = 0; i < 6; i++) {
        out[i * 2] = hexd[b[i] >> 4];
        out[i * 2 + 1] = hexd[b[i] & 0xf];
    }
    out[12] = '\0';
}

static bool enqueue_frame(v1_conn_t *c, uint8_t type, const uint8_t *payload, uint32_t len)
{
    if (!c || c->fd < 0) {
        return false;
    }
    if (c->oq_n >= OUTQ) {
        return false;
    }
    uint8_t *buf = (uint8_t *)malloc(HDR + len);
    if (!buf) {
        return false;
    }
    memset(buf, 0, HDR);
    buf[0] = 0x53;
    buf[1] = 0x48;
    buf[2] = 1;
    buf[3] = type;
    buf[6] = (uint8_t)(len & 0xff);
    buf[7] = (uint8_t)((len >> 8) & 0xff);
    buf[8] = (uint8_t)((len >> 16) & 0xff);
    buf[9] = (uint8_t)((len >> 24) & 0xff);
    if (len && payload) {
        memcpy(buf + HDR, payload, len);
    }
    uint8_t i = (uint8_t)((c->oq_r + c->oq_n) % OUTQ);
    c->outq[i].p = buf;
    c->outq[i].n = (uint16_t)(HDR + len);
    c->outq[i].off = 0;
    c->oq_n++;
    return true;
}

/* 调用方已持锁。只入队，不在锁里 send。 */
static bool send_frame(v1_conn_t *c, uint8_t type, const uint8_t *payload, uint32_t len)
{
    return enqueue_frame(c, type, payload, len);
}

/* 不持锁。发一帧；发不出去就留在队列里，下次再试。 */
static int drain_one(v1_conn_t *c)
{
    if (!s_mu || xSemaphoreTake(s_mu, pdMS_TO_TICKS(20)) != pdTRUE) {
        return 0;
    }
    if (c->oq_n == 0 || c->fd < 0) {
        xSemaphoreGive(s_mu);
        return 0;
    }
    uint8_t *p = c->outq[c->oq_r].p;
    uint16_t n = c->outq[c->oq_r].n;
    uint16_t off = c->outq[c->oq_r].off;
    int fd = c->fd;
    xSemaphoreGive(s_mu);
    int wr = send(fd, p + off, (size_t)(n - off), MSG_DONTWAIT);
    if (xSemaphoreTake(s_mu, pdMS_TO_TICKS(20)) != pdTRUE) {
        return 0;
    }
    if (wr < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            xSemaphoreGive(s_mu);
            return 0;
        }
        c->fd = -1;
        xSemaphoreGive(s_mu);
        return -1;
    }
    if (wr == 0) {
        c->fd = -1;
        xSemaphoreGive(s_mu);
        return -1;
    }
    c->outq[c->oq_r].off = (uint16_t)(off + wr);
    if (c->outq[c->oq_r].off >= c->outq[c->oq_r].n) {
        free(c->outq[c->oq_r].p);
        c->outq[c->oq_r].p = NULL;
        c->oq_r = (uint8_t)((c->oq_r + 1) % OUTQ);
        c->oq_n--;
    }
    xSemaphoreGive(s_mu);
    return 1;
}

static bool send_json(v1_conn_t *c, uint8_t type, const char *json)
{
    return send_frame(c, type, (const uint8_t *)json, (uint32_t)strlen(json));
}

static void send_error(v1_conn_t *c, const char *code, const char *msg)
{
    char buf[192];
    snprintf(buf, sizeof(buf), "{\"code\":\"%s\",\"message\":\"%s\"}", code, msg ? msg : "");
    send_json(c, T_ERROR, buf);
}

static void send_ctrl_err(v1_conn_t *c, double id, const char *code, const char *msg)
{
    char buf[256];
    snprintf(buf, sizeof(buf),
             "{\"id\":%.0f,\"ok\":false,\"error\":{\"code\":\"%s\",\"message\":\"%s\"}}", id, code,
             msg ? msg : "");
    send_json(c, T_CONTROL, buf);
}

static void send_ctrl_ok(v1_conn_t *c, double id, const char *result_json)
{
    char buf[640];
    snprintf(buf, sizeof(buf), "{\"id\":%.0f,\"ok\":true,\"result\":%s}", id,
             result_json ? result_json : "{}");
    send_json(c, T_CONTROL, buf);
}

static int find_port(bridge_ctx_t *ctx, const char *id)
{
    if (!ctx || !id) {
        return -1;
    }
    for (int i = 0; i < ctx->port_count; i++) {
        if (ctx->port_ids[i] && strcmp(ctx->port_ids[i], id) == 0) {
            return i;
        }
    }
    return -1;
}

static void put_bytes(v1_cap_t *cap, uint64_t off, const uint8_t *p, uint32_t n)
{
    size_t capn = cap_size(cap);
    size_t i = (size_t)(off % capn);
    size_t first = capn - i;
    if (first > n) {
        first = n;
    }
    memcpy(cap->bytes + i, p, first);
    if (n > first) {
        memcpy(cap->bytes, p + first, n - first);
    }
}

static void spill_path(int port, uint64_t off, char *out, size_t n)
{
#if CONFIG_BRIDGE_SD_ENABLE
    snprintf(out, n, "%s/shb-cache/p%d/%016llx.bin", CONFIG_BRIDGE_SD_MOUNT_POINT, port,
             (unsigned long long)off);
#else
    (void)port;
    (void)off;
    if (n) {
        out[0] = '\0';
    }
#endif
}

static bool read_spill(v1_cap_t *cap, uint64_t off, uint8_t *p, uint32_t n)
{
#if CONFIG_BRIDGE_SD_ENABLE
    for (int i = 0; i < cap->spill_n && n; i++) {
        uint64_t s = cap->spill[i].off;
        uint32_t len = cap->spill[i].len;
        if (off < s || off >= s + len) {
            continue;
        }
        uint32_t skip = (uint32_t)(off - s);
        uint32_t take = n;
        if (skip + take > len) {
            take = len - skip;
        }
        char path[96];
        spill_path(cap->port_index, s, path, sizeof(path));
        FILE *f = fopen(path, "rb");
        if (!f) {
            return false;
        }
        if (fseek(f, (long)skip, SEEK_SET) != 0 || fread(p, 1, take, f) != take) {
            fclose(f);
            return false;
        }
        fclose(f);
        off += take;
        p += take;
        n -= take;
    }
    return n == 0;
#else
    (void)cap;
    (void)off;
    (void)p;
    (void)n;
    return false;
#endif
}

static bool get_bytes(v1_cap_t *cap, uint64_t off, uint8_t *p, uint32_t n)
{
    if (off < cap->ram_base) {
        uint64_t ram = cap->ram_base;
        if (off + n <= ram) {
            return read_spill(cap, off, p, n);
        }
        uint32_t head = (uint32_t)(ram - off);
        if (!read_spill(cap, off, p, head)) {
            return false;
        }
        off = ram;
        p += head;
        n -= head;
    }
    size_t capn = cap_size(cap);
    size_t i = (size_t)(off % capn);
    size_t first = capn - i;
    if (first > n) {
        first = n;
    }
    memcpy(p, cap->bytes + i, first);
    if (n > first) {
        memcpy(p + first, cap->bytes, n - first);
    }
    return true;
}

static void drop_to(v1_cap_t *cap, uint64_t off)
{
    if (off < cap->oldest) {
        return;
    }
    if (off > cap->newest) {
        off = cap->newest;
    }
    cap->oldest = off;
}

static void send_gap(v1_sess_t *s, v1_cap_t *cap, uint64_t off)
{
    uint8_t raw[DATA_HDR];
    memset(raw, 0, sizeof(raw));
    raw[0] = (uint8_t)s->slot;
    memcpy(raw + 1, &cap->id, 8);
    memcpy(raw + 13, &off, 8);
    uint64_t ts = now_ms();
    memcpy(raw + 21, &ts, 8);
    raw[29] = FLAG_GAP;
    if (!send_frame(s->conn, T_DATA, raw, DATA_HDR)) {
        return;
    }
    s->sent = off;
    if (s->acked < off) {
        s->acked = off;
    }
    s->last_ms = now_ms();
}

static void send_batch(v1_sess_t *s, v1_cap_t *cap, uint64_t off, uint32_t n)
{
    uint8_t *frame = (uint8_t *)malloc(DATA_HDR + n);
    if (!frame) {
        return;
    }
    memset(frame, 0, DATA_HDR);
    frame[0] = (uint8_t)s->slot;
    memcpy(frame + 1, &cap->id, 8);
    memcpy(frame + 13, &off, 8);
    uint64_t ts = now_ms();
    memcpy(frame + 21, &ts, 8);
    frame[29] = FLAG_BATCH;
    if (!get_bytes(cap, off, frame + DATA_HDR, n)) {
        free(frame);
        return;
    }
    bool ok = send_frame(s->conn, T_DATA, frame + 0, DATA_HDR + n);
    /* send_frame copies; the local header+payload must include payload. */
    free(frame);
    if (!ok) {
        return;
    }
    s->sent = off + n;
    s->last_ms = now_ms();
}

static void flush_sess(v1_sess_t *s, v1_cap_t *cap)
{
    if (!s->used || s->paused || !s->conn || s->conn->fd < 0 || !cap->live) {
        return;
    }
    if (s->sent < cap->oldest) {
        send_gap(s, cap, cap->oldest);
    }
    while (s->sent < cap->newest && s->conn->fd >= 0 && s->conn->oq_n < OUTQ - 2) {
        uint64_t room = s->window - (s->sent - s->acked);
        if (room == 0) {
            break;
        }
        uint64_t avail = cap->newest - s->sent;
        uint32_t n = (uint32_t)(avail < room ? avail : room);
        if (n > BATCH_MAX) {
            n = BATCH_MAX;
        }
        if (s->sent < cap->oldest) {
            send_gap(s, cap, cap->oldest);
            continue;
        }
        send_batch(s, cap, s->sent, n);
    }
}

static void flush_cap(v1_cap_t *cap)
{
    for (int i = 0; i < MAX_CONNS; i++) {
        if (!s_conn[i].used) {
            continue;
        }
        for (int j = 0; j < MAX_SESS; j++) {
            v1_sess_t *s = &s_conn[i].sess[j];
            if (s->used && s->port_index == cap->port_index) {
                flush_sess(s, cap);
            }
        }
    }
}

static uint64_t retain_floor(v1_cap_t *cap)
{
    bool any = false;
    uint64_t floor = cap->newest;
    for (int i = 0; i < MAX_CONNS; i++) {
        for (int j = 0; j < MAX_SESS; j++) {
            v1_sess_t *s = &s_conn[i].sess[j];
            if (!s->used || s->port_index != cap->port_index) {
                continue;
            }
            if (!any || s->acked < floor) {
                floor = s->acked;
            }
            any = true;
        }
    }
    if (!any) {
        return cap->linger_ack;
    }
    return floor;
}

static void spill_unlink(v1_cap_t *cap, uint64_t off)
{
    char path[96];
    spill_path(cap->port_index, off, path, sizeof(path));
    if (path[0]) {
        unlink(path);
    }
}

static void spill_clear(v1_cap_t *cap)
{
    for (int i = 0; i < cap->spill_n; i++) {
        spill_unlink(cap, cap->spill[i].off);
    }
    cap->spill_n = 0;
    free(cap->stage);
    cap->stage = NULL;
    cap->stage_len = 0;
    cap->stage_busy = false;
}

static void reclaim(v1_cap_t *cap)
{
    uint64_t floor = retain_floor(cap);
    if (floor > cap->oldest) {
        int w = 0;
        for (int i = 0; i < cap->spill_n; i++) {
            if (cap->spill[i].off + cap->spill[i].len <= floor) {
                spill_unlink(cap, cap->spill[i].off);
            } else {
                cap->spill[w++] = cap->spill[i];
            }
        }
        cap->spill_n = w;
        drop_to(cap, floor);
        if (floor > cap->ram_base && floor <= cap->newest) {
            cap->ram_base = floor;
        }
    }
}

static bool stage_spill(v1_cap_t *cap)
{
#if CONFIG_BRIDGE_SD_ENABLE
    if (cap->stage_busy || cap->spill_n >= SPILL_MAX || !bridge_sd_ready()) {
        return false;
    }
    uint64_t have = cap->newest - cap->ram_base;
    if (have < 4096) {
        return false;
    }
    uint32_t n = have > SPILL_CHUNK ? SPILL_CHUNK : (uint32_t)have;
    uint8_t *buf = (uint8_t *)malloc(n);
    if (!buf) {
        return false;
    }
    if (!get_bytes(cap, cap->ram_base, buf, n)) {
        free(buf);
        return false;
    }
    cap->stage = buf;
    cap->stage_off = cap->ram_base;
    cap->stage_len = n;
    cap->stage_busy = true;
    cap->ram_base += n;
    return true;
#else
    (void)cap;
    return false;
#endif
}

static bool append_bytes(v1_cap_t *cap, const uint8_t *p, uint32_t n)
{
    if (!cap->bytes || n == 0) {
        return false;
    }
    while (cap->newest - cap->ram_base + n > cap_size(cap)) {
        if (stage_spill(cap)) {
            continue;
        }
        uint64_t drop = cap->ram_base + n;
        if (drop > cap->newest) {
            drop = cap->newest;
        }
        drop_to(cap, drop);
        if (cap->ram_base < cap->oldest) {
            cap->ram_base = cap->oldest;
        }
        for (int i = 0; i < MAX_CONNS; i++) {
            for (int j = 0; j < MAX_SESS; j++) {
                v1_sess_t *s = &s_conn[i].sess[j];
                if (s->used && s->port_index == cap->port_index && s->sent < cap->oldest) {
                    send_gap(s, cap, cap->oldest);
                }
            }
        }
        if (cap->newest == cap->oldest) {
            break;
        }
    }
    if (cap->stage_busy || cap->newest - cap->ram_base + n > cap_size(cap)) {
        return false;
    }
    put_bytes(cap, cap->newest, p, n);
    cap->newest += n;
    return true;
}

static void jump_gap(v1_cap_t *cap, uint64_t new_off)
{
    if (new_off < cap->newest) {
        new_off = cap->newest;
    }
    spill_clear(cap);
    cap->oldest = new_off;
    cap->ram_base = new_off;
    cap->newest = new_off;
    for (int i = 0; i < MAX_CONNS; i++) {
        for (int j = 0; j < MAX_SESS; j++) {
            v1_sess_t *s = &s_conn[i].sess[j];
            if (s->used && s->port_index == cap->port_index && s->sent < new_off) {
                send_gap(s, cap, new_off);
            }
        }
    }
}

static void pull_uart(v1_cap_t *cap)
{
    if (!s_ctx || cap->uart_paused || cap->port_index < 0 || cap->port_index >= s_ctx->port_count) {
        return;
    }
    bridge_port_t *port = s_ctx->ports[cap->port_index];
    if (!port) {
        return;
    }
    bridge_port_stat_t st;
    bridge_port_stat(port, &st);
    if (!cap->cursor_set) {
        cap->uart_cursor = st.ring_end_offset;
        cap->cursor_set = true;
        return;
    }
    if (cap->uart_cursor < st.ring_start_offset) {
        uint64_t lost = st.ring_end_offset - cap->uart_cursor;
        cap->uart_cursor = st.ring_end_offset;
        jump_gap(cap, cap->newest + lost);
        return;
    }
    if (st.ring_end_offset <= cap->uart_cursor) {
        return;
    }
    uint8_t buf[1024];
    bridge_read_args_t args;
    memset(&args, 0, sizeof(args));
    args.has_from = true;
    args.from_offset = cap->uart_cursor;
    args.min_bytes = 1;
    args.max_bytes = sizeof(buf);
    args.max_frame = sizeof(buf);
    args.max_hold_ms = 1;
    args.has_max_wait = true;
    args.max_wait_ms = 0;
    args.has_idle = true;
    args.idle_timeout_ms = 0;
    args.has_budget = true;
    args.budget_ms = 0;
    bridge_read_result_t res;
    memset(&res, 0, sizeof(res));
    if (!bridge_port_read(port, &args, now_ms(), &res, buf, sizeof(buf)) || !res.done ||
        res.data_len == 0) {
        return;
    }
    if (!append_bytes(cap, buf, (uint32_t)res.data_len)) {
        return;
    }
    cap->uart_cursor = res.end;
    flush_cap(cap);
}

static void expire_linger(v1_cap_t *cap)
{
    if (!cap->live || cap->sessions > 0 || cap->linger_at == 0) {
        return;
    }
    if (now_ms() < cap->linger_at) {
        return;
    }
    spill_clear(cap);
    free(cap->bytes);
    memset(cap, 0, sizeof(*cap));
}

static bool write_spill_file(int port, uint64_t off, const uint8_t *p, uint32_t n)
{
#if CONFIG_BRIDGE_SD_ENABLE
    if (!bridge_sd_ready()) {
        return false;
    }
    char dir[80];
    snprintf(dir, sizeof(dir), "%s/shb-cache", CONFIG_BRIDGE_SD_MOUNT_POINT);
    mkdir(dir, 0775);
    snprintf(dir, sizeof(dir), "%s/shb-cache/p%d", CONFIG_BRIDGE_SD_MOUNT_POINT, port);
    mkdir(dir, 0775);
    char path[96];
    spill_path(port, off, path, sizeof(path));
    FILE *f = fopen(path, "wb");
    if (!f) {
        return false;
    }
    size_t wr = fwrite(p, 1, n, f);
    fclose(f);
    return wr == n;
#else
    (void)port;
    (void)off;
    (void)p;
    (void)n;
    return false;
#endif
}

static void commit_stages(void)
{
    for (int i = 0; i < BRIDGE_MAX_PORTS; i++) {
        if (!s_mu || xSemaphoreTake(s_mu, pdMS_TO_TICKS(20)) != pdTRUE) {
            return;
        }
        v1_cap_t *cap = &s_cap[i];
        if (!cap->live || !cap->stage_busy || !cap->stage) {
            xSemaphoreGive(s_mu);
            continue;
        }
        uint8_t *buf = cap->stage;
        uint32_t n = cap->stage_len;
        uint64_t off = cap->stage_off;
        int port = cap->port_index;
        cap->stage = NULL;
        xSemaphoreGive(s_mu);
        bool ok = write_spill_file(port, off, buf, n);
        free(buf);
        if (xSemaphoreTake(s_mu, pdMS_TO_TICKS(50)) != pdTRUE) {
            continue;
        }
        if (cap->live && cap->stage_off == off && cap->stage_busy) {
            if (ok && cap->spill_n < SPILL_MAX) {
                cap->spill[cap->spill_n].off = off;
                cap->spill[cap->spill_n].len = n;
                cap->spill_n++;
            } else if (!ok) {
                for (int c = 0; c < MAX_CONNS; c++) {
                    for (int j = 0; j < MAX_SESS; j++) {
                        v1_sess_t *s = &s_conn[c].sess[j];
                        if (s->used && s->port_index == port && s->sent < cap->ram_base) {
                            send_gap(s, cap, cap->ram_base);
                        }
                    }
                }
                if (cap->oldest < cap->ram_base) {
                    cap->oldest = cap->ram_base;
                }
            }
            cap->stage_busy = false;
        }
        xSemaphoreGive(s_mu);
    }
}

static void pump_task(void *arg)
{
    (void)arg;
    for (;;) {
        if (s_mu && xSemaphoreTake(s_mu, pdMS_TO_TICKS(50)) == pdTRUE) {
            for (int i = 0; i < BRIDGE_MAX_PORTS; i++) {
                if (!s_cap[i].live) {
                    continue;
                }
                expire_linger(&s_cap[i]);
                if (!s_cap[i].live) {
                    continue;
                }
                pull_uart(&s_cap[i]);
                reclaim(&s_cap[i]);
            }
            xSemaphoreGive(s_mu);
        }
        commit_stages();
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

static v1_cap_t *ensure_cap(int port_index)
{
    v1_cap_t *cap = &s_cap[port_index];
    if (cap->live && cap->bytes) {
        cap->linger_at = 0;
        return cap;
    }
    free(cap->bytes);
    memset(cap, 0, sizeof(*cap));
    cap->cap_bytes = s_cap_bytes;
    cap->bytes = (uint8_t *)heap_caps_malloc(cap->cap_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!cap->bytes) {
        cap->bytes = (uint8_t *)malloc(cap->cap_bytes);
    }
    if (!cap->bytes) {
        return NULL;
    }
    cap->live = true;
    cap->id = new_id();
    cap->port_index = port_index;
    return cap;
}

static v1_sess_t *alloc_sess(v1_conn_t *c)
{
    for (int i = 0; i < MAX_SESS; i++) {
        if (!c->sess[i].used) {
            return &c->sess[i];
        }
    }
    return NULL;
}

static v1_sess_t *sess_by_id(v1_conn_t *c, const char *id)
{
    if (!id) {
        return NULL;
    }
    for (int i = 0; i < MAX_SESS; i++) {
        if (c->sess[i].used && strcmp(c->sess[i].id, id) == 0) {
            return &c->sess[i];
        }
    }
    return NULL;
}

static void detach_sess(v1_sess_t *s)
{
    if (!s->used) {
        return;
    }
    v1_cap_t *cap = &s_cap[s->port_index];
    if (cap->writer == s) {
        cap->writer = NULL;
    }
    if (cap->sessions > 0) {
        cap->sessions--;
    }
    if (cap->live && cap->sessions == 0) {
        cap->linger_ack = s->acked;
        cap->linger_at = now_ms() + LINGER_MS;
    }
    memset(s, 0, sizeof(*s));
}

static double jid(cJSON *msg)
{
    cJSON *id = cJSON_GetObjectItem(msg, "id");
    return cJSON_IsNumber(id) ? id->valuedouble : 0;
}

static const char *jstr(cJSON *msg, const char *key)
{
    cJSON *v = cJSON_GetObjectItem(msg, key);
    return cJSON_IsString(v) ? v->valuestring : NULL;
}

static void on_hello(v1_conn_t *c, const uint8_t *payload, uint32_t len)
{
    char *txt = (char *)malloc((size_t)len + 1);
    if (!txt) {
        return;
    }
    memcpy(txt, payload, len);
    txt[len] = '\0';
    cJSON *msg = cJSON_Parse(txt);
    free(txt);
    if (!msg) {
        send_error(c, "bad_request", "invalid HELLO json");
        c->fd = -1;
        return;
    }
    const char *token = c->ctx->token;
    if (token && token[0]) {
        cJSON *tok = cJSON_GetObjectItem(msg, "token");
        const char *got = cJSON_IsString(tok) ? tok->valuestring : "";
        if (strlen(got) != strlen(token) || memcmp(got, token, strlen(token)) != 0) {
            send_error(c, "unauthorized", "bad token");
            cJSON_Delete(msg);
            c->fd = -1;
            return;
        }
    }
    c->authed = true;
    cJSON *root = cJSON_CreateObject();
    cJSON *result = cJSON_AddObjectToObject(root, "result");
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddStringToObject(result, "fw_version", BRIDGE_FW_VERSION);
    cJSON_AddStringToObject(result, "board", "esp32s3");
    char cid[16];
    hex12(cid);
    cJSON_AddStringToObject(result, "session_id", cid);
    cJSON *ports = cJSON_AddArrayToObject(result, "ports");
    for (int i = 0; i < c->ctx->port_count; i++) {
        cJSON *p = cJSON_CreateObject();
        cJSON_AddStringToObject(p, "id", c->ctx->port_ids[i] ? c->ctx->port_ids[i] : "uart");
        cJSON_AddStringToObject(p, "kind", "uart");
        const bridge_port_config_t *cfg = bridge_port_config(c->ctx->ports[i]);
        if (cfg) {
            cJSON_AddNumberToObject(p, "baud_rate", cfg->baud_rate);
        }
        cJSON_AddItemToArray(ports, p);
    }
    cJSON *caps = cJSON_AddObjectToObject(result, "caps");
    cJSON_AddBoolToObject(caps, "push", true);
    cJSON_AddBoolToObject(caps, "mux", true);
    cJSON_AddBoolToObject(caps, "batch", true);
    cJSON_AddNumberToObject(caps, "max_frame", 8192);
    cJSON *window = cJSON_AddObjectToObject(caps, "window");
    cJSON_AddNumberToObject(window, "default_bytes", WIN_DEFAULT);
    cJSON_AddNumberToObject(window, "max_bytes", WIN_MAX);
    cJSON_AddNumberToObject(window, "ack_interval_bytes", 4096);
    cJSON *capture = cJSON_AddObjectToObject(caps, "capture");
    cJSON_AddNumberToObject(capture, "linger_ms", LINGER_MS);
    cJSON_AddNumberToObject(capture, "max_bytes", s_cap_bytes);
    cJSON_AddStringToObject(capture, "history", "none");
    cJSON_AddBoolToObject(caps, "can_config", true);
    cJSON_AddBoolToObject(caps, "can_set_lines", true);
    cJSON_AddBoolToObject(caps, "can_break", true);
    cJSON_AddNumberToObject(caps, "lease_ms", LEASE_DEFAULT);
    cJSON_AddBoolToObject(caps, "events", false);
    char *out = cJSON_PrintUnformatted(root);
    if (out) {
        send_json(c, T_HELLO_ACK, out);
        free(out);
    }
    cJSON_Delete(root);
    cJSON_Delete(msg);
}

static void op_config(v1_conn_t *c, cJSON *msg)
{
    double id = jid(msg);
    int pi = find_port(c->ctx, jstr(msg, "port"));
    if (pi < 0) {
        send_ctrl_err(c, id, "port_not_found", "port not found");
        return;
    }
    if (s_cap[pi].live && s_cap[pi].sessions > 0) {
        v1_sess_t *s = sess_by_id(c, jstr(msg, "session_id"));
        if (!s) {
            s = s_cap[pi].writer;
        }
        if (!s || !s->writer || s->conn != c) {
            send_ctrl_err(c, id, "not_controller", "not writer");
            return;
        }
    }
    const bridge_port_config_t *cfg = bridge_port_config(c->ctx->ports[pi]);
    int baud = cfg ? cfg->baud_rate : 115200;
    int data_bits = cfg ? cfg->data_bits : 8;
    int stop_bits = cfg ? cfg->stop_bits : 1;
    const char *parity = cfg ? cfg->parity : "none";
    const char *flow = cfg ? cfg->flow_control : "none";
    cJSON *jb = cJSON_GetObjectItem(msg, "baud_rate");
    if (cJSON_IsNumber(jb)) {
        baud = (int)jb->valuedouble;
    }
    cJSON *jd = cJSON_GetObjectItem(msg, "data_bits");
    if (cJSON_IsNumber(jd)) {
        data_bits = (int)jd->valuedouble;
    }
    cJSON *js = cJSON_GetObjectItem(msg, "stop_bits");
    if (cJSON_IsNumber(js)) {
        stop_bits = (int)js->valuedouble;
    }
    if (jstr(msg, "parity")) {
        parity = jstr(msg, "parity");
    }
    if (jstr(msg, "flow_control")) {
        flow = jstr(msg, "flow_control");
    }
    if (bridge_port_configure(c->ctx->ports[pi], baud, (uint8_t)data_bits, (uint8_t)stop_bits, parity,
                              flow) != ESP_OK) {
        send_ctrl_err(c, id, "bad_request", "config rejected");
        return;
    }
    char result[160];
    snprintf(result, sizeof(result),
             "{\"port\":\"%s\",\"applied\":{\"baud_rate\":%d,\"data_bits\":%d,\"stop_bits\":%d}}",
             c->ctx->port_ids[pi], baud, data_bits, stop_bits);
    send_ctrl_ok(c, id, result);
}

static void op_subscribe(v1_conn_t *c, cJSON *msg)
{
    double id = jid(msg);
    int pi = find_port(c->ctx, jstr(msg, "port"));
    if (pi < 0) {
        send_ctrl_err(c, id, "port_not_found", "port not found");
        return;
    }
    for (int i = 0; i < MAX_SESS; i++) {
        if (c->sess[i].used && c->sess[i].port_index == pi) {
            detach_sess(&c->sess[i]);
        }
    }
    cJSON *from = cJSON_GetObjectItem(msg, "from");
    bool has_from = cJSON_IsObject(from);
    if (has_from) {
        cJSON *cid = cJSON_GetObjectItem(from, "capture_id");
        if (!s_cap[pi].live || !cJSON_IsNumber(cid) || (uint64_t)cid->valuedouble != s_cap[pi].id) {
            send_ctrl_err(c, id, "capture_expired", "capture not found/expired");
            return;
        }
    }
    v1_sess_t *s = alloc_sess(c);
    if (!s) {
        send_ctrl_err(c, id, "busy", "too many sessions");
        return;
    }
    bool created = !s_cap[pi].live || !s_cap[pi].bytes;
    v1_cap_t *cap = ensure_cap(pi);
    if (!cap) {
        send_ctrl_err(c, id, "internal", "capture alloc failed");
        return;
    }
    const char *mode = "new";
    uint64_t start = 0;
    if (has_from) {
        cJSON *off = cJSON_GetObjectItem(from, "offset");
        start = cJSON_IsNumber(off) ? (uint64_t)off->valuedouble : cap->newest;
        if (start < cap->oldest) {
            mode = "gap";
            start = cap->oldest;
        } else if (start > cap->newest) {
            start = cap->newest;
            mode = "resume";
        } else {
            mode = "resume";
        }
    } else if (!created) {
        mode = "resume";
        start = cap->newest;
    }
    const char *want = jstr(msg, "mode");
    if (want && strcmp(want, "live") == 0) {
        start = cap->newest;
        if (strcmp(mode, "new") == 0) {
            mode = "resume";
        }
    }
    uint32_t window = WIN_DEFAULT;
    cJSON *jw = cJSON_GetObjectItem(msg, "window_bytes");
    if (cJSON_IsNumber(jw) && jw->valuedouble >= 4096) {
        window = (uint32_t)jw->valuedouble;
    }
    if (window > WIN_MAX) {
        window = WIN_MAX;
    }
    uint32_t lease = LEASE_DEFAULT;
    cJSON *jl = cJSON_GetObjectItem(msg, "lease_ms");
    if (cJSON_IsNumber(jl) && jl->valuedouble >= 1000) {
        lease = (uint32_t)jl->valuedouble;
    }
    memset(s, 0, sizeof(*s));
    s->used = true;
    hex12(s->id);
    s->slot = pi;
    s->port_index = pi;
    s->sent = start;
    s->acked = start;
    s->window = window;
    s->lease_ms = lease;
    s->last_ms = now_ms();
    s->conn = c;
    cap->sessions++;
    cap->linger_at = 0;
    if (!cap->writer || cJSON_IsTrue(cJSON_GetObjectItem(msg, "claim_writer"))) {
        if (cap->writer && cap->writer != s && cap->writer->conn) {
            char ev[96];
            snprintf(ev, sizeof(ev), "{\"event\":\"writer_revoked\",\"session_id\":\"%s\"}",
                     cap->writer->id);
            cap->writer->writer = false;
            send_json(cap->writer->conn, T_EVENT, ev);
        }
        cap->writer = s;
        s->writer = true;
    }
    char result[384];
    snprintf(result, sizeof(result),
             "{\"session_id\":\"%s\",\"port_slot\":%d,\"capture_id\":%.0f,\"mode\":\"%s\","
             "\"writer\":%s,\"start\":{\"seq\":0,\"offset\":%.0f},"
             "\"oldest\":{\"seq\":0,\"offset\":%.0f},\"newest\":{\"seq\":0,\"offset\":%.0f},"
             "\"window_bytes\":%lu,\"linger_ms\":%u}",
             s->id, s->slot, (double)cap->id, mode, s->writer ? "true" : "false", (double)start,
             (double)cap->oldest, (double)cap->newest, (unsigned long)window, LINGER_MS);
    send_ctrl_ok(c, id, result);
    flush_sess(s, cap);
}

static void op_unsubscribe(v1_conn_t *c, cJSON *msg)
{
    double id = jid(msg);
    v1_sess_t *s = sess_by_id(c, jstr(msg, "session_id"));
    bool had = s != NULL;
    if (s) {
        detach_sess(s);
    }
    char result[40];
    snprintf(result, sizeof(result), "{\"unsubscribed\":%s}", had ? "true" : "false");
    send_ctrl_ok(c, id, result);
}

static void op_pause(v1_conn_t *c, cJSON *msg, bool pause)
{
    double id = jid(msg);
    v1_sess_t *s = sess_by_id(c, jstr(msg, "session_id"));
    if (!s) {
        send_ctrl_err(c, id, "bad_request", "session not found");
        return;
    }
    s->paused = pause;
    if (s->writer) {
        s_cap[s->port_index].uart_paused = pause;
    }
    s->last_ms = now_ms();
    send_ctrl_ok(c, id, pause ? "{\"paused\":true}" : "{\"resumed\":true}");
    if (!pause) {
        flush_sess(s, &s_cap[s->port_index]);
    }
}

static void op_write(v1_conn_t *c, cJSON *msg)
{
    double id = jid(msg);
    v1_sess_t *s = sess_by_id(c, jstr(msg, "session_id"));
    if (!s || !s->writer) {
        send_ctrl_err(c, id, "not_controller", "not writer");
        return;
    }
    const char *b64 = jstr(msg, "data_b64");
    size_t slen = b64 ? strlen(b64) : 0;
    size_t need = slen * 3 / 4 + 4;
    uint8_t *raw = (uint8_t *)malloc(need ? need : 1);
    size_t olen = 0;
    if (!raw) {
        send_ctrl_err(c, id, "internal", "oom");
        return;
    }
    if (b64 && slen &&
        mbedtls_base64_decode(raw, need, &olen, (const unsigned char *)b64, slen) != 0) {
        free(raw);
        send_ctrl_err(c, id, "bad_request", "bad base64");
        return;
    }
    uint64_t tx_off = 0;
    uint32_t tx_seq = 0;
    int n = bridge_port_write(c->ctx->ports[s->port_index], raw, olen, &tx_off, &tx_seq);
    free(raw);
    if (n < 0) {
        send_ctrl_err(c, id, "internal", "uart write failed");
        return;
    }
    char result[128];
    snprintf(result, sizeof(result),
             "{\"bytes_written\":%d,\"tx_offset\":%.0f,\"tx_log_seq\":%lu,\"deduped\":false}", n,
             (double)tx_off, (unsigned long)tx_seq);
    send_ctrl_ok(c, id, result);
}

static void op_lines(v1_conn_t *c, cJSON *msg)
{
    double id = jid(msg);
    v1_sess_t *s = sess_by_id(c, jstr(msg, "session_id"));
    if (!s || !s->writer) {
        send_ctrl_err(c, id, "not_controller", "not writer");
        return;
    }
    int dtr = -1;
    int rts = -1;
    if (cJSON_IsBool(cJSON_GetObjectItem(msg, "dtr"))) {
        dtr = cJSON_IsTrue(cJSON_GetObjectItem(msg, "dtr")) ? 1 : 0;
    }
    if (cJSON_IsBool(cJSON_GetObjectItem(msg, "rts"))) {
        rts = cJSON_IsTrue(cJSON_GetObjectItem(msg, "rts")) ? 1 : 0;
    }
    int od = 0;
    int orr = 0;
    esp_err_t err = bridge_port_set_lines(c->ctx->ports[s->port_index], dtr, rts, &od, &orr);
    if (err == ESP_ERR_NOT_SUPPORTED) {
        send_ctrl_err(c, id, "unsupported", "lines not wired");
        return;
    }
    if (err != ESP_OK) {
        send_ctrl_err(c, id, "internal", "set_lines failed");
        return;
    }
    char result[48];
    snprintf(result, sizeof(result), "{\"dtr\":%s,\"rts\":%s}", od ? "true" : "false",
             orr ? "true" : "false");
    send_ctrl_ok(c, id, result);
}

static void op_break(v1_conn_t *c, cJSON *msg)
{
    double id = jid(msg);
    v1_sess_t *s = sess_by_id(c, jstr(msg, "session_id"));
    if (!s || !s->writer) {
        send_ctrl_err(c, id, "not_controller", "not writer");
        return;
    }
    uint32_t ms = 300;
    cJSON *jd = cJSON_GetObjectItem(msg, "duration_ms");
    if (cJSON_IsNumber(jd)) {
        ms = (uint32_t)jd->valuedouble;
    }
    if (bridge_port_send_break(c->ctx->ports[s->port_index], ms) != ESP_OK) {
        send_ctrl_err(c, id, "internal", "break failed");
        return;
    }
    send_ctrl_ok(c, id, "{\"ok\":true}");
}

static void op_ping(v1_conn_t *c, cJSON *msg)
{
    double id = jid(msg);
    for (int i = 0; i < MAX_SESS; i++) {
        if (c->sess[i].used) {
            c->sess[i].last_ms = now_ms();
        }
    }
    cJSON *t = cJSON_GetObjectItem(msg, "t_ms");
    double tms = cJSON_IsNumber(t) ? t->valuedouble : (double)now_ms();
    char result[80];
    snprintf(result, sizeof(result), "{\"t_ms\":%.0f,\"device_tick_ms\":%.0f}", tms, (double)now_ms());
    send_ctrl_ok(c, id, result);
}

static void on_ack(v1_conn_t *c, const uint8_t *p, uint32_t len)
{
    if (len < ACK_LEN) {
        return;
    }
    uint8_t slot = p[0];
    uint64_t cap_id = 0;
    uint64_t off = 0;
    memcpy(&cap_id, p + 1, 8);
    memcpy(&off, p + 9, 8);
    for (int i = 0; i < MAX_SESS; i++) {
        v1_sess_t *s = &c->sess[i];
        if (!s->used || s->slot != slot) {
            continue;
        }
        v1_cap_t *cap = &s_cap[s->port_index];
        if (!cap->live || cap->id != cap_id) {
            return;
        }
        if (off > s->sent) {
            off = s->sent;
        }
        if (off > s->acked) {
            s->acked = off;
        }
        s->last_ms = now_ms();
        reclaim(cap);
        flush_sess(s, cap);
        return;
    }
}

static void op_cache_set(v1_conn_t *c, cJSON *msg)
{
    double id = jid(msg);
    cJSON *jb = cJSON_GetObjectItem(msg, "cap_bytes");
    uint32_t raw = 0;
    if (cJSON_IsNumber(jb)) {
        raw = (uint32_t)jb->valuedouble;
    } else {
        jb = cJSON_GetObjectItem(msg, "cap_kb");
        if (!cJSON_IsNumber(jb)) {
            send_ctrl_err(c, id, "bad_request", "cap_bytes or cap_kb required");
            return;
        }
        raw = (uint32_t)jb->valuedouble * 1024u;
    }
    size_t n = clamp_cap(raw);
    bool saved = save_cap_nvs(n);
    s_cap_bytes = n;
    bool live = false;
    for (int i = 0; i < BRIDGE_MAX_PORTS; i++) {
        if (s_cap[i].live) {
            live = true;
            break;
        }
    }
    char result[128];
    snprintf(result, sizeof(result),
             "{\"cap_bytes\":%u,\"saved\":%s,\"applied\":\"%s\"}", (unsigned)n,
             saved ? "true" : "false", live ? "next" : "now");
    send_ctrl_ok(c, id, result);
    ESP_LOGI(TAG, "send cache %u KiB saved=%d apply=%s", (unsigned)(n / 1024), saved,
             live ? "next" : "now");
}

static void on_control(v1_conn_t *c, const uint8_t *payload, uint32_t len)
{
    char *txt = (char *)malloc((size_t)len + 1);
    if (!txt) {
        return;
    }
    memcpy(txt, payload, len);
    txt[len] = '\0';
    cJSON *msg = cJSON_Parse(txt);
    free(txt);
    if (!msg) {
        send_error(c, "bad_request", "invalid control json");
        return;
    }
    if (!c->authed) {
        send_ctrl_err(c, jid(msg), "unauthorized", "HELLO required");
        cJSON_Delete(msg);
        c->fd = -1;
        return;
    }
    const char *op = jstr(msg, "op");
    if (!op) {
        send_ctrl_err(c, jid(msg), "bad_request", "missing op");
    } else if (strcmp(op, "ping") == 0) {
        op_ping(c, msg);
    } else if (strcmp(op, "config") == 0) {
        op_config(c, msg);
    } else if (strcmp(op, "cache_set") == 0) {
        op_cache_set(c, msg);
    } else if (strcmp(op, "subscribe") == 0) {
        op_subscribe(c, msg);
    } else if (strcmp(op, "unsubscribe") == 0) {
        op_unsubscribe(c, msg);
    } else if (strcmp(op, "pause") == 0) {
        op_pause(c, msg, true);
    } else if (strcmp(op, "resume") == 0) {
        op_pause(c, msg, false);
    } else if (strcmp(op, "write") == 0) {
        op_write(c, msg);
    } else if (strcmp(op, "set_lines") == 0) {
        op_lines(c, msg);
    } else if (strcmp(op, "break") == 0) {
        op_break(c, msg);
    } else if (strcmp(op, "history_info") == 0 || strcmp(op, "history_query") == 0 ||
               strcmp(op, "history_export") == 0) {
        send_ctrl_err(c, jid(msg), "unsupported", "history not available");
    } else {
        send_ctrl_err(c, jid(msg), "unknown_op", "unknown op");
    }
    cJSON_Delete(msg);
}

static void handle_frame(v1_conn_t *c, uint8_t type, const uint8_t *payload, uint32_t len)
{
    if (type == T_HELLO) {
        on_hello(c, payload, len);
    } else if (type == T_CONTROL) {
        on_control(c, payload, len);
    } else if (type == T_ACK) {
        on_ack(c, payload, len);
    }
}

static void check_lease(v1_conn_t *c)
{
    uint64_t t = now_ms();
    for (int i = 0; i < MAX_SESS; i++) {
        v1_sess_t *s = &c->sess[i];
        if (!s->used || t - s->last_ms <= s->lease_ms) {
            continue;
        }
        char ev[96];
        snprintf(ev, sizeof(ev), "{\"event\":\"lease_expired\",\"session_id\":\"%s\"}", s->id);
        send_json(c, T_EVENT, ev);
        detach_sess(s);
    }
}

static v1_conn_t *alloc_conn(void)
{
    for (int i = 0; i < MAX_CONNS; i++) {
        if (!s_conn[i].used) {
            memset(&s_conn[i], 0, sizeof(s_conn[i]));
            s_conn[i].used = true;
            s_conn[i].fd = -1;
            return &s_conn[i];
        }
    }
    return NULL;
}

void bridge_v1_start(bridge_ctx_t *ctx)
{
    s_ctx = ctx;
    load_cap_nvs();
    if (!s_mu) {
        s_mu = xSemaphoreCreateMutex();
        xTaskCreate(pump_task, "v1_pump", 6144, NULL, 6, NULL);
    }
    ESP_LOGI(TAG, "v1 batch push ready (ram %u KiB, spill %u KiB, history none)",
             (unsigned)(s_cap_bytes / 1024), (unsigned)((SPILL_CHUNK * SPILL_MAX) / 1024));
}

void bridge_v1_session(bridge_ctx_t *ctx, int fd)
{
    xSemaphoreTake(s_mu, portMAX_DELAY);
    v1_conn_t *c = alloc_conn();
    if (c) {
        c->fd = fd;
        c->ctx = ctx;
        c->rx_cap = 8192;
        c->rx = (uint8_t *)malloc(c->rx_cap);
    }
    xSemaphoreGive(s_mu);
    if (!c || !c->rx) {
        ESP_LOGE(TAG, "no session slot");
        return;
    }
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) {
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }
    uint8_t tmp[1024];
    while (c->fd >= 0) {
        int n = recv(fd, tmp, sizeof(tmp), MSG_DONTWAIT);
        if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
            break;
        }
        if (n == 0) {
            break;
        }
        if (xSemaphoreTake(s_mu, portMAX_DELAY) != pdTRUE) {
            break;
        }
        if (n < 0) {
            check_lease(c);
            for (int j = 0; j < MAX_SESS; j++) {
                if (c->sess[j].used) {
                    flush_sess(&c->sess[j], &s_cap[c->sess[j].port_index]);
                }
            }
            xSemaphoreGive(s_mu);
            if (drain_one(c) < 0) {
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(2));
            continue;
        }
        if (c->rx_len + (size_t)n > c->rx_cap) {
            size_t ncap = c->rx_cap * 2;
            while (ncap < c->rx_len + (size_t)n) {
                ncap *= 2;
            }
            uint8_t *nb = ncap > 65536 ? NULL : (uint8_t *)realloc(c->rx, ncap);
            if (!nb) {
                send_error(c, "frame_too_large", "rx buffer");
                xSemaphoreGive(s_mu);
                break;
            }
            c->rx = nb;
            c->rx_cap = ncap;
        }
        memcpy(c->rx + c->rx_len, tmp, (size_t)n);
        c->rx_len += (size_t)n;
        size_t off = 0;
        bool bad = false;
        while (c->rx_len - off >= HDR) {
            uint8_t *b = c->rx + off;
            if (b[0] != 0x53 || b[1] != 0x48 || b[2] != 1) {
                send_error(c, "bad_frame", "bad magic");
                bad = true;
                break;
            }
            uint32_t len = (uint32_t)b[6] | ((uint32_t)b[7] << 8) | ((uint32_t)b[8] << 16) |
                           ((uint32_t)b[9] << 24);
            if (len > 8192) {
                send_error(c, "frame_too_large", "payload");
                bad = true;
                break;
            }
            if (c->rx_len - off < HDR + len) {
                break;
            }
            handle_frame(c, b[3], b + HDR, len);
            off += HDR + len;
            if (c->fd < 0) {
                bad = true;
                break;
            }
        }
        if (off && c->rx) {
            memmove(c->rx, c->rx + off, c->rx_len - off);
            c->rx_len -= off;
        }
        xSemaphoreGive(s_mu);
        while (drain_one(c) > 0) {
        }
        if (bad || c->fd < 0) {
            break;
        }
    }
    xSemaphoreTake(s_mu, portMAX_DELAY);
    for (int i = 0; i < OUTQ; i++) {
        free(c->outq[i].p);
        c->outq[i].p = NULL;
    }
    for (int i = 0; i < MAX_SESS; i++) {
        if (c->sess[i].used) {
            detach_sess(&c->sess[i]);
        }
    }
    free(c->rx);
    memset(c, 0, sizeof(*c));
    xSemaphoreGive(s_mu);
}
