/**
 * ================================================================
 * IP 地理位置模块实现
 * 使用可配置的 IP 定位 API (默认 ipapi.co, 支持 HTTPS)
 * 异步非阻塞设计，使用 WiFiClientSecure
 * API URL 可通过 NVS / 网页配置页面编辑
 * ================================================================
 */

#include "ip_geolocation.h"
#include "nvs_storage.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>

// ==================== 运行时配置（默认值，可从 NVS 覆盖） ====================
static char g_api_host[IPGEO_HOST_MAX_LEN] = IPGEO_DEFAULT_HOST;
static uint16_t g_api_port = IPGEO_DEFAULT_PORT;
static char g_api_path[IPGEO_PATH_MAX_LEN] = IPGEO_DEFAULT_PATH;

// 运行时常量
#define IPGEO_TIMEOUT_MS    10000
#define IPGEO_CACHE_MS      3600000   // 1 小时缓存
#define IPGEO_RETRY_MS      60000     // 失败 1 分钟后重试

// ==================== 内部状态 ====================
static enum {
    GEO_IDLE,           // 空闲
    GEO_REQUESTING,     // 请求中
    GEO_SUCCESS,        // 成功
    GEO_FAILED          // 失败
} g_state = GEO_IDLE;

static GeoLocation g_cached = {0};
static unsigned long g_request_start = 0;
static unsigned long g_last_attempt = 0;
static String g_response_buffer;

static WiFiClientSecure* g_client = nullptr;

// ==================== 工具函数 ====================
static void parse_json_field(const char* json, const char* key, char* out, size_t out_len) {
    out[0] = '\0';
    char search_key[32];
    snprintf(search_key, sizeof(search_key), "\"%s\":", key);
    const char* p = strstr(json, search_key);
    if (!p) return;
    p += strlen(search_key);
    
    // 跳过空格
    while (*p == ' ' || *p == '\t') p++;
    
    if (*p == '"') {
        // 字符串值
        p++;
        size_t i = 0;
        while (*p && *p != '"' && i < out_len - 1) {
            if (*p == '\\' && *(p+1)) {
                p++; // 跳过转义符
            }
            out[i++] = *p++;
        }
        out[i] = '\0';
    } else {
        // 数字或布尔值
        size_t i = 0;
        while (*p && (*p != ',' && *p != '}' && *p != ' ') && i < out_len - 1) {
            out[i++] = *p++;
        }
        out[i] = '\0';
    }
}

static float parse_json_float(const char* json, const char* key) {
    char val[32];
    parse_json_field(json, key, val, sizeof(val));
    return atof(val);
}

static bool parse_json_bool(const char* json, const char* key) {
    char val[16];
    parse_json_field(json, key, val, sizeof(val));
    return (strcmp(val, "true") == 0);
}

// ==================== 公共接口 ====================
void ipgeo_init() {
    g_state = GEO_IDLE;
    g_cached.valid = false;
    g_cached.updated_at = 0;
    g_response_buffer.reserve(2048);
    
    // 从 NVS 加载 API URL 配置（如有则覆盖默认值）
    storage_get_ipgeo_config(g_api_host, sizeof(g_api_host),
                              &g_api_port, g_api_path, sizeof(g_api_path));
    
    Serial.printf("[IPGEO] 初始化完成, API: %s:%d%s\n", g_api_host, g_api_port, g_api_path);
}

void ipgeo_request_update() {
    // 检查缓存是否仍有效
    if (g_cached.valid && (millis() - g_cached.updated_at) < IPGEO_CACHE_MS) {
        Serial.println("[IPGEO] 缓存仍有效，跳过请求");
        return;
    }
    
    // 检查重试间隔
    if (g_state == GEO_FAILED && (millis() - g_last_attempt) < IPGEO_RETRY_MS) {
        return;
    }
    
    if (g_state == GEO_REQUESTING) {
        return; // 已有请求在进行中
    }
    
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("[IPGEO] WiFi 未连接，无法定位");
        return;
    }
    
    // 创建 HTTPS 客户端
    if (g_client) {
        delete g_client;
    }
    g_client = new WiFiClientSecure();
    g_client->setInsecure(); // 简化证书验证（生产环境建议用证书）
    
    if (!g_client->connect(g_api_host, g_api_port)) {
        Serial.println("[IPGEO] 连接 API 失败");
        g_state = GEO_FAILED;
        g_last_attempt = millis();
        delete g_client;
        g_client = nullptr;
        return;
    }
    
    // 发送 HTTP GET 请求
    String request = String("GET ") + g_api_path + " HTTP/1.1\r\n" +
                     "Host: " + g_api_host + "\r\n" +
                     "User-Agent: CS_INV_ESP32/1.0\r\n" +
                     "Accept: application/json\r\n" +
                     "Connection: close\r\n\r\n";
    
    g_client->print(request);
    g_response_buffer = "";
    g_request_start = millis();
    g_state = GEO_REQUESTING;
    
    Serial.println("[IPGEO] 正在请求定位...");
}

