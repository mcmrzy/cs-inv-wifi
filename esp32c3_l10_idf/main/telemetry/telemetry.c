/*
 * telemetry.c — 遥测上报 (运行参数 → MQTT)
 *
 * V2.1 协议 (CS-L10-6K2_MQTT_上报协议设计_V2.1.md):
 *  - heartbeat: 57 值 {sys[11], pv[5], ac[11], chr[3], bat[5], eng[14], fan[2], diag[3], sock[3]}
 *    默认180s + 启动立即 + query_telemetry 补报 + 调试模式5s; 实时快照未连接时直接丢弃
 *  - info: 设备能力与固件信息 (连接时/信息变化/query_info)
 *  - config: v2 语义键值对 (连接时/写入生效后/query_config, 42 键工程单位 + rev)
 *  - alarm: {source, code, level, state} 事件闭环 (QoS1)
 *  - status/LWT 由 mqtt_handler 负责, 本模块不再发布 data/status
 *
 * 数据源: arm_poll 全局 g_run_param / g_only_read / g_ctrl_param / g_arm_stat (mutex 保护)
 * 信封:   mqtt_publish() 自动拼接 cs_inv/{sn}/ 前缀 + {"t","v":2,"data":...}
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <time.h>
#include "arm_param.h"
#include "arm_poll.h"
#include "telemetry.h"
#ifdef HOST_TEST
/* 宿主单元测试桩 (build-host 临时目录, 测完即删): 屏蔽 ESP-IDF 依赖, 仅编译
 * heartbeat JSON 构造 / 告警判断纯逻辑。固件构建不定义 HOST_TEST, 以下代码
 * 不参与编译, 目标码行为不变; mqtt/arm_poll/BLE 外部符号由宿主测试程序提供桩。 */
#define ESP_LOGI(...)  do { } while (0)
#define ESP_LOGW(...)  do { } while (0)
#define ESP_LOGE(...)  do { } while (0)
#define SN_MAX_LEN          32
#define TELEMETRY_PERIOD_MS 180000U
#define ONLINE_PERIOD_MS    90000U
#define ARM_FAIL_THRESHOLD  3
#define PROTO_TOPIC_HEARTBEAT "heartbeat"
#define PROTO_TOPIC_INFO      "info"
#define PROTO_TOPIC_ALARM     "alarm"
#define PROTO_TOPIC_CONFIG    "config"
#ifndef FIRMWARE_VERSION
#define FIRMWARE_VERSION "V0.0.0.host"
#endif
extern char     g_device_sn[SN_MAX_LEN];
extern int      mqtt_publish(const char *sub_topic, const char *data, int len,
                             int qos, int retain);
extern bool     mqtt_is_connected(void);
extern int64_t  esp_timer_get_time(void);
extern uint32_t ble_ct_now(void);
extern void     ble_ct_telemetry_notify(const char *json, uint16_t len);
#else
#include "esp_log.h"
#include "esp_timer.h"
#include "config.h"
#include "cs_protocol_defs.h"
#include "mqtt_handler.h"
#include "ble_ct_telemetry.h"
#include "ble_ct_clock.h"
#endif

static const char *TAG = "TELE";

/* 主题名 (不带前缀, 前缀 cs_inv/{sn}/ + 信封由 mqtt_publish 拼接) */
#define TOPIC_HEARTBEAT  PROTO_TOPIC_HEARTBEAT
#define TOPIC_INFO       PROTO_TOPIC_INFO
#define TOPIC_ALARM      PROTO_TOPIC_ALARM
#define TOPIC_CONFIG     PROTO_TOPIC_CONFIG

/* ============ heartbeat 量纲策略 (按现场 ARM 固件版本自动选择) ============
 * 现场同一型号多批 ARM 固件混装: 旧固件按厂家原始换算输出 (ESP 侧需补偿), 厂家
 * 修正后的新固件直接输出契约量纲 (直传)。模式完全由 ARM 只读区 0x190 的
 * ARMFirmwareVersion (u16: 高字节主版本 / 低字节次版本, 见 fmt_ver) 决定,
 * 无需为不同批次分别出固件。 */

/* 厂家 ARM 从该版本起输出契约量纲 (0xFFFF = 尚未发布 → 永远走补偿)。
   *** 厂家修好后只需把下面改成实际版本号 (如 0x0102 表示 V1.2), 其余代码不动 *** */
#define ARM_FW_CONTRACT_UNITS_FROM  0xFFFFu

/* 厂家实现未赋值字段 (InvCurr/PvTemp/TransformerTemp/并机 socket) 后置 1,
   相应槽位改为发布实际值; 与上面的量纲修正相互独立 (0 = 继续发布 null) */
#define ARM_UNSUPPORTED_FIELDS_IMPLEMENTED  0

/* 量纲模式 */
typedef enum {
    HB_UNITS_COMPENSATE = 0,   /* 旧 ARM: ESP 侧补偿换算 (当前默认) */
    HB_UNITS_CONTRACT   = 1,   /* 新 ARM: 契约量纲直传 */
} hb_units_t;

/* ARM 固件是否已输出契约量纲: 版本 >= 阈值 → true; 版本读不到 (0) → false
 * (保持补偿, 安全默认: 版本未知时宁可多补一次, 也不把已放大的值直传云端) */
static bool arm_units_are_contract(void)
{
    uint16_t ver = g_only_read.ARMFirmwareVersion;
    if (ver == 0) {
        return false;
    }
    return ver >= ARM_FW_CONTRACT_UNITS_FROM;
}

static uint32_t s_now_ms;
static uint32_t s_last_hb_ms;        /* V2 heartbeat 上次发布时刻 (30s) */
static uint32_t s_last_online_ms;    /* 在线状态上次发布时刻 (90s) */
static uint32_t s_last_info_ms;      /* 上次 info 发布时刻 (arm 时间戳) */
static uint32_t s_last_config_ms;    /* 上次 config 发布时刻 (arm 时间戳) */
static volatile bool s_hb_requested; /* query_telemetry 请求补报 */
static volatile bool s_config_requested; /* query_config/写入生效 请求补报 */
static bool     s_alarm_armed;       /* 首次收到运行参数后开始检测 */
static uint64_t s_last_warning;
static uint32_t s_last_fault;
static uint16_t s_last_bms;
static uint32_t s_arm_fail_count;    /* ARM 连续无应答计数 (心跳周期) */
static bool     s_fault_reported;    /* 故障已上报, 恢复前不重复触发 */
static int      s_units_logged = -1; /* 已打印过的量纲模式 (-1=未打印过一次) */

