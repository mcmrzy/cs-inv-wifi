/**
 * SN Encoder 实现 — 辰烁产品序列号编码模块
 * 遵循文档: CS-SN-STD-001 V1.1
 */

#include "sn_encoder.h"
#include <string.h>
#include <ctype.h>
#include <stdio.h>
#include "esp_log.h"

static const char *SN_TAG = "sn_encoder";

// ==================== 年份编码表 ====================
// 36 进制, 跳过 O(14) 和 I(8)
// 索引 0='1', 1='2', ... 8='9', 9='A', ... 32='Z'
static const char YEAR_CODES[] = "123456789ABCDEFGHJKLMNPQRSTUVWXYZ";
static const int  YEAR_COUNT = sizeof(YEAR_CODES) - 1;  // 33

// ==================== 客户等级集合 ====================
static const char VALID_GRADES[] = "ABCDEPX";
static const int  VALID_GRADES_COUNT = sizeof(VALID_GRADES) - 1;  // 7

// ==================== 常用 ISO 3166-1 alpha-2 国家代码 ====================
// 用于快速验证 (完整 ISO 列表过长, 此处收录高频市场)
static const char* const VALID_COUNTRIES[] = {
    "CN", "DE", "NL", "AU", "BR", "JP", "US", "ZA",  // 文档高频示例
    "GB", "FR", "IT", "ES", "PT", "BE", "AT", "CH",  // 欧洲
    "SE", "NO", "DK", "FI", "PL", "CZ", "HU", "RO",  // 欧洲
    "GR", "IE", "BG", "HR", "SK", "SI", "LT", "LV",  // 欧洲
    "EE", "LU", "MT", "CY",                           // 欧洲
    "KR", "IN", "TH", "VN", "MY", "SG", "PH", "ID",  // 亚太
    "PK", "BD", "LK", "TW", "HK", "MO",               // 亚太
    "MX", "CL", "CO", "AR", "PE",                     // 拉美
    "EG", "NG", "KE", "MA", "TN", "GH", "ET",         // 非洲
    "AE", "SA", "QA", "KW", "OM", "IL", "TR",         // 中东
    "CA", "NZ", "RU", "UA", "KZ", "UZ",               // 其他
    "99", "ZZ",                                        // 特殊码
};
static const int VALID_COUNTRIES_COUNT = sizeof(VALID_COUNTRIES) / sizeof(VALID_COUNTRIES[0]);

// ==================== 字符值映射 (用于校验和计算) ====================
// '0'-'9' → 0-9, 'A'-'Z' → 10-35
static uint8_t char_to_value(char c) {
    if (c >= '0' && c <= '9') return (uint8_t)(c - '0');
    if (c >= 'A' && c <= 'Z') return (uint8_t)(c - 'A' + 10);
    if (c >= 'a' && c <= 'z') return (uint8_t)(c - 'a' + 10);
    return 0;
}

static char value_to_char(uint8_t v) {
    if (v < 10) return (char)('0' + v);
    return (char)('A' + (v - 10));
}

// ==================== 校验位计算 (CRC-16 MODBUS) ====================
/**
 * 校验位算法 (CRC-16 MODBUS):
 *   参数: Poly=0x8005, Init=0xFFFF, RefIn=true, RefOut=true
 *   步骤:
 *     1. 对前 15 位字符串的 ASCII 字节做 CRC-16 MODBUS 运算
 *     2. 取低 8 位 (crc & 0xFF)
 *     3. 对 36 取模 (lo % 36)
 *     4. 映射为 36 进制字符 (0-9→'0'-'9', 10-35→'A'-'Z')
 */
static uint16_t crc16_modbus(const uint8_t* data, size_t len) {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++) {
            if (crc & 0x0001) {
                crc = (crc >> 1) ^ 0xA001;  // 0x8005 反转后为 0xA001
            } else {
                crc >>= 1;
            }
        }
    }
    return crc;
}

char sn_compute_check_digit(const char* prefix_15) {
    if (!prefix_15) return '?';

    for (int i = 0; i < SN_PREFIX_LEN; i++) {
        if (prefix_15[i] == '\0') return '?';
    }

    uint16_t crc = crc16_modbus((const uint8_t*)prefix_15, SN_PREFIX_LEN);
    uint8_t lo = crc & 0xFF;  // 取低 8 位
    uint8_t mod = lo % 36;
    return value_to_char(mod);
}

