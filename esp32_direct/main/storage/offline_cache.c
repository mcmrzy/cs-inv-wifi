/**
 * offline_cache.c — SPIFFS-based offline message cache with write batching
 *
 * Design goals:
 *   1. Survive power loss (SPIFFS handles this)
 *   2. Minimize flash wear (batch N messages into one file write)
 *   3. 7-day retention with automatic cleanup
 *   4. Drop-in replacement for the old NVS ring buffer
 *
 * File naming: /b<seq>  (e.g. /b000001, /b000002, ...)
 * Each file contains a JSON array of cached messages.
 */

#include "offline_cache.h"
#include "nvs_storage.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <sys/time.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_spiffs.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static const char *TAG = "OFFLINE_CACHE";

/* ---- Configuration ---- */
#define SPIFFS_PARTITION  "cache"
#define SPIFFS_MOUNT      "/cache"
#define MAX_BATCH_ITEMS   10        /* flush after this many messages */
#define FLUSH_INTERVAL_MS 60000     /* or after this many ms */
#define MAX_CACHE_FILES   500       /* max batch files on disk */
#define TTL_DAYS          7

/* ---- NVS keys for metadata persistence ---- */
#define META_NAMESPACE    "cache_meta"
#define KEY_NEXT_SEQ      "next_seq"
#define KEY_TOTAL_MSGS    "total_msgs"
#define KEY_LAST_SYNC     "last_sync"

/* ---- State ---- */
static bool s_initialized = false;
static SemaphoreHandle_t s_mutex = NULL;

/* Write batch buffer */
static OfflineCacheItem *s_batch = NULL;
/* OTA 静默窗口: 暂停周期补传 (缓冲继续积压, 事后照常补) */
static volatile bool s_quiet = false;

void offline_cache_set_quiet(bool quiet)
{
    s_quiet = quiet;
}
static int s_batch_count = 0;
static int64_t s_batch_start_ms = 0;
static int64_t s_last_process_ms = 0;

/* Persistent metadata */
static uint32_t s_next_seq = 1;
static uint32_t s_total_msgs = 0;   /* total messages across all files */
static uint32_t s_last_sync = 0;

/* ---- Helpers ---- */

static int64_t now_ms(void) {
    return esp_timer_get_time() / 1000;
}

static uint32_t now_epoch(void) {
    time_t t = time(NULL);
    return (t > 1000000000) ? (uint32_t)t : 0;
}

static void load_meta(void) {
    nvs_handle_t h;
    if (nvs_open(META_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        int32_t v;
        uint32_t u;
        if (nvs_get_i32(h, KEY_NEXT_SEQ, &v) == ESP_OK && v > 0)
            s_next_seq = (uint32_t)v;
        if (nvs_get_i32(h, KEY_TOTAL_MSGS, &v) == ESP_OK && v >= 0)
            s_total_msgs = (uint32_t)v;
        if (nvs_get_u32(h, KEY_LAST_SYNC, &u) == ESP_OK)
            s_last_sync = u;
        nvs_close(h);
    }
}

static void save_meta(void) {
    nvs_handle_t h;
    if (nvs_open(META_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_i32(h, KEY_NEXT_SEQ, (int32_t)s_next_seq);
        nvs_set_i32(h, KEY_TOTAL_MSGS, (int32_t)s_total_msgs);
        nvs_set_u32(h, KEY_LAST_SYNC, s_last_sync);
        nvs_commit(h);
        nvs_close(h);
    }
}

static void make_filename(uint32_t seq, char *buf, size_t len) {
    snprintf(buf, len, SPIFFS_MOUNT "/b%08lu", (unsigned long)seq);
}

/* Count files in the cache directory */
static int count_files(void) {
    int count = 0;
    DIR *dir = opendir(SPIFFS_MOUNT);
    if (!dir) return 0;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (ent->d_name[0] == 'b') count++;
    }
    closedir(dir);
    return count;
}

/* Find the lowest sequence number among existing files */
static uint32_t find_min_seq(void) {
    uint32_t min_seq = UINT32_MAX;
    DIR *dir = opendir(SPIFFS_MOUNT);
    if (!dir) return min_seq;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (ent->d_name[0] == 'b') {
            uint32_t seq = (uint32_t)strtoul(ent->d_name + 1, NULL, 10);
            if (seq < min_seq) min_seq = seq;
        }
    }
    closedir(dir);
    return min_seq;
}