/* ---- 单设备调试模式 (云端 set_debug_telemetry) ----
 * 临时切换 heartbeat 周期; 到期/关闭/重启自动恢复默认 TELEMETRY_PERIOD_MS。
 * deadline 与 now_ms() 同源 (单调时钟), 不受 NTP 回拨影响; 不持久化。 */
static volatile bool     s_debug_active;
static volatile uint32_t s_debug_interval_ms;
static volatile uint32_t s_debug_deadline_ms;   /* 单调 ms, 0=未启用 */

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

/* ---- 版本号格式化: u16 0x0102 → "V1.2" ---- */
static void fmt_ver(char *buf, size_t len, uint16_t ver)
{
    snprintf(buf, len, "V%u.%u",
             (unsigned)((ver >> 8) & 0xFF), (unsigned)(ver & 0xFF));
}

/* ---- 在线状态上报 (90s, 无需ARM应答) ---- */
static void publish_online(void)
{
    if (!mqtt_is_connected()) return;
    char buf[128];
    int n = snprintf(buf, sizeof(buf),
        "{\"uptime\":%lu}", (unsigned long)(s_now_ms / 1000));
    if (n > 0 && n < (int)sizeof(buf)) {
        mqtt_publish("online", buf, 0, 0, 0);
    }
}

/* ---- heartbeat 增量拼接助手 (逐段追加, 不额外占用任务栈) ----
 * vsnprintf 返回"本应写入"的字节数: 截断时 *pos 会越过 cap-1, 由调用方在拼接
 * 完成后统一判 *pos >= cap 丢弃整帧 (不发布半截 JSON)。每次调用带溢出保护,
 * 已溢出后跳过追加; 对合法值的输出与旧版一把大 snprintf 逐字节一致。 */
static void hb_append(char *buf, size_t cap, size_t *pos, const char *fmt, ...)
{
    if (*pos >= cap) {
        return;                          /* 已溢出: 不再追加, 末尾统一丢弃 */
    }
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf + *pos, cap - *pos, fmt, ap);
    va_end(ap);
    if (n > 0) {
        *pos += (size_t)n;
    }
}

/* heartbeat 数值拼接 + 量程护栏: 界限与服务器 heartbeat_v2.go 的 bounded() 一一
 * 对应 (固件 raw 值 = 服务器工程单位上限 × 缩放, 如 ACOutputVolt 2500 raw =
 * 250V@0.1V), 越界发布字面量 null (服务器落 NULL), 合法值原样发布、数值不变。 */
static void hb_num(char *buf, size_t cap, size_t *pos,
                   unsigned long v, unsigned long vmax)
{
    if (v <= vmax) {
        hb_append(buf, cap, pos, ",%lu", v);
    } else {
        hb_append(buf, cap, pos, ",null");
    }
}

/* 组内首个数值 (紧跟 opener 的 '[', 无前导逗号), 量程规则同 hb_num */
static void hb_num1(char *buf, size_t cap, size_t *pos,
                    unsigned long v, unsigned long vmax)
{
    if (v <= vmax) {
        hb_append(buf, cap, pos, "%lu", v);
    } else {
        hb_append(buf, cap, pos, "null");
    }
}

/* 带符号版本 (bat_curr: 服务器界 -150..150A@0.1A → raw -1500..1500) */
static void hb_num_i(char *buf, size_t cap, size_t *pos,
                     int32_t v, int32_t vmin, int32_t vmax)
{
    if (v >= vmin && v <= vmax) {
        hb_append(buf, cap, pos, ",%" PRId32, v);
    } else {
        hb_append(buf, cap, pos, ",null");
    }
}

/* ×10 缩放助手 (1V/1W/1VA/1% → 契约 0.1V/0.1W/0.1VA/0.1%): u32 中间量防 u16
 * 溢出; u32 源超过 UINT32_MAX/10 时 ×10 会回绕, 钳为 UINT32_MAX 交给量程护栏
 * 判 null */
static uint32_t hb_scale10(uint32_t v)
{
    return (v > UINT32_MAX / 10) ? UINT32_MAX : v * 10;
}

/* ---- 量纲变换 (补偿模式换算 / 契约模式直传) ----
 * 契约模式下 ARM 源值即契约量纲, 原样返回; 补偿模式沿用旧换算。
 * 量程护栏不在这里, 由 hb_num/hb_num1/hb_num_i 按契约界限判定 (两模式一致)。 */
static uint32_t hb_units_x10(hb_units_t m, uint32_t v)
{
    return (m == HB_UNITS_CONTRACT) ? v : hb_scale10(v);
}

static uint32_t hb_units_div(hb_units_t m, uint32_t v, uint32_t div)
{
    return (m == HB_UNITS_CONTRACT) ? v : v / div;
}

/* ---- heartbeat 策略选择 (版本自适应, 见上方"量纲策略"与函数头 ⚠ 清单) ----
 * 每次发布重新判定 (现场换 ARM / ARM 升级后无需重启 ESP); 策略首次生效或发生
 * 变化时打印一次, 便于现场/云端日志判断该设备走的是哪套换算。 */
static void hb_build(const RunParamDef *p, hb_units_t units);   /* 实现见下 */

static void publish_heartbeat(const RunParamDef *p)
{
    hb_units_t units = arm_units_are_contract() ? HB_UNITS_CONTRACT
                                                : HB_UNITS_COMPENSATE;
    if (s_units_logged != (int)units) {
        s_units_logged = (int)units;
        ESP_LOGI(TAG, "ARM fw=0x%04X → units=%s",
                 (unsigned)g_only_read.ARMFirmwareVersion,
                 (units == HB_UNITS_CONTRACT) ? "contract" : "compensate");
    }
    hb_build(p, units);
}

