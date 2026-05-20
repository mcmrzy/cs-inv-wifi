/**
 * SN Encoder — 辰烁产品序列号编码模块
 * ==========================================
 * 遵循文档: CS-SN-STD-001 V1.1
 * 适用: 光伏储能逆变器及储能系统全产品线
 *
 * 编码格式 (16 位): MM-CC-PPPP-YW-SSSSS-C
 *   1-2  : 生产厂商 (字母+字母/数字)
 *   3-4  : 销售国家/地区 (ISO 3166-1 alpha-2)
 *   5-8  : 客户编码 (1位等级+3位流水)
 *   9-10 : 生产日期 (1位年码+1位月码)
 *   11-15: 生产序列 (5位数字)
 *   16   : 校验码 (字母/数字)
 */

#ifndef SN_ENCODER_H
#define SN_ENCODER_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// ==================== SN 尺寸常量 ====================
#define SN_FULL_LEN      16    // 标准 SN 长度 (不含 null)
#define SN_PREFIX_LEN    15    // 前 15 位 (校验位之前)
#define SN_BUF_LEN       25    // 缓冲区长度 (含扩展空间 + null)

// ==================== SN 字段位置 ====================
#define SN_POS_MFR       0     // 生产厂商: 1-2 位, 2 chars
#define SN_POS_COUNTRY   2     // 国家/地区: 3-4 位, 2 chars
#define SN_POS_CUSTOMER  4     // 客户编码: 5-8 位, 4 chars
#define SN_POS_DATE      8     // 生产日期: 9-10 位, 2 chars
#define SN_POS_SEQ       10    // 生产序列: 11-15 位, 5 chars
#define SN_POS_CHECK     15    // 校验码:   16 位, 1 char

// ==================== SN 字段结构体 ====================
typedef struct {
    char manufacturer[3];   // 2 chars + null, 如 "H1"
    char country[3];        // 2 chars + null, 如 "DE"
    char customer[5];       // 4 chars + null, 如 "A001"
    char date_code[3];      // 2 chars + null, 如 "2A"
    char sequence[6];       // 5 chars + null, 如 "00088"
    char check_digit;       // 1 char,        如 'V'
} SNFields;

// ==================== 客户等级代码 ====================
#define SN_CUST_KEY_ACCOUNT    'A'   // 海外战略大客户 (Key Account)
#define SN_CUST_DISTRIBUTOR    'B'   // 区域核心分销商 (Distributor)
#define SN_CUST_INSTALLER      'C'   // 安装商/一般经销商 (Installer/Dealer)
#define SN_CUST_RETAIL         'X'   // 零售/无固定合同客户 (Retail)
#define SN_CUST_PROJECT        'P'   // 项目代码 (Project)
#define SN_CUST_AUTH_INSTALLER 'D'   // 授权安装商/EPC
#define SN_CUST_REGIONAL       'E'   // 区域二级代理

// ==================== 特殊国家代码 ====================
#define SN_COUNTRY_UNSPECIFIED "99"  // 未指定销售区域 (借机/测试用机)
#define SN_COUNTRY_OTHER       "ZZ"  // 其他未列明地区

// ==================== 年份基准 ====================
#define SN_YEAR_BASE 2024             // 年份编码基准年

// ==================== 核心 API ====================

/**
 * 验证完整 SN (格式 + 校验位)
 * @param sn  16 位序列号字符串 (不含 null)
 * @return true 表示合法, false 表示不合法
 */
bool sn_validate(const char* sn);

/**
 * 仅验证 SN 格式 (不验证校验位)
 * @param sn  16 位序列号字符串
 * @return true 表示格式正确
 */
bool sn_validate_format(const char* sn);

/**
 * 解析 SN 各字段
 * @param sn   16 位序列号
 * @param out  输出字段结构体 (不可为 null)
 * @return true 表示解析成功
 */
bool sn_parse(const char* sn, SNFields* out);

/**
 * 计算前 15 位的校验码
 * @param prefix_15  前 15 位字符串
 * @return 校验字符 (0-9 或 A-Z)
 */
char sn_compute_check_digit(const char* prefix_15);

/**
 * 由字段生成完整 SN
 * @param fields   各字段值
 * @param out      输出缓冲区
 * @param out_len  缓冲区大小
 * @return true 表示生成成功
 */
bool sn_generate(const SNFields* fields, char* out, size_t out_len);

/**
 * 格式化 SN 为人眼可读形式 (用空格分段)
 * 例: "H1DEA0012A00088V" → "H1DE A001 2A 00088 V"
 * @param sn       16 位 SN
 * @param out      输出缓冲区
 * @param out_len  缓冲区大小
 */
void sn_printable(const char* sn, char* out, size_t out_len);

/**
 * 获取 SN 的简短标识 (末 6 位, 用于 AP SSID 等场景)
 * 对 MAC 格式 SN, 提取 MAC 部分末 6 位
 * @param sn       原始 SN
 * @param out      输出缓冲区
 * @param out_len  缓冲区大小
 */
void sn_short_id(const char* sn, char* out, size_t out_len);

// ==================== 字段校验辅助函数 ====================

/** 验证两位国家代码是否为合法 ISO 3166-1 alpha-2 (含特殊码 99, ZZ) */
bool sn_is_valid_country(const char* cc);

/** 验证客户等级代码 */
bool sn_is_valid_customer_grade(char g);

/** 验证年份代码 (1-9, A-Z 跳过 I/O) */
bool sn_is_valid_year_code(char c);

/** 验证月份代码 (1-9, A-C) */
bool sn_is_valid_month_code(char c);

/** 解码年份码 → 公历年份 (如 '2' → 2025) ; 失败返回 -1 */
int  sn_year_code_decode(char c);

/** 公历年份 → 年份码 (如 2025 → '2') */
char sn_year_code_encode(int year);

/** 月份 → 月份码 (1→'1', 10→'A', 11→'B', 12→'C') */
char sn_month_code_encode(int month);

#endif // SN_ENCODER_H
