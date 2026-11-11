#include "bridge_proto.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"
#include "mbedtls/base64.h"
#include "sdkconfig.h"

#include "bridge_port.h"
#include "bridge_sd.h"
#include "bridge_event.h"

#define TAG "bridge_proto"

#define DEDUP_SLOTS 32
#define DEDUP_TTL_MS 60000
#define DEDUP_MAX_JSON 16384
#define DEDUP_OP_ID_MAX 47
#define DEFAULT_LOG_LIMIT 100
#define MAX_LOG_LIMIT 500

typedef struct {
    bool used;
    char op_id[DEDUP_OP_ID_MAX + 1];
    uint64_t ts_ms;
    char *json;
} dedup_slot_t;

static dedup_slot_t s_dedup[DEDUP_SLOTS];
static int s_dedup_next;

/* ── small helpers ─────────────────────────────────────────────────────── */

static const cJSON *obj(const cJSON *o, const char *k)
{
    return cJSON_GetObjectItemCaseSensitive(o, k);
}

static const cJSON *jid(const cJSON *msg)
{
    return obj(msg, "id");
}

static const char *jstr(const cJSON *o, const char *k)
{
    const cJSON *v = obj(o, k);
    return cJSON_IsString(v) ? v->valuestring : NULL;
}

static bool jbool(const cJSON *o, const char *k, bool *out)
{
    const cJSON *v = obj(o, k);
    if (cJSON_IsBool(v)) {
        *out = cJSON_IsTrue(v);
        return true;
    }
    if (cJSON_IsNumber(v)) {
        *out = v->valuedouble != 0;
        return true;
    }
    return false;
}

static bool ju32(const cJSON *o, const char *k, uint32_t *out)
{
    const cJSON *v = obj(o, k);
    if (!cJSON_IsNumber(v) || v->valuedouble < 0) {
        return false;
    }
    *out = (uint32_t)v->valuedouble;
    return true;
}

static bool ju64(const cJSON *o, const char *k, uint64_t *out)
{
    const cJSON *v = obj(o, k);
    if (!cJSON_IsNumber(v) || v->valuedouble < 0) {
        return false;
    }
    *out = (uint64_t)v->valuedouble;
    return true;
}

static bool ct_streq(const char *a, const char *b)
{
    if (!a || !b) {
        return false;
    }
    size_t la = strlen(a);
    size_t lb = strlen(b);
    size_t n = la < lb ? la : lb;
    unsigned diff = (unsigned)(la ^ lb);
    for (size_t i = 0; i < n; i++) {
        diff |= (unsigned)(a[i] ^ b[i]);
    }
    return diff == 0;
}

static char *b64_encode_alloc(const uint8_t *data, size_t len)
{
    size_t need = 4 * ((len + 2) / 3) + 1;
    char *out = (char *)malloc(need);
    if (!out) {
        return NULL;
    }
    size_t olen = 0;
    int rc = mbedtls_base64_encode((unsigned char *)out, need, &olen,
                                   len ? data : (const uint8_t *)"", len);
    if (rc != 0) {
        free(out);
        return NULL;
    }
    out[olen] = '\0';
    return out;
}

static uint8_t *b64_decode_alloc(const char *s, size_t *out_len)
{
    *out_len = 0;
    if (!s || s[0] == '\0') {
        uint8_t *p = (uint8_t *)calloc(1, 1);
        return p;
    }
    size_t slen = strlen(s);
    size_t need = slen * 3 / 4 + 4;
    uint8_t *out = (uint8_t *)malloc(need);
    if (!out) {
        return NULL;
    }
    size_t olen = 0;
    int rc = mbedtls_base64_decode(out, need, &olen, (const unsigned char *)s, slen);
    if (rc != 0) {
        free(out);
        return NULL;
    }
    *out_len = olen;
    return out;
}

static char *hex_alloc(const uint8_t *data, size_t len)
{
    static const char digits[] = "0123456789abcdef";
    char *out = (char *)malloc(len * 2 + 1);
    if (!out) {
        return NULL;
    }
    for (size_t i = 0; i < len; i++) {
        out[i * 2] = digits[data[i] >> 4];
        out[i * 2 + 1] = digits[data[i] & 0x0F];
    }
    out[len * 2] = '\0';
    return out;
}

static char *json_print(cJSON *root)
{
    char *s = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return s;
}

static char *make_ok(const cJSON *id, cJSON *result)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        cJSON_Delete(result);
        return NULL;
    }
    if (!result) {
        result = cJSON_CreateObject();
        if (!result) {
            cJSON_Delete(root);
            return NULL;
        }
    }
    if (id) {
        cJSON_AddItemToObject(root, "id", cJSON_Duplicate(id, true));
    } else {
        cJSON_AddNullToObject(root, "id");
    }
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddItemToObject(root, "result", result);
    return json_print(root);
}

static char *make_err(const cJSON *id, const char *code, const char *message)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return NULL;
    }
    if (id) {
        cJSON_AddItemToObject(root, "id", cJSON_Duplicate(id, true));
    } else {
        cJSON_AddNullToObject(root, "id");
    }
    cJSON_AddBoolToObject(root, "ok", false);
    cJSON *err = cJSON_AddObjectToObject(root, "error");
    cJSON_AddStringToObject(err, "code", code ? code : "io_error");
    cJSON_AddStringToObject(err, "message", message ? message : "");
    return json_print(root);
}

static bool port_matches(const cJSON *msg)
{
    const char *pn = jstr(msg, "port");
    return !pn || strcmp(pn, BRIDGE_PORT_ID) == 0;
}

/* ── op_id dedup ───────────────────────────────────────────────────────── */

static void dedup_free(dedup_slot_t *s)
{
    if (s->json) {
        free(s->json);
    }
    memset(s, 0, sizeof(*s));
}

void bridge_proto_reset(void)
{
    for (int i = 0; i < DEDUP_SLOTS; i++) {
        dedup_free(&s_dedup[i]);
    }
    s_dedup_next = 0;
}

