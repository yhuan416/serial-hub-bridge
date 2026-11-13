#include "bridge_sd.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "driver/gpio.h"
#include "driver/sdspi_host.h"
#include "driver/spi_master.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "bridge_event.h"
#include "bridge.h"
#include "bridge_time.h"
#include "bridge_port.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "sdmmc_cmd.h"

#ifndef CONFIG_BRIDGE_SD_FORMAT_IF_FAILED
#define CONFIG_BRIDGE_SD_FORMAT_IF_FAILED 0
#endif
#ifndef CONFIG_BRIDGE_SD_SEGMENT_MAX_MB
#define CONFIG_BRIDGE_SD_SEGMENT_MAX_MB 3584
#endif
#ifndef CONFIG_BRIDGE_SD_QUEUE_BYTES
#define CONFIG_BRIDGE_SD_QUEUE_BYTES 3145728
#endif
#ifndef CONFIG_BRIDGE_SD_UNACKED_FREE_MB
#define CONFIG_BRIDGE_SD_UNACKED_FREE_MB 256
#endif

#define TAG "bridge_sd"

#define SD_DIR CONFIG_BRIDGE_SD_MOUNT_POINT "/shb"
#define EXPORT_CHUNK_MAX 32768
#define EXPORT_LINES_MAX 512
#define RETENTION_EVERY 60
#define SD_Q_HDR 20

/* Export/writer scratch is large and transient; prefer PSRAM so internal DRAM
 * stays available for Wi-Fi/PHY and TCP session stacks. */
static void *sd_alloc(size_t n)
{
    void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) {
        p = malloc(n);
    }
    return p;
}

typedef struct __attribute__((packed)) {
    uint32_t seq;
    uint32_t tick_ms;
    uint16_t len;
    uint16_t raw_len;
    uint8_t dir;
    uint8_t port;
    uint16_t flags;
    uint32_t generation;
} sd_q_hdr_t;

typedef struct {
    char id[32];
    char path[4][192];
    FILE *raw[2];
    FILE *idx[2];
    uint64_t raw_bytes[2];
    uint32_t idx_count[2];
    uint32_t first_seq;
    uint32_t last_seq;
    bridge_port_t *port;
    uint32_t resume_seq;
    uint32_t seg_boot_id;
    int64_t time_base;
    bool time_synced;
    bool initial_mount;
    uint32_t generation;
} sd_log_t;

static sd_log_t s_logs[BRIDGE_MAX_PORTS];
static int s_cur_port;
static int s_port_count = 1;
#define s_id (s_logs[s_cur_port].id)
#define s_path (s_logs[s_cur_port].path)
#define s_raw (s_logs[s_cur_port].raw)
#define s_idx (s_logs[s_cur_port].idx)
#define s_raw_bytes (s_logs[s_cur_port].raw_bytes)
#define s_idx_count (s_logs[s_cur_port].idx_count)
#define s_first_seq (s_logs[s_cur_port].first_seq)
#define s_last_seq (s_logs[s_cur_port].last_seq)
#define s_port (s_logs[s_cur_port].port)
#define s_resume_seq (s_logs[s_cur_port].resume_seq)
#define s_seg_boot_id (s_logs[s_cur_port].seg_boot_id)
#define s_time_base (s_logs[s_cur_port].time_base)
#define s_time_synced (s_logs[s_cur_port].time_synced)
#define s_initial_mount (s_logs[s_cur_port].initial_mount)
#define s_generation (s_logs[s_cur_port].generation)

static SemaphoreHandle_t s_lock;
static sdmmc_card_t *s_card;
static esp_timer_handle_t s_timer;
static esp_timer_handle_t s_retry_timer;
static volatile bool s_mounted;
static volatile bool s_broken;
static bool s_sync_error;
static bool s_enabled;
static bool s_bus_ready;
static uint32_t s_write_errors;
static uint32_t s_mount_fail_count;
static uint32_t s_last_write_event_ms;

static uint32_t s_sync_ticks;
static uint32_t s_boot_id;

static uint8_t *s_qbuf;
static size_t s_qcap;
static size_t s_qhead;
static size_t s_qtail;
static size_t s_qused;
static SemaphoreHandle_t s_qlock;
static SemaphoreHandle_t s_qsig;
static TaskHandle_t s_qtask;
static uint32_t s_qdrop;
static uint64_t s_qdrop_bytes;
static uint32_t s_qdrop_event_ms;

static esp_err_t sd_try_mount(int attempts);
static void sd_remount(void);
static void sd_writer_task(void *arg);
static bool sd_seg_id_ok(const char *id);
static void sd_paths_for(const char *id, char paths[4][192]);
static void sd_port_dir(char *out, size_t cap);
static void sd_index_bounds_path(const char *path, uint32_t *count,
                                 uint32_t *first, uint32_t *last);
static bool sd_q_idle(void);

static void sd_lock(void)
{
    if (s_lock) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
}

static void sd_unlock(void)
{
    if (s_lock) {
        xSemaphoreGive(s_lock);
    }
}

static void sd_fsync(FILE *f)
{
    if (!f) {
        return;
    }
    fflush(f);
    if (fsync(fileno(f)) != 0 && !s_sync_error) {
        s_sync_error = true;
        ESP_LOGW(TAG, "fsync failed (data still buffered in FAT)");
        bridge_event_push(BRIDGE_EVENT_WARN, "sd.sync",
                          "fsync failed; data may remain in FAT cache");
    }
}

static void sd_close_segment_locked(void)
{
    for (int i = 0; i < 2; i++) {
        if (s_raw[i]) {
            sd_fsync(s_raw[i]);
            fclose(s_raw[i]);
            s_raw[i] = NULL;
        }
        if (s_idx[i]) {
            sd_fsync(s_idx[i]);
            fclose(s_idx[i]);
            s_idx[i] = NULL;
        }
        s_raw_bytes[i] = 0;
        s_idx_count[i] = 0;
    }
    s_first_seq = 0;
    s_last_seq = 0;
}

#define SD_ACK_MAGIC 0x5348414bU
#define SD_TIME_MAGIC 0x5348544dU

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t boot_id;
    int64_t epoch_base_ms;
    uint32_t synced;
    uint32_t reserved;
} sd_time_meta_t;

static void sd_time_path(const char *id, char *out, size_t cap)
{
    char dir[96];
    sd_port_dir(dir, sizeof(dir));
    snprintf(out, cap, "%s/time-%.63s.bin", dir, id);
}

static bool sd_read_time_path(const char *id, int64_t *base, bool *synced, uint32_t *boot_id)
{
    char path[160];
    sd_time_path(id, path, sizeof(path));
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    sd_time_meta_t m = {0};
    bool ok = fread(&m, sizeof(m), 1, f) == 1 && m.magic == SD_TIME_MAGIC;
    fclose(f);
    if (!ok) return false;
    if (base) *base = m.epoch_base_ms;
    if (synced) *synced = m.synced != 0;
    if (boot_id) *boot_id = m.boot_id;
    return true;
}

static bool sd_write_time_path(const char *id, uint32_t boot_id, int64_t base, bool synced)
{
    char path[160];
    sd_time_path(id, path, sizeof(path));
    sd_time_meta_t m = {
        .magic = SD_TIME_MAGIC,
        .boot_id = boot_id,
        .epoch_base_ms = base,
        .synced = synced ? 1U : 0U,
        .reserved = 0,
    };
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    bool ok = fwrite(&m, sizeof(m), 1, f) == 1;
    fflush(f);
    fsync(fileno(f));
    fclose(f);
    return ok;
}

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t rx;
    uint32_t tx;
} sd_ack_t;

static void sd_ack_path(const char *id, char *out, size_t cap)
{
    char dir[96];
    sd_port_dir(dir, sizeof(dir));
    snprintf(out, cap, "%s/ack-%.63s.bin", dir, id);
}

