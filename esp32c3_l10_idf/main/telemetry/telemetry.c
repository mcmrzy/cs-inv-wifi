/*
 * telemetry.c — 遥测上报 (运行参数 → MQTT)
 *
 * V2.1 协议 (CS-L10-6K2_MQTT_上报协议设计_V2.1.md):
 *  - heartbeat: 57 值 {sys[11], pv[5], ac[11], chr[3], bat[5], eng[14], fan[2], diag[3], sock[3]}
 *    180s + 启动立即 + query_telemetry 补报; 实时快照未连接时直接丢弃
 *  - info: 设备能力与固件信息 (连接时/信息变化/query_info)
 *  - config: v2 语义键值对 (连接时/写入生效后/query_config, 42 键工程单位 + rev)
 *  - alarm: {source, code, level, state} 事件闭环 (QoS1)
 *  - status/LWT 由 mqtt_handler 负责, 本模块不再发布 data/status
 *
 * 数据源: arm_poll 全局 g_run_param / g_only_read / g_ctrl_param / g_arm_stat (mutex 保护)
 * 信封:   mqtt_publish() 自动拼接 cs_inv/{sn}/ 前缀 + {"t","v":2,"data":...}
 */
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <time.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "config.h"
#include "cs_protocol_defs.h"
#include "arm_param.h"
#include "arm_poll.h"
#include "mqtt_handler.h"
#include "telemetry.h"
#include "ble_ct_telemetry.h"
#include "ble_ct_clock.h"

static const char *TAG = "TELE";

/* 主题名 (不带前缀, 前缀 cs_inv/{sn}/ + 信封由 mqtt_publish 拼接) */
#define TOPIC_HEARTBEAT  PROTO_TOPIC_HEARTBEAT
#define TOPIC_INFO       PROTO_TOPIC_INFO
#define TOPIC_ALARM      PROTO_TOPIC_ALARM
#define TOPIC_CONFIG     PROTO_TOPIC_CONFIG

static uint32_t s_now_ms;
static uint32_t s_last_hb_ms;        /* V2 heartbeat 上次发布时刻 (180s) */
static uint32_t s_last_online_ms;    /* 在线状态上次发布时刻 (90s) */
static uint32_t s_last_info_ms;      /* 上次 info 发布时刻 (arm 时间戳) */
static uint32_t s_last_config_ms;    /* 上次 config 发布时刻 (arm 时间戳) */
static volatile bool s_hb_requested; /* query_telemetry 请求补报 */
static volatile bool s_config_requested; /* query_config/写入生效 请求补报 */
static bool     s_alarm_armed;       /* 首次收到运行参数后开始检测 */
static uint64_t s_last_warning;
static uint32_t s_last_fault;
static uint16_t s_last_bms;
static uint32_t s_arm_fail_count;    /* ARM 连续无应答计数 (180s 周期) */
static bool     s_fault_reported;    /* 故障已上报, 恢复前不重复触发 */

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