/* ---- heartbeat 组包 (57 值, 量纲按 units 二选一) ---- */
static void hb_build(const RunParamDef *p, hb_units_t units)
{

    /* 带符号电池电流: 旧版 ARM BatChgCurr/BatDischgCurr 为 0.01A (Ibat 0.1A×10,
     * 偏大 10×); bit2=Charge → +BatChgCurr; bit3=Discharging → -BatDischgCurr;
     * 补偿模式 ÷10 → 契约 0.1A (实测 -13340 → -1334), 契约模式直传 */
    int32_t bat_curr = 0;
    if (p->SysStatus & 0x04) {
        bat_curr = (int32_t)hb_units_div(units, p->BatChgCurr, 10);
    } else if (p->SysStatus & 0x08) {
        bat_curr = -(int32_t)hb_units_div(units, p->BatDischgCurr, 10);
    }

    /* 原始量纲整数 (含 0.1/0.01 缩放), 服务器按字段定义还原;
       Warning 仅低 16 位有效: 新版 ARM bit16~31 未初始化随机、bit32~63 恒 0,
       发布前屏蔽高位, 防止垃圾位进入云端告警位图 (两种量纲模式下都保留该屏蔽);
       sys[6]/sys[7] (变压器/PV 温度)、diag[0] InvCurr、sock 3 值均属 ARM 未赋值/
       未实现字段 (读数为未初始化内存, 实测 0xDEAD、25108 等垃圾值) → 除非
       ARM_UNSUPPORTED_FIELDS_IMPLEMENTED 置 1, 否则一律发布 null;
       量程护栏与服务器 heartbeat_v2.go bounded() 一一对应 (契约量纲界限), 越界 →
       null (见 hb_num); 补偿值与契约值界限相同, 故两种模式共用同一组界限;
       57 值: sys[11]/pv[5]/ac[11]/chr[3]/bat[5]/eng[14]/fan[2]/diag[3]/sock[3]

       ⚠ 量纲表（2026-09-21 依厂家提供的 ARM 源码 App(8)
       source/Collector/Collector.c ReadRunParam 逐字段重校，并以实机
       H1ZZX00139000038 线上解密 dump + ARM 屏幕显示缩放交叉验证；下列换算
       **仅在补偿模式执行** (hb_units_x10 / hb_units_div 内二选一)，契约模式
       全部直传）：
         ×10: Vpv1/Vpv2 (DSP 1V, 屏幕 "%uV" 原值显示实证) Ppv (DSP 1W)
              OutputWatt/OutputVA (Pload/Sload 1W/1VA, ParamForm 原值显示实证)
              BatChrWatt/BatDisChrWatt (Pbat 1W)
         直传 (线上已是契约量纲):
              ACOutputVolt — VloadA 直传, 实机 2310=231.0V (与 49.70Hz 互证;
                按 DSP 注释 1V 再 ×10 会得 2310V 触界变 null, 已改正)
              LoadPercent — SysParam(0.1%) 被 ARM ×10, 线上 0.1% 直传配服务器
                sys[9] scale=0.1 即还原百分比 (旧注释按 1% 批次 ×10 系双倍放大)
              ACChrCurr — IgridA(0.01A)×10 → 线上 0.1A (旧 ÷100 系误算)
              BusVolt(0.01V, 服务器 sys[8] scale=0.01 一致) BatVolt(0.01V)
              GridVolt(0.1V) GridFreq/ACOutputFreq(0.01Hz) ACInWatt/VA
              ACChrWatt/VA ACDisChrWatt/VA eng(0.1kWh) 温度(0.1℃) SOC(1%)
              风扇(1%) ParaChgCurr(1A) WorkTimeTotal(1s)
         ÷10: bat 电流（ARM =Ibat×10，Ibat 0.1A → 线上 0.01A）
         ÷1000: Buck1/2Curr、OutputCurr（ARM =Ipv×100、=IloadA×100，DSP
              0.01A → 线上为安培×100）——注意 ARM 侧 ×100 写入 u16 会在
              >6.55A 时回绕（实测满载 27.07A → 回绕存成 8556），回绕值无法
              在此还原，需厂家把 ARM 改为 ×10（见交付说明）
         恒 null: TransformerTemp/PvTemp/InvCurr/sock 3 值 — App(8)
              ReadRunParam 对这些字段无赋值语句, 发布即 malloc 堆垃圾
              (实测 0xDEAD/25108 等); 厂家补实现后置
              ARM_UNSUPPORTED_FIELDS_IMPLEMENTED=1 再放行

       *** 补偿模式与契约模式的切换由 ARM_FW_CONTRACT_UNITS_FROM 决定
       (arm_units_are_contract)：厂家修好后只需改该常量，本函数拼接代码不动 *** */
    char buf[768];
    size_t pos = 0;
    /* sys[11]: SysStatus/FaultValue/Warning/BmsWarning/BatOverCharge 不设限;
       InvertTemp≤1000 BoostTemp≤1200 BusVolt≤50000 LoadPercent≤1200 (raw);
       LoadPercent 直传: ARM 对 SysParam(0.1%, 屏幕按整数+小数拼显) 又 ×10,
       线上恰为 0.1% 契约量纲, 与服务器 sys[9] scale=0.1 直连还原百分比 */
    hb_append(buf, sizeof(buf), &pos,
              "{\"sys\":[%u,%lu,%" PRIu64 ",%u",
              (unsigned)p->SysStatus, (unsigned long)p->FaultValue,
              p->Warning & 0xFFFF, (unsigned)p->BmsWarning);
    hb_num(buf, sizeof(buf), &pos, p->InvertTemp, 1000);
    hb_num(buf, sizeof(buf), &pos, p->BoostTemp, 1200);
#if ARM_UNSUPPORTED_FIELDS_IMPLEMENTED
    hb_num(buf, sizeof(buf), &pos, p->TransformerTemp, 1200);   /* 0.1℃ 界 120.0 */
    hb_num(buf, sizeof(buf), &pos, p->PvTemp, 1200);            /* 0.1℃ 界 120.0 */
#else
    hb_append(buf, sizeof(buf), &pos, ",null,null");            /* ARM 未赋值 */
#endif
    hb_num(buf, sizeof(buf), &pos, p->BusVolt, 50000);
    hb_num(buf, sizeof(buf), &pos, p->LoadPercent, 1200);
    hb_append(buf, sizeof(buf), &pos, ",%u", (unsigned)p->BatOverCharge);
    /* pv[5]: Vpv 1V ×10→0.1V 界 5000 (500V; 旧界 1500=150V 低于实机 PV
       常见 200~450V 工作电压, 把正常值全部打成 null); Buck = Ipv×100 且 DSP Ipv1=10mA
       (UsartDsp.c:306) → ARM 值 0.0001A, ÷1000 → 0.1A 界 300; Ppv 1W ×10→0.1W
       (×10/÷1000 仅补偿模式, 契约模式直传; 界限两模式相同) */
    hb_append(buf, sizeof(buf), &pos, "],\"pv\":[");
    hb_num1(buf, sizeof(buf), &pos, hb_units_x10(units, p->Vpv1), 5000);
    hb_num(buf, sizeof(buf), &pos, hb_units_div(units, p->Buck1Curr, 1000), 300);
    hb_num(buf, sizeof(buf), &pos, hb_units_x10(units, p->Vpv2), 5000);
    hb_num(buf, sizeof(buf), &pos, hb_units_div(units, p->Buck2Curr, 1000), 300);
    hb_num(buf, sizeof(buf), &pos, hb_units_x10(units, p->Ppv), 75000);
    /* ac[11]: 输出电压线上即 0.1V 直传 (见 ⚠ 量纲表); 有功/视在 1W/1VA
       ×10 (补偿模式); OutputCurr =
       IloadA×100 且 DSP IloadA=0.01A (UsartDsp.c:291) → ARM 值 0.0001A,
       ÷1000 → 0.1A (补偿模式); ACOutputFreq/GridVolt/GridFreq/ACInWatt/
       ACInVA/ACDisChr* 均为契约量纲直传 (两模式相同) */
    hb_append(buf, sizeof(buf), &pos, "],\"ac\":[");
    hb_num1(buf, sizeof(buf), &pos, p->ACOutputVolt, 2500);
    hb_num(buf, sizeof(buf), &pos, p->ACOutputFreq, 5500);
    hb_num(buf, sizeof(buf), &pos, hb_units_x10(units, p->OutputWatt), 75000);
    hb_num(buf, sizeof(buf), &pos, hb_units_x10(units, p->OutputVA), 75000);
    hb_num(buf, sizeof(buf), &pos, hb_units_div(units, p->OutputCurr, 1000), 1000);
    hb_num(buf, sizeof(buf), &pos, p->GridVolt, 3000);
    hb_num(buf, sizeof(buf), &pos, p->GridFreq, 5500);
    hb_num(buf, sizeof(buf), &pos, p->ACInWatt, 75000);
    hb_num(buf, sizeof(buf), &pos, p->ACInVA, 75000);
    hb_num(buf, sizeof(buf), &pos, p->ACDisChrWatt, 75000);
    hb_num(buf, sizeof(buf), &pos, p->ACDisChrVA, 75000);
    /* chr[3]: ACChrWatt/ACChrVA 已 0.1 直传; ACChrCurr = IgridA(0.01A)×10
       → 线上即 0.1A 契约量纲, 直传 (旧 ÷100 为误算, 输出缩小 100 倍) */
    hb_append(buf, sizeof(buf), &pos, "],\"chr\":[");
    hb_num1(buf, sizeof(buf), &pos, p->ACChrWatt, 75000);
    hb_num(buf, sizeof(buf), &pos, p->ACChrVA, 75000);
    hb_num(buf, sizeof(buf), &pos, p->ACChrCurr, 1500);
    /* bat[5]: 电压≤7000 (0.01V) SOC≤100 电流±1500 (0.1A; 旧 ARM 0.01A 补偿
       模式 ÷10); 充/放功率 1W ×10→0.1W 界 75000 (补偿模式) */
    hb_append(buf, sizeof(buf), &pos, "],\"bat\":[");
    hb_num1(buf, sizeof(buf), &pos, p->BatVolt, 7000);
    hb_num(buf, sizeof(buf), &pos, p->BatterySOC, 100);
    hb_num_i(buf, sizeof(buf), &pos, bat_curr, -1500, 1500);
    hb_num(buf, sizeof(buf), &pos, hb_units_x10(units, p->BatChrWatt), 75000);
    hb_num(buf, sizeof(buf), &pos, hb_units_x10(units, p->BatDisChrWatt), 75000);
    /* eng[14]: 能量统计 (0.1kWh) 暂不设限, 固件侧透传 */
    hb_append(buf, sizeof(buf), &pos,
              "],\"eng\":[%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu",
              (unsigned long)p->EGen_today, (unsigned long)p->EGen_total,
              (unsigned long)p->Epv_today, (unsigned long)p->Epv_total,
              (unsigned long)p->Eac_chrToday, (unsigned long)p->Eac_chrTotal,
              (unsigned long)p->Ebat_dischrToday, (unsigned long)p->Ebat_dischrTotal,
              (unsigned long)p->Ebat_chrToday, (unsigned long)p->Ebat_chrTotal,
              (unsigned long)p->Eac_dischrToday, (unsigned long)p->Eac_dischrTotal,
              (unsigned long)p->Eop_dischrToday, (unsigned long)p->Eop_dischrTotal);
    /* fan[2]: 转速 ≤100 (新版 ARM 两字段固定 0; 之前看到的 90/100 是错位的
       LoadPercent) */
    hb_append(buf, sizeof(buf), &pos, "],\"fan\":[");
    hb_num1(buf, sizeof(buf), &pos, p->MpptFanSpeed, 100);
    hb_num(buf, sizeof(buf), &pos, p->InvFanSpeed, 100);
    /* diag[3]: InvCurr ARM 未赋值 (随机) → null; ParaChgCurr≤600 (raw);
       WorkTimeTotal u32 全域不设限 */
    hb_append(buf, sizeof(buf), &pos, "],\"diag\":[");
#if ARM_UNSUPPORTED_FIELDS_IMPLEMENTED
    /* 量纲未见说明, 按 ARM 原值发布 (界 = 服务器 0-100A @0.1A), 待厂家确认 */
    hb_num1(buf, sizeof(buf), &pos, p->InvCurr, 1000);
#else
    hb_append(buf, sizeof(buf), &pos, "null");
#endif
    hb_num(buf, sizeof(buf), &pos, p->ParaChgCurr, 600);
    hb_append(buf, sizeof(buf), &pos, ",%lu", (unsigned long)p->WorkTimeTotal);
    /* sock[3]: ARM 未实现并机/插座, 读数为未初始化内存 → null (见函数头注释);
       实现后按位掩码直传 (u16 全域, 服务器 ranged 0-2^16, 无需量程护栏) */
    hb_append(buf, sizeof(buf), &pos, "],\"sock\":[");
#if ARM_UNSUPPORTED_FIELDS_IMPLEMENTED
    hb_append(buf, sizeof(buf), &pos, "%u,%u,%u]}",
              (unsigned)p->PairedSocket, (unsigned)p->OnlineSocket,
              (unsigned)p->OnSocket);
#else
    hb_append(buf, sizeof(buf), &pos, "null,null,null]}");
#endif

    if (pos >= sizeof(buf)) {
        ESP_LOGE(TAG, "heartbeat payload too large (%u)", (unsigned)pos);
        return;
    }
    buf[pos] = '\0';
    mqtt_publish(TOPIC_HEARTBEAT, buf, 0, 1, 0);
    ESP_LOGI(TAG, "heartbeat published (%d bytes)", (int)pos);
}