static bool sd_read_ack_path(const char *id, uint32_t *rx, uint32_t *tx)
{
    char path[160];
    sd_ack_path(id, path, sizeof(path));
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    sd_ack_t a = {0};
    bool ok = fread(&a, sizeof(a), 1, f) == 1 && a.magic == SD_ACK_MAGIC;
    fclose(f);
    if (ok) {
        if (rx) *rx = a.rx;
        if (tx) *tx = a.tx;
    }
    return ok;
}

static bool sd_write_ack_path(const char *id, uint32_t rx, uint32_t tx)
{
    char path[160];
    sd_ack_path(id, path, sizeof(path));
    sd_ack_t a = {.magic = SD_ACK_MAGIC, .rx = rx, .tx = tx};
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    bool ok = fwrite(&a, sizeof(a), 1, f) == 1;
    fflush(f);
    fsync(fileno(f));
    fclose(f);
    return ok;
}

static void sd_retention_locked(void)
{
    typedef struct {
        char id[40];
        uint64_t size;
        uint32_t latest_rx;
        uint32_t latest_tx;
        uint32_t acked_rx;
        uint32_t acked_tx;
        time_t mtime;
        bool acked;
        bool deleted;
    } seg_ent_t;
    const int max_segs = 64;
    seg_ent_t *segs = (seg_ent_t *)calloc(max_segs, sizeof(seg_ent_t));
    if (!segs) return;
    char pdir[96];
    sd_port_dir(pdir, sizeof(pdir));
    DIR *d = opendir(pdir);
    if (!d) {
        free(segs);
        return;
    }
    int n = 0;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        const char *name = de->d_name;
        const char *p = NULL;
        if (strncmp(name, "raw-rx-", 7) == 0 || strncmp(name, "raw-tx-", 7) == 0 ||
            strncmp(name, "idx-rx-", 7) == 0 || strncmp(name, "idx-tx-", 7) == 0) {
            p = name + 7;
        } else {
            continue;
        }
        const char *dot = strstr(p, ".bin");
        if (!dot) continue;
        size_t idlen = (size_t)(dot - p);
        if (idlen == 0 || idlen >= sizeof(segs[0].id)) continue;
        char id[40] = {0};
        memcpy(id, p, idlen);
        if (!sd_seg_id_ok(id)) continue;
        int idx = -1;
        for (int i = 0; i < n; i++) {
            if (strcmp(segs[i].id, id) == 0) {
                idx = i;
                break;
            }
        }
        if (idx < 0) {
            if (n >= max_segs) continue;
            idx = n++;
            snprintf(segs[idx].id, sizeof(segs[idx].id), "%s", id);
            segs[idx].size = 0;
            segs[idx].latest_rx = 0;
            segs[idx].latest_tx = 0;
            segs[idx].acked_rx = 0;
            segs[idx].acked_tx = 0;
            segs[idx].mtime = 0;
            segs[idx].acked = false;
            segs[idx].deleted = false;
            sd_read_ack_path(id, &segs[idx].acked_rx, &segs[idx].acked_tx);
        }
        char full[320];
        snprintf(full, sizeof(full), "%s/%.200s", pdir, name);
        struct stat st;
        if (stat(full, &st) == 0) {
            segs[idx].size += (uint64_t)st.st_size;
            if (st.st_mtime > segs[idx].mtime) segs[idx].mtime = st.st_mtime;
        }
        if (strncmp(name, "idx-rx-", 7) == 0) {
            uint32_t cnt = 0, first = 0, last = 0;
            sd_index_bounds_path(full, &cnt, &first, &last);
            segs[idx].latest_rx = last;
        } else if (strncmp(name, "idx-tx-", 7) == 0) {
            uint32_t cnt = 0, first = 0, last = 0;
            sd_index_bounds_path(full, &cnt, &first, &last);
            segs[idx].latest_tx = last;
        }
    }
    closedir(d);

    uint64_t total = 0;
    for (int i = 0; i < n; i++) {
        total += segs[i].size;
        segs[i].acked = segs[i].acked_rx >= segs[i].latest_rx &&
                        segs[i].acked_tx >= segs[i].latest_tx;
    }
    const uint64_t retain = (uint64_t)CONFIG_BRIDGE_SD_RETAIN_MB * 1024ULL * 1024ULL;

    /* Prefer deleting fully archived segments. */
    while (total > retain) {
        int oldest = -1;
        for (int i = 0; i < n; i++) {
            if (segs[i].deleted || !segs[i].acked) continue;
            if (s_id[0] && strcmp(segs[i].id, s_id) == 0) continue;
            if (oldest < 0 || segs[i].mtime < segs[oldest].mtime) oldest = i;
        }
        if (oldest < 0) break;
        char paths[4][192];
        sd_paths_for(segs[oldest].id, paths);
        for (int k = 0; k < 4; k++) unlink(paths[k]);
        char ack[160];
        sd_ack_path(segs[oldest].id, ack, sizeof(ack));
        unlink(ack);
        ESP_LOGI(TAG, "retention: remove archived segment %s (%llu KiB)", segs[oldest].id,
                 (unsigned long long)(segs[oldest].size / 1024));
        bridge_event_push(BRIDGE_EVENT_INFO, "sd.retention",
                          "removed archived segment %.39s (%llu KiB)", segs[oldest].id,
                          (unsigned long long)(segs[oldest].size / 1024));
        total -= segs[oldest].size;
        segs[oldest].deleted = true;
    }

    /* Last-resort: if the card is nearly full, drop the oldest unarchived
     * segment with a loud warning rather than failing all future writes. */
    uint64_t fs_total = 0, fs_free = 0;
    if (esp_vfs_fat_info(CONFIG_BRIDGE_SD_MOUNT_POINT, &fs_total, &fs_free) == ESP_OK) {
        const uint64_t reserve = (uint64_t)CONFIG_BRIDGE_SD_UNACKED_FREE_MB * 1024ULL * 1024ULL;
        while (total > retain && fs_free < reserve) {
            int oldest = -1;
            for (int i = 0; i < n; i++) {
                if (segs[i].deleted) continue;
                if (s_id[0] && strcmp(segs[i].id, s_id) == 0) continue;
                if (oldest < 0 || segs[i].mtime < segs[oldest].mtime) oldest = i;
            }
            if (oldest < 0) break;
            char paths[4][192];
            sd_paths_for(segs[oldest].id, paths);
            for (int k = 0; k < 4; k++) unlink(paths[k]);
            char ack[160];
            sd_ack_path(segs[oldest].id, ack, sizeof(ack));
            unlink(ack);
            ESP_LOGW(TAG, "retention: FORCE remove unarchived segment %s (%llu KiB)",
                     segs[oldest].id, (unsigned long long)(segs[oldest].size / 1024));
            bridge_event_push(BRIDGE_EVENT_WARN, "sd.retention_unacked",
                              "forced removed unarchived segment %.39s (%llu KiB)",
                              segs[oldest].id,
                              (unsigned long long)(segs[oldest].size / 1024));
            total -= segs[oldest].size;
            segs[oldest].deleted = true;
            fs_free += segs[oldest].size;
        }
    }
    free(segs);
}

static bool sd_seg_id_ok(const char *id)
{
    if (!id || !id[0] || strlen(id) >= sizeof(s_id)) {
        return false;
    }
    for (const char *c = id; *c; c++) {
        if (!((*c >= '0' && *c <= '9') || (*c >= 'a' && *c <= 'f') ||
              (*c >= 'A' && *c <= 'F') || *c == '-')) {
            return false;
        }
    }
    return true;
}

static void sd_port_dir(char *out, size_t cap)
{
    snprintf(out, cap, "%s/p%d", SD_DIR, s_cur_port);
}

static void sd_paths_for(const char *id, char paths[4][192])
{
    char dir[96];
    sd_port_dir(dir, sizeof(dir));
    snprintf(paths[0], sizeof(paths[0]), "%s/raw-rx-%.63s.bin", dir, id);
    snprintf(paths[1], sizeof(paths[1]), "%s/idx-rx-%.63s.bin", dir, id);
    snprintf(paths[2], sizeof(paths[2]), "%s/raw-tx-%.63s.bin", dir, id);
    snprintf(paths[3], sizeof(paths[3]), "%s/idx-tx-%.63s.bin", dir, id);
}