static const char *dedup_lookup(const char *op_id)
{
    if (!op_id || !op_id[0]) {
        return NULL;
    }
    uint64_t now = bridge_now_ms();
    for (int i = 0; i < DEDUP_SLOTS; i++) {
        dedup_slot_t *s = &s_dedup[i];
        if (!s->used) {
            continue;
        }
        if (now - s->ts_ms > DEDUP_TTL_MS) {
            dedup_free(s);
            continue;
        }
        if (strcmp(s->op_id, op_id) == 0) {
            return s->json;
        }
    }
    return NULL;
}

static void dedup_remember(const char *op_id, const cJSON *result)
{
    if (!op_id || !op_id[0] || strlen(op_id) > DEDUP_OP_ID_MAX || !result) {
        return;
    }
    char *json = cJSON_PrintUnformatted(result);
    if (!json) {
        return;
    }
    size_t len = strlen(json);
    if (len > DEDUP_MAX_JSON) {
        free(json);
        return;
    }

    uint64_t now = bridge_now_ms();
    dedup_slot_t *slot = NULL;
    for (int i = 0; i < DEDUP_SLOTS; i++) {
        if (s_dedup[i].used && strcmp(s_dedup[i].op_id, op_id) == 0) {
            slot = &s_dedup[i];
            break;
        }
    }
    if (!slot) {
        for (int i = 0; i < DEDUP_SLOTS && !slot; i++) {
            if (!s_dedup[i].used || now - s_dedup[i].ts_ms > DEDUP_TTL_MS) {
                slot = &s_dedup[i];
            }
        }
    }
    if (!slot) {
        slot = &s_dedup[s_dedup_next];
        s_dedup_next = (s_dedup_next + 1) % DEDUP_SLOTS;
    }

    if (slot->json) {
        free(slot->json);
    }
    strncpy(slot->op_id, op_id, DEDUP_OP_ID_MAX);
    slot->op_id[DEDUP_OP_ID_MAX] = '\0';
    slot->used = true;
    slot->ts_ms = now;
    slot->json = json;
}

static cJSON *dedup_result(const char *json)
{
    cJSON *r = cJSON_Parse(json);
    if (!r) {
        return NULL;
    }
    cJSON_DeleteItemFromObject(r, "deduped");
    cJSON_AddBoolToObject(r, "deduped", true);
    return r;
}

/* ── capability / info payload ─────────────────────────────────────────── */

static cJSON *build_caps(bridge_ctx_t *ctx)
{
    const bridge_port_config_t *cfg = bridge_port_config(ctx->port);
    cJSON *caps = cJSON_CreateObject();
    if (!caps) {
        return NULL;
    }
    cJSON_AddBoolToObject(caps, "auth", ctx->token && ctx->token[0]);
    const bool has_sd = bridge_sd_ready();
    cJSON_AddBoolToObject(caps, "has_sd", has_sd);
    cJSON_AddNumberToObject(caps, "ring_bytes", cfg ? (double)cfg->ring_bytes : 0);
    cJSON_AddNumberToObject(caps, "max_hold_ms", (double)ctx->max_hold_ms);
    cJSON_AddNumberToObject(caps, "max_frame", (double)ctx->max_frame);
    cJSON_AddBoolToObject(caps, "can_set_lines", bridge_port_can_set_lines(ctx->port));
    cJSON_AddBoolToObject(caps, "can_break", true);
    cJSON_AddBoolToObject(caps, "can_config", true);

    cJSON *lf = cJSON_AddArrayToObject(caps, "log_filter");
    cJSON_AddItemToArray(lf, cJSON_CreateString("seq"));
    cJSON_AddItemToArray(lf, cJSON_CreateString("dir"));

    cJSON_AddBoolToObject(caps, "events", true);
    cJSON_AddStringToObject(caps, "log_storage", has_sd ? "raw+index" : "ram");
    cJSON_AddBoolToObject(caps, "log_export", has_sd);
    cJSON_AddBoolToObject(caps, "binary_frames", false);
    if (cfg) {
        cJSON_AddNumberToObject(caps, "log_line_max_bytes", (double)cfg->log_line_max);
    }
    return caps;
}

static cJSON *build_info(bridge_ctx_t *ctx)
{
    const bridge_port_config_t *cfg = bridge_port_config(ctx->port);
    cJSON *res = cJSON_CreateObject();
    if (!res) {
        return NULL;
    }

    cJSON_AddStringToObject(res, "proto", BRIDGE_PROTO_VERSION);
    cJSON_AddStringToObject(res, "fw_version", BRIDGE_FW_VERSION);
#ifdef CONFIG_IDF_TARGET
    cJSON_AddStringToObject(res, "board", CONFIG_IDF_TARGET);
#endif
    cJSON *caps = build_caps(ctx);
    if (!caps) {
        cJSON_Delete(res);
        return NULL;
    }
    cJSON_AddItemToObject(res, "caps", caps);

    cJSON *ports = cJSON_AddArrayToObject(res, "ports");
    cJSON *p = cJSON_CreateObject();
    cJSON_AddStringToObject(p, "id", BRIDGE_PORT_ID);
    cJSON_AddStringToObject(p, "kind", BRIDGE_PORT_KIND);
    if (cfg) {
        cJSON *pins = cJSON_AddObjectToObject(p, "pins");
        cJSON_AddNumberToObject(pins, "tx", cfg->tx_pin);
        cJSON_AddNumberToObject(pins, "rx", cfg->rx_pin);
    }
    cJSON_AddItemToArray(ports, p);

    cJSON *time = cJSON_AddObjectToObject(res, "time");
    cJSON_AddNumberToObject(time, "epoch_ms", 0);
    cJSON_AddBoolToObject(time, "synced", false);
    cJSON_AddStringToObject(time, "source", "tick");
    cJSON_AddNumberToObject(time, "tick_ms", (double)bridge_now_ms());

    return res;
}

/* ── READ / TRANSACT ───────────────────────────────────────────────────── */