/* ---- BLE 通道遥测（核心子集, 对齐协议 §6.1 + App InverterRealtime.fromJson） ---- */
/* 字段名以 App 解析器为准（ac/battery/pv/sys_status/energy/load_power）,
 * 单位换算为工程单位（V / A / W / Hz / %）。 */
static void publish_ble_telemetry(const RunParamDef *p)
{
    char updated_at[40];
    uint32_t t = ble_ct_now();
    if (t > 0) {
        time_t tt = (time_t)t;
        struct tm tm;
        gmtime_r(&tt, &tm);
        snprintf(updated_at, sizeof(updated_at), "%04d-%02d-%02dT%02d:%02d:%02dZ",
                 tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                 tm.tm_hour, tm.tm_min, tm.tm_sec);
    } else {
        snprintf(updated_at, sizeof(updated_at), "1970-01-01T00:00:00Z");
    }

    /* 带符号电池电流 (0.01A, 新版 ARM 偏大 10×): bit3=Discharge → 负 */
    int bat_curr = (p->SysStatus & 0x08)
                       ? -(int)p->BatDischgCurr
                       : (p->SysStatus & 0x04) ? (int)p->BatChgCurr : 0;
    /* PV 电流 (0.1A) = Ppv(1W) ×10 / Vpv1(1V)；Vpv1==0 时置 0 */
    unsigned pv_curr = (p->Vpv1 > 0) ? (unsigned)((uint32_t)p->Ppv * 10 / p->Vpv1) : 0;
    /* 负载电流 0.1A: ARM OutputCurr = IloadA×100, 与 heartbeat ac[4] 同按
       ÷1000 → 0.1A */
    unsigned out_curr_a10 = (unsigned)(p->OutputCurr / 1000);

    char buf[600];
    int n = snprintf(buf, sizeof(buf),
        "{\"device_sn\":\"%s\","
        "\"updated_at\":\"%s\","
        "\"load_power\":%lu,"
        "\"ac\":{\"voltage\":%u.%u,\"current\":%u.%u,\"power\":%lu,"
        "\"frequency\":%u.%02u,\"load_percent\":%u.%u,\"pf\":0},"
        "\"battery\":{\"soc\":%u,\"voltage\":%u.%02u,\"current\":%d.%d,"
        "\"power\":%ld,\"charge_state\":\"%s\"},"
        "\"pv\":{\"pv_voltage\":%u.%u,\"pv_current\":%u.%u,\"pv_power\":%lu},"
        "\"sys_status\":{\"state\":\"%s\",\"fault_code\":%lu,\"temp_inv\":%u.%u},"
        "\"energy\":{\"daily_pv\":%u.%u,\"total_pv\":%u.%u,"
        "\"daily_load\":%u.%u,\"total_load\":%u.%u,\"runtime_hours\":0}}",
        g_device_sn, updated_at,
        (unsigned long)p->OutputWatt,
        (unsigned)(p->ACOutputVolt / 10), (unsigned)(p->ACOutputVolt % 10),
        out_curr_a10 / 10, out_curr_a10 % 10,
        (unsigned long)p->OutputWatt,
        (unsigned)(p->ACOutputFreq / 100), (unsigned)(p->ACOutputFreq % 100),
        (unsigned)(p->LoadPercent / 10), (unsigned)(p->LoadPercent % 10),
        (unsigned)p->BatterySOC,
        (unsigned)(p->BatVolt / 100), (unsigned)(p->BatVolt % 100),
        (int)(bat_curr / 100),
        (int)(((bat_curr < 0 ? -bat_curr : bat_curr) / 10) % 10),
        (long)((p->SysStatus & 0x08) ? -(long)p->BatDisChrWatt
                                     : (long)p->BatChrWatt),
        (p->SysStatus & 0x02) ? "fault"
            : (p->SysStatus & 0x08) ? "discharging"
            : (p->SysStatus & 0x04) ? "charging" : "standby",
        (unsigned)p->Vpv1, 0U,
        (unsigned)(pv_curr / 10), (unsigned)(pv_curr % 10),
        (unsigned long)p->Ppv,
        "offgrid", (unsigned long)p->FaultValue,
        (unsigned)(p->InvertTemp / 10), (unsigned)(p->InvertTemp % 10),
        (unsigned)(p->Epv_today / 10), (unsigned)(p->Epv_today % 10),
        (unsigned)(p->Epv_total / 10), (unsigned)(p->Epv_total % 10),
        (unsigned)(p->EGen_today / 10), (unsigned)(p->EGen_today % 10),
        (unsigned)(p->EGen_total / 10), (unsigned)(p->EGen_total % 10));

    if (n > 0 && n < (int)sizeof(buf)) {
        ble_ct_telemetry_notify(buf, (uint16_t)n);
    }
}

