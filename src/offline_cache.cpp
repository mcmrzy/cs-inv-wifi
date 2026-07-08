/**
 * offline_cache.cpp — SPIFFS-based offline message cache with write batching
 *
 * Design goals:
 *   1. Survive power loss (SPIFFS handles this)
 *   2. Minimize flash wear (batch N messages into one file write)
 *   3. 7-day retention with automatic cleanup
 *   4. Drop-in replacement for the old NVS/Preferences cache
 *
 * File naming: /b<seq>  (e.g. /b000001, /b000002, ...)
 * Each file contains a JSON array of cached messages.
 */

#include "offline_cache.h"
#include "nvs_storage.h"
#include <string.h>
#include <time.h>
#include <SPIFFS.h>

#ifdef CHIP_ESP32C3
  #include <Preferences.h>
#endif

// ---- Configuration ----
#define SPIFFS_MOUNT      "/"
#define MAX_BATCH_ITEMS   10        // flush after this many messages
#define FLUSH_INTERVAL_MS 60000     // or after this many ms
#define MAX_CACHE_FILES   500       // max batch files on disk

// ---- NVS keys for metadata persistence ----
#ifdef CHIP_ESP32C3
static Preferences g_meta_prefs;
#endif
#define META_NAMESPACE    "cache_meta"
#define KEY_NEXT_SEQ      "next_seq"
#define KEY_TOTAL_MSGS    "total_msgs"
#define KEY_LAST_SYNC     "last_sync"
#define KEY_LAST_EPOCH    "last_epoch"

// ---- State ----
static bool s_initialized = false;
static SemaphoreHandle_t s_mutex = NULL;

// Write batch buffer
static OfflineCacheItem *s_batch = NULL;
static int s_batch_count = 0;
static unsigned long s_batch_start_ms = 0;

// Persistent metadata
static uint32_t s_next_seq = 1;
static uint32_t s_total_msgs = 0;
static uint32_t s_last_sync = 0;

// ---- Helpers ----

static uint32_t now_epoch(void) {
    return (uint32_t)time(nullptr);
}

static void load_meta(void) {
#ifdef CHIP_ESP32C3
    g_meta_prefs.begin(META_NAMESPACE, true); // read-only
    s_next_seq = g_meta_prefs.getUInt(KEY_NEXT_SEQ, 1);
    s_total_msgs = g_meta_prefs.getUInt(KEY_TOTAL_MSGS, 0);
    s_last_sync = g_meta_prefs.getUInt(KEY_LAST_SYNC, 0);
    g_meta_prefs.end();
#endif
}

static void save_meta(void) {
#ifdef CHIP_ESP32C3
    g_meta_prefs.begin(META_NAMESPACE, false); // read-write
    g_meta_prefs.putUInt(KEY_NEXT_SEQ, s_next_seq);
    g_meta_prefs.putUInt(KEY_TOTAL_MSGS, s_total_msgs);
    g_meta_prefs.putUInt(KEY_LAST_SYNC, s_last_sync);
    g_meta_prefs.end();
#endif
}

// 冷启动时恢复上次 NTP epoch，避免 RTC 从 0 开始
static void restore_rtc_from_nvs(void) {
#ifdef CHIP_ESP32C3
    g_meta_prefs.begin(META_NAMESPACE, true);
    uint32_t saved_epoch = g_meta_prefs.getUInt(KEY_LAST_EPOCH, 0);
    g_meta_prefs.end();

    if (saved_epoch > 1000000000) {
        struct timeval tv;
        tv.tv_sec = (time_t)saved_epoch;
        tv.tv_usec = 0;
        settimeofday(&tv, NULL);
        Serial.printf("[SYS] Restored RTC from NVS: epoch=%lu\n", (unsigned long)saved_epoch);
    }
#endif
}

// 保存当前 epoch 到 NVS（NTP 同步后调用）
void offline_cache_save_epoch(void) {
#ifdef CHIP_ESP32C3
    time_t t = time(nullptr);
    if (t > 1000000000) {
        g_meta_prefs.begin(META_NAMESPACE, false);
        g_meta_prefs.putUInt(KEY_LAST_EPOCH, (uint32_t)t);
        g_meta_prefs.end();
    }
#endif
}

static void make_filename(uint32_t seq, char *buf, size_t len) {
    snprintf(buf, len, "/b%08lu", (unsigned long)seq);
}

// 获取文件名的 basename（跳过前导 '/'）
// Arduino SPIFFS 的 file.name() 返回 "/b000001"，需要跳过 '/'
static const char* cache_basename(const char* path) {
    if (path && path[0] == '/') return path + 1;
    return path;
}

