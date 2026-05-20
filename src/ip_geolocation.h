/**
 * ================================================================
 * IP 地理位置模块
 * 功能: WiFi 连接后通过 IP 获取大致地理位置（城市级）
 * API: ipapi.co (免费，无需 API key，限速 1000 req/day)
 * ================================================================
 */

#ifndef IP_GEOLOCATION_H
#define IP_GEOLOCATION_H

#include <Arduino.h>

// 地理位置数据结构（简化版：仅 IP + 城市）
struct GeoLocation {
    bool valid;           // 定位是否成功
    char ip[16];          // 公网 IP
    char city[64];        // 城市
    unsigned long updated_at; // 最后更新时间 (millis)
};

// 初始化
void ipgeo_init();

// 触发 IP 定位请求（异步，非阻塞）
// 在 WiFi 连接成功后调用
void ipgeo_request_update();

// 检查定位是否完成并获取结果
// 返回 true 表示有新数据可用
bool ipgeo_check_result(GeoLocation* out);

// 获取缓存的地理位置（无论是否过期）
bool ipgeo_get_cached(GeoLocation* out);

// 将地理位置转为 JSON 字符串（用于嵌入 status 消息）
// out 缓冲区建议 >= 256 字节
void ipgeo_to_json(const GeoLocation* geo, char* out, size_t out_len);

// 获取定位状态描述
const char* ipgeo_status_string();

// ==================== API URL 配置 ====================
// 获取当前 API URL 配置（host, port, path）
void ipgeo_get_config(char* host, size_t host_len,
                       uint16_t* port, char* path, size_t path_len);
// 设置并持久化 API URL 配置
bool ipgeo_set_config(const char* host, uint16_t port, const char* path);
// 恢复默认 API URL
void ipgeo_reset_config();

#endif // IP_GEOLOCATION_H
