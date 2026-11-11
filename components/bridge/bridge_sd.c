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
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "sdkconfig.h"
#include "sdmmc_cmd.h"

#ifndef CONFIG_BRIDGE_SD_FORMAT_IF_FAILED
#define CONFIG_BRIDGE_SD_FORMAT_IF_FAILED 0
#endif

#define TAG "bridge_sd"

#define SD_DIR CONFIG_BRIDGE_SD_MOUNT_POINT "/shb"
#define EXPORT_CHUNK_MAX 32768
#define EXPORT_LINES_MAX 512
#define RETENTION_EVERY 60

static SemaphoreHandle_t s_lock;
static sdmmc_card_t *s_card;
static esp_timer_handle_t s_timer;
static bool s_mounted;
static bool s_sync_error;

static char s_id[32];
static char s_path[4][96]; /* raw_rx, idx_rx, raw_tx, idx_tx */
static FILE *s_raw[2];
static FILE *s_idx[2];
static uint64_t s_raw_bytes[2];
static uint32_t s_idx_count[2];
static uint32_t s_first_seq;
static uint32_t s_last_seq;
static uint32_t s_sync_ticks;

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
        ESP_LOGW(TAG, "fsync failed (data still buffered in FAT)");
        s_sync_error = true;
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

static void sd_retention_locked(void)
{
    typedef struct {
        char name[64];
        uint64_t size;
        time_t mtime;
        bool deleted;
    } ent_t;

    const int max_ents = 64;
    ent_t *ents = (ent_t *)calloc(max_ents, sizeof(ent_t));
    if (!ents) {
        return;
    }

    int n = 0;
    DIR *d = opendir(SD_DIR);
    if (!d) {
        free(ents);
        return;
    }
    struct dirent *de;
    while ((de = readdir(d)) != NULL && n < max_ents) {
        if (de->d_name[0] == '.' || strlen(de->d_name) >= sizeof(ents[n].name)) {
            continue;
        }
        char full[128];
        snprintf(full, sizeof(full), "%s/%.63s", SD_DIR, de->d_name);
        struct stat st;
        if (stat(full, &st) != 0) {
            continue;
        }
        snprintf(ents[n].name, sizeof(ents[n].name), "%.63s", de->d_name);
        ents[n].size = (uint64_t)st.st_size;
        ents[n].mtime = st.st_mtime;
        ents[n].deleted = false;
        n++;
    }
    closedir(d);

    uint64_t total = 0;
    for (int i = 0; i < n; i++) {
        total += ents[i].size;
    }
    const uint64_t retain = (uint64_t)CONFIG_BRIDGE_SD_RETAIN_MB * 1024ULL * 1024ULL;

    while (total > retain) {
        int oldest = -1;
        for (int i = 0; i < n; i++) {
            if (!ents[i].deleted && (oldest < 0 || ents[i].mtime < ents[oldest].mtime)) {
                oldest = i;
            }
        }
        if (oldest < 0) {
            break;
        }
        if (strstr(ents[oldest].name, s_id)) {
            ents[oldest].deleted = true;
            total -= ents[oldest].size;
            continue;
        }
        char full[128];
        snprintf(full, sizeof(full), "%s/%.63s", SD_DIR, ents[oldest].name);
        ESP_LOGI(TAG, "retention: remove %s (%llu KiB)", full,
                 (unsigned long long)(ents[oldest].size / 1024));
        unlink(full);
        total -= ents[oldest].size;
        ents[oldest].deleted = true;
    }
    free(ents);
}

static esp_err_t sd_open_segment_locked(void)
{
    snprintf(s_id, sizeof(s_id), "%08lx-%04x", (unsigned long)time(NULL),
             (unsigned)(esp_random() & 0xFFFF));

    snprintf(s_path[0], sizeof(s_path[0]), "%s/raw-rx-%s.bin", SD_DIR, s_id);
    snprintf(s_path[1], sizeof(s_path[1]), "%s/idx-rx-%s.bin", SD_DIR, s_id);
    snprintf(s_path[2], sizeof(s_path[2]), "%s/raw-tx-%s.bin", SD_DIR, s_id);
    snprintf(s_path[3], sizeof(s_path[3]), "%s/idx-tx-%s.bin", SD_DIR, s_id);

    s_raw[0] = fopen(s_path[0], "wb");
    s_idx[0] = fopen(s_path[1], "wb");
    s_raw[1] = fopen(s_path[2], "wb");
    s_idx[1] = fopen(s_path[3], "wb");
    if (!s_raw[0] || !s_idx[0] || !s_raw[1] || !s_idx[1]) {
        ESP_LOGE(TAG, "open segment failed");
        sd_close_segment_locked();
        return ESP_FAIL;
    }
    for (int i = 0; i < 2; i++) {
        setvbuf(s_raw[i], NULL, _IOFBF, 8192);
        setvbuf(s_idx[i], NULL, _IOFBF, 4096);
    }
    ESP_LOGI(TAG, "segment %s", s_id);
    return ESP_OK;
}

