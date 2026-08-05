/*
 * cmd_handler.c — 云端命令 → WRITE_CTRLPARAM 地址映射
 *
 * V2.0 下行信封 (CS-L10-6K2_MQTT_上报协议设计_V2.0.md §11.2):
 *   {"v":2,"task_id":"...","t":...,"cmd":"set_charge_limit","args":[50]}
 *   兼容旧格式 {"cmd":"...","value":<number>} (及 name/action/param 别名)
 *
 * 映射表以 ARM 源码 CollectorParamAddr.h 索引为准, value 按各字段单位缩放:
 *  scale=10 → 值*0.1 单位 (如电流 0.1A, 电压 0.1V)
 *  scale=100 → 值*0.01 单位 (如频率 0.01Hz)
 *  scale=1 → 原始整数
 *
 * 响应 (cmd/response, QoS1, 信封内 data):
 *  {"task_id":..,"cmd":"<cmd>","result":"OK","err":0}              已排队下发
 *  {"task_id":..,"cmd":"<cmd>","result":"INVALID_ARGS","err":-2}  参数错误/未知命令
 *  {"task_id":..,"cmd":"<cmd>","result":"BUSY","err":-3}          ARM 链路忙
 *  {"task_id":..,"cmd":"<cmd>","result":"EXEC_FAILED","err":-1}  ARM 应答 err!=0 或超时 (补发)
 *  {"task_id":..,"cmd":"<cmd>","result":"NOT_SUPPORTED","err":-4} 未支持命令 (含 query_config 阶段 2)
 */
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include "esp_log.h"
#include "cJSON.h"
#include "config.h"
#include "cs_protocol.h"
#include "arm_param.h"
#include "arm_poll.h"
#include "mqtt_handler.h"
#include "cs_protocol_defs.h"
#include "telemetry.h"
#include "cmd_handler.h"

static const char *TAG = "CMD";

/* ---- 命令映射表 ---- */
typedef struct {
    const char *name;   /* 云端命令名 */
    uint16_t    addr;   /* 控制参数地址 */
    uint16_t    scale;  /* 值缩放: 实际写入 = value * scale */
} cmd_map_t;

static const cmd_map_t s_cmd_map[] = {
    { "output_priority",         INDEX_OUTPUT_PRIORITY,         1   },
    { "set_charge_limit",        INDEX_MAX_CHARGE_CURRENT_ALL,  10  }, /* 0.1A */
    { "ac_volt_range",           INDEX_ACIN_VOLT_RANGE,         1   },
    { "battery_capacity",        INDEX_BAT_CAPACITY,            1   },
    { "battery_type",            INDEX_BATTERY_TYPE,            1   },
    { "overload_restart",        INDEX_OVERLOAD_RESTART,        1   },
    { "high_temp_restart",       INDEX_HIGH_TEMP_RESTART,       1   },
    { "output_voltage",          INDEX_OUTPUT_VOLTAGE,          10  }, /* 0.1V */
    { "output_frequency",        INDEX_OUTPUT_FREQUENCY,        100 }, /* 0.01Hz */
    { "master_slave",            INDEX_MASTER_SLAVE,            1   },
    { "set_ac_charge_current",   INDEX_MAX_CHARGE_CITY_CURRENT, 10  }, /* 0.1A */
    { "low_volt_return_city",    INDEX_LOW_VOLT_RETURN_CITY,    10  },
    { "high_volt_return_bat",    INDEX_HIGH_VOLT_RETURN_BAT,    10  },
    { "bat_charge_priority",     INDEX_BAT_CHARGE_PRIORITY,     1   },
    { "alarm_control",           INDEX_ALARM_CONTROL,           1   },
    { "backlight_ctrl",          INDEX_BACKLIGHT_CTRL,          1   },
    { "power_shutdown_alarm",    INDEX_POWER_SHUTDOWN_ALARM,    1   },
    { "overload_use_city_power", INDEX_OVERLOAD_USE_CITY_POWER, 1   },
    { "max_dischg_curr",         INDEX_PARA_MAX_DISCHG_CURR,    10  }, /* 0.1A */
    { "max_chg_curr",            INDEX_PARA_MAX_CHG_CURR,       10  }, /* 0.1A */
    { "recover_threshold_volt",  INDEX_RECOVER_THRESHOLD_VOLT,  10  },
    { "solar_power_balance",     INDEX_SOLAR_POWER_BALANCE,     1   },
    { "ac_output_mode",          INDEX_AC_OUTPUT_MODE,          1   },
    { "li_bat_material",         INDEX_LI_BAT_MATERIAL,         1   },
    { "cell_serial_lifepo4",     INDEX_CELL_SERIAL_LIFEPO4,     1   },
    { "cell_serial_li_nmc",      INDEX_CELL_SERIAL_LI_NMC,      1   },
    { "eq_enable",               INDEX_EQ_ENABLE,               1   },
    { "eq_voltage_set",          INDEX_EQ_VOLTAGE_SET,          10  },
    { "eq_time_set",             INDEX_EQ_TIME_SET,             1   },
    { "eq_timeout_set",          INDEX_EQ_TIMEOUT_SET,          1   },
    { "eq_interval_set",         INDEX_EQ_INTERVAL_SET,         1   },
    { "eq_activate_set",         INDEX_EQ_ACTIVATE_SET,         1   },
    { "charge_time_set",         INDEX_CHARGE_TIME_SET,         1   },
    { "close_charge_time_set",   INDEX_CLOSE_CHARGE_TIME_SET,   1   },
    { "lvolt_open_gen",          INDEX_LVOLT_OPEN_GEN,          10  },
    { "hvolt_close_gen",         INDEX_HVOLT_CLOSE_GEN,         10  },
    { "soc_back_utl",            INDEX_SOC_BACK_UTL,            1   },
    { "soc_back_bat",            INDEX_SOC_BACK_BAT,            1   },
    { "soc_back_gen",            INDEX_SOC_BACK_GEN,            1   },
    { "soc_close_gen",           INDEX_SOC_CLOSE_GEN,           1   },
    { "set_soc_low",             INDEX_SOCCUTOFF,               1   },
    { "buzzer_en",               INDEX_BUZZER_EN,               1   },
    { "s_grade_soc",             INDEX_S_GRADE_SOC,             1   },
    { "a_grade_soc",             INDEX_A_GRADE_SOC,             1   },
    { "b_grade_soc",             INDEX_B_GRADE_SOC,             1   },
    { "manual_socket",           INDEX_MANUAL_SOCKET,           1   },
    { "control_socket",          INDEX_CONTROL_SOCKET,          1   },
    { "recover_threshold_soc",   INDEX_RECOVER_THRESHOLD_SOC,   1   },
    { "li_bat_protocol_type",    INDEX_LI_BAT_PROTOCOL_TYPE,    1   },
    { "gen_rate_watt",           INDEX_GEN_RATE_WATT,           1   },
    { "ge_balance_en",           INDEX_GE_BALANCE_EN,           1   },
    { "soc_max_utl_chg",         INDEX_SOC_MAX_UTL_CHG,         1   },
    { "vmax_utl_chg",            INDEX_VMAX_UTL_CHG,            10  },
};