static void parse_read_args(const cJSON *msg,
                            bool has_from,
                            uint64_t from,
                            bridge_read_args_t *a)
{
    memset(a, 0, sizeof(*a));
    a->has_from = has_from;
    a->from_offset = from;

    uint32_t v;
    if (ju32(msg, "min_bytes", &v)) {
        a->min_bytes = v ? v : 1;
    } else {
        a->min_bytes = 1;
    }
    if (ju32(msg, "max_bytes", &v)) {
        a->max_bytes = v;
    }
    if (ju32(msg, "max_wait_ms", &v)) {
        a->has_max_wait = true;
        a->max_wait_ms = v;
    }
    if (ju32(msg, "idle_timeout_ms", &v)) {
        a->has_idle = true;
        a->idle_timeout_ms = v;
    }
    if (ju32(msg, "budget_ms", &v)) {
        a->has_budget = true;
        a->budget_ms = v;
    }
}

static bool run_read(bridge_ctx_t *ctx, bridge_read_args_t *a, uint64_t started,
                     bridge_read_result_t *res)
{
    uint32_t max_bytes = a->max_bytes ? a->max_bytes : ctx->max_frame;
    if (max_bytes > ctx->max_frame) {
        max_bytes = ctx->max_frame;
    }
    if (max_bytes > ctx->scratch_cap) {
        max_bytes = (uint32_t)ctx->scratch_cap;
    }
    a->max_bytes = max_bytes;
    a->max_frame = ctx->max_frame;
    a->max_hold_ms = ctx->max_hold_ms;

    for (;;) {
        if (bridge_port_read(ctx->port, a, started, res, ctx->scratch, ctx->scratch_cap)) {
            return true;
        }
        bridge_port_wait(ctx->port, res->wait_ms);
    }
}

static cJSON *read_result_json(bridge_ctx_t *ctx, const bridge_read_result_t *res)
{
    cJSON *r = cJSON_CreateObject();
    if (!r) {
        return NULL;
    }
    char *b64 = b64_encode_alloc(ctx->scratch, res->data_len);
    if (!b64) {
        cJSON_Delete(r);
        return NULL;
    }
    cJSON_AddStringToObject(r, "data_b64", b64);
    free(b64);

    cJSON_AddNumberToObject(r, "offset_from", (double)res->start);
    cJSON_AddNumberToObject(r, "offset_to", (double)res->end);
    cJSON_AddNumberToObject(r, "next_offset", (double)res->end);
    cJSON_AddBoolToObject(r, "matched", res->matched);
    cJSON_AddStringToObject(r, "stopped_reason", res->stopped_reason ? res->stopped_reason : "idle");
    cJSON_AddBoolToObject(r, "gap", res->gap);

    bridge_port_stat_t st;
    bridge_port_stat(ctx->port, &st);
    cJSON_AddNumberToObject(r, "ring_start_offset", (double)st.ring_start_offset);
    cJSON_AddNumberToObject(r, "ring_end_offset", (double)st.ring_end_offset);
    cJSON_AddBoolToObject(r, "hold_clamped", res->hold_clamped);
    return r;
}

static char *handle_read(bridge_ctx_t *ctx, const cJSON *msg)
{
    const cJSON *id = jid(msg);
    bridge_read_args_t a;
    parse_read_args(msg, false, 0, &a);
    if (ju64(msg, "from_offset", &a.from_offset)) {
        a.has_from = true;
    }

    uint8_t *match = NULL;
    size_t match_len = 0;
    const char *mb64 = jstr(msg, "match_b64");
    if (mb64) {
        match = b64_decode_alloc(mb64, &match_len);
        if (!match) {
            return make_err(id, "bad_request", "invalid match_b64");
        }
        a.match = match;
        a.match_len = match_len;
    }

    bridge_read_result_t res;
    run_read(ctx, &a, bridge_now_ms(), &res);
    free(match);

    cJSON *r = read_result_json(ctx, &res);
    if (!r) {
        return make_err(id, "io_error", "OOM building response");
    }
    return make_ok(id, r);
}

static char *handle_transact(bridge_ctx_t *ctx, const cJSON *msg)
{
    const cJSON *id = jid(msg);
    const char *op_id = jstr(msg, "op_id");
    if (op_id) {
        const char *hit = dedup_lookup(op_id);
        if (hit) {
            cJSON *r = dedup_result(hit);
            if (r) {
                return make_ok(id, r);
            }
        }
    }

    uint8_t *data = NULL;
    size_t data_len = 0;
    const char *db64 = jstr(msg, "data_b64");
    if (db64) {
        data = b64_decode_alloc(db64, &data_len);
        if (!data) {
            return make_err(id, "bad_request", "invalid data_b64");
        }
    }

    uint64_t barrier = bridge_port_rx_offset(ctx->port);
    uint64_t tx_offset = 0;
    uint32_t tx_seq = 0;
    int n = bridge_port_write(ctx->port, data, data_len, &tx_offset, &tx_seq);
    free(data);
    if (n < 0) {
        return make_err(id, "io_error", "uart write failed");
    }

    bridge_read_args_t a;
    parse_read_args(msg, true, barrier, &a);

    uint8_t *match = NULL;
    size_t match_len = 0;
    const char *mb64 = jstr(msg, "match_b64");
    if (mb64) {
        match = b64_decode_alloc(mb64, &match_len);
        if (!match) {
            return make_err(id, "bad_request", "invalid match_b64");
        }
        a.match = match;
        a.match_len = match_len;
    }

    bridge_read_result_t res;
    run_read(ctx, &a, bridge_now_ms(), &res);
    free(match);

    cJSON *r = read_result_json(ctx, &res);
    if (!r) {
        return make_err(id, "io_error", "OOM building response");
    }
    cJSON_AddNumberToObject(r, "barrier_offset", (double)barrier);
    cJSON_AddNumberToObject(r, "tx_offset", (double)tx_offset);
    cJSON_AddNumberToObject(r, "tx_log_seq", (double)tx_seq);

    if (op_id) {
        dedup_remember(op_id, r);
        cJSON_AddBoolToObject(r, "deduped", false);
    }
    return make_ok(id, r);
}