// ==================== SN 格式验证 ====================
bool sn_validate_format(const char* sn) {
    if (!sn) return false;
    
    size_t len = strlen(sn);
    if (len != SN_FULL_LEN) return false;
    
    // 1. 验证全部字符为 ASCII 字母数字
    for (int i = 0; i < SN_FULL_LEN; i++) {
        if (!isalnum((unsigned char)sn[i])) return false;
    }
    
    // 2. 转大写副本用于后续校验
    char buf[SN_FULL_LEN + 1];
    for (int i = 0; i < SN_FULL_LEN; i++) {
        buf[i] = (char)toupper((unsigned char)sn[i]);
    }
    buf[SN_FULL_LEN] = '\0';
    
    // 3. 厂商码 (1-2位): 第1位必须是字母, 第2位是字母或数字
    if (!isalpha((unsigned char)buf[0])) return false;
    if (!isalnum((unsigned char)buf[1])) return false;
    
    // 4. 国家码 (3-4位): 两位大写字母或特殊码
    char cc[3] = { buf[2], buf[3], '\0' };
    if (!sn_is_valid_country(cc)) return false;
    
    // 5. 客户等级 (5位): 必须是有效等级代码
    if (!sn_is_valid_customer_grade(buf[4])) return false;
    
    // 6. 客户流水 (6-8位): 三位数字
    for (int i = 5; i <= 7; i++) {
        if (!isdigit((unsigned char)buf[i])) return false;
    }
    
    // 7. 年份码 (9位)
    if (!sn_is_valid_year_code(buf[8])) return false;
    
    // 8. 月份码 (10位)
    if (!sn_is_valid_month_code(buf[9])) return false;
    
    // 9. 生产序列 (11-15位): 五位数字
    for (int i = 10; i <= 14; i++) {
        if (!isdigit((unsigned char)buf[i])) return false;
    }
    
    // 10. 校验位 (16位): 字母或数字
    if (!isalnum((unsigned char)buf[15])) return false;
    
    return true;
}

// ==================== 完整 SN 验证 (格式 + 校验位) ====================
bool sn_validate(const char* sn) {
    if (!sn_validate_format(sn)) return false;
    
    // 转大写后验证校验位
    char prefix[SN_PREFIX_LEN + 1];
    for (int i = 0; i < SN_PREFIX_LEN; i++) {
        prefix[i] = (char)toupper((unsigned char)sn[i]);
    }
    prefix[SN_PREFIX_LEN] = '\0';
    
    char expected = sn_compute_check_digit(prefix);
    char actual = (char)toupper((unsigned char)sn[SN_POS_CHECK]);
    
    return (expected == actual);
}

// ==================== SN 解析 ====================
bool sn_parse(const char* sn, SNFields* out) {
    if (!sn || !out) return false;
    if (!sn_validate_format(sn)) return false;
    
    memset(out, 0, sizeof(SNFields));
    
    // 统一转大写
    char buf[SN_FULL_LEN + 1];
    for (int i = 0; i < SN_FULL_LEN; i++) {
        buf[i] = (char)toupper((unsigned char)sn[i]);
    }
    buf[SN_FULL_LEN] = '\0';
    
    memcpy(out->manufacturer, buf + SN_POS_MFR, 2);
    out->manufacturer[2] = '\0';
    
    memcpy(out->country, buf + SN_POS_COUNTRY, 2);
    out->country[2] = '\0';
    
    memcpy(out->customer, buf + SN_POS_CUSTOMER, 4);
    out->customer[4] = '\0';
    
    memcpy(out->date_code, buf + SN_POS_DATE, 2);
    out->date_code[2] = '\0';
    
    memcpy(out->sequence, buf + SN_POS_SEQ, 5);
    out->sequence[5] = '\0';
    
    out->check_digit = buf[SN_POS_CHECK];
    
    return true;
}

// ==================== SN 生成 ====================
bool sn_generate(const SNFields* fields, char* out, size_t out_len) {
    if (!fields || !out || out_len < SN_FULL_LEN + 1) {
        ESP_LOGE(SN_TAG, "sn_generate: invalid params (out_len=%u)", (unsigned)out_len);
        return false;
    }
    
    // 构建前 15 位
    char prefix[SN_PREFIX_LEN + 1];
    
    // 厂商码 (2位)
    if (strlen(fields->manufacturer) < 2) {
        ESP_LOGE(SN_TAG, "sn_generate: manufacturer too short");
        return false;
    }
    prefix[0] = (char)toupper((unsigned char)fields->manufacturer[0]);
    prefix[1] = (char)toupper((unsigned char)fields->manufacturer[1]);
    
    // 国家码 (2位)
    if (strlen(fields->country) < 2) {
        ESP_LOGE(SN_TAG, "sn_generate: country too short");
        return false;
    }
    prefix[2] = (char)toupper((unsigned char)fields->country[0]);
    prefix[3] = (char)toupper((unsigned char)fields->country[1]);
    
    // 客户码 (4位)
    if (strlen(fields->customer) < 4) {
        ESP_LOGE(SN_TAG, "sn_generate: customer too short");
        return false;
    }
    prefix[4] = (char)toupper((unsigned char)fields->customer[0]);
    for (int i = 5; i <= 7; i++) {
        prefix[i] = fields->customer[i - 4];
    }
    
    // 日期码 (2位)
    if (strlen(fields->date_code) < 2) {
        ESP_LOGE(SN_TAG, "sn_generate: date_code too short");
        return false;
    }
    prefix[8] = (char)toupper((unsigned char)fields->date_code[0]);
    prefix[9] = (char)toupper((unsigned char)fields->date_code[1]);
    
    // 序列号 (5位)
    if (strlen(fields->sequence) < 5) {
        ESP_LOGE(SN_TAG, "sn_generate: sequence too short");
        return false;
    }
    for (int i = 10; i <= 14; i++) {
        prefix[i] = fields->sequence[i - 10];
    }
    
    prefix[SN_PREFIX_LEN] = '\0';
    ESP_LOGD(SN_TAG, "sn_generate: built prefix='%s'", prefix);
    
    // 校验前 15 位格式
    if (!sn_validate_format(prefix)) {
        ESP_LOGE(SN_TAG, "sn_generate: format validation failed for prefix='%s'", prefix);
        return false;
    }
    
    // 计算校验位
    char check = sn_compute_check_digit(prefix);
    
    // 组装完整 SN
    memcpy(out, prefix, SN_PREFIX_LEN);
    out[SN_POS_CHECK] = check;
    out[SN_FULL_LEN] = '\0';
    
    return true;
}