#define CMD_MAP_SIZE (sizeof(s_cmd_map) / sizeof(s_cmd_map[0]))

/* ---- 响应发送 (信封内 data, 由 mqtt_publish 补信封) ---- */
static void send_response(const char *cmd, const char *result, int err,
                          const char *task_id)
{
    char buf[192];
    int n;
    if (task_id && task_id[0]) {
        n = snprintf(buf, sizeof(buf),
                     "{\"task_id\":\"%s\",\"cmd\":\"%s\",\"result\":\"%s\",\"err\":%d}",
                     task_id, cmd, result, err);
    } else {
        n = snprintf(buf, sizeof(buf),
                     "{\"cmd\":\"%s\",\"result\":\"%s\",\"err\":%d}",
                     cmd, result, err);
    }
    if (n > 0 && n < (int)sizeof(buf)) {
        mqtt_publish(PROTO_TOPIC_CMD_RESP, buf, 0, 1, 0);
    }
}

/* ---- 待确认命令 (ARM 写结果闭环, V2.0 §11.4) ---- */
typedef struct {
    bool     active;
    uint16_t addr;
    char     cmd[32];
    char     task_id[48];
} pending_cmd_t;

static pending_cmd_t s_pending;

/* ARM 写应答回调: 应答 err!=0 或超时 → 补发 EXEC_FAILED (err==0 时排队已回 OK) */
static void on_arm_write_done(uint8_t cmd, uint16_t param1, uint16_t param2,
                              int err)
{
    (void)param2;
    if (cmd != CS_CMD_WRITE_CTRLPARAM) {
        return;   /* UTC 等其它写由 sntp_sync 回调处理 */
    }
    if (!s_pending.active || s_pending.addr != param1) {
        return;
    }
    s_pending.active = false;
    if (err != 0) {
        ESP_LOGW(TAG, "cmd %s: ARM exec failed err=%d", s_pending.cmd, err);
        send_response(s_pending.cmd, PROTO_ERR_EXEC_FAILED, -1,
                      s_pending.task_id);
    }
}