static bool sd_stat_size_mtime(const char *path, uint64_t *size, int64_t *mtime)
{
    struct stat st;
    if (stat(path, &st) != 0) {
        return false;
    }
    if (size) {
        *size = (uint64_t)st.st_size;
    }
    if (mtime) {
        *mtime = (int64_t)st.st_mtime;
    }
    return true;
}

static void sd_index_bounds_path(const char *path,
                                 uint32_t *count,
                                 uint32_t *first,
                                 uint32_t *last)
{
    *count = 0;
    *first = 0;
    *last = 0;
    FILE *f = fopen(path, "rb");
    if (!f) {
        return;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return;
    }
    long sz = ftell(f);
    if (sz < 0) {
        fclose(f);
        return;
    }
    *count = (uint32_t)(sz / (long)sizeof(sd_idx_rec_t));
    sd_idx_rec_t rec;
    if (*count > 0 && fseek(f, 0, SEEK_SET) == 0 &&
        fread(&rec, sizeof(rec), 1, f) == 1) {
        *first = rec.seq;
    }
    if (*count > 0 &&
        fseek(f, (long)(*count - 1) * (long)sizeof(sd_idx_rec_t), SEEK_SET) == 0 &&
        fread(&rec, sizeof(rec), 1, f) == 1) {
        *last = rec.seq;
    }
    fclose(f);
}

typedef struct {
    char id[40];
    uint64_t raw_bytes[2];
    uint32_t index_count[2];
    uint32_t first_seq;
    uint32_t last_seq;
    uint32_t latest_seq[2];
    uint32_t acked_rx;
    uint32_t acked_tx;
    int64_t epoch_base_ms;
    int64_t mtime_s;
    bool acked;
    bool time_synced;
} sd_scan_seg_t;

/* Scan complete segment quads (raw/idx rx/tx). Newest last; active is set by caller. */
static int sd_scan_segments_locked(sd_scan_seg_t *out, int max, uint32_t *global_latest)
{
    if (!out || max <= 0) {
        return 0;
    }
    if (global_latest) {
        *global_latest = 0;
    }
    char pdir[96];
    sd_port_dir(pdir, sizeof(pdir));
    DIR *d = opendir(pdir);
    if (!d) {
        return 0;
    }
    int n = 0;
    struct dirent *de;
    const char *prefix = "idx-rx-";
    while ((de = readdir(d)) != NULL) {
        const char *name = de->d_name;
        if (strncmp(name, prefix, strlen(prefix)) != 0) {
            continue;
        }
        const char *dot = strstr(name + strlen(prefix), ".bin");
        if (!dot || dot[4] != '\0') {
            continue;
        }
        size_t idlen = (size_t)(dot - (name + strlen(prefix)));
        if (idlen == 0 || idlen >= sizeof(out[0].id)) {
            continue;
        }
        char id[40] = {0};
        memcpy(id, name + strlen(prefix), idlen);
        if (!sd_seg_id_ok(id)) {
            continue;
        }
        char paths[4][192];
        sd_paths_for(id, paths);
        uint64_t sizes[4] = {0};
        int64_t mtimes[4] = {0};
        bool ok = true;
        for (int k = 0; k < 4; k++) {
            if (!sd_stat_size_mtime(paths[k], &sizes[k], &mtimes[k])) {
                ok = false;
                break;
            }
        }
        if (!ok) {
            continue;
        }
        sd_scan_seg_t seg = {0};
        snprintf(seg.id, sizeof(seg.id), "%s", id);
        for (int k = 0; k < 2; k++) {
            uint32_t cnt = 0, first = 0, last = 0;
            sd_index_bounds_path(paths[1 + k * 2], &cnt, &first, &last);
            seg.raw_bytes[k] = sizes[k * 2];
            seg.index_count[k] = cnt;
            if (first && (!seg.first_seq || first < seg.first_seq)) {
                seg.first_seq = first;
            }
            seg.latest_seq[k] = last;
            if (last > seg.last_seq) {
                seg.last_seq = last;
            }
        }
        sd_read_ack_path(id, &seg.acked_rx, &seg.acked_tx);
        seg.acked = seg.acked_rx >= seg.latest_seq[0] && seg.acked_tx >= seg.latest_seq[1];
        uint32_t boot_id = 0;
        if (!sd_read_time_path(id, &seg.epoch_base_ms, &seg.time_synced, &boot_id)) {
            seg.epoch_base_ms = 0;
            seg.time_synced = false;
        }
        seg.mtime_s = mtimes[0];
        for (int k = 1; k < 4; k++) {
            if (mtimes[k] > seg.mtime_s) {
                seg.mtime_s = mtimes[k];
            }
        }
        if (global_latest && seg.last_seq > *global_latest) {
            *global_latest = seg.last_seq;
        }
        if (n < max) {
            out[n++] = seg;
        }
    }
    closedir(d);
    /* insertion sort by mtime, newest last; tie by id */
    for (int i = 1; i < n; i++) {
        sd_scan_seg_t key = out[i];
        int j = i - 1;
        while (j >= 0 &&
               (out[j].mtime_s > key.mtime_s ||
                (out[j].mtime_s == key.mtime_s && strcmp(out[j].id, key.id) > 0))) {
            out[j + 1] = out[j];
            j--;
        }
        out[j + 1] = key;
    }
    return n;
}

static void sd_repair_dir_locked(const char *id, int dir)
{
    char paths[4][192];
    sd_paths_for(id, paths);
    const char *raw_path = paths[dir * 2];
    const char *idx_path = paths[1 + dir * 2];
    uint64_t raw_size = 0;
    int64_t mt = 0;
    if (!sd_stat_size_mtime(raw_path, &raw_size, &mt)) return;

    FILE *idx = fopen(idx_path, "r+b");
    if (!idx) return;
    if (fseek(idx, 0, SEEK_END) != 0) {
        fclose(idx);
        return;
    }
    long sz = ftell(idx);
    uint32_t count = sz > 0 ? (uint32_t)(sz / (long)sizeof(sd_idx_rec_t)) : 0;
    uint32_t valid = 0;
    uint64_t last_end = 0;
    uint32_t last_seq = 0;
    if (count > 0 && fseek(idx, 0, SEEK_SET) == 0) {
        sd_idx_rec_t rec;
        for (uint32_t i = 0; i < count; i++) {
            if (fread(&rec, sizeof(rec), 1, idx) != 1) break;
            if (rec.offset + rec.len > raw_size) break;
            valid++;
            last_end = rec.offset + rec.len;
            last_seq = rec.seq;
        }
    }
    bool repaired = false;
    if (valid < count) {
        if (truncate(idx_path, (off_t)valid * (off_t)sizeof(sd_idx_rec_t)) == 0) {
            count = valid;
            repaired = true;
            bridge_event_push(BRIDGE_EVENT_WARN, "sd.repair",
                              "seg=%.39s dir=%s truncated idx %lu -> %lu",
                              id, dir ? "tx" : "rx", (unsigned long)(sz / (long)sizeof(sd_idx_rec_t)),
                              (unsigned long)valid);
        }
    }
    if (raw_size > last_end) {
        uint32_t seq = (s_last_seq > last_seq ? s_last_seq : last_seq) + 1;
        if (seq == 0) seq = 1;
        uint64_t off = last_end;
        while (off < raw_size) {
            uint32_t len = (uint32_t)(raw_size - off);
            if (len > 0xffff) len = 0xffff;
            sd_idx_rec_t rec = {
                .offset = off,
                .seq = seq,
                .tick_ms = (uint32_t)bridge_now_ms(),
                .len = (uint16_t)len,
                .dir = (uint8_t)dir,
                .reserved = 0,
            };
            if (fseek(idx, 0, SEEK_END) != 0 ||
                fwrite(&rec, sizeof(rec), 1, idx) != 1) {
                bridge_event_push(BRIDGE_EVENT_ERROR, "sd.repair",
                                  "seg=%.39s dir=%s failed to append recovery index",
                                  id, dir ? "tx" : "rx");
                break;
            }
            count++;
            if (s_first_seq == 0 || seq < s_first_seq) s_first_seq = seq;
            if (seq > s_last_seq) s_last_seq = seq;
            off += len;
            seq++;
            repaired = true;
        }
        fflush(idx);
        fsync(fileno(idx));
        bridge_event_push(BRIDGE_EVENT_WARN, "sd.repair",
                          "seg=%.39s dir=%s indexed %llu orphan raw bytes",
                          id, dir ? "tx" : "rx",
                          (unsigned long long)(raw_size - last_end));
    }
    fclose(idx);
    if (repaired) {
        s_idx_count[dir] = count;
    }
}

