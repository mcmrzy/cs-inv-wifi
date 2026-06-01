/**
 * offline_cache.cpp - 离线数据缓存模块
 * 
 * 功能：
 * 1. 当MQTT未连接时，将数据存储到Flash/NVS
 * 2. 每个数据自动添加Unix时间戳
 * 3. 连接成功后发送所有缓存数据
 * 4. 自动清理超过7天的过期数据
 */

#include "offline_cache.h"
#include "nvs_storage.h"
#include <string.h>
#include <time.h>

#ifdef CHIP_ESP32C3
  #include <Preferences.h>
#else
  #include <EEPROM.h>
#endif

// NVS 命名空间和键
#define OFFLINE_CACHE_NAMESPACE "offline_cache"
#define OFFLINE_CACHE_COUNT_KEY  "count"
#define OFFLINE_CACHE_INDEX_KEY  "next_index"
#define OFFLINE_CACHE_SYNC_KEY   "last_sync"
#define OFFLINE_CACHE_ITEM(i)    "item_" #i

// 全局变量
static Preferences g_prefs;
static bool g_initialized = false;
static uint32_t g_last_sync_time = 0;

// 静态函数声明
static uint32_t get_current_timestamp(void);
static bool load_item(int index, OfflineCacheItem* item);
static bool save_item(const OfflineCacheItem* item);
static void remove_item_at(int index);

// ==================== 初始化 ====================
void offline_cache_init(void) {
    if (g_initialized) return;
    
    // 打开 Preferences
    if (!g_prefs.begin(OFFLINE_CACHE_NAMESPACE, false)) {
        Serial.println("[OFFLINE_CACHE] Failed to open NVS");
        return;
    }
    
    // 清理过期数据
    offline_cache_cleanup();
    
    // 加载最后同步时间
    g_last_sync_time = g_prefs.getUInt(OFFLINE_CACHE_SYNC_KEY, 0);
    
    g_initialized = true;
    Serial.printf("[OFFLINE_CACHE] Initialized. Cached items: %d\n", offline_cache_count());
}

// ==================== 工具函数 ====================
static uint32_t get_current_timestamp(void) {
    time_t now = time(nullptr);
    if (now < 1000000000) {  // NTP未同步，返回0
        return 0;
    }
    return (uint32_t)now;
}

static bool load_item(int index, OfflineCacheItem* item) {
    if (index < 0 || index >= OFFLINE_CACHE_MAX_ITEMS) return false;
    
    char key[16];
    snprintf(key, sizeof(key), "item_%d", index);
    
    String json = g_prefs.getString(key, "");
    if (json.length() == 0) return false;
    
    // 简单的JSON解析
    // 格式: {"topic":"xxx","payload":"yyy","ts":1234567890,"idx":1}
    memset(item, 0, sizeof(OfflineCacheItem));
    
    const char* p = json.c_str();
    char* out;
    
    // 解析 topic
    const char* tp = strstr(p, "\"topic\":\"");
    if (tp) {
        tp += 9;
        out = item->topic;
        while (*tp && *tp != '"' && (size_t)(out - item->topic) < sizeof(item->topic) - 1) {
            if (*tp == '\\' && *(tp + 1)) tp++;
            *out++ = *tp++;
        }
        *out = '\0';
    }
    
    // 解析 payload
    const char* pp = strstr(p, "\"payload\":\"");
    if (pp) {
        pp += 10;
        out = item->payload;
        while (*pp && *pp != '"' && (size_t)(out - item->payload) < sizeof(item->payload) - 1) {
            if (*pp == '\\' && *(pp + 1)) pp++;
            *out++ = *pp++;
        }
        *out = '\0';
    }
    
    // 解析 timestamp
    const char* tsp = strstr(p, "\"ts\":");
    if (tsp) {
        item->timestamp = atoi(tsp + 5);
    }
    
    // 解析 index
    const char* ip = strstr(p, "\"idx\":");
    if (ip) {
        item->index = atoi(ip + 6);
    }
    
    return true;
}

static bool save_item(const OfflineCacheItem* item) {
    char key[16];
    snprintf(key, sizeof(key), "item_%d", item->index);
    
    // 构建JSON
    char json[sizeof(OfflineCacheItem) + 64];
    int len = snprintf(json, sizeof(json),
        "{\"topic\":\"%s\",\"payload\":\"%s\",\"ts\":%u,\"idx\":%u}",
        item->topic, item->payload, item->timestamp, item->index);
    
    return g_prefs.putString(key, json);
}

// ==================== 添加数据 ====================
bool offline_cache_add(const char* topic, const char* payload, uint32_t timestamp) {
    if (!g_initialized) offline_cache_init();
    if (!topic || !payload) return false;
    
    int count = offline_cache_count();
    
    // 检查是否已满
    if (count >= OFFLINE_CACHE_MAX_ITEMS) {
        Serial.println("[OFFLINE_CACHE] Cache full, removing oldest item");
        // 移除最老的数据
        remove_item_at(0);
        count--;
    }
    
    // 获取时间戳
    if (timestamp == 0) {
        timestamp = get_current_timestamp();
    }
    
    // 创建新条目
    OfflineCacheItem item;
    memset(&item, 0, sizeof(item));
    strncpy(item.topic, topic, sizeof(item.topic) - 1);
    strncpy(item.payload, payload, sizeof(item.payload) - 1);
    item.timestamp = timestamp;
    item.index = count;
    
    // 保存
    if (save_item(&item)) {
        // 更新计数
        g_prefs.putInt(OFFLINE_CACHE_COUNT_KEY, count + 1);
        Serial.printf("[OFFLINE_CACHE] Added: topic=%s, ts=%u, total=%d\n", 
                      topic, timestamp, count + 1);
        return true;
    }
    
    return false;
}