static void handle_cmd(const char *cmd_name, double value, const char *task_id)
{
    const cmd_map_t *entry = NULL;
    int i;
    for (i = 0; i < (int)CMD_MAP_SIZE; i++) {
        if (strcmp(s_cmd_map[i].name, cmd_name) == 0) {
            entry = &s_cmd_map[i];
            break;
        }
    }
    if (!entry) {
        ESP_LOGW(TAG, "unknown cmd: %s", cmd_name);
        send_response(cmd_name, PROTO_ERR_NOT_SUPPORTED, -4, task_id);
        return;
    }

    /* 值域检查: 0 <= value*scale <= 65535 */
    if (value < 0 || value * entry->scale > 65535) {
        ESP_LOGW(TAG, "cmd %s value out of range: %f", cmd_name, value);
        send_response(cmd_name, "OUT_OF_RANGE", -5, task_id);
        return;
    }
    uint16_t raw = (uint16_t)(value * entry->scale);
    uint8_t data[2];
    data[0] = (uint8_t)(raw & 0xFF);
    data[1] = (uint8_t)(raw >> 8);

    int rc = arm_poll_send_write(CS_CMD_WRITE_CTRLPARAM, entry->addr, 2, data);
    if (rc != 0) {
        ESP_LOGW(TAG, "cmd %s: ARM busy", cmd_name);
        send_response(cmd_name, PROTO_ERR_BUSY, -3, task_id);
        return;
    }
    /* 记录待确认命令: ARM 应答失败时补发 EXEC_FAILED */
    s_pending.active = true;
    s_pending.addr = entry->addr;
    snprintf(s_pending.cmd, sizeof(s_pending.cmd), "%s", cmd_name);
    snprintf(s_pending.task_id, sizeof(s_pending.task_id), "%s",
             (task_id && task_id[0]) ? task_id : "");
    ESP_LOGI(TAG, "cmd %s addr=0x%04X raw=%u queued", cmd_name, entry->addr, raw);
    send_response(cmd_name, PROTO_ERR_OK, 0, task_id);
}

/* ---- 查询命令 (V2.0 §11.3, 无参数) ---- */
static bool handle_query_cmd(const char *cmd_name, const char *task_id)
{
    if (strcmp(cmd_name, PROTO_CMD_QUERY_TELEMETRY) == 0) {
        telemetry_request_heartbeat();
        send_response(cmd_name, PROTO_ERR_OK, 0, task_id);
        return true;
    }
    if (strcmp(cmd_name, PROTO_CMD_QUERY_INFO) == 0) {
        telemetry_request_info();
        send_response(cmd_name, PROTO_ERR_OK, 0, task_id);
        return true;
    }
    if (strcmp(cmd_name, PROTO_CMD_QUERY_CONFIG) == 0) {
        /* config 上报为阶段 2 (READ_CTRLPARAM 未实现), 暂不支持 */
        send_response(cmd_name, PROTO_ERR_NOT_SUPPORTED, -4, task_id);
        return true;
    }
    return false;
}

void cmd_handler_init(void)
{
    arm_poll_set_write_cb(on_arm_write_done);
    s_pending.active = false;
    ESP_LOGI(TAG, "init done (%d cmds)", (int)CMD_MAP_SIZE);
}

void cmd_handler_on_mqtt_message(const char *topic, const char *payload,
                                 unsigned int len)
{
    /* 只处理 cs_inv/{sn}/cmd (mqtt_handler 已按前缀过滤) */
    if (!topic || !payload || len == 0 || len > 1024) {
        return;
    }
    if (strstr(topic, "/cmd") == NULL ||
        strcmp(strrchr(topic, '/'), "/cmd") != 0) {
        return;
    }

    cJSON *root = cJSON_ParseWithLength(payload, len);
    if (!root) {
        ESP_LOGW(TAG, "bad JSON: %.*s", (int)len, payload);
        return;
    }

    cJSON *jcmd = cJSON_GetObjectItemCaseSensitive(root, "cmd");
    if (!jcmd) jcmd = cJSON_GetObjectItemCaseSensitive(root, "name");
    if (!jcmd) jcmd = cJSON_GetObjectItemCaseSensitive(root, "action");

    if (!cJSON_IsString(jcmd) || !jcmd->valuestring || !jcmd->valuestring[0]) {
        ESP_LOGW(TAG, "missing cmd name");
        cJSON_Delete(root);
        return;
    }

    /* task_id (V2 下行信封, 可选; 旧格式无 task_id 时省略) */
    cJSON *jtask = cJSON_GetObjectItemCaseSensitive(root, "task_id");
    const char *task_id = (cJSON_IsString(jtask) && jtask->valuestring)
                              ? jtask->valuestring : "";

    /* 值: value / param / args[0] 兼容 (V2 信封 args 数组) */
    cJSON *jval = cJSON_GetObjectItemCaseSensitive(root, "value");
    if (!jval) jval = cJSON_GetObjectItemCaseSensitive(root, "param");
    if (!jval) {
        cJSON *jargs = cJSON_GetObjectItemCaseSensitive(root, "args");
        if (cJSON_IsArray(jargs) && cJSON_GetArraySize(jargs) > 0) {
            cJSON *first = cJSON_GetArrayItem(jargs, 0);
            if (cJSON_IsNumber(first)) jval = first;
        }
    }

    /* 查询命令: 无需数值参数 */
    if (handle_query_cmd(jcmd->valuestring, task_id)) {
        cJSON_Delete(root);
        return;
    }

    if (!cJSON_IsNumber(jval)) {
        ESP_LOGW(TAG, "cmd %s: value must be number", jcmd->valuestring);
        send_response(jcmd->valuestring, PROTO_ERR_INVALID_ARGS, -2, task_id);
        cJSON_Delete(root);
        return;
    }

    handle_cmd(jcmd->valuestring, jval->valuedouble, task_id);
    cJSON_Delete(root);
}