static bool sd_open_existing_locked(const char *id)
{
    char paths[4][192];
    sd_paths_for(id, paths);
    uint64_t raw_sizes[2] = {0};
    int64_t mt = 0;
    if (!sd_stat_size_mtime(paths[0], &raw_sizes[0], &mt) ||
        !sd_stat_size_mtime(paths[2], &raw_sizes[1], &mt)) {
        return false;
    }
    FILE *raw[2] = {fopen(paths[0], "ab"), fopen(paths[2], "ab")};
    FILE *idx[2] = {fopen(paths[1], "ab"), fopen(paths[3], "ab")};
    if (!raw[0] || !raw[1] || !idx[0] || !idx[1]) {
        for (int i = 0; i < 2; i++) {
            if (raw[i]) fclose(raw[i]);
            if (idx[i]) fclose(idx[i]);
        }
        return false;
    }
    snprintf(s_id, sizeof(s_id), "%s", id);
    for (int i = 0; i < 2; i++) {
        s_raw[i] = raw[i];
        s_idx[i] = idx[i];
        s_raw_bytes[i] = 0;
        s_idx_count[i] = 0;
        setvbuf(s_raw[i], NULL, _IOFBF, 8192);
        setvbuf(s_idx[i], NULL, _IOFBF, 4096);
    }
    s_first_seq = 0;
    s_last_seq = 0;
    for (int i = 0; i < 2; i++) {
        uint64_t sz = 0;
        int64_t m = 0;
        sd_stat_size_mtime(paths[i * 2], &sz, &m);
        s_raw_bytes[i] = sz;
        uint32_t cnt = 0, first = 0, last = 0;
        sd_index_bounds_path(paths[1 + i * 2], &cnt, &first, &last);
        s_idx_count[i] = cnt;
        if (first && (!s_first_seq || first < s_first_seq)) s_first_seq = first;
        if (last > s_last_seq) s_last_seq = last;
    }
    s_sync_error = false;
    s_seg_boot_id = 0;
    s_time_base = 0;
    s_time_synced = false;
    sd_read_time_path(id, &s_time_base, &s_time_synced, &s_seg_boot_id);
    sd_repair_dir_locked(id, 0);
    sd_repair_dir_locked(id, 1);
    ESP_LOGI(TAG, "resume segment %s raw=%llu/%llu idx=%lu/%lu seq=%lu..%lu",
             s_id, (unsigned long long)s_raw_bytes[0], (unsigned long long)s_raw_bytes[1],
             (unsigned long)s_idx_count[0], (unsigned long)s_idx_count[1],
             (unsigned long)s_first_seq, (unsigned long)s_last_seq);
    return true;
}

static esp_err_t sd_create_segment_locked(void)
{
    snprintf(s_id, sizeof(s_id), "%08lx-%04x", (unsigned long)time(NULL),
             (unsigned)(esp_random() & 0xFFFF));
    sd_paths_for(s_id, s_path);
    for (int i = 0; i < 2; i++) {
        s_raw[i] = fopen(s_path[i * 2], "wb");
        s_idx[i] = fopen(s_path[1 + i * 2], "wb");
        if (!s_raw[i] || !s_idx[i]) {
            ESP_LOGE(TAG, "open segment failed");
            sd_close_segment_locked();
            return ESP_FAIL;
        }
        s_raw_bytes[i] = 0;
        s_idx_count[i] = 0;
        setvbuf(s_raw[i], NULL, _IOFBF, 8192);
        setvbuf(s_idx[i], NULL, _IOFBF, 4096);
    }
    s_first_seq = 0;
    s_last_seq = 0;
    s_sync_error = false;
    s_seg_boot_id = s_boot_id;
    s_time_base = 0;
    s_time_synced = false;
    if (bridge_time_synced()) {
        s_time_base = bridge_time_base_ms();
        s_time_synced = s_time_base > 0;
    }
    sd_write_time_path(s_id, s_seg_boot_id, s_time_base, s_time_synced);
    ESP_LOGI(TAG, "new segment %s", s_id);
    bridge_event_push(BRIDGE_EVENT_INFO, "sd.segment", "new segment %s", s_id);
    return ESP_OK;
}

static esp_err_t sd_resume_or_create_locked(bool force_new)
{
    sd_scan_seg_t segs[BRIDGE_SD_MAX_SEGMENTS];
    uint32_t global_latest = 0;
    int n = sd_scan_segments_locked(segs, BRIDGE_SD_MAX_SEGMENTS, &global_latest);
    if (force_new) {
        esp_err_t err = sd_create_segment_locked();
        if (err == ESP_OK) {
            s_resume_seq = global_latest;
            bridge_event_push(BRIDGE_EVENT_INFO, "sd.new_boot",
                              "new boot segment %s (previous latest seq=%lu)", s_id,
                              (unsigned long)global_latest);
        }
        return err;
    }
    if (n > 0) {
        int best = 0;
        for (int i = 1; i < n; i++) {
            if (segs[i].last_seq > segs[best].last_seq ||
                (segs[i].last_seq == segs[best].last_seq &&
                 segs[i].mtime_s > segs[best].mtime_s)) {
                best = i;
            }
        }
        const char *id = segs[best].id;
        if (sd_open_existing_locked(id)) {
            s_resume_seq = global_latest > s_last_seq ? global_latest : s_last_seq;
            bridge_event_push(BRIDGE_EVENT_INFO, "sd.resume",
                              "resumed seg=%s seq=%lu..%lu", s_id,
                              (unsigned long)s_first_seq, (unsigned long)s_last_seq);
            return ESP_OK;
        }
        ESP_LOGW(TAG, "resume %s failed; creating new segment", id);
    }
    esp_err_t err = sd_create_segment_locked();
    if (err == ESP_OK && global_latest) {
        s_resume_seq = global_latest;
    }
    return err;
}

static esp_err_t sd_rotate_locked(void)
{
    sd_close_segment_locked();
    esp_err_t err = sd_create_segment_locked();
    if (err != ESP_OK) {
        s_broken = true;
        bridge_event_push(BRIDGE_EVENT_ERROR, "sd.rotate", "rotate failed");
    }
    return err;
}