// Count files in SPIFFS
static int count_files(void) {
    int count = 0;
    File root = SPIFFS.open("/");
    if (!root || !root.isDirectory()) return 0;
    File file = root.openNextFile();
    while (file) {
        if (cache_basename(file.name())[0] == 'b') count++;
        file = root.openNextFile();
    }
    return count;
}

// Find the lowest sequence number among existing files
static uint32_t find_min_seq(void) {
    uint32_t min_seq = UINT32_MAX;
    File root = SPIFFS.open("/");
    if (!root || !root.isDirectory()) return min_seq;
    File file = root.openNextFile();
    while (file) {
        const char* name = cache_basename(file.name());
        if (name[0] == 'b') {
            uint32_t seq = (uint32_t)strtoul(name + 1, NULL, 10);
            if (seq < min_seq) min_seq = seq;
        }
        file = root.openNextFile();
    }
    return min_seq;
}

// Count total messages across all batch files
static uint32_t count_all_messages(void) {
    uint32_t total = 0;
    File root = SPIFFS.open("/");
    if (!root || !root.isDirectory()) return 0;
    File file = root.openNextFile();
    while (file) {
        if (cache_basename(file.name())[0] == 'b') {
            // Count '[' occurrences as rough message count
            // Actually, count opening braces for items
            String content = file.readString();
            int idx = 0;
            while ((idx = content.indexOf("\"t\":", idx)) >= 0) {
                total++;
                idx += 4;
            }
        }
        file = root.openNextFile();
    }
    return total;
}

// Delete files older than TTL_DAYS
static void cleanup_expired(void) {
    uint32_t now = now_epoch();
    if (now == 0) return;

    uint32_t cutoff = now - ((uint32_t)OFFLINE_CACHE_TTL_DAYS * 86400);
    int removed = 0;

    File root = SPIFFS.open("/");
    if (!root || !root.isDirectory()) return;

    File file = root.openNextFile();
    while (file) {
        if (cache_basename(file.name())[0] == 'b') {
            // Read first timestamp from the batch
            String content = file.readString();
            int tsIdx = content.indexOf("\"ts\":");
            if (tsIdx >= 0) {
                uint32_t ts = (uint32_t)atol(content.c_str() + tsIdx + 5);
                if (ts < cutoff) {
                    String path = String("/") + file.name();
                    SPIFFS.remove(path);
                    removed++;
                }
            } else {
                // No timestamp, remove
                String path = String("/") + file.name();
                SPIFFS.remove(path);
                removed++;
            }
        }
        file = root.openNextFile();
    }

    if (removed > 0) {
        s_total_msgs = count_all_messages();
        save_meta();
        Serial.printf("[OFFLINE_CACHE] Cleanup: removed %d expired batch files\n", removed);
    }
}

// Evict oldest files if we have too many
static void evict_if_needed(void) {
    while (count_files() >= MAX_CACHE_FILES) {
        uint32_t min_seq = find_min_seq();
        if (min_seq == UINT32_MAX) break;
        char path[32];
        make_filename(min_seq, path, sizeof(path));
        Serial.printf("[OFFLINE_CACHE] Evicting oldest batch: %s\n", path);
        SPIFFS.remove(path);
    }
}

// ---- Internal: flush batch buffer to SPIFFS ----

static bool flush_batch(void) {
    if (s_batch_count == 0) return true;

    evict_if_needed();

    // Build JSON array manually (avoid ArduinoJson dependency)
    String json = "[";
    for (int i = 0; i < s_batch_count; i++) {
        if (i > 0) json += ",";
        json += "{\"t\":\"";
        // Escape topic
        for (const char *p = s_batch[i].topic; *p; p++) {
            if (*p == '"' || *p == '\\') json += '\\';
            json += *p;
        }
        json += "\",\"p\":\"";
        // Escape payload
        for (const char *p = s_batch[i].payload; *p; p++) {
            if (*p == '"' || *p == '\\') json += '\\';
            json += *p;
        }
        json += "\",\"ts\":";
        json += String(s_batch[i].timestamp);
        json += "}";
    }
    json += "]";

    char path[32];
    make_filename(s_next_seq, path, sizeof(path));

    File f = SPIFFS.open(path, FILE_WRITE);
    if (!f) {
        Serial.printf("[OFFLINE_CACHE] Failed to create %s\n", path);
        return false;
    }
    size_t written = f.print(json);
    f.close();

    if (written == 0) {
        SPIFFS.remove(path);
        return false;
    }

    Serial.printf("[OFFLINE_CACHE] Flushed batch #%lu: %d msgs -> %s (%zu bytes)\n",
                  (unsigned long)s_next_seq, s_batch_count, path, written);

    s_next_seq++;
    s_total_msgs += s_batch_count;
    s_batch_count = 0;
    save_meta();

    return true;
}