/* ---- info (V2.0 §7, QoS1) ---- */
static void publish_info(const OnlyReadDataDef *o)
{
    char arm_ver[16], dsp_ver[16], hw_ver[16], bl_ver[16];
    fmt_ver(arm_ver, sizeof(arm_ver), o->ARMFirmwareVersion);
    fmt_ver(dsp_ver, sizeof(dsp_ver), o->DSPFirmwareVersion);
    fmt_ver(hw_ver, sizeof(hw_ver), o->HardwareVersion);
    fmt_ver(bl_ver, sizeof(bl_ver), o->BLVersion);

    /* SN: 只读信息 Inverter_SN1..4 (4×u32, ≤16 位 ASCII); 非法时省略字段 */
    char sn_str[17] = {0};
    char sn_json[32] = {0};   /* "sn":"" + ≤16 ASCII 字符 + "," + NUL */
    if (onlyread_sn_to_str(o, sn_str, sizeof(sn_str))) {
        snprintf(sn_json, sizeof(sn_json), "\"sn\":\"%s\",", sn_str);
    }

    /* 额定参数: ARM 只读区按自然单位填充 (Collector.c: NomOpVolt=240 → 240V,
     * NomOpFreq=60 → 60Hz, NomBatVolt=60 → 60V); 协议文档的 0.1V/0.01Hz/0.01V
     * 标度与 ARM 实际实现不符 (同类问题见后端 7e2d1fc50 的双格式兼容), 直接透传 */
    char rated_v[16], rated_f[16], bat_nom[16];
    snprintf(rated_v, sizeof(rated_v), "%u", (unsigned)o->NomOpVolt);
    snprintf(rated_f, sizeof(rated_f), "%u", (unsigned)o->NomOpFreq);
    snprintf(bat_nom, sizeof(bat_nom), "%u", (unsigned)o->NomBatVolt);

    /* 模块号: InverterModuleH/L 合并 u32 (BCD 存储, 按数字字符串上报) */
    char module[16];
    snprintf(module, sizeof(module), "%lu",
             (unsigned long)(((uint32_t)o->InverterModuleH << 16) | o->InverterModuleL));

    /* battery_type: 控制参数 0x0004 未读取 (阶段 2), 出厂默认 LiFePO4 */
    char buf[512];
    int n = snprintf(buf, sizeof(buf),
        "{%s\"model\":\"CS-L10-6K2\",\"manufacturer\":\"辰烁科技\","
        "\"firmware_arm\":\"%s\",\"firmware_esp\":\"%s\","
        "\"firmware_dsp\":\"%s\",\"firmware_bms\":null,"
        "\"device_type\":\"off_grid_inverter\",\"phase\":\"single\","
        "\"rated_power\":%u,\"rated_voltage\":%s,\"rated_frequency\":%s,"
        "\"battery_nominal_voltage\":%s,\"battery_type\":\"LiFePO4\","
        "\"cell_count\":0,\"temp_sensor_count\":0,"
        "\"inverter_module\":\"%s\",\"hardware_version\":\"%s\","
        "\"bootloader_version\":\"%s\"}",
        sn_json, arm_ver, FIRMWARE_VERSION, dsp_ver,
        (unsigned)o->RateWatt, rated_v, rated_f, bat_nom,
        module, hw_ver, bl_ver);
    if (n <= 0 || n >= (int)sizeof(buf)) {
        ESP_LOGE(TAG, "info payload too large (%d)", n);
        return;
    }
    mqtt_publish(TOPIC_INFO, buf, 0, 1, 0);
    ESP_LOGI(TAG, "info published (arm=%s dsp=%s rate_watt=%u)", arm_ver, dsp_ver,
             (unsigned)o->RateWatt);
}