static esp_err_t sd_try_mount(int attempts)
{
#if !CONFIG_BRIDGE_SD_ENABLE
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (!s_enabled) {
        return ESP_ERR_INVALID_STATE;
    }
    sd_lock();
    if (s_mounted) {
        sd_unlock();
        return ESP_OK;
    }
    sd_unlock();

    if (!s_bus_ready) {
        spi_bus_config_t buscfg = {
            .sclk_io_num = 12,
            .mosi_io_num = 11,
            .miso_io_num = 13,
            .quadwp_io_num = -1,
            .quadhd_io_num = -1,
            .max_transfer_sz = 160 * 80 * 2,
        };
        esp_err_t bus_err = spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO);
        s_bus_ready = (bus_err == ESP_OK || bus_err == ESP_ERR_INVALID_STATE);
        if (!s_bus_ready) {
            ESP_LOGW(TAG, "SPI2 init: %s", esp_err_to_name(bus_err));
        }
    }

    esp_vfs_fat_sdmmc_mount_config_t mnt = {
        .format_if_mount_failed = CONFIG_BRIDGE_SD_FORMAT_IF_FAILED,
        .max_files = 10,
        .allocation_unit_size = 16 * 1024,
    };
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    sdspi_device_config_t slot = {
        .host_id = host.slot,
        .gpio_cs = (gpio_num_t)CONFIG_BRIDGE_SD_CS_GPIO,
        .gpio_cd = GPIO_NUM_NC,
        .gpio_wp = GPIO_NUM_NC,
        .gpio_int = GPIO_NUM_NC,
    };

    esp_err_t err = ESP_FAIL;
    for (int attempt = 0; attempt < attempts; attempt++) {
        if (attempt) {
            vTaskDelay(pdMS_TO_TICKS(500));
        }
        err = esp_vfs_fat_sdspi_mount(CONFIG_BRIDGE_SD_MOUNT_POINT, &host, &slot, &mnt, &s_card);
        if (err == ESP_OK) {
            break;
        }
        ESP_LOGW(TAG, "SD mount attempt %d/%d failed: %s", attempt + 1, attempts,
                 esp_err_to_name(err));
    }
    if (err != ESP_OK) {
        s_card = NULL;
        s_mount_fail_count++;
        if (s_mount_fail_count == 1 || s_mount_fail_count % 10 == 0) {
            bridge_event_push(BRIDGE_EVENT_WARN, "sd.mount",
                              "mount failed (%lu): %s", (unsigned long)s_mount_fail_count,
                              esp_err_to_name(err));
        }
        return err;
    }

    if (mkdir(SD_DIR, 0777) != 0) {
        /* EEXIST is fine */
    }
    for (int i = 0; i < s_port_count; i++) {
        char pdir[96];
        snprintf(pdir, sizeof(pdir), "%s/p%d", SD_DIR, i);
        if (mkdir(pdir, 0777) != 0) {
            /* EEXIST is fine */
        }
    }

    sd_lock();
    if (s_mounted) {
        sd_unlock();
        return ESP_OK;
    }
    for (int i = 0; i < s_port_count; i++) {
        s_cur_port = i;
        sd_retention_locked();
        err = sd_resume_or_create_locked(s_initial_mount && CONFIG_BRIDGE_NTP_ENABLE);
        if (err == ESP_OK) {
            s_initial_mount = false;
        } else {
            break;
        }
    }
    if (err == ESP_OK) {
        s_mounted = true;
        s_broken = false;
        s_mount_fail_count = 0;
    }
    sd_unlock();

    for (int i = 0; i < s_port_count; i++) {
        if (s_logs[i].resume_seq && s_logs[i].port) {
            bridge_port_log_set_seq(s_logs[i].port, s_logs[i].resume_seq);
            s_logs[i].resume_seq = 0;
        }
    }

    if (err != ESP_OK) {
        esp_vfs_fat_sdcard_unmount(CONFIG_BRIDGE_SD_MOUNT_POINT, s_card);
        s_card = NULL;
        bridge_event_push(BRIDGE_EVENT_ERROR, "sd.mount",
                          "open segment failed: %s", esp_err_to_name(err));
        return err;
    }

    uint64_t total = 0, freeb = 0;
    esp_vfs_fat_info(CONFIG_BRIDGE_SD_MOUNT_POINT, &total, &freeb);
    for (int i = 0; i < s_port_count; i++) {
        s_cur_port = i;
        ESP_LOGI(TAG, "mounted %s seg=%s", s_logs[i].port ? "port" : "?", s_id);
        bridge_event_push(BRIDGE_EVENT_INFO, "sd.mount",
                          "mounted p%d seg=%s free=%.1f/%.1f MiB", i, s_id,
                          (double)freeb / 1048576.0, (double)total / 1048576.0);
    }
    return ESP_OK;
#endif
}

static void sd_remount(void)
{
    sd_lock();
    if (!s_mounted && !s_card) {
        sd_unlock();
        return;
    }
    for (int i = 0; i < s_port_count; i++) {
        s_cur_port = i;
        sd_close_segment_locked();
    }
    sdmmc_card_t *card = s_card;
    s_card = NULL;
    s_mounted = false;
    s_broken = false;
    uint32_t errors = s_write_errors;
    sd_unlock();

    if (card) {
        esp_err_t err = esp_vfs_fat_sdcard_unmount(CONFIG_BRIDGE_SD_MOUNT_POINT, card);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "unmount: %s", esp_err_to_name(err));
        }
    }
    bridge_event_push(BRIDGE_EVENT_WARN, "sd.remount",
                      "unmounted after %lu write errors; retrying",
                      (unsigned long)errors);
    if (s_retry_timer) {
        esp_timer_start_once(s_retry_timer, 1000000ULL);
    }
}

static void sd_retry_cb(void *arg)
{
    (void)arg;
    if (!s_enabled || s_mounted) {
        return;
    }
    if (sd_try_mount(1) != ESP_OK && s_retry_timer) {
        esp_timer_start_once(s_retry_timer, 10000000ULL);
    }
}

static void sd_sync_timer(void *arg)
{
    (void)arg;
    if (!s_enabled || !s_mounted) {
        return;
    }
    if (s_broken) {
        sd_remount();
        return;
    }
    sd_lock();
    for (int pidx = 0; pidx < s_port_count; pidx++) {
        s_cur_port = pidx;
        for (int i = 0; i < 2; i++) {
            sd_fsync(s_raw[i]);
            sd_fsync(s_idx[i]);
        }
        if (s_port) {
            bridge_port_log_flush_partial(s_port);
        }
        if (!s_time_base && bridge_time_synced() && sd_q_idle()) {
            /* A segment that started before NTP has an unknown tick->epoch base.
             * Seal it and start a fresh, time-anchored segment. */
            if (sd_rotate_locked() == ESP_OK) {
                bridge_event_push(BRIDGE_EVENT_INFO, "time.rotate",
                                  "NTP synced; new time-anchored segment %s", s_id);
            }
        }
        if ((s_sync_ticks + 1) % RETENTION_EVERY == 0) {
            sd_retention_locked();
        }
    }
    s_sync_ticks++;
    sd_unlock();
}

esp_err_t bridge_sd_init(bool spi_bus_ready, bridge_port_t **ports, int port_count)
{
#if !CONFIG_BRIDGE_SD_ENABLE
    (void)ports;
    (void)port_count;
    ESP_LOGI(TAG, "disabled by Kconfig");
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (!ports || port_count <= 0) return ESP_ERR_INVALID_ARG;
    s_port_count = port_count > BRIDGE_MAX_PORTS ? BRIDGE_MAX_PORTS : port_count;
    for (int i = 0; i < BRIDGE_MAX_PORTS; i++) {
        s_logs[i].port = i < s_port_count ? ports[i] : NULL;
        s_logs[i].initial_mount = true;
        s_logs[i].generation = 1;
        s_logs[i].time_base = 0;
        s_logs[i].time_synced = false;
        s_logs[i].seg_boot_id = 0;
        s_logs[i].resume_seq = 0;
    }
    if (s_enabled) {
        return s_mounted ? ESP_OK : ESP_FAIL;
    }
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) {
        return ESP_ERR_NO_MEM;
    }
    s_enabled = true;
    s_bus_ready = spi_bus_ready;
    s_boot_id = esp_random();
    s_time_base = 0;
    s_time_synced = false;
    s_qcap = CONFIG_BRIDGE_SD_QUEUE_BYTES;
    if (s_qcap < 65536) s_qcap = 65536;
    s_qbuf = (uint8_t *)heap_caps_malloc(s_qcap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_qbuf) {
        s_qbuf = (uint8_t *)heap_caps_malloc(s_qcap, MALLOC_CAP_8BIT);
    }
    if (!s_qbuf) {
        ESP_LOGW(TAG, "SD queue alloc failed (%u bytes); SD logging via queue disabled",
                 (unsigned)s_qcap);
        s_qcap = 0;
    } else {
        s_qlock = xSemaphoreCreateMutex();
        s_qsig = xSemaphoreCreateBinary();
        if (!s_qlock || !s_qsig ||
            xTaskCreate(sd_writer_task, "sd_writer", 8192, NULL, 5, &s_qtask) != pdPASS) {
            ESP_LOGW(TAG, "SD writer task init failed; SD logging via queue disabled");
            s_qcap = 0;
        } else {
            ESP_LOGI(TAG, "SD queue %u KiB, writer task started", (unsigned)(s_qcap / 1024));
        }
    }

    const esp_timer_create_args_t sync_args = {
        .callback = sd_sync_timer,
        .name = "sdlog_sync",
    };
    if (esp_timer_create(&sync_args, &s_timer) == ESP_OK) {
        esp_timer_start_periodic(s_timer, (uint64_t)CONFIG_BRIDGE_SD_SYNC_MS * 1000ULL);
    } else {
        ESP_LOGW(TAG, "sync timer create failed");
    }

    const esp_timer_create_args_t retry_args = {
        .callback = sd_retry_cb,
        .name = "sdlog_retry",
    };
    if (esp_timer_create(&retry_args, &s_retry_timer) != ESP_OK) {
        ESP_LOGW(TAG, "retry timer create failed; no hot-plug retry");
        s_retry_timer = NULL;
    }

    esp_err_t err = sd_try_mount(5);
    if (err != ESP_OK) {
        bridge_event_push(BRIDGE_EVENT_WARN, "sd.mount",
                          "not mounted; RAM-only, retrying every 10s");
        if (s_retry_timer) {
            esp_timer_start_once(s_retry_timer, 10000000ULL);
        }
    }
    return err;
