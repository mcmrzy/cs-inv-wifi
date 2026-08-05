/*
 * telemetry.c — 遥测上报 (运行参数 → MQTT)
 *
 * V2.0 协议 (CS-L10-6K2_MQTT_上报协议设计_V2.0.md):
 *  - heartbeat: 49 位置数组 {sys[11], pv[5], ac[11], chr[3], bat[5], eng[14]}
 *    180s + 启动立即 + query_telemetry 补报; 实时快照未连接时直接丢弃
 *  - info: 设备能力与固件信息 (连接时/信息变化/query_info)
 *  - alarm: {source, code, level, state} 事件闭环 (QoS1)
 *  - status/LWT 由 mqtt_handler 负责, 本模块不再发布 data/status
 *
 * 数据源: arm_poll 全局 g_run_param / g_only_read / g_arm_stat (mutex 保护)
 * 信封:   mqtt_publish() 自动拼接 cs_inv/{sn}/ 前缀 + {"t","v":2,"data":...}
 */
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "config.h"
#include "cs_protocol_defs.h"
#include "arm_param.h"
#include "arm_poll.h"
#include "mqtt_handler.h"
#include "telemetry.h"

static const char *TAG = "TELE";

/* 主题名 (不带前缀, 前缀 cs_inv/{sn}/ + 信封由 mqtt_publish 拼接) */
#define TOPIC_HEARTBEAT  PROTO_TOPIC_HEARTBEAT
#define TOPIC_INFO       PROTO_TOPIC_INFO
#define TOPIC_ALARM      PROTO_TOPIC_ALARM

static uint32_t s_now_ms;
static uint32_t s_last_hb_ms;        /* heartbeat 上次发布时刻 */
static uint32_t s_last_info_ms;      /* 上次 info 发布时刻 (arm 时间戳) */
static volatile bool s_hb_requested; /* query_telemetry 请求补报 */
static bool     s_alarm_armed;       /* 首次收到运行参数后开始检测 */
static uint64_t s_last_warning;
static uint32_t s_last_fault;
static uint16_t s_last_bms;

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

/* ---- heartbeat (V2.0 §6, QoS1) ---- */
static void publish_heartbeat(const RunParamDef *p)
{
    /* 实时快照: 未连接时丢弃, 不进入离线缓存 (V2.0 §4 上报失败不补发过期快照) */
    if (!mqtt_is_connected()) {
        ESP_LOGW(TAG, "heartbeat dropped (mqtt offline)");
        return;
    }

    /* 带符号电池电流: bit2=Charge → +BatChgCurr; bit3=Discharging → -BatDischgCurr */
    int32_t bat_curr = 0;
    if (p->SysStatus & 0x04) {
        bat_curr = (int32_t)p->BatChgCurr;
    } else if (p->SysStatus & 0x08) {
        bat_curr = -(int32_t)p->BatDischgCurr;
    }

    /* 原始量纲整数 (含 0.1/0.01 缩放), 服务器按字段定义还原;
       sys[6]/sys[7] (变压器/PV 温度) ARM 未赋值 → null */
    char buf[640];
    int n = snprintf(buf, sizeof(buf),
        "{\"sys\":[%u,%lu,%" PRIu64 ",%u,%u,%u,null,null,%u,%u,%u],"
        "\"pv\":[%u,%u,%u,%u,%lu],"
        "\"ac\":[%u,%u,%lu,%lu,%u,%u,%u,%lu,%lu,%lu,%lu],"
        "\"chr\":[%lu,%lu,%u],"
        "\"bat\":[%u,%u,%" PRId32 ",%lu,%lu],"
        "\"eng\":[%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu]}",
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
        (unsigned long)p->Eop_dischrToday, (unsigned long)p->Eop_dischrTotal);
    if (n <= 0 || n >= (int)sizeof(buf)) {
        ESP_LOGE(TAG, "heartbeat payload too large (%d)", n);
        return;
    }
    mqtt_publish(TOPIC_HEARTBEAT, buf, 0, 1, 0);
    ESP_LOGI(TAG, "heartbeat published (%d bytes)", n);
}

/* ---- info (V2.0 §7, QoS1) ---- */
static void publish_info(const OnlyReadDataDef *o)
{
    char arm_ver[16], dsp_ver[16], hw_ver[16], bl_ver[16];
    fmt_ver(arm_ver, sizeof(arm_ver), o->ARMFirmwareVersion);
    fmt_ver(dsp_ver, sizeof(dsp_ver), o->DSPFirmwareVersion);
    fmt_ver(hw_ver, sizeof(hw_ver), o->HardwareVersion);
    fmt_ver(bl_ver, sizeof(bl_ver), o->BLVersion);

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
        "{\"model\":\"CS-L10-6K2\",\"manufacturer\":\"辰烁科技\","
        "\"firmware_arm\":\"%s\",\"firmware_esp\":\"%s\","
        "\"firmware_dsp\":\"%s\",\"firmware_bms\":null,"
        "\"device_type\":\"off_grid_inverter\",\"phase\":\"single\","
        "\"rated_power\":%u,\"rated_voltage\":%s,\"rated_frequency\":%s,"
        "\"battery_nominal_voltage\":%s,\"battery_type\":\"LiFePO4\","
        "\"cell_count\":0,\"temp_sensor_count\":0,"
        "\"inverter_module\":\"%s\",\"hardware_version\":\"%s\","
        "\"bootloader_version\":\"%s\"}",
        arm_ver, FIRMWARE_VERSION, dsp_ver,
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
    s_last_info_ms = 0;
    s_hb_requested = false;
    s_alarm_armed = false;
    s_last_warning = 0;
    s_last_fault = 0;
    s_last_bms = 0;
    ESP_LOGI(TAG, "init done");
}

void telemetry_on_mqtt_connected(void)
{
    /* 已有 ARM 数据则立即上报 info + heartbeat (V2.0 §4 启动立即); 并请求重读 */
    if (arm_poll_take_param()) {
        if (g_arm_stat.last_info_ms != 0) {
            publish_info(&g_only_read);
        }
        if (g_arm_stat.poll_count > 0) {
            publish_heartbeat(&g_run_param);
        }
        arm_poll_give_param();
    }
    arm_poll_request_info();
}

void telemetry_request_heartbeat(void)
{
    s_hb_requested = true;   /* tick 内立即补报并重置周期 */
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
        if (g_arm_stat.poll_count > 0) {
            rp = g_run_param;
            have_run = true;
        }
        arm_poll_give_param();
    }

    if (!have_run) {
        return;    /* 尚未收到运行参数 */
    }

    /* 180s 心跳 + query_telemetry 立即补报 (V2.0 §4) */
    if (s_hb_requested || (s_now_ms - s_last_hb_ms) >= TELEMETRY_PERIOD_MS) {
        s_hb_requested = false;
        s_last_hb_ms = s_now_ms;
        publish_heartbeat(&rp);
    }

    /* 告警变化检测 (每次 tick, 不等心跳周期) */
    check_alarm(&rp);
}