static char *handle_write(bridge_ctx_t *ctx, const cJSON *msg)
{
    const cJSON *id = jid(msg);
    const char *op_id = jstr(msg, "op_id");
    if (op_id) {
        const char *hit = dedup_lookup(op_id);
        if (hit) {
            cJSON *r = dedup_result(hit);
            if (r) {
                return make_ok(id, r);
            }
        }
    }

    uint8_t *data = NULL;
    size_t data_len = 0;
    const char *db64 = jstr(msg, "data_b64");
    if (db64) {
        data = b64_decode_alloc(db64, &data_len);
        if (!data) {
            return make_err(id, "bad_request", "invalid data_b64");
        }
    }

    uint64_t tx_offset = 0;
    uint32_t tx_seq = 0;
    int n = bridge_port_write(ctx->port, data, data_len, &tx_offset, &tx_seq);
    free(data);
    if (n < 0) {
        return make_err(id, "io_error", "uart write failed");
    }

    cJSON *r = cJSON_CreateObject();
    cJSON_AddNumberToObject(r, "bytes_written", (double)data_len);
    cJSON_AddNumberToObject(r, "tx_offset", (double)tx_offset);
    cJSON_AddNumberToObject(r, "tx_log_seq", (double)tx_seq);
    if (op_id) {
        dedup_remember(op_id, r);
        cJSON_AddBoolToObject(r, "deduped", false);
    }
    return make_ok(id, r);
}

/* ── CONFIG / control ──────────────────────────────────────────────────── */

static char *handle_config(bridge_ctx_t *ctx, const cJSON *msg)
{
    const cJSON *id = jid(msg);
    const bridge_port_config_t *cfg = bridge_port_config(ctx->port);

    int baud = cfg ? cfg->baud_rate : 115200;
    uint32_t v;
    if (ju32(msg, "baud_rate", &v)) {
        if (v < 50 || v > 5000000) {
            return make_err(id, "bad_request", "baud_rate out of range (50..5000000)");
        }
        baud = (int)v;
    }

    uint8_t data_bits = cfg ? cfg->data_bits : 8;
    if (ju32(msg, "data_bits", &v)) {
        if (v < 5 || v > 8) {
            return make_err(id, "bad_request", "data_bits must be 5..8");
        }
        data_bits = (uint8_t)v;
    }

    uint8_t stop_bits = cfg ? cfg->stop_bits : 1;
    if (ju32(msg, "stop_bits", &v)) {
        if (v != 1 && v != 2) {
            return make_err(id, "bad_request", "stop_bits must be 1 or 2");
        }
        stop_bits = (uint8_t)v;
    }

    const char *parity = jstr(msg, "parity");
    if (!parity) {
        parity = cfg ? cfg->parity : "none";
    } else if (strcmp(parity, "none") != 0 && strcmp(parity, "even") != 0 &&
               strcmp(parity, "odd") != 0) {
        return make_err(id, "bad_request", "parity must be none|even|odd");
    }

    const char *flow = jstr(msg, "flow_control");
    if (!flow) {
        flow = cfg ? cfg->flow_control : "none";
    } else if (strcmp(flow, "none") != 0 && strcmp(flow, "rtscts") != 0) {
        return make_err(id, "bad_request", "flow_control must be none|rtscts");
    }

    esp_err_t err = bridge_port_configure(ctx->port, baud, data_bits, stop_bits, parity, flow);
    if (err == ESP_ERR_NOT_SUPPORTED) {
        return make_err(id, "unsupported", "flow_control=rtscts requires RTS/CTS pins");
    }
    if (err != ESP_OK) {
        return make_err(id, "io_error", esp_err_to_name(err));
    }

    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "port", BRIDGE_PORT_ID);
    cJSON *applied = cJSON_AddObjectToObject(r, "applied");
    cJSON_AddNumberToObject(applied, "baud_rate", baud);
    cJSON_AddNumberToObject(applied, "data_bits", data_bits);
    cJSON_AddNumberToObject(applied, "stop_bits", stop_bits);
    cJSON_AddStringToObject(applied, "parity", parity);
    cJSON_AddStringToObject(applied, "flow_control", flow);
    return make_ok(id, r);
}

static char *handle_set_lines(bridge_ctx_t *ctx, const cJSON *msg)
{
    const cJSON *id = jid(msg);
    if (!bridge_port_can_set_lines(ctx->port)) {
        return make_err(id, "unsupported", "no DTR/RTS control on this bridge");
    }

    bool b;
    int dtr = -1;
    int rts = -1;
    if (jbool(msg, "dtr", &b)) {
        dtr = b ? 1 : 0;
    }
    if (jbool(msg, "rts", &b)) {
        rts = b ? 1 : 0;
    }

    int out_dtr = -1;
    int out_rts = -1;
    esp_err_t err = bridge_port_set_lines(ctx->port, dtr, rts, &out_dtr, &out_rts);
    if (err == ESP_ERR_NOT_SUPPORTED) {
        return make_err(id, "unsupported", "requested line is not wired on this bridge");
    }
    if (err != ESP_OK) {
        return make_err(id, "io_error", esp_err_to_name(err));
    }

    cJSON *r = cJSON_CreateObject();
    if (out_dtr >= 0) {
        cJSON_AddBoolToObject(r, "dtr", out_dtr != 0);
    } else {
        cJSON_AddNullToObject(r, "dtr");
    }
    if (out_rts >= 0) {
        cJSON_AddBoolToObject(r, "rts", out_rts != 0);
    } else {
        cJSON_AddNullToObject(r, "rts");
    }
    return make_ok(id, r);
}

static char *handle_break(bridge_ctx_t *ctx, const cJSON *msg)
{
    const cJSON *id = jid(msg);
    uint32_t ms = 300;
    ju32(msg, "duration_ms", &ms);
    if (ms == 0) {
        ms = 300;
    }
    esp_err_t err = bridge_port_send_break(ctx->port, ms);
    if (err != ESP_OK) {
        return make_err(id, "io_error", esp_err_to_name(err));
    }
    cJSON *r = cJSON_CreateObject();
    cJSON_AddNumberToObject(r, "duration_ms", ms);
    return make_ok(id, r);
}