// ==================== 格式化 ====================
void sn_printable(const char* sn, char* out, size_t out_len) {
    if (!sn || !out || out_len < 1) {
        if (out && out_len > 0) out[0] = '\0';
        return;
    }
    
    // 直接返回原始 SN 字符串
    strncpy(out, sn, out_len - 1);
    out[out_len - 1] = '\0';
}

void sn_short_id(const char* sn, char* out, size_t out_len) {
    if (!sn || !out || out_len < 7) {
        if (out && out_len > 0) out[0] = '\0';
        return;
    }
    
    size_t slen = strlen(sn);
    
    // MAC 格式 SN: 取 MAC 部分的末 6 位
    if (slen >= 6 && strncmp(sn, "MAC", 3) == 0) {
        const char* src = sn + 3;
        size_t mac_len = slen - 3;
        const char* last6 = (mac_len > 6) ? src + mac_len - 6 : src;
        strncpy(out, last6, 6);
        out[6] = '\0';
        return;
    }
    
    // 标准 SN: 取末 6 位
    if (slen >= 6) {
        strncpy(out, sn + slen - 6, 6);
        out[6] = '\0';
    } else {
        strncpy(out, sn, out_len - 1);
        out[out_len - 1] = '\0';
    }
}

// ==================== 字段校验辅助函数 ====================

bool sn_is_valid_country(const char* cc) {
    if (!cc || strlen(cc) < 2) return false;
    
    char upper[3];
    upper[0] = (char)toupper((unsigned char)cc[0]);
    upper[1] = (char)toupper((unsigned char)cc[1]);
    upper[2] = '\0';
    
    // 数字国家码 (99 = 未指定)
    if (isdigit((unsigned char)upper[0]) && isdigit((unsigned char)upper[1])) {
        return true;
    }
    
    // 必须在已知列表中
    for (int i = 0; i < VALID_COUNTRIES_COUNT; i++) {
        if (strcmp(upper, VALID_COUNTRIES[i]) == 0) return true;
    }
    
    // 宽限: 只要是两位大写字母且全为字母就通过
    // (ISO 3166-1 完整列表很长, 这里放宽但标记为有效)
    if (isalpha((unsigned char)upper[0]) && isalpha((unsigned char)upper[1])) {
        return true;
    }
    
    return false;
}

bool sn_is_valid_customer_grade(char g) {
    g = (char)toupper((unsigned char)g);
    for (int i = 0; i < VALID_GRADES_COUNT; i++) {
        if (VALID_GRADES[i] == g) return true;
    }
    // 也允许数字 (兼容扩展)
    return (g >= '0' && g <= '9');
}

bool sn_is_valid_year_code(char c) {
    c = (char)toupper((unsigned char)c);
    for (int i = 0; i < YEAR_COUNT; i++) {
        if (YEAR_CODES[i] == c) return true;
    }
    return false;
}

bool sn_is_valid_month_code(char c) {
    c = (char)toupper((unsigned char)c);
    if (c >= '1' && c <= '9') return true;
    if (c == 'A' || c == 'B' || c == 'C') return true;
    return false;
}

int sn_year_code_decode(char c) {
    c = (char)toupper((unsigned char)c);
    for (int i = 0; i < YEAR_COUNT; i++) {
        if (YEAR_CODES[i] == c) return SN_YEAR_BASE + i;
    }
    return -1;
}

char sn_year_code_encode(int year) {
    int offset = year - SN_YEAR_BASE;
    if (offset < 0) offset = 0;
    offset = offset % YEAR_COUNT;
    return YEAR_CODES[offset];
}

char sn_month_code_encode(int month) {
    if (month >= 1 && month <= 9) return (char)('0' + month);
    if (month == 10) return 'A';
    if (month == 11) return 'B';
    if (month == 12) return 'C';
    return '?';
}