#endif
}

bool bridge_sd_ready(void)
{
    return s_mounted;
}

void bridge_sd_sync(void)
{
    sd_lock();
    if (!s_mounted || s_broken) {
        sd_unlock();
        return;
    }
    for (int i = 0; i < 2; i++) {
        sd_fsync(s_raw[i]);
        sd_fsync(s_idx[i]);
    }
    sd_unlock();
}

static void sd_note_write_error(const char *dir)
{
    s_write_errors++;
    s_broken = true;
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    if (s_last_write_event_ms == 0 || now - s_last_write_event_ms >= 5000) {
        s_last_write_event_ms = now;
        bridge_event_push(BRIDGE_EVENT_ERROR, "sd.write",
                          "%s fwrite failed; remount pending (errors=%lu)", dir,
                          (unsigned long)s_write_errors);
    }
}

static void sd_q_write(const uint8_t *src, size_t len)
{
    size_t first = s_qcap - s_qtail;
    if (first > len) first = len;
    memcpy(s_qbuf + s_qtail, src, first);
    if (len > first) {
        memcpy(s_qbuf, src + first, len - first);
    }
    s_qtail = (s_qtail + len) % s_qcap;
    s_qused += len;
}

static void sd_q_read(uint8_t *dst, size_t len)
{
    size_t first = s_qcap - s_qhead;
    if (first > len) first = len;
    memcpy(dst, s_qbuf + s_qhead, first);
    if (len > first) {
        memcpy(dst + first, s_qbuf, len - first);
    }
    s_qhead = (s_qhead + len) % s_qcap;
    s_qused -= len;
}

static void sd_q_reset_locked(void)
{
    s_qhead = 0;
    s_qtail = 0;
    s_qused = 0;
}

static void sd_q_reset(void)
{
    if (!s_qlock) return;
    xSemaphoreTake(s_qlock, portMAX_DELAY);
    sd_q_reset_locked();
    xSemaphoreGive(s_qlock);
}

static bool sd_q_idle(void)
{
    if (!s_qlock) return true;
    xSemaphoreTake(s_qlock, portMAX_DELAY);
    bool idle = s_qused == 0;
    xSemaphoreGive(s_qlock);
    return idle;
}

static bool sd_q_dequeue(sd_q_hdr_t *h, uint8_t *data)
{
    if (!s_qlock || !s_qbuf || !h || !data) {
        return false;
    }
    xSemaphoreTake(s_qlock, portMAX_DELAY);
    if (s_qused < SD_Q_HDR) {
        xSemaphoreGive(s_qlock);
        return false;
    }
    sd_q_read((uint8_t *)h, SD_Q_HDR);
    if (h->len > CONFIG_BRIDGE_MAX_FRAME || h->raw_len < h->len ||
        h->raw_len > (uint16_t)(h->len + 1)) {
        sd_q_reset_locked();
        xSemaphoreGive(s_qlock);
        bridge_event_push(BRIDGE_EVENT_ERROR, "sd.queue_bad", "corrupt queue record");
        return false;
    }
    if (s_qused < h->len) {
        sd_q_reset_locked();
        xSemaphoreGive(s_qlock);
        bridge_event_push(BRIDGE_EVENT_ERROR, "sd.queue_bad", "truncated queue record");
        return false;
    }
    sd_q_read(data, h->len);
    xSemaphoreGive(s_qlock);
    return true;
}

static void sd_q_note_drop(size_t len)
{
    s_qdrop++;
    s_qdrop_bytes += len;
    uint32_t now = (uint32_t)bridge_now_ms();
    if (s_qdrop_event_ms == 0 || now - s_qdrop_event_ms >= 10000) {
        s_qdrop_event_ms = now;
        bridge_event_push(BRIDGE_EVENT_WARN, "sd.queue_drop",
                          "SD queue full; dropped %llu bytes (total %llu, events %lu)",
                          (unsigned long long)len, (unsigned long long)s_qdrop_bytes,
                          (unsigned long)s_qdrop);
    }
}

bool bridge_sd_submit(uint8_t port, uint32_t seq, int64_t tick_ms, uint8_t dir,
                      const uint8_t *data, uint16_t len, uint16_t raw_len)
{
    if (!s_qbuf || !s_qlock || !s_qcap || !data || port >= s_port_count || dir > 1 ||
        len > CONFIG_BRIDGE_MAX_FRAME || raw_len < len || raw_len > (uint16_t)(len + 1) ||
        (len == 0 && raw_len == 0)) {
        return false;
    }
    const size_t total = SD_Q_HDR + len;
    xSemaphoreTake(s_qlock, portMAX_DELAY);
    if (s_qused + total > s_qcap) {
        xSemaphoreGive(s_qlock);
        sd_q_note_drop(len);
        return false;
    }
    sd_q_hdr_t h = {
        .seq = seq,
        .tick_ms = (uint32_t)tick_ms,
        .len = len,
        .raw_len = raw_len,
        .dir = dir,
        .port = port,
        .flags = 0,
        .generation = s_logs[port].generation,
    };
    sd_q_write((const uint8_t *)&h, sizeof(h));
    sd_q_write(data, len);
    xSemaphoreGive(s_qlock);
    if (s_qsig) {
        xSemaphoreGive(s_qsig);
    }
    return true;
}

