#ifndef OFFLINE_CACHE_H
#define OFFLINE_CACHE_H

#include "config.h"
#include <stdint.h>
#include <stdbool.h>

// 缓存配置（使用config.h中的默认值）
#ifndef OFFLINE_CACHE_MAX_ITEMS
#define OFFLINE_CACHE_MAX_ITEMS  1000   // 最多存储1000条数据
#endif

#ifndef OFFLINE_CACHE_TTL_DAYS
#define OFFLINE_CACHE_TTL_DAYS   7      // 数据保留7天
#endif

// 缓存数据结构
typedef struct {
    char topic[64];           // 子主题
    char payload[512];        // 消息内容
    uint32_t timestamp;       // Unix时间戳（秒）
    uint32_t index;           // 存储索引
} OfflineCacheItem;

// ==================== 初始化 ====================
// 初始化离线缓存，清理过期数据
void offline_cache_init(void);

// ==================== 缓存操作 ====================
// 添加数据到缓存（MQTT未连接时调用）
// timestamp: Unix时间戳，如果为0则自动获取
// 返回: true=成功, false=失败（缓存满）
bool offline_cache_add(const char* topic, const char* payload, uint32_t timestamp);

// 获取缓存数量
int offline_cache_count(void);

// 获取指定索引的数据
// 返回: true=成功, false=索引无效
bool offline_cache_get(int index, OfflineCacheItem* item);

// 删除指定索引的数据
void offline_cache_remove(int index);

// 清理过期数据（超过7天）
void offline_cache_cleanup(void);

// 发送所有缓存数据（MQTT连接成功时调用）
// send_func: 发送回调，返回true表示发送成功
// 返回: 成功发送的数据条数
int offline_cache_flush_all(bool (*send_func)(const char* topic, const char* payload, uint32_t timestamp));

// 获取最后同步时间（用于状态显示）
uint32_t offline_cache_get_last_sync_time(void);

// 设置最后同步时间
void offline_cache_set_last_sync_time(uint32_t timestamp);

// 获取缓存总字节数
size_t offline_cache_total_bytes(void);

// 清除所有缓存
void offline_cache_clear(void);

#endif // OFFLINE_CACHE_H