// ==================== Public API ====================

void offline_cache_init(void) {
    if (s_initialized) return;

    if (!s_mutex) {
        s_mutex = xSemaphoreCreateMutex();
    }

    s_batch = (OfflineCacheItem *)calloc(MAX_BATCH_ITEMS, sizeof(OfflineCacheItem));
    s_batch_count = 0;
    s_batch_start_ms = millis();

    if (!SPIFFS.begin(true)) { // formatOnFail=true
        Serial.println("[OFFLINE_CACHE] SPIFFS mount failed!");
        return;
    }

    restore_rtc_from_nvs();  // 恢复上次 epoch 到 RTC
    load_meta();
    cleanup_expired();

    Serial.printf("[OFFLINE_CACHE] Initialized. Batch files: %d, total msgs: %lu\n",
                  count_files(), (unsigned long)s_total_msgs);

    s_initialized = true;
}

bool offline_cache_add(const char *topic, const char *payload, uint32_t timestamp) {
    if (!s_initialized || !topic || !payload) return false;

    if (s_mutex) xSemaphoreTake(s_mutex, portMAX_DELAY);

    if (timestamp == 0) timestamp = now_epoch();

    OfflineCacheItem *item = &s_batch[s_batch_count];
    memset(item, 0, sizeof(*item));
    strncpy(item->topic, topic, sizeof(item->topic) - 1);
    strncpy(item->payload, payload, sizeof(item->payload) - 1);
    item->timestamp = timestamp;
    item->index = s_batch_count;
    s_batch_count++;

    Serial.printf("[OFFLINE_CACHE] Cached: %s (ts=%u)\n",
                  topic, (unsigned)timestamp);

    bool need_flush = (s_batch_count >= MAX_BATCH_ITEMS) ||
                      (millis() - s_batch_start_ms >= FLUSH_INTERVAL_MS);

    if (need_flush) {
        flush_batch();
        s_batch_start_ms = millis();
    }

    if (s_mutex) xSemaphoreGive(s_mutex);
    return true;
}

int offline_cache_count(void) {
    if (!s_initialized) return 0;
    if (s_mutex) xSemaphoreTake(s_mutex, portMAX_DELAY);
    int count = s_total_msgs + s_batch_count;
    if (s_mutex) xSemaphoreGive(s_mutex);
    return count;
}

bool offline_cache_get(int index, OfflineCacheItem *item) {
    (void)index; (void)item;
    return false; // not supported in batch mode
}

void offline_cache_remove(int index) {
    (void)index;
    // not needed — flush_all handles removal
}

void offline_cache_cleanup(void) {
    if (!s_initialized) return;
    if (s_mutex) xSemaphoreTake(s_mutex, portMAX_DELAY);
    cleanup_expired();
    if (s_mutex) xSemaphoreGive(s_mutex);
}

void offline_cache_clear(void) {
    if (!s_initialized) return;
    if (s_mutex) xSemaphoreTake(s_mutex, portMAX_DELAY);

    // Clear batch buffer
    s_batch_count = 0;

    // Delete all batch files
    File root = SPIFFS.open("/");
    if (root && root.isDirectory()) {
        File file = root.openNextFile();
        while (file) {
            if (cache_basename(file.name())[0] == 'b') {
                String path = String("/") + file.name();
                SPIFFS.remove(path);
            }
            file = root.openNextFile();
        }
    }

    s_total_msgs = 0;
    save_meta();
    Serial.println("[OFFLINE_CACHE] All cache cleared");
    if (s_mutex) xSemaphoreGive(s_mutex);
}