static void sd_writer_task(void *arg)
{
    (void)arg;
    uint8_t *buf = (uint8_t *)sd_alloc(CONFIG_BRIDGE_MAX_FRAME);
    if (!buf) {
        ESP_LOGE(TAG, "writer buffer alloc failed");
        vTaskDelete(NULL);
        return;
    }
    for (;;) {
        if (!s_enabled) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        sd_lock();
        if (!s_mounted || s_broken) {
            sd_unlock();
            if (s_qsig) xSemaphoreTake(s_qsig, pdMS_TO_TICKS(1000));
            else vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        sd_q_hdr_t h;
        if (!sd_q_dequeue(&h, buf)) {
            sd_unlock();
            if (s_qsig) xSemaphoreTake(s_qsig, pdMS_TO_TICKS(1000));
            else vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        if (h.port >= s_port_count || h.generation != s_logs[h.port].generation) {
            sd_unlock();
            continue; /* cleared port: discard stale queue records */
        }
        s_cur_port = h.port;
        const int dir = h.dir;
        uint64_t off = s_raw_bytes[dir];
        bool ok = fwrite(buf, 1, h.len, s_raw[dir]) == h.len;
        if (ok && h.raw_len == (uint16_t)(h.len + 1)) {
            ok = fputc('\n', s_raw[dir]) == '\n';
        }
        if (!ok) {
            sd_note_write_error(dir ? "tx" : "rx");
            sd_unlock();
            continue;
        }
        s_raw_bytes[dir] += h.raw_len;
        sd_idx_rec_t rec = {
            .offset = off,
            .seq = h.seq,
            .tick_ms = h.tick_ms,
            .len = h.raw_len,
            .dir = (uint8_t)dir,
            .reserved = 0,
        };
        if (fwrite(&rec, 1, sizeof(rec), s_idx[dir]) != sizeof(rec)) {
            sd_note_write_error(dir ? "tx" : "rx");
            sd_unlock();
            continue;
        }
        s_idx_count[dir]++;
        if (s_first_seq == 0 || h.seq < s_first_seq) s_first_seq = h.seq;
        if (h.seq > s_last_seq) s_last_seq = h.seq;
        const uint64_t seg_max = (uint64_t)CONFIG_BRIDGE_SD_SEGMENT_MAX_MB * 1024ULL * 1024ULL;
        if (seg_max > 0 && s_raw_bytes[0] + s_raw_bytes[1] >= seg_max) {
            sd_rotate_locked();
        }
        sd_unlock();
    }
}

void bridge_sd_get_status(int port, bridge_sd_status_t *out)
{
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->mounted = s_mounted;
    if (s_qlock) {
        xSemaphoreTake(s_qlock, portMAX_DELAY);
        out->queue_bytes = s_qcap;
        out->queue_used = s_qused;
        out->queue_dropped = s_qdrop;
        out->queue_dropped_bytes = s_qdrop_bytes;
        xSemaphoreGive(s_qlock);
    }
    if (!s_mounted) {
        return;
    }
    sd_lock();
    if (port < 0 || port >= s_port_count) {
        sd_unlock();
        return;
    }
    s_cur_port = port;
    uint64_t total = 0, freeb = 0;
    if (esp_vfs_fat_info(CONFIG_BRIDGE_SD_MOUNT_POINT, &total, &freeb) == ESP_OK) {
        out->capacity_bytes = total;
        out->free_bytes = freeb;
        out->used_bytes = total > freeb ? total - freeb : 0;
    }
    out->raw_bytes = s_raw_bytes[0] + s_raw_bytes[1];
    out->index_count = s_idx_count[0] + s_idx_count[1];
    out->write_errors = s_write_errors;
    out->broken = s_broken;
    out->earliest_seq = s_first_seq;
    out->latest_seq = s_last_seq;
    out->epoch_base_ms = s_time_base;
    out->time_synced = s_time_synced;
    snprintf(out->segment, sizeof(out->segment), "%s", s_id);

    sd_scan_seg_t segs[BRIDGE_SD_MAX_SEGMENTS];
    uint32_t global_latest = 0;
    int n = sd_scan_segments_locked(segs, BRIDGE_SD_MAX_SEGMENTS, &global_latest);
    out->segment_count = (size_t)n;
    for (int i = 0; i < n; i++) {
        bridge_sd_segment_t *d = &out->segments[i];
        snprintf(d->id, sizeof(d->id), "%s", segs[i].id);
        d->raw_bytes = segs[i].raw_bytes[0] + segs[i].raw_bytes[1];
        d->index_count = segs[i].index_count[0] + segs[i].index_count[1];
        d->first_seq = segs[i].first_seq;
        d->latest_seq = segs[i].last_seq;
        d->latest_rx = segs[i].latest_seq[0];
        d->latest_tx = segs[i].latest_seq[1];
        d->acked_rx = segs[i].acked_rx;
        d->acked_tx = segs[i].acked_tx;
        d->acked = segs[i].acked;
        d->epoch_base_ms = segs[i].epoch_base_ms;
        d->time_synced = segs[i].time_synced;
        d->mtime_s = segs[i].mtime_s;
        d->active = strcmp(segs[i].id, s_id) == 0;
        if (d->active) {
            if (s_last_seq > d->latest_seq) d->latest_seq = s_last_seq;
            if (s_idx_count[0] + s_idx_count[1] > d->index_count) {
                d->index_count = s_idx_count[0] + s_idx_count[1];
            }
            if (s_raw_bytes[0] + s_raw_bytes[1] > d->raw_bytes) {
                d->raw_bytes = s_raw_bytes[0] + s_raw_bytes[1];
            }
            d->epoch_base_ms = s_time_base;
            d->time_synced = s_time_synced;
            d->acked = d->acked_rx >= d->latest_rx && d->acked_tx >= d->latest_tx;
        }
    }
    out->global_latest_seq = global_latest > s_last_seq ? global_latest : s_last_seq;
    sd_unlock();
}

uint32_t bridge_sd_latest_seq(int port)
{
    if (!s_mounted || port < 0 || port >= s_port_count) {
        return 0;
    }
    sd_lock();
    s_cur_port = port;
    sd_scan_seg_t segs[BRIDGE_SD_MAX_SEGMENTS];
    uint32_t global_latest = 0;
    sd_scan_segments_locked(segs, BRIDGE_SD_MAX_SEGMENTS, &global_latest);
    if (s_last_seq > global_latest) {
        global_latest = s_last_seq;
    }
    sd_unlock();
    return global_latest;
}

static void sd_unlink_segment_locked(const char *id)
{
    char paths[4][192];
    sd_paths_for(id, paths);
    for (int i = 0; i < 4; i++) {
        unlink(paths[i]);
    }
    char ack[160];
    sd_ack_path(id, ack, sizeof(ack));
    unlink(ack);
    char time_path[160];
    sd_time_path(id, time_path, sizeof(time_path));
    unlink(time_path);
}

static void sd_unlink_all_locked(void)
{
    char pdir[96];
    sd_port_dir(pdir, sizeof(pdir));
    DIR *d = opendir(pdir);
    if (!d) {
        return;
    }
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        const char *name = de->d_name;
        if (!(strncmp(name, "raw-rx-", 7) == 0 || strncmp(name, "raw-tx-", 7) == 0 ||
              strncmp(name, "idx-rx-", 7) == 0 || strncmp(name, "idx-tx-", 7) == 0 ||
              strncmp(name, "ack-", 4) == 0 || strncmp(name, "time-", 5) == 0)) {
            continue;
        }
        char full[320];
        snprintf(full, sizeof(full), "%s/%.200s", pdir, name);
        unlink(full);
    }
    closedir(d);
}

esp_err_t bridge_sd_ack(int port, const char *segment, uint8_t dir, uint32_t seq)
{
    if (!segment || !segment[0] || dir > 1 || !sd_seg_id_ok(segment) ||
        port < 0 || port >= s_port_count) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_mounted) {
        return ESP_ERR_INVALID_STATE;
    }
    sd_lock();
    s_cur_port = port;
    uint32_t rx = 0, tx = 0;
    sd_read_ack_path(segment, &rx, &tx);
    if (dir == 0) {
        if (seq > rx) rx = seq;
    } else {
        if (seq > tx) tx = seq;
    }
    bool ok = sd_write_ack_path(segment, rx, tx);
    sd_unlock();
    return ok ? ESP_OK : ESP_FAIL;
}