/* Count total messages across all batch files (expensive, use sparingly) */
static uint32_t count_all_messages(void) {
    uint32_t total = 0;
    DIR *dir = opendir(SPIFFS_MOUNT);
    if (!dir) return 0;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (ent->d_name[0] != 'b') continue;
        char path[280];
        snprintf(path, sizeof(path), SPIFFS_MOUNT "/%s", ent->d_name);
        FILE *f = fopen(path, "r");
        if (!f) continue;
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fclose(f);
        if (sz <= 0) continue;
        char *buf = malloc(sz + 1);
        if (!buf) continue;
        f = fopen(path, "r");
        if (!f) { free(buf); continue; }
        fread(buf, 1, sz, f);
        buf[sz] = '\0';
        fclose(f);
        cJSON *arr = cJSON_Parse(buf);
        free(buf);
        if (arr && cJSON_IsArray(arr)) {
            total += cJSON_GetArraySize(arr);
        }
        cJSON_Delete(arr);
    }
    closedir(dir);
    return total;
}

/* Delete files older than TTL_DAYS */
static void cleanup_expired(void) {
    uint32_t now = now_epoch();
    if (now == 0) return; /* no time yet */

    uint32_t cutoff = now - (TTL_DAYS * 86400);
    int removed = 0;

    DIR *dir = opendir(SPIFFS_MOUNT);
    if (!dir) return;

    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (ent->d_name[0] != 'b') continue;
        char path[280];
        snprintf(path, sizeof(path), SPIFFS_MOUNT "/%s", ent->d_name);

        /* Read first message's timestamp */
        FILE *f = fopen(path, "r");
        if (!f) continue;
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fclose(f);
        if (sz <= 0) { remove(path); removed++; continue; }

        char *buf = malloc(sz + 1);
        if (!buf) continue;
        f = fopen(path, "r");
        if (!f) { free(buf); continue; }
        fread(buf, 1, sz, f);
        buf[sz] = '\0';
        fclose(f);

        cJSON *arr = cJSON_Parse(buf);
        free(buf);
        if (!arr || !cJSON_IsArray(arr)) {
            cJSON_Delete(arr);
            remove(path);
            removed++;
            continue;
        }

        cJSON *first = cJSON_GetArrayItem(arr, 0);
        if (first) {
            cJSON *ts = cJSON_GetObjectItem(first, "ts");
            if (ts && cJSON_IsNumber(ts) && (uint32_t)ts->valuedouble < cutoff) {
                cJSON_Delete(arr);
                remove(path);
                removed++;
                continue;
            }
        }
        cJSON_Delete(arr);
    }
    closedir(dir);

    if (removed > 0) {
        s_total_msgs = count_all_messages();
        save_meta();
        ESP_LOGI(TAG, "Cleanup: removed %d expired batch files", removed);
    }
}

/* Evict oldest files if we have too many */
static void evict_if_needed(void) {
    bool changed = false;
    while (count_files() >= MAX_CACHE_FILES) {
        uint32_t min_seq = find_min_seq();
        if (min_seq == UINT32_MAX) break;
        char path[280];
        make_filename(min_seq, path, sizeof(path));
        ESP_LOGW(TAG, "Evicting oldest batch: %s", path);
        if (remove(path) == 0) changed = true;
    }
    if (changed) {
        s_total_msgs = count_all_messages();
        save_meta();
    }
}