static void sd_sync_timer(void *arg)
{
    (void)arg;
    sd_lock();
    for (int i = 0; i < 2; i++) {
        sd_fsync(s_raw[i]);
        sd_fsync(s_idx[i]);
    }
    if (++s_sync_ticks % RETENTION_EVERY == 0) {
        sd_retention_locked();
    }
    sd_unlock();
}

esp_err_t bridge_sd_init(bool spi_bus_ready)
{
#if !CONFIG_BRIDGE_SD_ENABLE
    ESP_LOGI(TAG, "disabled by Kconfig");
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (s_mounted) {
        return ESP_OK;
    }
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) {
        return ESP_ERR_NO_MEM;
    }

    if (!spi_bus_ready) {
        /* The LCD normally initializes SPI2 first; if it is disabled or
         * failed, make sure the bus exists with the same pins. */
        spi_bus_config_t buscfg = {
            .sclk_io_num = 12,
            .mosi_io_num = 11,
            .miso_io_num = 13,
            .quadwp_io_num = -1,
            .quadhd_io_num = -1,
            .max_transfer_sz = 160 * 80 * 2,
        };
        esp_err_t bus_err = spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO);
        if (bus_err != ESP_OK && bus_err != ESP_ERR_INVALID_STATE) {
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
    for (int attempt = 0; attempt < 5; attempt++) {
        if (attempt) {
            vTaskDelay(pdMS_TO_TICKS(500));
        }
        err = esp_vfs_fat_sdspi_mount(CONFIG_BRIDGE_SD_MOUNT_POINT, &host, &slot, &mnt, &s_card);
        if (err == ESP_OK) {
            break;
        }
        ESP_LOGW(TAG, "SD mount attempt %d/5 failed: %s", attempt + 1, esp_err_to_name(err));
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SD mount failed: %s (RAM-only mode)", esp_err_to_name(err));
        return err;
    }

    if (mkdir(SD_DIR, 0777) != 0) {
        /* EEXIST is fine */
    }

    sd_lock();
    sd_retention_locked();
    err = sd_open_segment_locked();
    sd_unlock();
    if (err != ESP_OK) {
        esp_vfs_fat_sdcard_unmount(CONFIG_BRIDGE_SD_MOUNT_POINT, s_card);
        s_card = NULL;
        return err;
    }

    const esp_timer_create_args_t targs = {
        .callback = sd_sync_timer,
        .name = "sdlog_sync",
    };
    if (esp_timer_create(&targs, &s_timer) == ESP_OK) {
        esp_timer_start_periodic(s_timer, (uint64_t)CONFIG_BRIDGE_SD_SYNC_MS * 1000ULL);
    } else {
        ESP_LOGW(TAG, "sync timer create failed");
    }

    s_mounted = true;
    uint64_t total = 0, freeb = 0;
    esp_vfs_fat_info(CONFIG_BRIDGE_SD_MOUNT_POINT, &total, &freeb);
    ESP_LOGI(TAG, "mounted at %s, %.1f/%.1f MiB free, dir=%s",
             CONFIG_BRIDGE_SD_MOUNT_POINT, (double)freeb / 1048576.0,
             (double)total / 1048576.0, SD_DIR);
    return ESP_OK;
#endif
}

bool bridge_sd_ready(void)
{
    return s_mounted;
}

void bridge_sd_sync(void)
{
    if (!s_mounted) {
        return;
    }
    sd_lock();
    for (int i = 0; i < 2; i++) {
        sd_fsync(s_raw[i]);
        sd_fsync(s_idx[i]);
    }
    sd_unlock();
}

void bridge_sd_log_rx(const uint8_t *data, size_t len)
{
    if (!s_mounted || !data || len == 0) {
        return;
    }
    sd_lock();
    if (s_raw[0] && fwrite(data, 1, len, s_raw[0]) == len) {
        s_raw_bytes[0] += len;
    }
    sd_unlock();
}