/* ---- config (V2.1 §9, v2 语义键值对, QoS1) ---- */
/* 键名与 scale 对照 V2.1 9.2 表 (42 键, 索引 0x0000-0x0029);
   值为工程单位 (raw/scale), 服务器按 device_config_schema 校验 */
typedef struct {
    const char *name;
    uint8_t     scale;   /* 1 / 10 / 100 */
} cfg_key_t;

static const cfg_key_t s_cfg_keys[CTRL_PARAM_COUNT] = {
    { "set_output_priority",        1   },
    { "set_max_charge_current",     10  },  /* 0.1A */
    { "set_ac_volt_range",          1   },
    { "set_battery_capacity",       1   },
    { "set_battery_type",           1   },
    { "set_overload_restart",       1   },
    { "set_high_temp_restart",      1   },
    { "set_output_voltage",         10  },  /* 0.1V */
    { "set_output_frequency",       100 },  /* 0.01Hz */
    { "set_master_slave",           1   },
    { "set_ac_charge_current",      10  },  /* 0.1A */
    { "set_low_volt_return_utl",    10  },  /* 0.1V */
    { "set_high_volt_return_bat",   10  },  /* 0.1V */
    { "set_charge_priority",        1   },
    { "set_alarm_control",          1   },
    { "set_backlight_ctrl",         1   },
    { "set_power_shutdown_alarm",   1   },
    { "set_overload_use_city_power",1   },
    { "set_max_discharge_current",  10  },  /* 0.1A */
    { "set_max_chg_curr",           10  },  /* 0.1A */
    { "set_recover_threshold_volt", 10  },  /* 0.1V */
    { "set_solar_power_balance",    1   },
    { "set_ac_output_mode",         1   },
    { "set_li_bat_material",        1   },
    { "set_cell_serial_lifepo4",    1   },
    { "set_cell_serial_li_nmc",     1   },
    { "set_equalize_enable",        1   },
    { "set_equalize_voltage",       10  },  /* 0.1V */
    { "set_equalize_time",          1   },
    { "set_equalize_timeout",       1   },
    { "set_equalize_interval",      1   },
    { "set_equalize_activate",      1   },
    { "set_charge_time",            1   },
    { "set_close_charge_time",      1   },
    { "set_gen_start_voltage",      10  },  /* 0.1V */
    { "set_gen_stop_voltage",       10  },  /* 0.1V */
    { "set_soc_back_utl",           1   },
    { "set_soc_back_bat",           1   },
    { "set_soc_back_gen",           1   },
    { "set_soc_close_gen",          1   },
    { "set_soc_cutoff",             1   },
    { "set_buzzer",                 1   },
};

static void publish_config(const uint16_t *ctrl, uint32_t rev)
{
    /* CtrlParamAlterTime 为 0 表示出厂未修改, 用当前时间兜底 (后端要求 rev > 0) */
    if (rev == 0) {
        rev = (uint32_t)time(NULL);
        ESP_LOGW(TAG, "CtrlParamAlterTime=0, using current time as rev=%lu",
                 (unsigned long)rev);
    }
    /* 静态缓冲: 42 键 ~35B/键 + 信封 ≈ 1600B, 避免 main_loop 栈压力 */
    static char buf[1800];
    int n = snprintf(buf, sizeof(buf), "{\"rev\":%lu,\"params\":{",
                     (unsigned long)rev);

    for (int i = 0; i < CTRL_PARAM_COUNT && n > 0 && n < (int)sizeof(buf); i++) {
        uint16_t raw = ctrl[i];
        const char *sep = i ? "," : "";
        int m;
        switch (s_cfg_keys[i].scale) {
        case 100:
            m = snprintf(buf + n, sizeof(buf) - n, "%s\"%s\":%u.%02u", sep,
                         s_cfg_keys[i].name, (unsigned)(raw / 100),
                         (unsigned)(raw % 100));
            break;
        case 10:
            m = snprintf(buf + n, sizeof(buf) - n, "%s\"%s\":%u.%u", sep,
                         s_cfg_keys[i].name, (unsigned)(raw / 10),
                         (unsigned)(raw % 10));
            break;
        default:
            m = snprintf(buf + n, sizeof(buf) - n, "%s\"%s\":%u", sep,
                         s_cfg_keys[i].name, (unsigned)raw);
            break;
        }
        if (m < 0) {
            break;
        }
        n += m;
    }

    if (n <= 0 || n >= (int)sizeof(buf) - 2) {
        ESP_LOGE(TAG, "config payload too large (%d)", n);
        return;
    }
    buf[n++] = '}';
    buf[n++] = '}';
    buf[n] = '\0';
    mqtt_publish(TOPIC_CONFIG, buf, 0, 1, 0);
    ESP_LOGI(TAG, "config published (rev=%lu, %d bytes)", (unsigned long)rev, n);
}