/* ---- Internal: read one batch file into cJSON array (caller holds mutex) ---- */
static cJSON *read_batch_file(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); return NULL; }
    char *buf = malloc(sz + 1);
    if (!buf) { fclose(f); return NULL; }
    fread(buf, 1, sz, f);
    buf[sz] = '\0';
    fclose(f);
    cJSON *arr = cJSON_Parse(buf);
    free(buf);
    if (!arr || !cJSON_IsArray(arr)) {
        cJSON_Delete(arr);
        return NULL;
    }
    return arr;
}

/* ---- Internal: flush batch buffer to SPIFFS ---- */

/* SPIFFS 满时删除最旧的批次文件腾出空间 */
static void evict_oldest_batch(void) {
    uint32_t min_seq = find_min_seq();
    if (min_seq == UINT32_MAX) return;
    char path[280];
    make_filename(min_seq, path, sizeof(path));
    ESP_LOGW(TAG, "SPIFFS full, evicting oldest: %s", path);
    if (remove(path) == 0) {
        s_total_msgs = count_all_messages();
        save_meta();
    }
}

static esp_err_t flush_batch(void) {
    if (s_batch_count == 0) return ESP_OK;

    evict_if_needed();

    cJSON *arr = cJSON_CreateArray();
    if (!arr) return ESP_FAIL;

    for (int i = 0; i < s_batch_count; i++) {
        cJSON *item = cJSON_CreateObject();
        if (!item) {
            cJSON_Delete(arr);
            return ESP_ERR_NO_MEM;
        }
        cJSON_AddStringToObject(item, "t", s_batch[i].topic);
        cJSON_AddStringToObject(item, "p", s_batch[i].payload);
        cJSON_AddNumberToObject(item, "ts", s_batch[i].timestamp);
        cJSON_AddItemToArray(arr, item);
    }

    char *json = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    if (!json) return ESP_FAIL;

    char path[280];
    make_filename(s_next_seq, path, sizeof(path));

    size_t expected = strlen(json);
    size_t written = 0;
    bool synced = false;
    for (int attempt = 0; attempt < 2 && !synced; ++attempt) {
        FILE *f = fopen(path, "w");
        if (f) {
            written = fwrite(json, 1, expected, f);
            synced = written == expected && fflush(f) == 0 && fsync(fileno(f)) == 0;
            fclose(f);
        }
        if (!synced) {
            remove(path);
            if (attempt == 0) evict_oldest_batch();
        }
    }
    free(json);

    if (!synced) {
        ESP_LOGE(TAG, "Failed to persist batch %s after retry", path);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Flushed batch #%lu: %d msgs -> %s (%zu bytes)",
             (unsigned long)s_next_seq, s_batch_count, path, written);

    s_next_seq++;
    s_total_msgs += s_batch_count;
    s_batch_count = 0;
    if (s_batch) {          /* 冲空即归还, 空闲期不占堆 (下次 add 再分配) */
        free(s_batch);
        s_batch = NULL;
    }
    save_meta();

    return ESP_OK;
}

/* ---- Public API ---- */

