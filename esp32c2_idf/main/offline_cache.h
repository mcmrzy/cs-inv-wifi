#ifndef OFFLINE_CACHE_H
#define OFFLINE_CACHE_H

#include "config.h"
#include <stdint.h>
#include <stdbool.h>

typedef struct {
    char topic[64];
    char payload[512];
    uint32_t timestamp;
    uint32_t index;
} OfflineCacheItem;

void offline_cache_init(void);

bool offline_cache_add(const char* topic, const char* payload, uint32_t timestamp);

int offline_cache_count(void);

bool offline_cache_get(int index, OfflineCacheItem* item);

void offline_cache_remove(int index);

void offline_cache_cleanup(void);

int offline_cache_flush_all(bool (*send_func)(const char* topic, const char* payload, uint32_t timestamp));

uint32_t offline_cache_get_last_sync_time(void);

void offline_cache_set_last_sync_time(uint32_t timestamp);

size_t offline_cache_total_bytes(void);

void offline_cache_clear(void);

#endif // OFFLINE_CACHE_H