int offline_cache_flush_all(bool (*send_func)(const char *topic, const char *payload, uint32_t timestamp)) {
    if (!s_initialized || !send_func) return 0;

    if (s_mutex) xSemaphoreTake(s_mutex, portMAX_DELAY);

    // First flush any buffered messages
    if (s_batch_count > 0) {
        flush_batch();
        s_batch_start_ms = millis();
    }

    int sent = 0;
    int failed = 0;

    Serial.println("[OFFLINE_CACHE] Flushing cached batches...");

    while (true) {
        uint32_t min_seq = find_min_seq();
        if (min_seq == UINT32_MAX) break; // no more files

        char path[32];
        make_filename(min_seq, path, sizeof(path));

        File f = SPIFFS.open(path, FILE_READ);
        if (!f) {
            SPIFFS.remove(path);
            continue;
        }

        String content = f.readString();
        f.close();

        if (content.length() == 0) {
            SPIFFS.remove(path);
            continue;
        }

        // Parse the JSON array manually
        // Format: [{"t":"topic","p":"payload","ts":123}, ...]
        bool batch_ok = true;
        int pos = 0;

        while (pos < (int)content.length()) {
            // Find next item start
            int itemStart = content.indexOf('{', pos);
            if (itemStart < 0) break;

            // 用花括号深度计数找到正确的闭合 }，避免 payload 内嵌套 {} 导致截断
            int depth = 0;
            int itemEnd = -1;
            for (int i = itemStart; i < (int)content.length(); i++) {
                if (content[i] == '{') depth++;
                else if (content[i] == '}') {
                    depth--;
                    if (depth == 0) { itemEnd = i; break; }
                }
            }
            if (itemEnd < 0) break;

            String itemStr = content.substring(itemStart, itemEnd + 1);
            pos = itemEnd + 1;

            // Parse "t":"..."
            int tIdx = itemStr.indexOf("\"t\":\"");
            if (tIdx < 0) continue;
            int tStart = tIdx + 5;
            int tEnd = itemStr.indexOf('"', tStart);
            if (tEnd < 0) continue;

            // Handle escaped quotes in topic
            String topic = "";
            for (int i = tStart; i < tEnd; i++) {
                if (itemStr[i] == '\\' && i + 1 < tEnd) {
                    i++;
                    topic += itemStr[i];
                } else {
                    topic += itemStr[i];
                }
            }

            // Parse "p":"..."
            int pIdx = itemStr.indexOf("\"p\":\"");
            if (pIdx < 0) continue;
            int pStart = pIdx + 5;
            // 逐字符扫描，跳过 \" 转义，找到 payload 结束的真正引号
            int pEnd = -1;
            for (int i = pStart; i < (int)itemStr.length(); i++) {
                if (itemStr[i] == '\\') {
                    i++;  // 跳过转义字符
                } else if (itemStr[i] == '"') {
                    pEnd = i;
                    break;
                }
            }
            if (pEnd < 0) continue;

            String payload = "";
            for (int i = pStart; i < pEnd; i++) {
                if (itemStr[i] == '\\' && i + 1 < pEnd) {
                    i++;
                    payload += itemStr[i];
                } else {
                    payload += itemStr[i];
                }
            }

            // Parse "ts":... 从 payload 结束位置之后搜索，避免匹配到 payload 内部
            int tsIdx = itemStr.indexOf("\"ts\":", pEnd);
            uint32_t ts = 0;
            if (tsIdx >= 0) {
                ts = (uint32_t)atol(itemStr.c_str() + tsIdx + 5);
            }

            // ts 不是有效 epoch（NTP 未同步时缓存的），用当前时间替代
            if (ts < 1000000000) {
                ts = (uint32_t)time(nullptr);
            }

            if (send_func(topic.c_str(), payload.c_str(), ts)) {
                sent++;
            } else {
                failed++;
                batch_ok = false;
                Serial.println("[OFFLINE_CACHE] Send failed, pausing flush");
                break;
            }
        }

        if (batch_ok) {
            SPIFFS.remove(path);
            uint32_t msgCount = 0;
            int idx = 0;
            while ((idx = content.indexOf("\"t\":", idx)) >= 0) {
                msgCount++;
                idx += 4;
            }
            s_total_msgs -= (s_total_msgs >= msgCount) ? msgCount : s_total_msgs;
        } else {
            break;
        }
    }

    if (sent > 0 && failed == 0) {
        s_last_sync = now_epoch();
        save_meta();
    }

    save_meta();
    Serial.printf("[OFFLINE_CACHE] Flush done: sent=%d, failed=%d, remaining files=%d\n",
                  sent, failed, count_files());

    if (s_mutex) xSemaphoreGive(s_mutex);
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
    if (s_mutex) xSemaphoreTake(s_mutex, portMAX_DELAY);

    size_t total = 0;
    File root = SPIFFS.open("/");
    if (root && root.isDirectory()) {
        File file = root.openNextFile();
        while (file) {
            if (cache_basename(file.name())[0] == 'b') {
                total += file.size();
            }
            file = root.openNextFile();
        }
    }

    if (s_mutex) xSemaphoreGive(s_mutex);
    return total;
}