void offline_cache_init(void) {
    if (s_initialized) return;

    s_mutex = xSemaphoreCreateMutex();
    configASSERT(s_mutex);

    /* 批缓冲按需分配 (2026-09-22): 常态(已连接)下缓存为空, 这 10×584B 常年闲置;
     * 首次需要缓存时才 malloc, 冲空后立刻归还。 */
    s_batch = NULL;
    s_batch_count = 0;
    s_batch_start_ms = now_ms();
    s_last_process_ms = s_batch_start_ms;

    esp_vfs_spiffs_conf_t spiffs_cfg = {
        .base_path = SPIFFS_MOUNT,
        .partition_label = SPIFFS_PARTITION,
        .max_files = 5,
        .format_if_mount_failed = false,
    };
    esp_err_t ret = esp_vfs_spiffs_register(&spiffs_cfg);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "SPIFFS mount failed (%s), formatting...",
                 esp_err_to_name(ret));
        esp_spiffs_format(SPIFFS_PARTITION);
        ret = esp_vfs_spiffs_register(&spiffs_cfg);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "SPIFFS format+mount failed: %s", esp_err_to_name(ret));
            return;
        }
    }

    load_meta();

    size_t total = 0, used = 0;
    esp_spiffs_info(SPIFFS_PARTITION, &total, &used);
    ESP_LOGI(TAG, "SPIFFS: total=%lu, used=%lu", (unsigned long)total, (unsigned long)used);

    cleanup_expired();
    s_total_msgs = count_all_messages();
    uint32_t max_seq = 0;
    DIR *scan = opendir(SPIFFS_MOUNT);
    if (scan) {
        struct dirent *ent;
        while ((ent = readdir(scan)) != NULL) {
            if (ent->d_name[0] == 'b') {
                uint32_t seq = (uint32_t)strtoul(ent->d_name + 1, NULL, 10);
                if (seq > max_seq) max_seq = seq;
            }
        }
        closedir(scan);
    }
    s_next_seq = max_seq + 1;
    save_meta();

    int files = count_files();
    ESP_LOGI(TAG, "Initialized. Batch files: %d, total msgs: %lu",
             files, (unsigned long)s_total_msgs);

    s_initialized = true;
}

bool offline_cache_add(const char *topic, const char *payload, uint32_t timestamp) {
    if (!s_initialized || !topic || !payload || strlen(topic) >= sizeof(s_batch[0].topic) ||
        strlen(payload) >= sizeof(s_batch[0].payload)) return false;

    if (xSemaphoreTake(s_mutex, portMAX_DELAY) != pdTRUE) return false;

    /* A retained full batch must flush first: success frees s_batch, so the
     * next message needs a fresh allocation even if a buffer existed on entry. */
    if (s_batch_count >= MAX_BATCH_ITEMS && flush_batch() != ESP_OK) {
        xSemaphoreGive(s_mutex);
        return false;
    }
    if (!s_batch) {
        s_batch = calloc(MAX_BATCH_ITEMS, sizeof(OfflineCacheItem));
        if (!s_batch) {
            ESP_LOGE(TAG, "batch alloc failed (%u bytes); message dropped",
                     (unsigned)(MAX_BATCH_ITEMS * sizeof(OfflineCacheItem)));
            xSemaphoreGive(s_mutex);
            return false;
        }
    }
    if (timestamp == 0) timestamp = now_epoch();

    OfflineCacheItem *item = &s_batch[s_batch_count];
    memset(item, 0, sizeof(*item));
    strncpy(item->topic, topic, sizeof(item->topic) - 1);
    strncpy(item->payload, payload, sizeof(item->payload) - 1);
    item->timestamp = timestamp;
    item->index = s_batch_count;
    if (s_batch_count == 0) s_batch_start_ms = now_ms();
    s_batch_count++;

    ESP_LOGD(TAG, "Buffered: %s (%d bytes), batch %d/%d",
             topic, (int)strlen(payload), s_batch_count, MAX_BATCH_ITEMS);

    const bool stored = s_batch_count < MAX_BATCH_ITEMS || flush_batch() == ESP_OK;

    xSemaphoreGive(s_mutex);
    return stored;
}

void offline_cache_process(void) {
    if (!s_initialized || !s_mutex) return;

    const int64_t current_ms = now_ms();
    if (current_ms - s_last_process_ms < 1000) return;
    s_last_process_ms = current_ms;

    if (xSemaphoreTake(s_mutex, portMAX_DELAY) != pdTRUE) return;
    if (s_quiet) {
        /* 不能持锁 return：main_loop 下一次调用会永久阻塞在同一把非递归 mutex，
         * 随后持续触发 Task WDT，且 OTA 结束也无法恢复。 */
        xSemaphoreGive(s_mutex);
        return;
    }
    if (s_batch_count > 0 && current_ms - s_batch_start_ms >= FLUSH_INTERVAL_MS) {
        if (flush_batch() != ESP_OK) {
            ESP_LOGW(TAG, "Periodic batch flush failed; retaining %d messages in RAM",
                     s_batch_count);
        }
    }
    xSemaphoreGive(s_mutex);
}