/* ── LOG_* ─────────────────────────────────────────────────────────────── */

static char *handle_log_stats(bridge_ctx_t *ctx, const cJSON *msg)
{
    const cJSON *id = jid(msg);
    bridge_port_stat_t st;
    bridge_port_stat(ctx->port, &st);

    bridge_sd_status_t sd;
    bridge_sd_get_status(&sd);
    const bool use_sd = sd.mounted;

    uint32_t ram_first = 0;
    bool has_ram_first = bridge_port_log_earliest_seq(ctx->port, &ram_first);
    uint32_t ram_latest = bridge_port_log_latest_seq(ctx->port);
    uint32_t ram_count = bridge_port_log_count(ctx->port);

    uint32_t seq = 0;
    bool has_earliest = false;
    uint32_t latest_seq = 0;
    uint64_t capacity = 0;
    uint64_t used = 0;
    if (use_sd) {
        has_earliest = sd.earliest_seq != 0;
        seq = sd.earliest_seq;
        latest_seq = sd.latest_seq;
        capacity = sd.capacity_bytes;
        used = sd.used_bytes;
    } else {
        has_earliest = has_ram_first;
        seq = ram_first;
        latest_seq = ram_latest;
        capacity = bridge_port_log_capacity_bytes(ctx->port);
        used = bridge_port_log_used_bytes(ctx->port);
    }

    cJSON *r = cJSON_CreateObject();
    cJSON_AddNumberToObject(r, "capacity_bytes", (double)capacity);
    cJSON_AddNumberToObject(r, "used_bytes", (double)used);
    if (has_earliest) {
        cJSON_AddNumberToObject(r, "earliest_seq", seq);
    } else {
        cJSON_AddNullToObject(r, "earliest_seq");
    }
    cJSON_AddNumberToObject(r, "latest_seq", (double)latest_seq);
    if (has_ram_first) {
        cJSON_AddNumberToObject(r, "ram_earliest_seq", (double)ram_first);
    } else {
        cJSON_AddNullToObject(r, "ram_earliest_seq");
    }
    cJSON_AddNumberToObject(r, "ram_latest_seq", (double)ram_latest);
    cJSON_AddNumberToObject(r, "ram_count", (double)ram_count);
    if (use_sd) {
        if (sd.earliest_seq) {
            cJSON_AddNumberToObject(r, "sd_earliest_seq", (double)sd.earliest_seq);
        } else {
            cJSON_AddNullToObject(r, "sd_earliest_seq");
        }
        cJSON_AddNumberToObject(r, "sd_latest_seq", (double)sd.latest_seq);
        cJSON_AddNumberToObject(r, "global_latest_seq", (double)sd.global_latest_seq);
        cJSON_AddStringToObject(r, "segment", sd.segment);
        cJSON_AddNumberToObject(r, "raw_bytes", (double)sd.raw_bytes);
        cJSON_AddNumberToObject(r, "index_count", (double)sd.index_count);
        cJSON *segs = cJSON_AddArrayToObject(r, "segments");
        for (size_t i = 0; i < sd.segment_count; i++) {
            const bridge_sd_segment_t *sg = &sd.segments[i];
            cJSON *o = cJSON_CreateObject();
            cJSON_AddStringToObject(o, "id", sg->id);
            cJSON_AddNumberToObject(o, "raw_bytes", (double)sg->raw_bytes);
            cJSON_AddNumberToObject(o, "index_count", (double)sg->index_count);
            cJSON_AddNumberToObject(o, "first_seq", (double)sg->first_seq);
            cJSON_AddNumberToObject(o, "latest_seq", (double)sg->latest_seq);
            cJSON_AddNumberToObject(o, "mtime_s", (double)sg->mtime_s);
            cJSON_AddBoolToObject(o, "active", sg->active);
            cJSON_AddItemToArray(segs, o);
        }
    } else {
        cJSON_AddNullToObject(r, "sd_earliest_seq");
        cJSON_AddNullToObject(r, "sd_latest_seq");
    }
    cJSON_AddStringToObject(r, "source", "device");
    cJSON_AddBoolToObject(r, "authoritative", true);
    cJSON_AddStringToObject(r, "storage", use_sd ? "raw+index" : "ram");
    cJSON_AddNumberToObject(r, "sd_write_errors", (double)sd.write_errors);
    cJSON_AddBoolToObject(r, "sd_broken", sd.broken);

    cJSON_AddNumberToObject(r, "rx_bytes", (double)st.rx_bytes);
    cJSON_AddNumberToObject(r, "tx_bytes", (double)st.tx_bytes);
    cJSON_AddNumberToObject(r, "uart_overflow_events", (double)st.uart_overflow_events);
    cJSON_AddNumberToObject(r, "uart_overflow_bytes", (double)st.uart_overflow_bytes);

    cJSON *ring = cJSON_AddObjectToObject(r, "ring");
    cJSON_AddNumberToObject(ring, "bytes", (double)st.ring_capacity);
    cJSON_AddNumberToObject(ring, "used", (double)st.ring_used);
    cJSON_AddNumberToObject(ring, "start_offset", (double)st.ring_start_offset);
    cJSON_AddNumberToObject(ring, "end_offset", (double)st.ring_end_offset);
    cJSON_AddNumberToObject(ring, "dropped_bytes", (double)st.ring_dropped_bytes);

    cJSON *q = cJSON_AddObjectToObject(r, "sd_queue");
    cJSON_AddNumberToObject(q, "bytes", (double)sd.queue_bytes);
    cJSON_AddNumberToObject(q, "used", (double)sd.queue_used);
    cJSON_AddNumberToObject(q, "dropped", (double)sd.queue_dropped);
    cJSON_AddNumberToObject(q, "dropped_bytes", (double)sd.queue_dropped_bytes);

    cJSON *time = cJSON_AddObjectToObject(r, "time");
    cJSON_AddBoolToObject(time, "synced", false);
    cJSON_AddStringToObject(time, "source", "tick");
    return make_ok(id, r);
}