bool ipgeo_check_result(GeoLocation* out) {
    if (g_state != GEO_REQUESTING) {
        if (g_cached.valid && out) {
            memcpy(out, &g_cached, sizeof(GeoLocation));
        }
        return g_cached.valid && (millis() - g_cached.updated_at) < 5000; // 5秒内算"新"
    }
    
    if (!g_client || !g_client->connected()) {
        // 连接已关闭，处理响应
        if (g_response_buffer.length() == 0) {
            Serial.println("[IPGEO] 无响应数据");
            g_state = GEO_FAILED;
            g_last_attempt = millis();
            delete g_client;
            g_client = nullptr;
            return false;
        }
        
        // 查找 JSON 开始位置
        int json_start = g_response_buffer.indexOf("{");
        if (json_start < 0) {
            Serial.println("[IPGEO] 响应中无 JSON");
            g_state = GEO_FAILED;
            g_last_attempt = millis();
            delete g_client;
            g_client = nullptr;
            return false;
        }
        
        String json = g_response_buffer.substring(json_start);
        const char* json_cstr = json.c_str();
        
        // 检查 API 错误
        char error_msg[128];
        parse_json_field(json_cstr, "error", error_msg, sizeof(error_msg));
        if (strlen(error_msg) > 0) {
            Serial.printf("[IPGEO] API 错误: %s\n", error_msg);
            g_state = GEO_FAILED;
            g_last_attempt = millis();
            delete g_client;
            g_client = nullptr;
            return false;
        }
        
        // 解析数据（仅 IP + 城市）
        parse_json_field(json_cstr, "ip", g_cached.ip, sizeof(g_cached.ip));
        parse_json_field(json_cstr, "city", g_cached.city, sizeof(g_cached.city));
        g_cached.valid = true;
        g_cached.updated_at = millis();
        g_state = GEO_SUCCESS;
        
        Serial.printf("[IPGEO] 定位成功: %s -> %s\n", g_cached.ip, g_cached.city);
        
        delete g_client;
        g_client = nullptr;
        
        if (out) {
            memcpy(out, &g_cached, sizeof(GeoLocation));
        }
        return true;
    }
    
    // 读取可用数据
    while (g_client->available()) {
        char c = g_client->read();
        g_response_buffer += c;
    }
    
    // 检查超时
    if (millis() - g_request_start > IPGEO_TIMEOUT_MS) {
        Serial.println("[IPGEO] 请求超时");
        g_state = GEO_FAILED;
        g_last_attempt = millis();
        delete g_client;
        g_client = nullptr;
        return false;
    }
    
    return false; // 仍在等待
}

bool ipgeo_get_cached(GeoLocation* out) {
    if (!out) return g_cached.valid;
    if (g_cached.valid) {
        memcpy(out, &g_cached, sizeof(GeoLocation));
    }
    return g_cached.valid;
}

void ipgeo_to_json(const GeoLocation* geo, char* out, size_t out_len) {
    if (!geo || !geo->valid) {
        snprintf(out, out_len, "null");
        return;
    }
    
    snprintf(out, out_len, "{\"ip\":\"%s\",\"city\":\"%s\"}",
             geo->ip, geo->city);
}

const char* ipgeo_status_string() {
    switch (g_state) {
        case GEO_IDLE: return g_cached.valid ? "已缓存" : "未定位";
        case GEO_REQUESTING: return "定位中";
        case GEO_SUCCESS: return "定位成功";
        case GEO_FAILED: return "定位失败";
        default: return "未知";
    }
}

// ==================== API URL 配置 ====================
void ipgeo_get_config(char* host, size_t host_len,
                       uint16_t* port, char* path, size_t path_len) {
    strncpy(host, g_api_host, host_len - 1);
    host[host_len - 1] = '\0';
    *port = g_api_port;
    strncpy(path, g_api_path, path_len - 1);
    path[path_len - 1] = '\0';
}

bool ipgeo_set_config(const char* host, uint16_t port, const char* path) {
    if (!host || strlen(host) == 0) return false;
    if (!path || strlen(path) == 0) return false;
    
    // 持久化到 NVS
    if (!storage_set_ipgeo_config(host, port, path)) {
        return false;
    }
    
    // 更新运行时变量
    strncpy(g_api_host, host, sizeof(g_api_host) - 1);
    g_api_host[sizeof(g_api_host) - 1] = '\0';
    g_api_port = port;
    strncpy(g_api_path, path, sizeof(g_api_path) - 1);
    g_api_path[sizeof(g_api_path) - 1] = '\0';
    
    // 使缓存失效，下次自动使用新 URL
    g_cached.valid = false;
    
    Serial.printf("[IPGEO] API URL 已更新: %s:%d%s\n", g_api_host, g_api_port, g_api_path);
    return true;
}

void ipgeo_reset_config() {
    storage_clear_ipgeo_config();
    strncpy(g_api_host, IPGEO_DEFAULT_HOST, sizeof(g_api_host) - 1);
    g_api_port = IPGEO_DEFAULT_PORT;
    strncpy(g_api_path, IPGEO_DEFAULT_PATH, sizeof(g_api_path) - 1);
    g_cached.valid = false;
    Serial.println("[IPGEO] API URL 已恢复默认");
}