int offline_cache_count(void) {
    if (!s_initialized) return 0;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    int count = s_total_msgs + s_batch_count;
    xSemaphoreGive(s_mutex);
    return count;
}

void offline_cache_cleanup(void) {
    if (!s_initialized) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    cleanup_expired();
    xSemaphoreGive(s_mutex);
}

void offline_cache_clear(void) {
    if (!s_initialized) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);

    /* Clear batch buffer */
    s_batch_count = 0;

    /* Delete all batch files */
    DIR *dir = opendir(SPIFFS_MOUNT);
    if (dir) {
        struct dirent *ent;
        while ((ent = readdir(dir)) != NULL) {
            if (ent->d_name[0] == 'b') {
                char path[280];
                snprintf(path, sizeof(path), SPIFFS_MOUNT "/%s", ent->d_name);
                remove(path);
            }
        }
        closedir(dir);
    }

    s_total_msgs = 0;
    s_next_seq = 1;
    s_last_sync = 0;
    nvs_handle_t meta;
    if (nvs_open(META_NAMESPACE, NVS_READWRITE, &meta) == ESP_OK) {
        nvs_erase_all(meta);
        nvs_commit(meta);
        nvs_close(meta);
    }
    ESP_LOGI(TAG, "All cache cleared");
    xSemaphoreGive(s_mutex);
}

bool offline_cache_reserve_space(size_t bytes) {
    if (!s_initialized || !s_mutex) return false;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    size_t total = 0, used = 0;
    bool ok = esp_spiffs_info(SPIFFS_PARTITION, &total, &used) == ESP_OK;
    while (ok && total - used < bytes) {
        uint32_t before = find_min_seq();
        if (before == UINT32_MAX) {
            ok = false;
            break;
        }
        evict_oldest_batch();
        ok = esp_spiffs_info(SPIFFS_PARTITION, &total, &used) == ESP_OK;
    }
    if (ok) {
        s_total_msgs = count_all_messages();
        save_meta();
    }
    xSemaphoreGive(s_mutex);
    return ok;
}

