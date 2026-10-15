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
extern bool direct_dsp_get_measurement(RunParamDef *measurement, uint32_t *received_ms);
extern bool direct_config_snapshot_wire(uint16_t values[CTRL_PARAM_COUNT],
                                       bool valid[CTRL_PARAM_COUNT],uint32_t *revision);
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
static const char *reset_reason_name(void) { return "UNKNOWN"; }
static uint32_t nvs_free_entries(void) { return 0; }
static uint32_t heap_free_bytes(void) { return 0; }
static uint32_t heap_max_block(void) { return 0; }
static uint32_t heap_alloc_blocks(void) { return 0; }
static uint32_t heap_min_free(void) { return 0; }
#else
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "nvs_flash.h"
#include "config.h"
#include "cs_protocol_defs.h"
#include "mqtt_handler.h"
#include "ble_ct_telemetry.h"
#include "ble_ct_clock.h"

/* ---- 复位原因名 (online 上报携带): 远程即可判复位根因。
 * 下载/传输中曾出现间歇性猝死, 此字段直接区分 BROWNOUT(欠压) /
 * PANIC(软件崩溃) / TASK_WDT(看门狗) / POWERON(掉电)。PANIC 的 backtrace
 * 用 USB-Serial/JTAG 现场抓 (idf.py monitor), 不做 coredump-to-flash。 ---- */
static const char *reset_reason_name(void)
{
    switch (esp_reset_reason()) {
    case ESP_RST_POWERON:   return "POWERON";
    case ESP_RST_SW:        return "SW";
    case ESP_RST_PANIC:     return "PANIC";
    case ESP_RST_INT_WDT:   return "INT_WDT";
    case ESP_RST_TASK_WDT:  return "TASK_WDT";
    case ESP_RST_WDT:       return "WDT";
    case ESP_RST_BROWNOUT:  return "BROWNOUT";
    case ESP_RST_DEEPSLEEP: return "DEEPSLEEP";
    case ESP_RST_PWR_GLITCH:return "PWR_GLITCH";
    default:                return "UNKNOWN";
    }
}

/* ---- 堆余量 (online 上报携带) ----
 * 云端 OTA 下载要一块 ~20KB 的 TLS 上下文, 设备侧原先只在失败报文里能看到
 * 那一刻的 free/max, 分不清"天生就这么小"还是"随 uptime 被吃掉的"。90s 一条
 * 的曲线把两者分开: 重启后立刻看第一条, 再对比几小时后。 */