/* ---- heartbeat (V2.0 §6, QoS1, 仅ARM应答成功时调用) ---- */
static void publish_heartbeat(const RunParamDef *p)
{

    /* 带符号电池电流: bit2=Charge → +BatChgCurr; bit3=Discharging → -BatDischgCurr */
    int32_t bat_curr = 0;
    if (p->SysStatus & 0x04) {
        bat_curr = (int32_t)p->BatChgCurr;
    } else if (p->SysStatus & 0x08) {
        bat_curr = -(int32_t)p->BatDischgCurr;
    }

    /* 原始量纲整数 (含 0.1/0.01 缩放), 服务器按字段定义还原;
       sys[6]/sys[7] (变压器/PV 温度) ARM 未赋值 → null;
       fan/diag/sock 为 V2.1 新增组 (49 值旧固件无此三组, 服务端自适应) */
    char buf[768];
    int n = snprintf(buf, sizeof(buf),
        "{\"sys\":[%u,%lu,%" PRIu64 ",%u,%u,%u,null,null,%u,%u,%u],"
        "\"pv\":[%u,%u,%u,%u,%lu],"
        "\"ac\":[%u,%u,%lu,%lu,%u,%u,%u,%lu,%lu,%lu,%lu],"
        "\"chr\":[%lu,%lu,%u],"
        "\"bat\":[%u,%u,%" PRId32 ",%lu,%lu],"
        "\"eng\":[%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu],"
        "\"fan\":[%u,%u],"
        "\"diag\":[%u,%u,%lu],"
        "\"sock\":[%u,%u,%u]}",
        (unsigned)p->SysStatus,
        (unsigned long)p->FaultValue, p->Warning, (unsigned)p->BmsWarning,
        (unsigned)p->InvertTemp, (unsigned)p->BoostTemp,
        (unsigned)p->BusVolt, (unsigned)p->LoadPercent,
        (unsigned)p->BatOverCharge,
        (unsigned)p->Vpv1, (unsigned)p->Buck1Curr,
        (unsigned)p->Vpv2, (unsigned)p->Buck2Curr,
        (unsigned long)p->Ppv,
        (unsigned)p->ACOutputVolt, (unsigned)p->ACOutputFreq,
        (unsigned long)p->OutputWatt, (unsigned long)p->OutputVA,
        (unsigned)p->OutputCurr, (unsigned)p->GridVolt, (unsigned)p->GridFreq,
        (unsigned long)p->ACInWatt, (unsigned long)p->ACInVA,
        (unsigned long)p->ACDisChrWatt, (unsigned long)p->ACDisChrVA,
        (unsigned long)p->ACChrWatt, (unsigned long)p->ACChrVA,
        (unsigned)p->ACChrCurr,
        (unsigned)p->BatVolt, (unsigned)p->BatterySOC, bat_curr,
        (unsigned long)p->BatChrWatt, (unsigned long)p->BatDisChrWatt,
        (unsigned long)p->EGen_today, (unsigned long)p->EGen_total,
        (unsigned long)p->Epv_today, (unsigned long)p->Epv_total,
        (unsigned long)p->Eac_chrToday, (unsigned long)p->Eac_chrTotal,
        (unsigned long)p->Ebat_dischrToday, (unsigned long)p->Ebat_dischrTotal,
        (unsigned long)p->Ebat_chrToday, (unsigned long)p->Ebat_chrTotal,
        (unsigned long)p->Eac_dischrToday, (unsigned long)p->Eac_dischrTotal,
        (unsigned long)p->Eop_dischrToday, (unsigned long)p->Eop_dischrTotal,
        (unsigned)p->MpptFanSpeed, (unsigned)p->InvFanSpeed,
        (unsigned)p->InvCurr, (unsigned)p->ParaChgCurr,
        (unsigned long)p->WorkTimeTotal,
        (unsigned)p->PairedSocket, (unsigned)p->OnlineSocket,
        (unsigned)p->OnSocket);
    if (n <= 0 || n >= (int)sizeof(buf)) {
        ESP_LOGE(TAG, "heartbeat payload too large (%d)", n);
        return;
    }
    mqtt_publish(TOPIC_HEARTBEAT, buf, 0, 1, 0);
    ESP_LOGI(TAG, "heartbeat published (%d bytes)", n);
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

    /* 带符号电池电流: bit3=Discharge → 负 */
    int bat_curr = (p->SysStatus & 0x08)
                       ? -(int)p->BatDischgCurr
                       : (p->SysStatus & 0x04) ? (int)p->BatChgCurr : 0;
    /* PV 电流 (0.1A) = Ppv(0.1W) ×10 / Vpv1(0.1V)；Vpv1==0 时置 0 */
    unsigned pv_curr = (p->Vpv1 > 0) ? (unsigned)((uint32_t)p->Ppv * 10 / p->Vpv1) : 0;

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
        (unsigned long)(p->OutputWatt / 10),
        (unsigned)(p->ACOutputVolt / 10), (unsigned)(p->ACOutputVolt % 10),
        (unsigned)(p->OutputCurr / 10), (unsigned)(p->OutputCurr % 10),
        (unsigned long)(p->OutputWatt / 10),
        (unsigned)(p->ACOutputFreq / 100), (unsigned)(p->ACOutputFreq % 100),
        (unsigned)(p->LoadPercent / 10), (unsigned)(p->LoadPercent % 10),
        (unsigned)p->BatterySOC,
        (unsigned)(p->BatVolt / 100), (unsigned)(p->BatVolt % 100),
        (int)(bat_curr / 10), (int)((bat_curr < 0 ? -bat_curr : bat_curr) % 10),
        (long)((p->SysStatus & 0x08) ? -(long)(p->BatDisChrWatt / 10)
                                     : (long)(p->BatChrWatt / 10)),
        (p->SysStatus & 0x02) ? "fault"
            : (p->SysStatus & 0x08) ? "discharging"
            : (p->SysStatus & 0x04) ? "charging" : "standby",
        (unsigned)(p->Vpv1 / 10), (unsigned)(p->Vpv1 % 10),
        (unsigned)(pv_curr / 10), (unsigned)(pv_curr % 10),
        (unsigned long)(p->Ppv / 10),
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

    /* 额定参数: NomOpVolt 0.1V / NomOpFreq 0.01Hz / NomBatVolt 0.01V */
    char rated_v[16], rated_f[16], bat_nom[16];
    snprintf(rated_v, sizeof(rated_v), "%u.%u",
             (unsigned)(o->NomOpVolt / 10), (unsigned)(o->NomOpVolt % 10));
    snprintf(rated_f, sizeof(rated_f), "%u.%u",
             (unsigned)(o->NomOpFreq / 100), (unsigned)(o->NomOpFreq % 100));
    snprintf(bat_nom, sizeof(bat_nom), "%u.%02u",
             (unsigned)(o->NomBatVolt / 100), (unsigned)(o->NomBatVolt % 100));

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
    if (!s_alarm_armed) {
        s_alarm_armed = true;
        s_last_warning = p->Warning;
        s_last_fault = p->FaultValue;
        s_last_bms = p->BmsWarning;
        return;
    }

    alarm_evt_t evts[3];
    int nevt = 0;

    /* FaultValue (source=0, level=2): 0→非0 active; 非0→0 recovered (code 回填最后值) */
    if (p->FaultValue != s_last_fault) {
        evts[nevt].source = 0;
        evts[nevt].level  = 2;
        evts[nevt].code   = (p->FaultValue != 0) ? p->FaultValue : s_last_fault;
        evts[nevt].state  = (p->FaultValue != 0) ? 1 : 0;
        evts[nevt].prio   = 0;
        nevt++;
    }
    /* Warning (source=0, level=1, 64 位) */
    if (p->Warning != s_last_warning) {
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

    s_last_fault = p->FaultValue;
    s_last_warning = p->Warning;
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

void telemetry_reset_heartbeat_timer(void)
{
    s_last_hb_ms = now_ms();
    s_last_online_ms = now_ms();
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

    /* 180s V2 heartbeat (需ARM应答); query_telemetry 也可触发 */
    if (s_hb_requested || (s_now_ms - s_last_hb_ms) >= TELEMETRY_PERIOD_MS) {
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