int offline_cache_flush_all(bool (*send_func)(const char *topic, const char *payload, uint32_t timestamp)) {
    if (!s_initialized || !send_func) return 0;

    /* 首先刷新 RAM 批次缓冲到 SPIFFS (持 mutex) */
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(200)) != pdTRUE) return 0;
    if (s_batch_count > 0) {
        flush_batch();
        s_batch_start_ms = now_ms();
    }
    xSemaphoreGive(s_mutex);

    int sent = 0;
    int failed = 0;

    ESP_LOGI(TAG, "Flushing cached batches...");

    /* 逐文件处理: 读取持 mutex, 发送释放 mutex, 删除再持 mutex */
    while (1) {
        /* 1. 持 mutex: 找到最旧文件 + 读取内容 */
        if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(200)) != pdTRUE) break;

        uint32_t min_seq = find_min_seq();
        if (min_seq == UINT32_MAX) {
            xSemaphoreGive(s_mutex);
            break;
        }
        char path[280];
        make_filename(min_seq, path, sizeof(path));
        cJSON *arr = read_batch_file(path);
        if (!arr) {
            remove(path);
            xSemaphoreGive(s_mutex);
            continue;
        }
        xSemaphoreGive(s_mutex);

        /* 2. 不持 mutex: 逐条发送 (MQTT 可能阻塞数秒) */
        int n = cJSON_GetArraySize(arr);
        bool batch_ok = true;
        int first_unsent = n;

        for (int i = 0; i < n; i++) {
            cJSON *item = cJSON_GetArrayItem(arr, i);
            cJSON *jt = cJSON_GetObjectItem(item, "t");
            cJSON *jp = cJSON_GetObjectItem(item, "p");
            cJSON *jts = cJSON_GetObjectItem(item, "ts");

            if (!jt || !jp) continue;

            if (send_func(jt->valuestring, jp->valuestring,
                          jts ? (uint32_t)jts->valuedouble : 0)) {
                sent++;
            } else {
                failed++;
                batch_ok = false;
                first_unsent = i;
                ESP_LOGW(TAG, "Send failed, pausing flush");
                break;
            }
        }

        /* 3. 持 mutex: 删除已发送文件 / 重写未发送部分 */
        if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(200)) == pdTRUE) {
            if (!batch_ok && first_unsent > 0) {
                cJSON *remaining = cJSON_CreateArray();
                for (int i = first_unsent; remaining && i < n; ++i) {
                    cJSON *copy = cJSON_Duplicate(cJSON_GetArrayItem(arr, i), true);
                    if (!copy) { cJSON_Delete(remaining); remaining = NULL; break; }
                    cJSON_AddItemToArray(remaining, copy);
                }
                char *remaining_json = remaining ? cJSON_PrintUnformatted(remaining) : NULL;
                cJSON_Delete(remaining);
                if (remaining_json) {
                    char tmp_path[300];
                    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);
                    FILE *out = fopen(tmp_path, "w");
                    size_t expected = strlen(remaining_json);
                    bool saved = out && fwrite(remaining_json, 1, expected, out) == expected &&
                                 fflush(out) == 0 && fsync(fileno(out)) == 0;
                    if (out) fclose(out);
                    if (saved && rename(tmp_path, path) != 0) saved = false;
                    if (!saved) remove(tmp_path);
                    free(remaining_json);
                }
            }

            if (batch_ok) {
                remove(path);
                s_total_msgs -= (s_total_msgs >= (uint32_t)n) ? (uint32_t)n : s_total_msgs;
            }
            xSemaphoreGive(s_mutex);
        }

        cJSON_Delete(arr);
        if (!batch_ok) break;
    }

    /* 最终元数据更新 */
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(200)) == pdTRUE) {
        s_total_msgs = count_all_messages();
        if (sent > 0 && failed == 0) {
            s_last_sync = now_epoch();
        }
        save_meta();
        xSemaphoreGive(s_mutex);
    }

    ESP_LOGI(TAG, "Flush done: sent=%d, failed=%d", sent, failed);
    return sent;
}

uint32_t offline_cache_get_last_sync_time(void) {
    return s_last_sync;
}

void offline_cache_set_last_sync_time(uint32_t timestamp) {
    s_last_sync = timestamp;
    save_meta();
}

size_t offline_cache_total_bytes(void) {
    if (!s_initialized) return 0;
    xSemaphoreTake(s_mutex, portMAX_DELAY);

    size_t total = 0;
    DIR *dir = opendir(SPIFFS_MOUNT);
    if (dir) {
        struct dirent *ent;
        while ((ent = readdir(dir)) != NULL) {
            if (ent->d_name[0] == 'b') {
                char path[280];
                snprintf(path, sizeof(path), SPIFFS_MOUNT "/%s", ent->d_name);
                struct stat st;
                if (stat(path, &st) == 0) total += st.st_size;
            }
        }
        closedir(dir);
    }

    xSemaphoreGive(s_mutex);
    return total;
}

/* These are no longer needed with SPIFFS (ring buffer API) */
bool offline_cache_get(int index, OfflineCacheItem *item) {
    (void)index; (void)item;
    return false; /* not supported in batch mode */
}

void offline_cache_remove(int index) {
    (void)index;
    /* not needed — flush_all handles removal */
}