static char *handle_log_query(bridge_ctx_t *ctx, const cJSON *msg)
{
    const cJSON *id = jid(msg);
    const bridge_port_config_t *cfg = bridge_port_config(ctx->port);

    const char *dir = jstr(msg, "dir");
    int dir_filter = -1;
    if (dir) {
        if (strcmp(dir, "rx") == 0) {
            dir_filter = 0;
        } else if (strcmp(dir, "tx") == 0) {
            dir_filter = 1;
        } else {
            return make_err(id, "bad_request", "dir must be rx|tx");
        }
    }

    bool has_from = false;
    uint32_t seq_from = 0;
    const char *cursor = jstr(msg, "cursor");
    if (cursor && cursor[0]) {
        has_from = true;
        seq_from = (uint32_t)strtoul(cursor, NULL, 10) + 1;
    }
    if (ju32(msg, "seq_from", &seq_from)) {
        has_from = true;
    }
    bool has_to = false;
    uint32_t seq_to = 0;
    if (ju32(msg, "seq_to", &seq_to)) {
        has_to = true;
    }

    uint32_t limit = DEFAULT_LOG_LIMIT;
    if (ju32(msg, "limit", &limit)) {
        if (limit == 0) {
            limit = DEFAULT_LOG_LIMIT;
        }
        if (limit > MAX_LOG_LIMIT) {
            limit = MAX_LOG_LIMIT;
        }
    }

    size_t text_cap = (cfg ? cfg->log_line_max : 192) + 1;
    char *text = (char *)malloc(text_cap);
    if (!text) {
        return make_err(id, "io_error", "OOM");
    }

    cJSON *entries = cJSON_CreateArray();
    uint32_t count = 0;
    bool truncated = false;
    uint32_t last_seq = 0;
    bool have_last = false;

    uint32_t total = bridge_port_log_count(ctx->port);
    for (uint32_t i = 0; i < total; i++) {
        bridge_log_meta_t m;
        if (!bridge_port_log_get(ctx->port, i, &m, text, text_cap)) {
            continue;
        }
        if (dir_filter >= 0 && m.dir != (uint8_t)dir_filter) {
            continue;
        }
        if (has_from && m.seq < seq_from) {
            continue;
        }
        if (has_to && m.seq > seq_to) {
            continue;
        }
        if (count >= limit) {
            truncated = true;
            break;
        }

        /* `line` is ASCII-sanitised for JSON safety; `hex` is authoritative. */
        char *line = (char *)malloc(m.text_len + 1);
        if (!line) {
            break;
        }
        for (uint32_t k = 0; k < m.text_len; k++) {
            unsigned char c = (unsigned char)text[k];
            line[k] = (c >= 0x20 && c < 0x7F) ? (char)c : '.';
        }
        line[m.text_len] = '\0';
        char *hex = hex_alloc((const uint8_t *)text, m.text_len);
        if (!hex) {
            free(line);
            break;
        }

        cJSON *e = cJSON_CreateObject();
        cJSON_AddNumberToObject(e, "seq", m.seq);
        cJSON_AddNumberToObject(e, "ts", (double)m.ts_ms);
        cJSON_AddStringToObject(e, "dir", m.dir ? "tx" : "rx");
        cJSON_AddNumberToObject(e, "offset", (double)m.offset);
        cJSON_AddStringToObject(e, "line", line);
        cJSON_AddStringToObject(e, "hex", hex);
        cJSON_AddNumberToObject(e, "bytes", m.bytes);
        if (m.truncated) {
            cJSON_AddBoolToObject(e, "truncated", true);
        }
        cJSON_AddItemToArray(entries, e);

        free(line);
        free(hex);
        count++;
        last_seq = m.seq;
        have_last = true;
    }
    free(text);

    uint32_t earliest = 0;
    bool has_earliest = bridge_port_log_earliest_seq(ctx->port, &earliest);

    cJSON *r = cJSON_CreateObject();
    cJSON_AddItemToObject(r, "entries", entries);
    cJSON_AddNumberToObject(r, "count", count);
    cJSON_AddBoolToObject(r, "truncated", truncated);
    cJSON_AddNumberToObject(r, "ram_latest_seq", (double)bridge_port_log_latest_seq(ctx->port));
    cJSON_AddNumberToObject(r, "ram_count", (double)bridge_port_log_count(ctx->port));
    if (has_earliest) {
        cJSON_AddNumberToObject(r, "ram_earliest_seq", (double)earliest);
    } else {
        cJSON_AddNullToObject(r, "ram_earliest_seq");
    }
    bridge_sd_status_t sd;
    bridge_sd_get_status(&sd);
    if (sd.mounted && sd.earliest_seq) {
        cJSON_AddNumberToObject(r, "sd_earliest_seq", (double)sd.earliest_seq);
    } else {
        cJSON_AddNullToObject(r, "sd_earliest_seq");
    }
    if (sd.mounted) {
        cJSON_AddNumberToObject(r, "sd_latest_seq", (double)sd.latest_seq);
    } else {
        cJSON_AddNullToObject(r, "sd_latest_seq");
    }
    if (have_last) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%u", (unsigned)last_seq);
        cJSON_AddStringToObject(r, "next_cursor", buf);
    } else {
        cJSON_AddNullToObject(r, "next_cursor");
    }
    if (has_earliest) {
        cJSON_AddNumberToObject(r, "earliest_seq", earliest);
    } else {
        cJSON_AddNullToObject(r, "earliest_seq");
    }
    cJSON_AddStringToObject(r, "source", "device");
    cJSON_AddBoolToObject(r, "authoritative", true);
    cJSON_AddStringToObject(r, "time_source", "tick");
    return make_ok(id, r);
}