/* ---- ARM 通信故障告警 (source=2 ESP, code=1 comm_timeout) ---- */
static void publish_arm_fault_alarm(void)
{
    char buf[128];
    int n = snprintf(buf, sizeof(buf),
        "{\"source\":2,\"code\":1,\"level\":2,\"state\":1}");
    if (n > 0 && n < (int)sizeof(buf)) {
        mqtt_publish(TOPIC_ALARM, buf, 0, 1, 0);
        ESP_LOGW(TAG, "ARM fault alarm published");
    }
}

static void publish_arm_recovery_alarm(void)
{
    char buf[128];
    int n = snprintf(buf, sizeof(buf),
        "{\"source\":2,\"code\":1,\"level\":2,\"state\":0}");
    if (n > 0 && n < (int)sizeof(buf)) {
        mqtt_publish(TOPIC_ALARM, buf, 0, 1, 0);
        ESP_LOGI(TAG, "ARM recovery alarm published");
    }
}

/* ---- alarm 事件 (V2.0 §8) ---- */
typedef struct {
    int      source;  /* 0 PCS; 1 BMS */
    int      level;   /* 1 warning; 2 fault */
    uint64_t code;    /* 上报码值 (位掩码整体值) */
    int      state;   /* 1 active; 0 recovered */
    int      prio;    /* 0=fault 先发; 1=warning */
} alarm_evt_t;

static void check_alarm(const RunParamDef *p)
{
    /* 告警位量程护栏: L10 告警/故障码为低位位掩码。新版 ARM Warning 为 u64,
     * 但 bit16~31 未初始化随机、bit32~63 恒 0, 仅 bit0~15 有效 → 高 48 位非全 0
     * 视为未初始化内存垃圾, 跳过该项判断且不刷新基线, 防止垃圾值触发误告警
     * (与 heartbeat 发布 &0xFFFF 屏蔽一致); FaultValue 仅低 16 位有效, 判断不变;
     * 合法值行为与原来完全一致。BmsWarning 为 u16 全域, 不设护栏。 */
    bool fault_ok = (p->FaultValue >> 16) == 0;
    bool warn_ok  = (p->Warning >> 16) == 0;

    if (!s_alarm_armed) {
        s_alarm_armed = true;
        s_last_warning = warn_ok ? p->Warning : 0;
        s_last_fault = fault_ok ? p->FaultValue : 0;
        s_last_bms = p->BmsWarning;
        return;
    }

    alarm_evt_t evts[3];
    int nevt = 0;

    /* FaultValue (source=0, level=2): 0→非0 active; 非0→0 recovered (code 回填最后值) */
    if (fault_ok && p->FaultValue != s_last_fault) {
        evts[nevt].source = 0;
        evts[nevt].level  = 2;
        evts[nevt].code   = (p->FaultValue != 0) ? p->FaultValue : s_last_fault;
        evts[nevt].state  = (p->FaultValue != 0) ? 1 : 0;
        evts[nevt].prio   = 0;
        nevt++;
    }
    /* Warning (source=0, level=1, 64 位) */
    if (warn_ok && p->Warning != s_last_warning) {
        evts[nevt].source = 0;
        evts[nevt].level  = 1;
        evts[nevt].code   = (p->Warning != 0) ? p->Warning : s_last_warning;
        evts[nevt].state  = (p->Warning != 0) ? 1 : 0;
        evts[nevt].prio   = 1;
        nevt++;
    }
    /* BmsWarning (source=1, level=1) */
    if (p->BmsWarning != s_last_bms) {
        evts[nevt].source = 1;
        evts[nevt].level  = 1;
        evts[nevt].code   = (p->BmsWarning != 0) ? p->BmsWarning : s_last_bms;
        evts[nevt].state  = (p->BmsWarning != 0) ? 1 : 0;
        evts[nevt].prio   = 1;
        nevt++;
    }

    if (nevt == 0) {
        return;
    }

    /* 按 fault > warning 优先级逐条上报 (最多 3 条, V2.0 §8.1) */
    for (int i = 0; i < nevt; i++) {
        for (int j = i + 1; j < nevt; j++) {
            if (evts[j].prio < evts[i].prio) {
                alarm_evt_t tmp = evts[i];
                evts[i] = evts[j];
                evts[j] = tmp;
            }
        }
    }
    for (int i = 0; i < nevt; i++) {
        char buf[128];
        snprintf(buf, sizeof(buf),
                 "{\"source\":%d,\"code\":%" PRIu64 ",\"level\":%d,\"state\":%d}",
                 evts[i].source, evts[i].code, evts[i].level, evts[i].state);
        mqtt_publish(TOPIC_ALARM, buf, 0, 1, 0);
        ESP_LOGI(TAG, "alarm %s source=%d level=%d code=%" PRIu64,
                 evts[i].state ? "active" : "recovered",
                 evts[i].source, evts[i].level, evts[i].code);
    }

    /* 基线仅在字段合法时刷新 (垃圾值不污染基线, 恢复正常后仍能正确比对) */
    if (fault_ok) {
        s_last_fault = p->FaultValue;
    }
    if (warn_ok) {
        s_last_warning = p->Warning;
    }
    s_last_bms = p->BmsWarning;
}

void telemetry_init(void)
{
    s_now_ms = 0;
    s_last_hb_ms = 0;
    s_last_online_ms = 0;
    s_last_info_ms = 0;
    s_last_config_ms = 0;
    s_hb_requested = false;
    s_config_requested = false;
    s_alarm_armed = false;
    s_last_warning = 0;
    s_last_fault = 0;
    s_last_bms = 0;
    s_arm_fail_count = 0;
    s_fault_reported = false;
    s_units_logged = -1;   /* 下次发布重新打印量纲策略 */
    ESP_LOGI(TAG, "init done");
}