// ==================== 查询 ====================
int offline_cache_count(void) {
    if (!g_initialized) return 0;
    return g_prefs.getInt(OFFLINE_CACHE_COUNT_KEY, 0);
}

bool offline_cache_get(int index, OfflineCacheItem* item) {
    if (!g_initialized) return false;
    if (index < 0 || index >= offline_cache_count()) return false;
    return load_item(index, item);
}

uint32_t offline_cache_get_last_sync_time(void) {
    return g_last_sync_time;
}

void offline_cache_set_last_sync_time(uint32_t timestamp) {
    g_last_sync_time = timestamp;
    if (g_initialized) {
        g_prefs.putUInt(OFFLINE_CACHE_SYNC_KEY, timestamp);
    }
}

size_t offline_cache_total_bytes(void) {
    if (!g_initialized) return 0;
    
    size_t total = 0;
    int count = offline_cache_count();
    for (int i = 0; i < count; i++) {
        char key[16];
        snprintf(key, sizeof(key), "item_%d", i);
        total += g_prefs.getString(key, "").length();
    }
    return total;
}

// ==================== 删除和清理 ====================
static void remove_item_at(int index) {
    if (!g_initialized) return;
    
    int count = offline_cache_count();
    if (index < 0 || index >= count) return;
    
    // 清除该条目
    char key[16];
    snprintf(key, sizeof(key), "item_%d", index);
    g_prefs.remove(key);
    
    // 重新排列：将后面的数据前移
    for (int i = index; i < count - 1; i++) {
        OfflineCacheItem item;
        char src_key[16], dst_key[16];
        snprintf(src_key, sizeof(src_key), "item_%d", i + 1);
        snprintf(dst_key, sizeof(dst_key), "item_%d", i);
        
        String json = g_prefs.getString(src_key, "");
        if (json.length() > 0) {
            g_prefs.remove(src_key);
            g_prefs.putString(dst_key, json);
        }
    }
    
    // 清除最后一个
    snprintf(key, sizeof(key), "item_%d", count - 1);
    g_prefs.remove(key);
    
    // 更新计数
    g_prefs.putInt(OFFLINE_CACHE_COUNT_KEY, count - 1);
}

void offline_cache_remove(int index) {
    if (!g_initialized) return;
    remove_item_at(index);
}

void offline_cache_cleanup(void) {
    if (!g_initialized) return;
    
    uint32_t now = get_current_timestamp();
    if (now == 0) return;  // NTP未同步，不清理
    
    uint32_t cutoff = now - (OFFLINE_CACHE_TTL_DAYS * 24 * 3600);
    
    int count = offline_cache_count();
    int removed = 0;
    
    for (int i = count - 1; i >= 0; i--) {
        OfflineCacheItem item;
        if (load_item(i, &item)) {
            if (item.timestamp < cutoff) {
                remove_item_at(i);
                removed++;
            }
        }
    }
    
    if (removed > 0) {
        Serial.printf("[OFFLINE_CACHE] Cleanup: removed %d expired items\n", removed);
    }
}

void offline_cache_clear(void) {
    if (!g_initialized) return;
    
    int count = offline_cache_count();
    for (int i = 0; i < count; i++) {
        char key[16];
        snprintf(key, sizeof(key), "item_%d", i);
        g_prefs.remove(key);
    }
    g_prefs.putInt(OFFLINE_CACHE_COUNT_KEY, 0);
    
    Serial.println("[OFFLINE_CACHE] All cache cleared");
}

// ==================== 发送所有缓存数据 ====================
int offline_cache_flush_all(void (*send_func)(const char* topic, const char* payload, uint32_t timestamp)) {
    if (!g_initialized) return 0;
    if (!send_func) return 0;
    
    int count = offline_cache_count();
    if (count == 0) return 0;
    
    int sent = 0;
    uint32_t now = get_current_timestamp();
    
    Serial.printf("[OFFLINE_CACHE] Flushing %d cached items...\n", count);
    
    // 逐条发送，从最老的开始
    for (int i = 0; i < count; i++) {
        OfflineCacheItem item;
        if (load_item(i, &item)) {
            // 调用发送函数
            send_func(item.topic, item.payload, item.timestamp);
            sent++;
            
            // 每发送10条打印一次进度
            if (sent % 10 == 0) {
                Serial.printf("[OFFLINE_CACHE] Progress: %d/%d\n", sent, count);
            }
        }
    }
    
    // 更新最后同步时间
    if (now > 0) {
        offline_cache_set_last_sync_time(now);
    }
    
    // 清理所有已发送的数据
    offline_cache_clear();
    
    Serial.printf("[OFFLINE_CACHE] Flush complete: sent %d items\n", sent);
    return sent;
}