static char *handle_event_query(bridge_ctx_t *ctx, const cJSON *msg)
{
    (void)ctx;
    const cJSON *id = jid(msg);
    uint32_t seq_from = 1;
    uint32_t limit = 100;
    ju32(msg, "seq_from", &seq_from);
    if (ju32(msg, "limit", &limit)) {
        if (limit == 0) {
            limit = 100;
        }
        if (limit > 256) {
            limit = 256;
        }
    }
    const char *level = jstr(msg, "level");
    int level_filter = -1;
    if (level) {
        if (strcmp(level, "info") == 0) {
            level_filter = BRIDGE_EVENT_INFO;
        } else if (strcmp(level, "warn") == 0) {
            level_filter = BRIDGE_EVENT_WARN;
        } else if (strcmp(level, "error") == 0) {
            level_filter = BRIDGE_EVENT_ERROR;
        } else {
            return make_err(id, "bad_request", "level must be info|warn|error");
        }
    }

    bridge_event_t *evs = (bridge_event_t *)malloc(sizeof(bridge_event_t) * 256);
    if (!evs) {
        return make_err(id, "io_error", "OOM");
    }
    uint32_t n = bridge_event_query(seq_from, limit, evs, 256);
    cJSON *arr = cJSON_CreateArray();
    uint32_t count = 0;
    uint32_t last_seq = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (level_filter >= 0 && evs[i].level != (uint8_t)level_filter) {
            continue;
        }
        cJSON *e = cJSON_CreateObject();
        cJSON_AddNumberToObject(e, "seq", evs[i].seq);
        cJSON_AddNumberToObject(e, "ts", (double)evs[i].ts_ms);
        const char *lname = evs[i].level == BRIDGE_EVENT_ERROR
                                ? "error"
                                : (evs[i].level == BRIDGE_EVENT_WARN ? "warn" : "info");
        cJSON_AddStringToObject(e, "level", lname);
        cJSON_AddStringToObject(e, "code", evs[i].code);
        cJSON_AddStringToObject(e, "msg", evs[i].msg);
        cJSON_AddItemToArray(arr, e);
        count++;
        last_seq = evs[i].seq;
    }
    free(evs);

    uint32_t earliest = 0;
    bool has_earliest = bridge_event_earliest(&earliest);
    cJSON *r = cJSON_CreateObject();
    cJSON_AddItemToObject(r, "events", arr);
    cJSON_AddNumberToObject(r, "count", count);
    if (has_earliest) {
        cJSON_AddNumberToObject(r, "earliest_seq", earliest);
    } else {
        cJSON_AddNullToObject(r, "earliest_seq");
    }
    cJSON_AddNumberToObject(r, "latest_seq", (double)bridge_event_latest());
    if (count) {
        char cur[24];
        snprintf(cur, sizeof(cur), "%lu", (unsigned long)last_seq);
        cJSON_AddStringToObject(r, "next_cursor", cur);
    } else {
        cJSON_AddNullToObject(r, "next_cursor");
    }
    cJSON_AddStringToObject(r, "source", "device");
    cJSON_AddStringToObject(r, "time_source", "tick");
    return make_ok(id, r);
}

static char *handle_log_export(bridge_ctx_t *ctx, const cJSON *msg)
{
    const cJSON *id = jid(msg);
    if (!bridge_sd_ready()) {
        return make_err(id, "unsupported", "no SD card (log_storage=ram)");
    }

    bridge_sd_export_req_t req;
    memset(&req, 0, sizeof(req));
    const char *dir = jstr(msg, "dir");
    if (!dir || strcmp(dir, "rx") == 0) {
        req.dir = 0;
    } else if (strcmp(dir, "tx") == 0) {
        req.dir = 1;
    } else {
        return make_err(id, "bad_request", "dir must be rx|tx");
    }

    uint32_t v;
    if (ju32(msg, "seq_from", &v)) {
        req.has_seq_from = true;
        req.seq_from = v;
    }
    if (ju32(msg, "seq_to", &v)) {
        req.has_seq_to = true;
        req.seq_to = v;
    }
    if (ju32(msg, "max_bytes", &v)) {
        req.max_bytes = v;
    }
    const char *segment = jstr(msg, "segment");
    if (segment && segment[0]) {
        req.segment = segment;
    }
    const char *cursor = jstr(msg, "cursor");
    if (cursor && cursor[0]) {
        req.cursor = strtoull(cursor, NULL, 10);
    }
    uint64_t c64;
    if (ju64(msg, "cursor", &c64)) {
        req.cursor = c64;
    }

    bridge_sd_export_t ex;
    esp_err_t err = bridge_sd_export(&req, &ex);
    if (err != ESP_OK) {
        return make_err(id, "io_error", esp_err_to_name(err));
    }

    char *chunk_b64 = b64_encode_alloc(ex.chunk, ex.chunk_len);
    char *index_b64 = b64_encode_alloc(ex.index, ex.index_len);
    if (!chunk_b64 || !index_b64) {
        free(chunk_b64);
        free(index_b64);
        bridge_sd_export_free(&ex);
        return make_err(id, "io_error", "OOM building LOG_EXPORT response");
    }

    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "chunk_b64", chunk_b64);
    cJSON_AddStringToObject(r, "index_b64", index_b64);
    cJSON_AddNumberToObject(r, "bytes", (double)ex.chunk_len);
    if (ex.eof) {
        cJSON_AddNullToObject(r, "next_cursor");
    } else {
        char cur[24];
        snprintf(cur, sizeof(cur), "%llu", (unsigned long long)ex.next_cursor);
        cJSON_AddStringToObject(r, "next_cursor", cur);
    }
    cJSON_AddBoolToObject(r, "eof", ex.eof);
    if (ex.segment[0]) {
        cJSON_AddStringToObject(r, "segment", ex.segment);
    }
    cJSON_AddStringToObject(r, "source", "device");
    cJSON_AddBoolToObject(r, "authoritative", true);
    cJSON_AddStringToObject(r, "time_source", "tick");

    free(chunk_b64);
    free(index_b64);
    bridge_sd_export_free(&ex);
    return make_ok(id, r);
}