void telemetry_on_mqtt_connected(void)
{
    uint32_t now = now_ms();
    /* 立即上报在线状态 */
    publish_online();
    s_last_online_ms = now;
    /* 已有 ARM 数据则立即上报 info + heartbeat; 并请求重读 */
    if (arm_poll_take_param()) {
        if (g_arm_stat.last_info_ms != 0) {
            publish_info(&g_only_read);
        }
        if (g_arm_stat.poll_count > 0) {
            if (s_fault_reported) {
                s_fault_reported = false;
                publish_arm_recovery_alarm();
            }
            publish_heartbeat(&g_run_param);
            s_last_hb_ms = now;
            s_arm_fail_count = 0;
        }
        arm_poll_give_param();
    }
    arm_poll_request_info();
    /* config: 连接成功触发读取+上报 (V2.1 §9: 连接成功即上报) */
    telemetry_request_config();
}

void telemetry_request_heartbeat(void)
{
    s_hb_requested = true;   /* tick 内立即补报并重置周期 */
}

bool telemetry_debug_set(bool enabled, uint32_t interval_s, uint32_t duration_s,
                         uint32_t *out_effective_interval_s, int64_t *out_expires_at_unix)
{
    if (!enabled) {
        s_debug_active = false;
        s_debug_deadline_ms = 0;
        if (out_effective_interval_s) *out_effective_interval_s = (uint32_t)TELEMETRY_PERIOD_MS / 1000U;
        if (out_expires_at_unix) *out_expires_at_unix = (int64_t)time(NULL);
        telemetry_request_heartbeat();
        ESP_LOGI(TAG, "debug telemetry disabled, revert default period");
        return true;
    }
    if (interval_s < 5U)    interval_s = 5U;
    if (interval_s > 3600U) interval_s = 3600U;
    if (duration_s == 0U || duration_s > 86400U) duration_s = 3600U;
    s_debug_interval_ms = interval_s * 1000U;
    s_debug_deadline_ms = now_ms() + duration_s * 1000U;
    s_debug_active = true;
    if (out_effective_interval_s) *out_effective_interval_s = interval_s;
    if (out_expires_at_unix) *out_expires_at_unix = (int64_t)time(NULL) + (int64_t)duration_s;
    /* 立即按新周期补报一帧, 云端据此确认切换已生效 */
    telemetry_request_heartbeat();
    ESP_LOGI(TAG, "debug telemetry enabled: interval=%luus duration=%luus",
             (unsigned long)interval_s, (unsigned long)duration_s);
    return true;
}

bool telemetry_debug_active(void)
{
    return s_debug_active;
}

void telemetry_request_info(void)
{
    /* 立即补报现有 info (若有) + 请求 ARM 重读 */
    if (arm_poll_take_param()) {
        if (g_arm_stat.last_info_ms != 0) {
            publish_info(&g_only_read);
        }
        arm_poll_give_param();
    }
    arm_poll_request_info();
}

void telemetry_request_config(void)
{
    /* 请求 ARM 重读控制参数; tick 检测到 last_ctrl_ms 变化后发布 (V2.1 §9) */
    s_config_requested = true;
    arm_poll_request_config();
}

void telemetry_tick(void)
{
    s_now_ms = now_ms();

    RunParamDef rp;
    bool have_run = false;

    if (arm_poll_take_param()) {
        /* info 变化 → 发布一次 */
        if (g_arm_stat.last_info_ms != 0 && g_arm_stat.last_info_ms != s_last_info_ms) {
            s_last_info_ms = g_arm_stat.last_info_ms;
            publish_info(&g_only_read);
        }
        /* config: 有请求且 ARM 已重读 (last_ctrl_ms 变化) → 发布一次 */
        if (s_config_requested && g_arm_stat.last_ctrl_ms != 0 &&
            g_arm_stat.last_ctrl_ms != s_last_config_ms) {
            s_last_config_ms = g_arm_stat.last_ctrl_ms;
            s_config_requested = false;
            publish_config(g_ctrl_param, g_read_write.CtrlParamAlterTime);
        }
        if (g_arm_stat.poll_count > 0) {
            rp = g_run_param;
            have_run = true;
        }
        arm_poll_give_param();
    }

    /* 90s 在线上报 (无需ARM应答) */
    if ((s_now_ms - s_last_online_ms) >= ONLINE_PERIOD_MS) {
        s_last_online_ms = s_now_ms;
        publish_online();
    }

    /* ARM 连通判定: 最近一次应答在 240s 内 (2×120s 断线超时) */
    bool arm_reachable = g_arm_stat.last_rx_ms != 0 &&
                         (s_now_ms - g_arm_stat.last_rx_ms) < 240000;
    /* 有 RUNPARAM 数据 + ARM 可达 → 可上报 heartbeat */
    bool arm_ok = have_run && arm_reachable;

    /* 调试模式到期: 自动恢复默认周期 (重启同样恢复默认, 调试开关不持久化) */
    if (s_debug_active && s_debug_deadline_ms != 0 &&
        (int32_t)(s_now_ms - s_debug_deadline_ms) >= 0) {
        s_debug_active = false;
        s_debug_deadline_ms = 0;
        ESP_LOGI(TAG, "debug telemetry expired, revert default period");
    }
    uint32_t hb_period_ms = s_debug_active ? s_debug_interval_ms
                                           : (uint32_t)TELEMETRY_PERIOD_MS;

    /* V2 heartbeat (需ARM应答; 调试模式按切换后的周期); query_telemetry 也可触发 */
    if (s_hb_requested || (s_now_ms - s_last_hb_ms) >= hb_period_ms) {
        s_hb_requested = false;
        s_last_hb_ms = s_now_ms;

        if (arm_reachable) {
            s_arm_fail_count = 0;
            if (s_fault_reported) {
                s_fault_reported = false;
                publish_arm_recovery_alarm();
            }
        }
        if (arm_ok) {
            publish_heartbeat(&rp);
            publish_ble_telemetry(&rp);   /* BLE 通道遥测推送（核心子集） */
        } else if (!arm_reachable) {
            s_arm_fail_count++;
            ESP_LOGW(TAG, "ARM no response (%lu/%d)",
                     (unsigned long)s_arm_fail_count, ARM_FAIL_THRESHOLD);
            if (s_arm_fail_count >= ARM_FAIL_THRESHOLD && !s_fault_reported) {
                s_fault_reported = true;
                publish_arm_fault_alarm();
            }
        }
    }

    /* 告警变化检测 (每次 tick, 不等心跳周期) */
    if (arm_ok) {
        check_alarm(&rp);
    }
}