void bridge_sd_log_tx(const uint8_t *data, size_t len)
{
    if (!s_mounted || !data || len == 0) {
        return;
    }
    sd_lock();
    if (s_raw[1] && fwrite(data, 1, len, s_raw[1]) == len) {
        s_raw_bytes[1] += len;
    }
    sd_unlock();
}

void bridge_sd_index_line(uint32_t seq, int64_t tick_ms, uint8_t dir,
                          uint64_t offset, uint32_t raw_len)
{
    if (!s_mounted || dir > 1 || raw_len == 0) {
        return;
    }
    sd_idx_rec_t rec = {
        .offset = offset,
        .seq = seq,
        .tick_ms = (uint32_t)tick_ms,
        .len = raw_len > 0xFFFF ? 0xFFFF : (uint16_t)raw_len,
        .dir = dir,
        .reserved = 0,
    };
    sd_lock();
    if (s_idx[dir] && fwrite(&rec, 1, sizeof(rec), s_idx[dir]) == sizeof(rec)) {
        s_idx_count[dir]++;
        if (s_first_seq == 0 || seq < s_first_seq) {
            s_first_seq = seq;
        }
        if (seq > s_last_seq) {
            s_last_seq = seq;
        }
    }
    sd_unlock();
}

void bridge_sd_get_status(bridge_sd_status_t *out)
{
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->mounted = s_mounted;
    if (!s_mounted) {
        return;
    }
    sd_lock();
    uint64_t total = 0, freeb = 0;
    if (esp_vfs_fat_info(CONFIG_BRIDGE_SD_MOUNT_POINT, &total, &freeb) == ESP_OK) {
        out->capacity_bytes = total;
        out->free_bytes = freeb;
        out->used_bytes = total > freeb ? total - freeb : 0;
    }
    out->raw_bytes = s_raw_bytes[0] + s_raw_bytes[1];
    out->index_count = s_idx_count[0] + s_idx_count[1];
    out->earliest_seq = s_first_seq;
    out->latest_seq = s_last_seq;
    snprintf(out->segment, sizeof(out->segment), "%s", s_id);
    sd_unlock();
}

esp_err_t bridge_sd_clear(void)
{
    if (!s_mounted) {
        return ESP_ERR_INVALID_STATE;
    }
    sd_lock();
    sd_close_segment_locked();
    for (int i = 0; i < 4; i++) {
        unlink(s_path[i]);
    }
    esp_err_t err = sd_open_segment_locked();
    s_sync_ticks = 0;
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

esp_err_t bridge_sd_export(const bridge_sd_export_req_t *req, bridge_sd_export_t *out)
{
    if (!req || !out) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
    if (!s_mounted) {
        return ESP_ERR_INVALID_STATE;
    }
    if (req->dir > 1) {
        return ESP_ERR_INVALID_ARG;
    }

    const int dir = req->dir;
    uint32_t max_bytes = req->max_bytes ? req->max_bytes : EXPORT_CHUNK_MAX;
    if (max_bytes > EXPORT_CHUNK_MAX) {
        max_bytes = EXPORT_CHUNK_MAX;
    }
    if (max_bytes < 256) {
        max_bytes = 256;
    }

    sd_lock();
    sd_fsync(s_raw[dir]);
    sd_fsync(s_idx[dir]);

    FILE *idx = fopen(s_path[1 + dir * 2], "rb");
    FILE *raw = fopen(s_path[dir * 2], "rb");
    if (!idx || !raw) {
        if (idx) fclose(idx);
        if (raw) fclose(raw);
        sd_unlock();
        return ESP_FAIL;
    }

    sd_idx_rec_t *recs = (sd_idx_rec_t *)malloc(sizeof(sd_idx_rec_t) * EXPORT_LINES_MAX);
    if (!recs) {
        fclose(idx);
        fclose(raw);
        sd_unlock();
        return ESP_ERR_NO_MEM;
    }

    const uint64_t raw_size = s_raw_bytes[dir];
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
        sd_unlock();
        return ESP_OK;
    }

    const size_t chunk_len = (size_t)(end_off - start_off);
    uint8_t *chunk = (uint8_t *)malloc(chunk_len ? chunk_len : 1);
    uint8_t *index = (uint8_t *)malloc(sizeof(sd_idx_rec_t) * count);
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