esp_err_t bridge_sd_clear(int port, const char *segment, bool *active_cleared)
{
    if (active_cleared) {
        *active_cleared = false;
    }
    if (!s_mounted || port < 0 || port >= s_port_count) {
        return ESP_ERR_INVALID_STATE;
    }
    if (segment && segment[0] && !sd_seg_id_ok(segment)) {
        return ESP_ERR_INVALID_ARG;
    }
    sd_lock();
    s_cur_port = port;
    s_generation++;
    esp_err_t err = ESP_OK;
    if (segment && segment[0]) {
        if (strcmp(segment, s_id) == 0) {
            sd_close_segment_locked();
            sd_unlink_segment_locked(segment);
            sd_q_reset();
            err = sd_create_segment_locked();
            if (active_cleared) {
                *active_cleared = true;
            }
        } else {
            sd_unlink_segment_locked(segment);
        }
    } else {
        sd_close_segment_locked();
        sd_unlink_all_locked();
        sd_q_reset();
        err = sd_create_segment_locked();
        if (active_cleared) {
            *active_cleared = true;
        }
    }
    s_sync_ticks = 0;
    if (err != ESP_OK) {
        s_broken = true;
    }
    sd_unlock();
    return err;
}

static uint32_t sd_idx_record_count(FILE *f)
{
    if (fseek(f, 0, SEEK_END) != 0) {
        return 0;
    }
    long sz = ftell(f);
    if (sz < 0) {
        return 0;
    }
    return (uint32_t)(sz / (long)sizeof(sd_idx_rec_t));
}

static bool sd_idx_read(FILE *f, uint32_t index, sd_idx_rec_t *rec)
{
    if (fseek(f, (long)index * (long)sizeof(*rec), SEEK_SET) != 0) {
        return false;
    }
    return fread(rec, 1, sizeof(*rec), f) == sizeof(*rec);
}

static uint32_t sd_idx_lower_bound(FILE *f, uint32_t n, uint32_t seq)
{
    uint32_t lo = 0, hi = n;
    sd_idx_rec_t rec;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (sd_idx_read(f, mid, &rec) && rec.seq < seq) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return lo;
}

static void sd_time_for_segment_locked(const char *id, int64_t *base, bool *synced)
{
    if (strcmp(id, s_id) == 0) {
        *base = s_time_base;
        *synced = s_time_synced;
        return;
    }
    uint32_t boot = 0;
    if (!sd_read_time_path(id, base, synced, &boot)) {
        *base = 0;
        *synced = false;
    }
}

esp_err_t bridge_sd_export(int port, const bridge_sd_export_req_t *req, bridge_sd_export_t *out)
{
    if (!req || !out || port < 0 || port >= s_port_count) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
    if (!s_mounted) {
        return ESP_ERR_INVALID_STATE;
    }
    if (req->dir > 1) {
        return ESP_ERR_INVALID_ARG;
    }
    const char *seg = (req->segment && req->segment[0]) ? req->segment : s_id;
    if (!sd_seg_id_ok(seg)) {
        return ESP_ERR_INVALID_ARG;
    }
    char paths[4][192];
    sd_paths_for(seg, paths);

    uint32_t max_bytes = req->max_bytes ? req->max_bytes : EXPORT_CHUNK_MAX;
    if (max_bytes > EXPORT_CHUNK_MAX) {
        max_bytes = EXPORT_CHUNK_MAX;
    }
    if (max_bytes < 256) {
        max_bytes = 256;
    }

    const int dir = req->dir;
    sd_lock();
    s_cur_port = port;
    if (strcmp(seg, s_id) == 0) {
        sd_fsync(s_raw[dir]);
        sd_fsync(s_idx[dir]);
    }
    uint64_t raw_size = 0;
    int64_t mt = 0;
    if (!sd_stat_size_mtime(paths[dir * 2], &raw_size, &mt)) {
        sd_unlock();
        return ESP_ERR_NOT_FOUND;
    }

    FILE *idx = fopen(paths[1 + dir * 2], "rb");
    FILE *raw = fopen(paths[dir * 2], "rb");
    if (!idx || !raw) {
        if (idx) fclose(idx);
        if (raw) fclose(raw);
        sd_unlock();
        return ESP_FAIL;
    }

    sd_idx_rec_t *recs = (sd_idx_rec_t *)sd_alloc(sizeof(sd_idx_rec_t) * EXPORT_LINES_MAX);
    if (!recs) {
        fclose(idx);
        fclose(raw);
        sd_unlock();
        return ESP_ERR_NO_MEM;
    }

    uint64_t start_off = 0, end_off = 0;
    uint32_t count = 0;
    bool more = false;
    uint32_t next_index = 0;

    const uint32_t nrec = sd_idx_record_count(idx);
    uint32_t start_index = 0;
    if (req->cursor > 0) {
        start_index = req->cursor > nrec ? nrec : (uint32_t)req->cursor;
    } else if (req->has_seq_from) {
        start_index = sd_idx_lower_bound(idx, nrec, req->seq_from);
    }
    if (start_index > nrec) {
        start_index = nrec;
    }
    if (fseek(idx, (long)start_index * (long)sizeof(sd_idx_rec_t), SEEK_SET) != 0) {
        fclose(idx);
        fclose(raw);
        free(recs);
        sd_unlock();
        return ESP_FAIL;
    }

    enum { IDX_BLOCK = 64 };
    sd_idx_rec_t block[IDX_BLOCK];
    uint32_t rec_index = start_index;
    bool stop = false;
    while (!stop) {
        size_t got = fread(block, sizeof(sd_idx_rec_t), IDX_BLOCK, idx);
        if (got == 0) {
            break;
        }
        for (size_t k = 0; k < got; k++) {
            sd_idx_rec_t rec = block[k];
            uint32_t this_index = rec_index;
            rec_index++;
            if (rec.dir != dir) {
                continue;
            }
            if (req->has_seq_to && rec.seq > req->seq_to) {
                stop = true; /* per-dir index is seq-ordered */
                break;
            }
            if (req->has_seq_from && rec.seq < req->seq_from) {
                continue;
            }
            if (rec.len == 0) {
                continue;
            }
            uint64_t end = rec.offset + rec.len;
            if (end > raw_size) {
                continue; /* tail not flushed yet */
            }
            if (count == 0) {
                start_off = rec.offset;
            } else if (end - start_off > max_bytes || count >= EXPORT_LINES_MAX) {
                more = true;
                next_index = this_index;
                stop = true;
                break;
            }
            recs[count++] = rec;
            end_off = end;
        }
    }
    fclose(idx);

    if (count == 0) {
        fclose(raw);
        free(recs);
        out->eof = true;
        out->next_cursor = req->cursor;
        snprintf(out->segment, sizeof(out->segment), "%s", seg);
        sd_time_for_segment_locked(seg, &out->time_base_ms, &out->time_synced);
        sd_unlock();
        return ESP_OK;
    }

    const size_t chunk_len = (size_t)(end_off - start_off);
    uint8_t *chunk = (uint8_t *)sd_alloc(chunk_len ? chunk_len : 1);
    uint8_t *index = (uint8_t *)sd_alloc(sizeof(sd_idx_rec_t) * count);
    if (!chunk || !index) {
        free(chunk);
        free(index);
        free(recs);
        fclose(raw);
        sd_unlock();
        return ESP_ERR_NO_MEM;
    }

    if (fseek(raw, (long)start_off, SEEK_SET) != 0 ||
        fread(chunk, 1, chunk_len, raw) != chunk_len) {
        free(chunk);
        free(index);
        free(recs);
        fclose(raw);
        sd_unlock();
        return ESP_FAIL;
    }
    fclose(raw);

    sd_idx_rec_t *out_recs = (sd_idx_rec_t *)index;
    for (uint32_t i = 0; i < count; i++) {
        out_recs[i] = recs[i];
        out_recs[i].offset = recs[i].offset - start_off;
    }

    out->chunk = chunk;
    out->chunk_len = chunk_len;
    out->index = index;
    out->index_len = sizeof(sd_idx_rec_t) * count;
    out->next_cursor = more ? next_index : 0;
    out->eof = !more;
    snprintf(out->segment, sizeof(out->segment), "%s", seg);
    sd_time_for_segment_locked(seg, &out->time_base_ms, &out->time_synced);

    free(recs);
    sd_unlock();
    return ESP_OK;
}

void bridge_sd_export_free(bridge_sd_export_t *out)
{
    if (!out) {
        return;
    }
    free(out->chunk);
    free(out->index);
    memset(out, 0, sizeof(*out));
}