static char *handle_log_clear(bridge_ctx_t *ctx, const cJSON *msg)
{
    const cJSON *id = jid(msg);
    const char *segment = jstr(msg, "segment");
    if (segment && segment[0] &&
        (strcmp(segment, "all") == 0 || strcmp(segment, "*") == 0)) {
        segment = NULL;
    }
    bool active_cleared = false;
    bool sd_cleared = false;
    if (bridge_sd_ready()) {
        esp_err_t err = bridge_sd_clear(segment, &active_cleared);
        if (err != ESP_OK) {
            return make_err(id, err == ESP_ERR_INVALID_ARG ? "bad_request" : "io_error",
                            esp_err_to_name(err));
        }
        sd_cleared = true;
    } else if (segment && segment[0]) {
        return make_err(id, "unsupported", "no SD card; cannot clear a segment");
    }
    const bool clear_ram = !segment || !segment[0] || active_cleared;
    if (clear_ram) {
        bridge_port_log_clear(ctx->port, !segment || !segment[0]);
    }
    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "cleared", true);
    cJSON_AddBoolToObject(r, "sd_cleared", sd_cleared);
    cJSON_AddBoolToObject(r, "ram_cleared", clear_ram);
    cJSON_AddBoolToObject(r, "active_cleared", active_cleared);
    if (segment && segment[0]) {
        cJSON_AddStringToObject(r, "segment", segment);
    }
    return make_ok(id, r);
}

/* ── dispatcher ────────────────────────────────────────────────────────── */

char *bridge_proto_handle(bridge_ctx_t *ctx,
                          bool *authed,
                          const char *line,
                          bool *close_after)
{
    if (close_after) {
        *close_after = false;
    }
    if (!ctx || !line || !authed) {
        return NULL;
    }

    cJSON *msg = cJSON_Parse(line);
    if (!msg) {
        return NULL; /* malformed line: ignore, same as the reference bridge */
    }

    const cJSON *id = jid(msg);
    const char *op = jstr(msg, "op");
    char *out = NULL;

    if (!op) {
        out = make_err(id, "bad_request", "missing op");
        goto done;
    }

    if (strcmp(op, "HELLO") == 0) {
        const char *proto = jstr(msg, "proto");
        if (!proto || strcmp(proto, BRIDGE_PROTO_VERSION) != 0) {
            out = make_err(id, "bad_request", "unsupported proto (expect v0)");
            goto done;
        }
        if (ctx->token && ctx->token[0]) {
            const char *tok = jstr(msg, "token");
            if (!ct_streq(tok, ctx->token)) {
                out = make_err(id, "unauthorized", "bad token");
                if (close_after) {
                    *close_after = true;
                }
                goto done;
            }
        }
        *authed = true;
        out = make_ok(id, build_info(ctx));
        goto done;
    }

    if (!*authed) {
        out = make_err(id, "unauthorized", "HELLO required");
        if (close_after) {
            *close_after = true;
        }
        goto done;
    }

    if (strcmp(op, "EVENT_QUERY") == 0) {
        out = handle_event_query(ctx, msg);
        goto done;
    }

    if (strcmp(op, "PING") == 0) {
        uint64_t t = 0;
        if (!ju64(msg, "t_ms", &t)) {
            t = 0;
        }
        cJSON *r = cJSON_CreateObject();
        cJSON_AddNumberToObject(r, "t_ms", (double)t);
        cJSON_AddNumberToObject(r, "device_tick_ms", (double)bridge_now_ms());
        out = make_ok(id, r);
        goto done;
    }

    if (!ctx->port) {
        out = make_err(id, "port_not_found", "no port configured");
        goto done;
    }

    if (strcmp(op, "INFO") == 0) {
        out = make_ok(id, build_info(ctx));
        goto done;
    }

    if (strcmp(op, "LIST_PORTS") == 0) {
        cJSON *r = cJSON_CreateObject();
        cJSON *ports = cJSON_AddArrayToObject(r, "ports");
        cJSON *p = cJSON_CreateObject();
        cJSON_AddStringToObject(p, "id", BRIDGE_PORT_ID);
        cJSON_AddStringToObject(p, "kind", BRIDGE_PORT_KIND);
        cJSON_AddItemToArray(ports, p);
        out = make_ok(id, r);
        goto done;
    }

    if (!port_matches(msg)) {
        out = make_err(id, "port_not_found", "unknown port (this bridge exposes uart0)");
        goto done;
    }

    if (strcmp(op, "CONFIG") == 0) {
        out = handle_config(ctx, msg);
    } else if (strcmp(op, "WRITE") == 0) {
        out = handle_write(ctx, msg);
    } else if (strcmp(op, "READ") == 0) {
        out = handle_read(ctx, msg);
    } else if (strcmp(op, "TRANSACT") == 0) {
        out = handle_transact(ctx, msg);
    } else if (strcmp(op, "SET_LINES") == 0) {
        out = handle_set_lines(ctx, msg);
    } else if (strcmp(op, "BREAK") == 0) {
        out = handle_break(ctx, msg);
    } else if (strcmp(op, "LOG_STATS") == 0) {
        out = handle_log_stats(ctx, msg);
    } else if (strcmp(op, "LOG_QUERY") == 0) {
        out = handle_log_query(ctx, msg);
    } else if (strcmp(op, "LOG_CLEAR") == 0) {
        out = handle_log_clear(ctx, msg);
    } else if (strcmp(op, "LOG_EXPORT") == 0) {
        out = handle_log_export(ctx, msg);
    } else if (strcmp(op, "CLOSE") == 0) {
        cJSON *r = cJSON_CreateObject();
        cJSON_AddStringToObject(r, "port", BRIDGE_PORT_ID);
        cJSON_AddBoolToObject(r, "closed", true);
        out = make_ok(id, r);
        if (close_after) {
            *close_after = true;
        }
    } else {
        out = make_err(id, "unknown_op", "unsupported op");
    }

done:
    cJSON_Delete(msg);
    return out;
}