static uint32_t heap_free_bytes(void) { return (uint32_t)esp_get_free_heap_size(); }
static uint32_t heap_max_block(void)
{
    return (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
}
/* 定位"堆随 uptime 流失": 已分配块数单调上涨 = 真泄漏; 块数平稳而 free 下降 =
 * 大块被占/合并受阻。minimum_free_bytes 是开机以来最低水位, 比瞬时值更能说明
 * "最紧的时候还剩多少"。 */
static uint32_t heap_alloc_blocks(void)
{
    multi_heap_info_t info;
    heap_caps_get_info(&info, MALLOC_CAP_8BIT);
    return (uint32_t)info.allocated_blocks;
}
static uint32_t heap_min_free(void)
{
    multi_heap_info_t info;
    heap_caps_get_info(&info, MALLOC_CAP_8BIT);
    return (uint32_t)info.minimum_free_bytes;
}
/* NVS 剩余条目: 写满会让配网凭据/SN/密钥的保存**静默失败**(写失败的路径见
 * app_main 配网处 / arm_poll SN 同步处), 现场表现得像"重启就丢配置"。
 * 24KB 的 nvs 分区、几百条上限, 这个数贴近 0 就是病灶。 */
static uint32_t nvs_free_entries(void)
{
    nvs_stats_t st;
    if (nvs_get_stats(NULL, &st) != ESP_OK) return 0;
    return (uint32_t)st.free_entries;
}
#endif

static const char *TAG = "TELE";

/* 主题名 (不带前缀, 前缀 cs_inv/{sn}/ + 信封由 mqtt_publish 拼接) */
#define TOPIC_HEARTBEAT  PROTO_TOPIC_HEARTBEAT
#define TOPIC_INFO       PROTO_TOPIC_INFO
#define TOPIC_ALARM      PROTO_TOPIC_ALARM
#define TOPIC_CONFIG     PROTO_TOPIC_CONFIG

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

/* OTA 静默窗口 (2026-09-22, 用户授权): ESP 自升级期间停掉周期心跳与 ARM 侧
 * 派生上报 —— 下载/写入是内存与 CPU 最紧的一段, 后台活动只会抢资源。
 * online(90s, 带 heap 曲线) 不受影响: 进度与存活必须实时可见。 */
static volatile bool s_quiet = false;

void telemetry_set_quiet(bool quiet)
{
    s_quiet = quiet;
}

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

/* ---- 在线状态上报 (90s, 无需ARM应答) ---- */
static void publish_online(void)
{
    if (!mqtt_is_connected()) return;
    char buf[224];
    int n = snprintf(buf, sizeof(buf),
        "{\"uptime\":%lu,\"reset_reason\":\"%s\",\"heap\":%lu,\"heap_max\":%lu,"
        "\"blk\":%lu,\"heap_min\":%lu,\"nvs\":%lu}",
        (unsigned long)(s_now_ms / 1000), reset_reason_name(),
        (unsigned long)heap_free_bytes(), (unsigned long)heap_max_block(),
        (unsigned long)heap_alloc_blocks(), (unsigned long)heap_min_free(),
        (unsigned long)nvs_free_entries());
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

/* s16 负值钳 0 (2026-09-22 对照表契约: 市电/负载/充电功率与电流 <0 视为测量
 * 毛刺; App(10) ReadRunParam 本身不钳位, 钳位在 ESP 侧补齐)。
 * 注: heartbeat 的数值量程护栏已随 v3 原样转发一并移交服务端
 * (heartbeat_v3.go bounded()), 固件侧不再保留 hb_num/hb_num1/hb_num_i;
 * 本函数仅 BLE 近场遥测 (App 期望工程单位) 使用。 */
static uint32_t hb_clamp0(int32_t v)
{
    return (v < 0) ? 0u : (uint32_t)v;
}

/* ---- heartbeat: ARM 运行参数原样转发 (v3, ESP 零解析) ----
 * 2026-09-22 起 ESP 不再做任何字段映射与量纲换算: ARM App(10) ReadRunParam
 * 直传的 RunParamDef (87 个 u16, pack(1)) 按字序原样发布为 {"run":[...]},
 * 服务端按同一份结构体定义做字段映射/量纲换算/界限判定
 * (device-communication/internal/telemetry/heartbeat_v3.go)。
 * 动机: 量纲知识集中到服务端一处 —— DSP/ARM 量纲调整只改服务端, 固件不再
 * 承担换算风险 (09-21/09-22 两次 ESP 侧换算结论错误均源于此)。
 * 信封版本 3 且不走离线缓存 (原因见 mqtt_publish_heartbeat_raw 注释)。 */
static void publish_heartbeat(const RunParamDef *p)
{
    const uint16_t *w = (const uint16_t *)p;
    static char buf[640];   /* 87 值 × 最多 6B ≈ 530B; 静态缓冲避免任务栈压力 */
    size_t pos = 0;

    hb_append(buf, sizeof(buf), &pos, "{\"run\":[");
    for (int i = 0; i < RUN_PARAM_WORDS; i++) {
        hb_append(buf, sizeof(buf), &pos, i ? ",%u" : "%u", (unsigned)w[i]);
    }
    hb_append(buf, sizeof(buf), &pos, "]}");

    if (pos >= sizeof(buf)) {
        ESP_LOGE(TAG, "heartbeat payload too large (%u)", (unsigned)pos);
        return;
    }
    buf[pos] = '\0';
    int result = mqtt_publish_heartbeat_raw(TOPIC_HEARTBEAT, buf, (int)pos);
    if (result != 0) {
        ESP_LOGW(TAG, "heartbeat queue failed (%d)", result);
    } else {
        ESP_LOGI(TAG, "heartbeat queued (%d bytes, raw run struct)", (int)pos);
    }
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

    /* 带符号电池电流 (struct 0.1A 直传; 下方显示层 ×10 转 0.01A 基准):
       bit3=Discharge → 负 */
    int bat_curr = (p->SysStatus & 0x08)
                       ? -(int)p->BatDischgCurr
                       : (p->SysStatus & 0x04) ? (int)p->BatChgCurr : 0;
    /* PV 电压 <60V 视为无输入（残压）归 0, 与服务端 pvVoltageNormalized 一致:
       无 PV 接入时 ARM 上送 ~11V 感应电压, 直接展示会被误读为"有 PV 输入" */
    unsigned pv_v = (p->Vpv1 < 60U) ? 0U : (unsigned)p->Vpv1;
    /* PV 电流 (0.1A) = Ppv(1W) ×10 / Vpv1(1V)；归零后置 0 */
    unsigned pv_curr = (pv_v > 0) ? (unsigned)((uint32_t)p->Ppv * 10 / pv_v) : 0;
    /* 负载电流 0.1A: ARM OutputCurr 0.01A ÷10 (与 heartbeat ac[4] 同系数) */
    unsigned out_curr_a10 = (unsigned)(hb_clamp0(p->OutputCurr) / 10);
    /* App(10) 结构体原值 → 显示基准: 负载百分比已是 0.1% (DSP PloadPercent×10);
       AC 频率 0.1Hz ×10→0.01Hz (printf 两位小数); 电池电压 0.1V×10→0.01V;
       电池电流 0.1A×10→0.01A (printf 保持历史显示分辨率) */
    unsigned load_pct_a10   = (unsigned)p->LoadPercent;
    unsigned ac_freq_cHz    = (unsigned)p->ACOutputFreq * 10;
    unsigned bat_v_centi    = (unsigned)p->BatVolt * 10;
    int      bat_curr_centi = bat_curr * 10;

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
        (unsigned)p->ACOutputVolt, 0U,   /* VloadA 为 1V 单位 (UsartDsp.c:290 "单位1V") */
        out_curr_a10 / 10, out_curr_a10 % 10,
        (unsigned long)p->OutputWatt,
        (unsigned)(ac_freq_cHz / 100), (unsigned)(ac_freq_cHz % 100),
        (load_pct_a10 / 10), (load_pct_a10 % 10),
        (unsigned)p->BatterySOC,
        (unsigned)(bat_v_centi / 100), (unsigned)(bat_v_centi % 100),
        (int)(bat_curr_centi / 100),
        (int)(((bat_curr_centi < 0 ? -bat_curr_centi : bat_curr_centi) / 10) % 10),
        (long)((p->SysStatus & 0x08) ? -(long)p->BatDisChrWatt
                                     : (long)p->BatChrWatt),
        (p->SysStatus & 0x02) ? "fault"
            : (p->SysStatus & 0x08) ? "discharging"
            : (p->SysStatus & 0x04) ? "charging" : "standby",
        (unsigned)pv_v, 0U,
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
    /* No ARM exists in this project. The supplied DSP application protocol
     * has no identity query: never turn zeroed legacy records into V0.0,
     * rated power=0 or an assumed battery chemistry. */
    (void)o;
    if(!mqtt_is_connected())return;
    char buf[640];
    int n = snprintf(buf, sizeof(buf),
        "{\"sn\":\"%s\",\"model\":\"CS-L10-6K2\",\"manufacturer\":\"辰烁科技\","
        "\"firmware_arm\":null,\"firmware_esp\":\"%s\","
        "\"firmware_dsp\":null,\"firmware_bms\":null,"
        "\"device_type\":\"off_grid_inverter\",\"phase\":\"single\","
        "\"rated_power\":null,\"rated_voltage\":null,\"rated_frequency\":null,"
        "\"battery_nominal_voltage\":null,\"battery_type\":null,"
        "\"cell_count\":null,\"temp_sensor_count\":null,"
        "\"inverter_module\":null,\"hardware_version\":null,"
        "\"bootloader_version\":null,\"dsp_identity_available\":false,"
        "\"dsp_protocol\":\"direct_uart_03_04_25\"}",
        g_device_sn,FIRMWARE_VERSION);
    if (n <= 0 || n >= (int)sizeof(buf)) {
        ESP_LOGE(TAG, "info payload too large (%d)", n);
        return;
    }
    int result=mqtt_publish(TOPIC_INFO, buf, 0, 1, 0);
    if(result<0)ESP_LOGW(TAG,"direct controller info queue failed (%d)",result);
    else ESP_LOGW(TAG,"direct controller info queued; DSP identity unavailable in supplied protocol");
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

static void publish_config(const uint16_t *ctrl, const bool *valid, uint32_t rev)
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

    bool first=true;
    for (int i = 0; i < CTRL_PARAM_COUNT && n > 0 && n < (int)sizeof(buf); i++) {
        if(!valid[i])continue;
        uint16_t raw=ctrl[i];
        const char *sep = first ? "" : ",";first=false;
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
    /* 告警位量程护栏: L10 告警/故障码为低位位掩码。App(10) Warning 为 u64,
     * bit0~15 = warningCode1 (bit0 合并风扇告警), bit16+ 已由 ARM memset 恒 0;
     * 高 48 位非全 0 仍视为异常值, 跳过该项判断且不刷新基线, 防止垃圾位触发
     * 误告警 (与 heartbeat 发布 &0xFFFF 屏蔽一致, 作兜底保留); FaultValue 仅
     * 低 16 位有效, 判断不变; 合法值行为与原来完全一致。BmsWarning 为 u16
     * 全域, 不设护栏。 */
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
    ESP_LOGI(TAG, "init done");
}

void telemetry_on_mqtt_connected(void)
{
    uint32_t now = now_ms();
    /* 立即上报在线状态 */
    publish_online();
    s_last_online_ms = now;
    /* ESP identity is available without an ARM/DSP reply. */
    publish_info(NULL);
    /* First heartbeat is handled by tick after a fresh command04 snapshot. */
    s_hb_requested = true;
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
    publish_info(NULL);
    arm_poll_request_info();
}

void telemetry_request_config(void)
{
    /* 请求 ARM 重读控制参数; tick 检测到 last_ctrl_ms 变化后发布 (V2.1 §9) */
    s_config_requested = true;
    s_last_config_ms = 0; /* Reconnect/query must also report an unchanged revision. */
    arm_poll_request_config();
}

void telemetry_tick(void)
{
    s_now_ms = now_ms();

    RunParamDef rp;
    bool have_run = false;
    uint32_t received_ms = 0;
    OnlyReadDataDef info;
    bool report_info = false;
    uint32_t config_ms = 0;

    if (arm_poll_take_param()) {
        /* info 变化 → 发布一次 */
        if (g_arm_stat.last_info_ms != 0 && g_arm_stat.last_info_ms != s_last_info_ms) {
            s_last_info_ms = g_arm_stat.last_info_ms;
            info = g_only_read;
            report_info = true;
        }
        /* config: 有请求且 ARM 已重读 (last_ctrl_ms 变化) → 发布一次 */
        if (s_config_requested && g_arm_stat.last_ctrl_ms != 0 &&
            g_arm_stat.last_ctrl_ms != s_last_config_ms) {
            config_ms = g_arm_stat.last_ctrl_ms;
        }
        arm_poll_give_param();
    }
    /* Providers acquire the same nonrecursive mutex. Network publishing must
     * happen after release, using a coherent values/mask/revision snapshot. */
    if(report_info)publish_info(&info);
    if(config_ms){
        uint16_t values[CTRL_PARAM_COUNT];
        bool valid[CTRL_PARAM_COUNT];
        uint32_t revision;
        if(direct_config_snapshot_wire(values,valid,&revision)){
            publish_config(values,valid,revision);
            s_last_config_ms = config_ms;
            s_config_requested = false;
        }
        /* A busy snapshot leaves the request pending for the next tick. */
    }
    have_run = direct_dsp_get_measurement(&rp, &received_ms);

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
    if ((s_hb_requested && have_run) || (s_now_ms - s_last_hb_ms) >= hb_period_ms) {
        /* Keep an explicit request pending until a fresh measurement arrives. */
        s_hb_requested = s_hb_requested && !have_run;
        s_last_hb_ms = s_now_ms;

        if (arm_reachable) {
            s_arm_fail_count = 0;
            if (s_fault_reported) {
                s_fault_reported = false;
                publish_arm_recovery_alarm();
            }
        }
        if (arm_ok) {
            if (!s_quiet) publish_heartbeat(&rp);
            publish_ble_telemetry(&rp);   /* BLE 通道遥测推送（核心子集） */
        } else if (!arm_reachable) {
            s_arm_fail_count++;
            ESP_LOGW(TAG, "DSP no response (%lu/%d)",
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
