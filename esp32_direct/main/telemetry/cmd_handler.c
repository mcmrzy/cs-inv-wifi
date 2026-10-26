/*
 * cmd_handler.c — 云端命令 → WRITE_CTRLPARAM 地址映射
 *
 * V2.1 下行信封 (CS-L10-6K2_MQTT_上报协议设计_V2.1.md §11.2):
 *   {"v":2,"task_id":"...","t":...,"cmd":"set_charge_limit","args":[50]}
 *   兼容旧格式 {"cmd":"...","value":<number>} (及 name/action/param 别名)
 *
 * 映射表以 ARM 源码 CollectorParamAddr.h 索引为准, value 按各字段单位缩放:
 *  scale=10 → 值*0.1 单位 (如电流 0.1A, 电压 0.1V)
 *  scale=100 → 值*0.01 单位 (如频率 0.01Hz)
 *  scale=1 → 原始整数
 *
 * 响应 (cmd/response, QoS1, 信封内 data, V2.1 §11.4):
 *  {"task_id":..,"cmd":"<cmd>","stage":"acknowledged","result":"OK","err":0}
 *  {"task_id":..,"cmd":"<cmd>","stage":"submitted","result":"OK","err":0,
 *   "submitted_args":[<工程单位>],"desired_revision":<CtrlParamAlterTime>}
 *  submitted 仅证明 ESP 保存及串口提交，不证明 DSP 生效。
 *  {"task_id":..,"cmd":"<cmd>","result":"INVALID_ARGS","err":-2}  参数错误/未知命令
 *  {"task_id":..,"cmd":"<cmd>","result":"BUSY","err":-3}          ARM 链路忙
 *  {"task_id":..,"cmd":"<cmd>","result":"EXEC_FAILED","err":-1}  ARM 应答 err!=0 或超时 (补发)
 *  {"task_id":..,"cmd":"<cmd>","result":"NOT_SUPPORTED","err":-4} 未支持命令
 */
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <math.h>
#include <time.h>
#include <stddef.h>
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "sntp_sync.h"
#include "mbedtls/sha256.h"
#include "esp_log.h"
#include "cJSON.h"
#include "config.h"
#include "cs_protocol.h"
#include "arm_param.h"
#include "arm_poll.h"
#include "direct_link.h"
#include "direct_battery_config.h"
#include "mqtt_handler.h"
#include "cs_protocol_defs.h"
#include "telemetry.h"
#include "cmd_handler.h"
#include "ble_ct_cmd.h"

static const char *TAG = "CMD";

/* ---- 命令映射表 ---- */
typedef struct {
    const char *name;   /* 云端命令名 */
    uint16_t    addr;   /* 控制参数地址 */
    uint16_t    scale;  /* 值缩放: 实际写入 = value * scale */
} cmd_map_t;

static const cmd_map_t s_cmd_map[] = {
    /* V2.1: 与 config 上报的 42 个键及云端 command_code 保持一致。 */
    { "set_output_priority",       INDEX_OUTPUT_PRIORITY,         1   },
    { "set_max_charge_current",    INDEX_MAX_CHARGE_CURRENT_ALL,  10  },
    { "set_ac_volt_range",         INDEX_ACIN_VOLT_RANGE,         1   },
    { "set_battery_capacity",      INDEX_BAT_CAPACITY,            1   },
    { "set_battery_type",          INDEX_BATTERY_TYPE,            1   },
    { "set_overload_restart",      INDEX_OVERLOAD_RESTART,        1   },
    { "set_high_temp_restart",     INDEX_HIGH_TEMP_RESTART,       1   },
    { "set_output_voltage",        INDEX_OUTPUT_VOLTAGE,          10  },
    { "set_output_frequency",      INDEX_OUTPUT_FREQUENCY,        100 },
    { "set_master_slave",          INDEX_MASTER_SLAVE,            1   },
    { "set_ac_charge_current",     INDEX_MAX_CHARGE_CITY_CURRENT, 10  },
    { "set_low_volt_return_utl",   INDEX_LOW_VOLT_RETURN_CITY,    10  },
    { "set_high_volt_return_bat",  INDEX_HIGH_VOLT_RETURN_BAT,    10  },
    { "set_charge_priority",       INDEX_BAT_CHARGE_PRIORITY,     1   },
    { "set_alarm_control",         INDEX_ALARM_CONTROL,           1   },
    { "set_backlight_ctrl",        INDEX_BACKLIGHT_CTRL,          1   },
    { "set_power_shutdown_alarm", INDEX_POWER_SHUTDOWN_ALARM,    1   },
    { "set_overload_use_city_power", INDEX_OVERLOAD_USE_CITY_POWER, 1 },
    { "set_max_discharge_current", INDEX_PARA_MAX_DISCHG_CURR,   10  },
    { "set_max_chg_curr",          INDEX_PARA_MAX_CHG_CURR,       10  },
    { "set_recover_threshold_volt", INDEX_RECOVER_THRESHOLD_VOLT, 10 },
    { "set_solar_power_balance",   INDEX_SOLAR_POWER_BALANCE,     1   },
    { "set_ac_output_mode",        INDEX_AC_OUTPUT_MODE,          1   },
    { "set_li_bat_material",       INDEX_LI_BAT_MATERIAL,         1   },
    { "set_cell_serial_lifepo4",   INDEX_CELL_SERIAL_LIFEPO4,     1   },
    { "set_cell_serial_li_nmc",    INDEX_CELL_SERIAL_LI_NMC,      1   },
    { "set_equalize_enable",       INDEX_EQ_ENABLE,               1   },
    { "set_equalize_voltage",      INDEX_EQ_VOLTAGE_SET,          10  },
    { "set_equalize_time",         INDEX_EQ_TIME_SET,             1   },
    { "set_equalize_timeout",      INDEX_EQ_TIMEOUT_SET,         1   },
    { "set_equalize_interval",     INDEX_EQ_INTERVAL_SET,        1   },
    { "set_equalize_activate",     INDEX_EQ_ACTIVATE_SET,        1   },
    { "set_charge_time",           INDEX_CHARGE_TIME_SET,         1   },
    { "set_close_charge_time",     INDEX_CLOSE_CHARGE_TIME_SET,   1   },
    { "set_gen_start_voltage",     INDEX_LVOLT_OPEN_GEN,          10  },
    { "set_gen_stop_voltage",      INDEX_HVOLT_CLOSE_GEN,         10  },
    { "set_soc_back_utl",          INDEX_SOC_BACK_UTL,            1   },
    { "set_soc_back_bat",          INDEX_SOC_BACK_BAT,            1   },
    { "set_soc_back_gen",          INDEX_SOC_BACK_GEN,            1   },
    { "set_soc_close_gen",         INDEX_SOC_CLOSE_GEN,           1   },
    { "set_soc_cutoff",            INDEX_SOCCUTOFF,               1   },
    { "set_buzzer",                INDEX_BUZZER_EN,               1   },

    /* 保留旧命令名，兼容已部署的 BLE 客户端与旧云端消息。 */
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

/* Bounded, boot-local replay protection. Never evict a live task or an
 * unexpired result: capacity pressure must not turn QoS1 into a second write. */
#define CLOUD_REPLAY_COUNT 8
#define CLOUD_REPLAY_TTL_MS (10LL * 60 * 1000)
#define CLOUD_TASK_MAX 64
#define CLOUD_CMD_MAX 47
#define CLOUD_RESPONSE_SIZE 640
typedef enum {CLOUD_ACK, CLOUD_COMPLETED, CLOUD_FAILED, CLOUD_SUBMITTED, CLOUD_DEBUG, CLOUD_BATTERY_READ, CLOUD_BATTERY_SUBMITTED} cloud_stage_t;
typedef struct {
    const char *result; /* Internal static result strings only. */
    int err;
    cloud_stage_t stage;
    double value;
    uint32_t revision, effective;
    int64_t expires_at;
} cloud_receipt_t;
typedef struct {
    bool used;
    bool terminal;
    int64_t completed_ms;
    char task_id[CLOUD_TASK_MAX + 1];
    unsigned char digest[32];
    cloud_receipt_t receipt;
} cloud_replay_t;
static cloud_replay_t s_cloud_replay[CLOUD_REPLAY_COUNT];
static portMUX_TYPE s_cloud_lock = portMUX_INITIALIZER_UNLOCKED;
typedef struct {
    char payload[1025];
    unsigned len;
    bool retained;
} cloud_input_t;
static cloud_input_t s_cloud_input[CMD_HANDLER_QUEUE_DEPTH];
static unsigned s_cloud_head, s_cloud_count;
static bool s_cloud_consumer;
static cmd_handler_stats_t s_cloud_stats;

/* Single MQTT/BLE schema. All fields are required; native units are exact
 * integer decivolts, amps and SOC percent. Never route through scalar writes. */
static const struct { const char *name; size_t offset; } s_battery_fields[] = {
#define BATTERY_FIELD(name) {#name, offsetof(direct_battery_config_t, name)}
    BATTERY_FIELD(type), BATTERY_FIELD(protocol), BATTERY_FIELD(material), BATTERY_FIELD(cells),
    BATTERY_FIELD(bulk_dv), BATTERY_FIELD(float_dv), BATTERY_FIELD(cutoff_dv),
    BATTERY_FIELD(max_charge_a), BATTERY_FIELD(ac_charge_a),
    BATTERY_FIELD(back_utility_dv), BATTERY_FIELD(back_battery_dv),
    BATTERY_FIELD(soc_low), BATTERY_FIELD(soc_high), BATTERY_FIELD(soc_cutoff),
#undef BATTERY_FIELD
};

static int battery_parse(const cJSON *json, direct_battery_config_t *out)
{
    if (!cJSON_IsObject(json)) return -10;
    memset(out, 0, sizeof(*out));
    unsigned seen = 0;
    for (const cJSON *item = json->child; item; item = item->next) {
        unsigned i;
        if (!item->string) return -10;
        for (i=0; i<DIRECT_BATTERY_FIELD_COUNT; ++i)
            if (!strcmp(item->string, s_battery_fields[i].name)) break;
        if (i==DIRECT_BATTERY_FIELD_COUNT && strcmp(item->string,"revision")) return -10;
        if ((seen & (1u<<i)) || !cJSON_IsNumber(item) || !isfinite(item->valuedouble) ||
            floor(item->valuedouble)!=item->valuedouble || item->valuedouble<0 ||
            item->valuedouble>(i==DIRECT_BATTERY_FIELD_COUNT ? (double)UINT32_MAX : (double)UINT16_MAX)) return -10;
        seen |= 1u<<i;
        if (i==DIRECT_BATTERY_FIELD_COUNT) out->revision=(uint32_t)item->valuedouble;
        else { uint16_t value=(uint16_t)item->valuedouble; memcpy((char *)out+s_battery_fields[i].offset,&value,sizeof(value)); }
    }
    if (seen != (1u<<(DIRECT_BATTERY_FIELD_COUNT+1u))-1u) return -10;
    direct_battery_result_t rc=direct_battery_validate(out);
    return rc==DIRECT_BATTERY_OK ? 0 : rc==DIRECT_BATTERY_PROTOCOL_UNSUPPORTED ? -15 :
           rc==DIRECT_BATTERY_PROFILE_REQUIRED ? -16 : -10;
}

static cJSON *battery_json(const direct_battery_config_t *config)
{
    cJSON *json=cJSON_CreateObject();
    if (!json) return NULL;
    for (unsigned i=0;i<DIRECT_BATTERY_FIELD_COUNT;++i) {
        uint16_t value;memcpy(&value,(const char *)config+s_battery_fields[i].offset,sizeof(value));
        if (!cJSON_AddNumberToObject(json,s_battery_fields[i].name,value)) { cJSON_Delete(json);return NULL; }
    }
    if (!cJSON_AddNumberToObject(json,"revision",config->revision)) { cJSON_Delete(json);return NULL; }
    return json;
}

cJSON *cmd_handler_get_battery_config(void)
{
    direct_battery_config_t config;
    return direct_battery_config_get(&config) ? battery_json(&config) : NULL;
}

static const char *battery_error(int rc)
{
    return rc==-14 ? "REVISION_CONFLICT" : rc==-15 ? "PROTOCOL_UNSUPPORTED" :
           rc==-16 ? "PROFILE_REQUIRED" : rc==ARM_WRITE_INVALID ? PROTO_ERR_INVALID_ARGS :
           rc==ARM_WRITE_UNSUPPORTED ? PROTO_ERR_NOT_SUPPORTED : rc==ARM_WRITE_UPGRADE_BUSY ? "UPGRADE_BUSY" :
           rc==ARM_WRITE_NOT_READY ? "NOT_READY" : rc==ARM_WRITE_SAVE_FAILED ? "STORAGE_FAILED" :
           rc==ARM_WRITE_SUBMIT_FAILED ? "SUBMIT_FAILED" : PROTO_ERR_BUSY;
}

static bool escape_json_string(char *out, size_t capacity, const char *value)
{
    size_t used = 0;
    for (const unsigned char *p = (const unsigned char *)(value ? value : ""); *p; ++p) {
        size_t need = *p < 32 ? 6 : (*p == '"' || *p == '\\' ? 2 : 1);
        if (used + need >= capacity) return false;
        if (*p < 32) {
            snprintf(out + used, capacity - used, "\\u%04x", *p);
        } else {
            if (need == 2) out[used++] = '\\';
            out[used] = (char)*p;
        }
        used += *p < 32 ? 6 : 1;
    }
    out[used] = '\0';
    return true;
}

static bool command_identifier_valid(const char *value, size_t maximum)
{
    if (strlen(value) > maximum) return false;
    /* Keep receipt bounds provable, including errors under heap exhaustion. */
    for (const unsigned char *p = (const unsigned char *)value; *p; ++p)
        if (*p < 32) return false;
    return true;
}

/* Finalizing a replay entry must work even when the heap is exhausted. */
static void send_receipt_data(const char *cmd, const char *task_id,
                          const cloud_receipt_t *receipt, bool remember,
                          const direct_battery_config_t *battery)
{
    char escaped_cmd[CLOUD_CMD_MAX * 2 + 1];
    char escaped_task[CLOUD_TASK_MAX * 2 + 1];
    char body[CLOUD_RESPONSE_SIZE];
    static const char *const stages[]={"acknowledged","completed","failed","submitted","completed","completed","submitted"};
    if (!escape_json_string(escaped_cmd, sizeof(escaped_cmd), cmd) ||
        !escape_json_string(escaped_task, sizeof(escaped_task), task_id)) return;
    int n = snprintf(body, sizeof(body),
                      "{\"cmd\":\"%s\",\"result\":\"%s\",\"err\":%d,\"stage\":\"%s\"%s%s%s}",
                      escaped_cmd, receipt->result, receipt->err, stages[receipt->stage],
                      task_id && task_id[0] ? ",\"task_id\":\"" : "",
                      task_id && task_id[0] ? escaped_task : "",
                      task_id && task_id[0] ? "\"" : "");
    if (n <= 0 || n >= (int)sizeof(body)) return;
    char extras[384]={0};
    if(receipt->stage==CLOUD_SUBMITTED)
        snprintf(extras,sizeof(extras),",\"submitted_args\":[%.17g],\"desired_revision\":%lu",receipt->value,(unsigned long)receipt->revision);
    else if(receipt->stage==CLOUD_DEBUG)
        snprintf(extras,sizeof(extras),",\"effective_interval_seconds\":%lu,\"expires_at\":%lld",(unsigned long)receipt->effective,(long long)receipt->expires_at);
    else if(receipt->stage==CLOUD_BATTERY_SUBMITTED)
        snprintf(extras,sizeof(extras),",\"saved\":true,\"submitted\":true,\"observed\":false,\"desired_revision\":%lu",(unsigned long)receipt->revision);
    else if(receipt->stage==CLOUD_BATTERY_READ) {
        if (!battery) return;
        const direct_battery_config_t *b=battery;
        snprintf(extras,sizeof(extras),",\"config\":{\"type\":%u,\"protocol\":%u,\"material\":%u,\"cells\":%u,\"bulk_dv\":%u,\"float_dv\":%u,\"cutoff_dv\":%u,\"max_charge_a\":%u,\"ac_charge_a\":%u,\"back_utility_dv\":%u,\"back_battery_dv\":%u,\"soc_low\":%u,\"soc_high\":%u,\"soc_cutoff\":%u,\"revision\":%lu}",
            b->type,b->protocol,b->material,b->cells,b->bulk_dv,b->float_dv,b->cutoff_dv,b->max_charge_a,b->ac_charge_a,b->back_utility_dv,b->back_battery_dv,b->soc_low,b->soc_high,b->soc_cutoff,(unsigned long)b->revision);
    }
    if (extras[0] && strlen(extras) + (size_t)n < sizeof(body)) {
        snprintf(body + n - 1, sizeof(body) - (size_t)n + 1, "%s}", extras);
    }
    if(remember && task_id && task_id[0]){
        portENTER_CRITICAL(&s_cloud_lock);
        for(unsigned i=0;i<CLOUD_REPLAY_COUNT;i++){
            cloud_replay_t *e=&s_cloud_replay[i];
            if(e->used && !strcmp(e->task_id,task_id)){
                e->receipt=*receipt;e->terminal=receipt->stage!=CLOUD_ACK;
                if(e->terminal)e->completed_ms=esp_timer_get_time()/1000;
                break;
            }
        }
        portEXIT_CRITICAL(&s_cloud_lock);
    }
    mqtt_publish(PROTO_TOPIC_CMD_RESP,body,0,1,0);
}

static void send_receipt(const char *cmd, const char *task_id,
                          const cloud_receipt_t *receipt, bool remember)
{
    send_receipt_data(cmd,task_id,receipt,remember,NULL);
}

/* ---- 响应发送 (信封内 data, 由 mqtt_publish 补信封) ---- */
static void send_response(const char *cmd, const char *result, int err,
                          const char *task_id)
{
    cloud_receipt_t receipt={.result=result,.err=err,.stage=err?CLOUD_FAILED:CLOUD_COMPLETED};
    send_receipt(cmd,task_id,&receipt,true);
}

/* This callback proves desired-state save and UART submission only. */
static void send_response_ok(const char *cmd, double value, const char *task_id)
{
    if (!strcmp(cmd,"set_battery_config")) {
        direct_battery_config_t config;
        if (direct_battery_config_get(&config)) {
            cloud_receipt_t receipt={.result=PROTO_ERR_OK,.stage=CLOUD_BATTERY_SUBMITTED,.revision=config.revision};
            send_receipt(cmd,task_id,&receipt,true);
        } else send_response(cmd,"RESULT_UNAVAILABLE",-3,task_id);
        return;
    }
    uint32_t rev = 0;
    if (arm_poll_take_param()) {
        rev = g_read_write.CtrlParamAlterTime;
        arm_poll_give_param();
    }

    cloud_receipt_t receipt={.result=PROTO_ERR_OK,.stage=CLOUD_SUBMITTED,.value=value,.revision=rev};
    send_receipt(cmd,task_id,&receipt,true);
}

/* Hash effective arguments in a fixed order, excluding transport metadata. */
static bool cloud_digest(const char *cmd, const cJSON *root,
                           const cJSON *value, unsigned char digest[32])
{
    cJSON *intent = cJSON_CreateArray();
    bool ok = intent && cJSON_AddItemToArray(intent, cJSON_CreateString(cmd));
    const cJSON *args = (!strcmp(cmd, PROTO_CMD_SET_DEBUG_TELEMETRY) || !strcmp(cmd,"set_battery_config"))
                        ? cJSON_GetObjectItemCaseSensitive(root, "args") : value;
    if (ok && args) {
        direct_battery_config_t config;
        cJSON *copy=!strcmp(cmd,"set_battery_config") && battery_parse(args,&config)==0 ? battery_json(&config) : cJSON_Duplicate(args,true);
        if (!copy || !cJSON_AddItemToArray(intent,copy)) { cJSON_Delete(copy);ok=false; }
    }
    char *canonical = ok ? cJSON_PrintUnformatted(intent) : NULL;
    ok = canonical && mbedtls_sha256((const unsigned char *)canonical,
                                     strlen(canonical), digest, 0) == 0;
    cJSON_free(canonical);
    cJSON_Delete(intent);
    return ok;
}

/* Lookup happens before new-task deadline validation: a completed task keeps
 * its result even after its original transport deadline has elapsed. */
static bool cloud_replay(const char *cmd,const char *task_id,const unsigned char digest[32])
{
    if(!task_id[0])return false;
    cloud_receipt_t receipt={0};bool found=false,conflict=false;
    int64_t now=esp_timer_get_time()/1000;
    portENTER_CRITICAL(&s_cloud_lock);
    for(unsigned i=0;i<CLOUD_REPLAY_COUNT;i++){
        cloud_replay_t *e=&s_cloud_replay[i];
        if(e->used && e->terminal && now-e->completed_ms>=CLOUD_REPLAY_TTL_MS)e->used=false;
        if(e->used && !strcmp(e->task_id,task_id)){
            found=true;conflict=memcmp(e->digest,digest,32)!=0;receipt=e->receipt;break;
        }
    }
    portEXIT_CRITICAL(&s_cloud_lock);
    if(conflict)receipt=(cloud_receipt_t){.result="IDEMPOTENCY_CONFLICT",.err=-6,.stage=CLOUD_FAILED};
    if(found)send_receipt(cmd,task_id,&receipt,false);
    return found;
}

static bool cloud_admit(const char *cmd, const char *task_id,const unsigned char digest[32])
{
    if(!task_id[0])return true;
    int slot = -1;
    int64_t now = esp_timer_get_time() / 1000;
    portENTER_CRITICAL(&s_cloud_lock);
    for (unsigned i = 0; i < CLOUD_REPLAY_COUNT; ++i) {
        cloud_replay_t *e = &s_cloud_replay[i];
        if (e->used && e->terminal && now - e->completed_ms >= CLOUD_REPLAY_TTL_MS)
            e->used = false;
        if (!e->used && slot<0)slot=(int)i;
    }
    cloud_receipt_t receipt={.result=PROTO_ERR_OK,.stage=CLOUD_ACK};
    if (slot >= 0) {
        cloud_replay_t *e = &s_cloud_replay[slot];
        memset(e, 0, sizeof(*e));
        e->used = true;
        memcpy(e->digest, digest, 32);
        snprintf(e->task_id, sizeof(e->task_id), "%s", task_id);
        e->receipt=receipt;
    }else s_cloud_stats.cache_full++;
    portEXIT_CRITICAL(&s_cloud_lock);
    if (slot < 0) {
        receipt=(cloud_receipt_t){.result=PROTO_ERR_BUSY,.err=-3,.stage=CLOUD_FAILED};
        send_receipt(cmd,task_id,&receipt,false);
        return false;
    }
    send_receipt(cmd,task_id,&receipt,false);
    return true;
}

static void cloud_forget(const char *task_id)
{
    if(!task_id || !task_id[0])return;
    portENTER_CRITICAL(&s_cloud_lock);
    for(unsigned i=0;i<CLOUD_REPLAY_COUNT;i++)
        if(s_cloud_replay[i].used && !strcmp(s_cloud_replay[i].task_id,task_id))s_cloud_replay[i].used=false;
    portEXIT_CRITICAL(&s_cloud_lock);
}

static bool command_deadline_valid(const cJSON *root, const char *cmd, const char *task_id)
{
    const cJSON *deadline = cJSON_GetObjectItemCaseSensitive(root, "expires_at");
    if (!deadline) return true; /* Old producers remain compatible. */
    if (!cJSON_IsNumber(deadline) || !isfinite(deadline->valuedouble) ||
        deadline->valuedouble <= 0 || deadline->valuedouble > 9007199254740991.0 ||
        floor(deadline->valuedouble) != deadline->valuedouble) {
        send_response(cmd, PROTO_ERR_INVALID_ARGS, -2, task_id);
        return false;
    }
    time_t now = time(NULL);
    if (!sntp_sync_is_synced() || now < 1704067200) {
        send_response(cmd, "CLOCK_UNSYNCED", -7, task_id);
        return false;
    }
    if ((double)now >= deadline->valuedouble) {
        portENTER_CRITICAL(&s_cloud_lock);s_cloud_stats.expired++;portEXIT_CRITICAL(&s_cloud_lock);
        send_response(cmd, "EXPIRED", -8, task_id);
        return false;
    }
    return true;
}

/* ---- 待确认命令 (ARM 写结果闭环, V2.1 §11.4) ---- */
typedef struct {
    bool     active;
    uint16_t addr;
    char     cmd[CLOUD_CMD_MAX + 1];
    char     task_id[CLOUD_TASK_MAX + 1];
    double value;
    bool battery;
} pending_cmd_t;

static pending_cmd_t s_pending;

typedef struct {
    bool active;
    uint32_t token;
    uint16_t addr;
    uint16_t expected_raw;
    uint16_t scale;
    char param_id[32];
    bool battery;
} pending_ble_cmd_t;

static pending_ble_cmd_t s_ble_pending;

int cmd_handler_write_battery_config(const cJSON *json, bool validate_only)
{
    direct_battery_config_t config;
    int rc=battery_parse(json,&config);
    if (rc || validate_only) return rc;
    portENTER_CRITICAL(&s_cloud_lock);
    if (s_ble_pending.active || s_pending.active) { portEXIT_CRITICAL(&s_cloud_lock);return ARM_WRITE_BUSY; }
    memset(&s_ble_pending,0,sizeof(s_ble_pending));
    s_ble_pending.active=true;s_ble_pending.battery=true;
    s_ble_pending.token=ble_ct_cmd_write_token();s_ble_pending.addr=INDEX_BATTERY_TYPE;
    s_ble_pending.scale=1;
    snprintf(s_ble_pending.param_id,sizeof(s_ble_pending.param_id),"battery_config");
    portEXIT_CRITICAL(&s_cloud_lock);
    rc=direct_battery_config_write(&config,true);
    if (rc) { portENTER_CRITICAL(&s_cloud_lock);s_ble_pending.active=false;portEXIT_CRITICAL(&s_cloud_lock); }
    return rc;
}

static void handle_battery_cmd(const direct_battery_config_t *config, const char *task_id)
{
    portENTER_CRITICAL(&s_cloud_lock);
    if (s_pending.active || s_ble_pending.active) {
        portEXIT_CRITICAL(&s_cloud_lock);cloud_forget(task_id);
        send_response("set_battery_config",PROTO_ERR_BUSY,-3,task_id);return;
    }
    memset(&s_pending,0,sizeof(s_pending));
    s_pending.active=true;s_pending.battery=true;s_pending.addr=INDEX_BATTERY_TYPE;
    snprintf(s_pending.cmd,sizeof(s_pending.cmd),"set_battery_config");
    snprintf(s_pending.task_id,sizeof(s_pending.task_id),"%s",task_id);
    portEXIT_CRITICAL(&s_cloud_lock);
    int rc=direct_battery_config_write(config,true);
    if (rc) {
        portENTER_CRITICAL(&s_cloud_lock);s_pending.active=false;portEXIT_CRITICAL(&s_cloud_lock);
        if (rc==ARM_WRITE_BUSY || rc==ARM_WRITE_UPGRADE_BUSY || rc==ARM_WRITE_NOT_READY) cloud_forget(task_id);
        send_response("set_battery_config",battery_error(rc),rc,task_id);
    }
}

/* Direct write callback: durable save + UART submission, or failure. */
static void on_arm_write_done(uint8_t cmd, uint16_t param1, uint16_t param2,
                              int err)
{
    if (cmd != CS_CMD_WRITE_CTRLPARAM || param2 != 2) {
        return;   /* UTC 等其它写由 sntp_sync 回调处理 */
    }
    pending_cmd_t cloud_finished = {0};
    pending_ble_cmd_t ble_finished = {0};
    /* Snapshot both owners before invoking any consumer. A later enqueue must
     * not be mistaken for the UART completion currently being dispatched. */
    portENTER_CRITICAL(&s_cloud_lock);
    if (s_ble_pending.active && s_ble_pending.addr == param1) {
        ble_finished = s_ble_pending;
        s_ble_pending.active = false;
    }
    if (s_pending.active && s_pending.addr == param1) {
        cloud_finished = s_pending;
        s_pending.active = false;
    }
    portEXIT_CRITICAL(&s_cloud_lock);
    if (ble_finished.active) {
        pending_ble_cmd_t finished = ble_finished;
        if (err != 0) {
            ble_ct_cmd_on_write_ack(finished.token, finished.param_id, err);
        } else {
            /* direct_poll success proves durable ESP save and submission of
             * all three UART sync frames. The desired store is NOT DSP ACK. */
            uint16_t actual_raw=0;
            uint32_t revision = 0;
            direct_battery_config_t config;
            bool have_value = finished.battery ? direct_battery_config_get(&config) : direct_config_get_wire(finished.addr, &actual_raw);
            bool have_revision = finished.battery ? have_value : arm_poll_take_param();
            if (finished.battery && have_value) revision=config.revision;
            else if (have_revision) {
                revision = g_read_write.CtrlParamAlterTime;
                arm_poll_give_param();
            }
            if (have_value && have_revision) {
                ble_ct_cmd_on_write_saved(finished.token, finished.param_id,
                    (double)actual_raw / finished.scale, revision);
            } else {
                ble_ct_cmd_on_write_ack(finished.token, finished.param_id, -3);
            }
        }
    }
    if (cloud_finished.active) {
        pending_cmd_t finished = cloud_finished;
        if (err != 0) {
            ESP_LOGW(TAG, "cmd %s: ARM exec failed err=%d", finished.cmd, err);
            const char *reason=(err==ARM_WRITE_SAVE_FAILED || err==-2)?"STORAGE_FAILED":
                               err==ARM_WRITE_SUBMIT_FAILED?"SUBMIT_FAILED":PROTO_ERR_EXEC_FAILED;
            send_response(finished.cmd, reason, err,
                          finished.task_id);
        } else {
            send_response_ok(finished.cmd, finished.value, finished.task_id);
            telemetry_request_config();
        }
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
    if (!isfinite(value) || value < 0 || value * entry->scale > 65535) {
        ESP_LOGW(TAG, "cmd %s value out of range: %f", cmd_name, value);
        send_response(cmd_name, "OUT_OF_RANGE", -5, task_id);
        return;
    }
    uint16_t raw = (uint16_t)(value * entry->scale);
    uint8_t data[2];
    data[0] = (uint8_t)(raw & 0xFF);
    data[1] = (uint8_t)(raw >> 8);

    portENTER_CRITICAL(&s_cloud_lock);
    if (s_pending.active || s_ble_pending.active) {
        portEXIT_CRITICAL(&s_cloud_lock);
        cloud_forget(task_id);
        send_response(cmd_name, PROTO_ERR_BUSY, -3, task_id);
        return;
    }
    /* Install ownership before enqueue: another task may complete immediately. */
    s_pending.active = true;
    s_pending.battery = false;
    s_pending.addr = entry->addr;
    s_pending.value = (double)raw / entry->scale;
    snprintf(s_pending.cmd, sizeof(s_pending.cmd), "%s", cmd_name);
    snprintf(s_pending.task_id, sizeof(s_pending.task_id), "%s",
             (task_id && task_id[0]) ? task_id : "");
    portEXIT_CRITICAL(&s_cloud_lock);
    int rc = arm_poll_send_write(CS_CMD_WRITE_CTRLPARAM, entry->addr, 2, data);
    if (rc != 0) {
        portENTER_CRITICAL(&s_cloud_lock);
        s_pending.active = false;
        portEXIT_CRITICAL(&s_cloud_lock);
        cloud_forget(task_id);
        const char *reason=rc==-16?"PROFILE_REQUIRED":rc==-15?"PROTOCOL_UNSUPPORTED":rc==-14?"REVISION_CONFLICT":rc==ARM_WRITE_INVALID?PROTO_ERR_INVALID_ARGS:
                           rc==ARM_WRITE_UNSUPPORTED?PROTO_ERR_NOT_SUPPORTED:
                           rc==ARM_WRITE_UPGRADE_BUSY?"UPGRADE_BUSY":
                           rc==ARM_WRITE_NOT_READY?"NOT_READY":PROTO_ERR_BUSY;
        send_response(cmd_name, reason, rc, task_id);
        return;
    }
    ESP_LOGI(TAG, "cmd %s addr=0x%04X raw=%u queued", cmd_name, entry->addr, raw);
    /* Success is reported by the save+UART submission callback, not enqueue. */
}

/* ---- BLE 本地通道：命令名 → 映射条目查找 ---- */
static const cmd_map_t *cmd_lookup(const char *name)
{
    if (!name) return NULL;
    for (int i = 0; i < (int)CMD_MAP_SIZE; i++) {
        if (strcmp(s_cmd_map[i].name, name) == 0) {
            return &s_cmd_map[i];
        }
    }
    return NULL;
}

cmd_handle_result_t cmd_handler_write_param(const char *name, double value)
{
    const cmd_map_t *entry = cmd_lookup(name);
    if (!entry) {
        ESP_LOGW(TAG, "BLE cmd unknown param_id: %s", name ? name : "(null)");
        return CMD_HANDLE_UNKNOWN;
    }

    /* 值域检查: 0 <= value*scale <= 65535 (与云端 handle_cmd 一致) */
    if (!isfinite(value) || value < 0 || value * entry->scale > 65535) {
        ESP_LOGW(TAG, "BLE cmd %s value out of range: %f", name, value);
        return CMD_HANDLE_OUT_OF_RANGE;
    }

    uint16_t raw = (uint16_t)(value * entry->scale);
    uint8_t data[2];
    data[0] = (uint8_t)(raw & 0xFF);
    data[1] = (uint8_t)(raw >> 8);

    portENTER_CRITICAL(&s_cloud_lock);
    if (s_ble_pending.active || s_pending.active) {
        portEXIT_CRITICAL(&s_cloud_lock);
        return CMD_HANDLE_BUSY;
    }
    memset(&s_ble_pending, 0, sizeof(s_ble_pending));
    s_ble_pending.active = true;
    s_ble_pending.token = ble_ct_cmd_write_token();
    s_ble_pending.addr = entry->addr;
    s_ble_pending.expected_raw = raw;
    s_ble_pending.scale = entry->scale;
    snprintf(s_ble_pending.param_id, sizeof(s_ble_pending.param_id), "%s", name);
    portEXIT_CRITICAL(&s_cloud_lock);
    int rc = arm_poll_send_write(CS_CMD_WRITE_CTRLPARAM, entry->addr, 2, data);
    if (rc != 0) {
        portENTER_CRITICAL(&s_cloud_lock);
        s_ble_pending.active = false;
        portEXIT_CRITICAL(&s_cloud_lock);
        ESP_LOGW(TAG, "BLE cmd %s: ARM busy", name);
        return rc==-16?CMD_HANDLE_PROFILE_REQUIRED:rc==-15?CMD_HANDLE_PROTOCOL_UNSUPPORTED:rc==-14?CMD_HANDLE_REVISION_CONFLICT:rc==ARM_WRITE_INVALID?CMD_HANDLE_OUT_OF_RANGE:
               rc==ARM_WRITE_UNSUPPORTED?CMD_HANDLE_UNSUPPORTED:
               rc==ARM_WRITE_UPGRADE_BUSY?CMD_HANDLE_UPGRADE_BUSY:
               rc==ARM_WRITE_NOT_READY?CMD_HANDLE_NOT_READY:CMD_HANDLE_BUSY;
    }
    ESP_LOGI(TAG, "BLE cmd %s addr=0x%04X raw=%u queued", name, entry->addr, raw);
    return CMD_HANDLE_OK;
}

bool cmd_handler_get_param(const char *name, double *out)
{
    if (!out) return false;
    const cmd_map_t *entry = cmd_lookup(name);
    if (!entry) return false;

    /* 仅支持落在 READ_CTRLPARAM 快照范围内 (索引 0x0000-0x0029) 的参数 */
    if (entry->addr >= DIRECT_DSP_CONFIG_COUNT) {
        ESP_LOGW(TAG, "BLE get_param %s: addr 0x%04X not in snapshot", name, entry->addr);
        return false;
    }

    uint16_t raw;if(!direct_config_get_wire(entry->addr,&raw))return false;
    *out = (double)raw / entry->scale;
    return true;
}

/* ---- 单设备调试模式: set_debug_telemetry ----
 * args = [enabled(0/1), interval_seconds(5..3600, 缺省 5), duration_seconds(0→默认3600)]
 * OK 回执附 effective_interval_seconds 与设备侧 expires_at (unix 秒)。
 * 与映射表命令不同: 本命令多参数、只改本地心跳调度, 不写 ARM。 */
static void handle_debug_cmd(const char *cmd_name, const cJSON *root,
                             const char *task_id)
{
    cJSON *jargs = cJSON_GetObjectItemCaseSensitive(root, "args");
    if (!cJSON_IsArray(jargs) || cJSON_GetArraySize(jargs) < 1) {
        send_response(cmd_name, PROTO_ERR_INVALID_ARGS, -2, task_id);
        return;
    }
    cJSON *je = cJSON_GetArrayItem(jargs, 0);
    if (!cJSON_IsNumber(je) ||
        (je->valuedouble != 0 && je->valuedouble != 1)) {
        send_response(cmd_name, PROTO_ERR_INVALID_ARGS, -2, task_id);
        return;
    }
    int enabled  = (int)je->valuedouble;
    int interval = 5;      /* 缺省 5s (调试高频) */
    int duration = 3600;   /* 缺省 60 分钟 */
    if (cJSON_GetArraySize(jargs) > 1) {
        cJSON *ji = cJSON_GetArrayItem(jargs, 1);
        if (cJSON_IsNumber(ji) && ji->valuedouble >= 5 && ji->valuedouble <= 3600) {
            interval = (int)ji->valuedouble;
        }
    }
    if (cJSON_GetArraySize(jargs) > 2) {
        cJSON *jd = cJSON_GetArrayItem(jargs, 2);
        if (cJSON_IsNumber(jd) && jd->valuedouble >= 0 && jd->valuedouble <= 86400) {
            duration = (int)jd->valuedouble;
        }
    }

    uint32_t effective = 0;
    int64_t  expires_at = 0;
    telemetry_debug_set(enabled ? true : false, (uint32_t)interval,
                        (uint32_t)duration, &effective, &expires_at);

    cloud_receipt_t receipt={.result=PROTO_ERR_OK,.stage=CLOUD_DEBUG,.effective=effective,.expires_at=expires_at};
    send_receipt(cmd_name,task_id,&receipt,true);
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
        /* 立即补报一次 config (V2.1 §11.3) */
        telemetry_request_config();
        send_response(cmd_name, PROTO_ERR_OK, 0, task_id);
        return true;
    }
    return false;
}

void cmd_handler_init(void)
{
    arm_poll_set_write_cb(on_arm_write_done);
    s_pending.active = false;
    s_ble_pending.active = false;
    memset(s_cloud_replay, 0, sizeof(s_cloud_replay));
    memset(s_cloud_input,0,sizeof(s_cloud_input));
    memset(&s_cloud_stats,0,sizeof(s_cloud_stats));
    s_cloud_head=s_cloud_count=0;s_cloud_consumer=false;
    ESP_LOGI(TAG, "init done (%d cmds)", (int)CMD_MAP_SIZE);
}

static void process_cloud_command(const char *payload,unsigned int len,bool retained)
{
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
    if ((jtask && (!cJSON_IsString(jtask) || !task_id[0])) ||
        !command_identifier_valid(task_id, CLOUD_TASK_MAX) ||
        !command_identifier_valid(jcmd->valuestring, CLOUD_CMD_MAX)) {
        cloud_receipt_t rejected={.result=PROTO_ERR_INVALID_ARGS,.err=-2,.stage=CLOUD_FAILED};
        send_receipt(jcmd->valuestring,task_id,&rejected,false);
        cJSON_Delete(root);
        return;
    }

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

    bool query=!strcmp(jcmd->valuestring,PROTO_CMD_QUERY_TELEMETRY) ||
               !strcmp(jcmd->valuestring,PROTO_CMD_QUERY_INFO) ||
               !strcmp(jcmd->valuestring,PROTO_CMD_QUERY_CONFIG) ||
               !strcmp(jcmd->valuestring,"get_battery_config");
    if(retained && !query){
        portENTER_CRITICAL(&s_cloud_lock);s_cloud_stats.retained_rejected++;portEXIT_CRITICAL(&s_cloud_lock);
        cloud_receipt_t rejected={.result="RETAINED_REJECTED",.err=-9,.stage=CLOUD_FAILED};
        send_receipt(jcmd->valuestring,task_id,&rejected,false);
        cJSON_Delete(root);return;
    }
    bool battery_write=!strcmp(jcmd->valuestring,"set_battery_config");
    direct_battery_config_t battery_config;
    if (battery_write) {
        int rc=battery_parse(cJSON_GetObjectItemCaseSensitive(root,"args"),&battery_config);
        if (rc) { send_response(jcmd->valuestring,battery_error(rc),rc==ARM_WRITE_INVALID?-2:rc,task_id);cJSON_Delete(root);return; }
    }
    unsigned char digest[32]={0};
    if(task_id[0]){
        if(!cloud_digest(jcmd->valuestring,root,jval,digest)){
            cloud_receipt_t rejected={.result=PROTO_ERR_BUSY,.err=-3,.stage=CLOUD_FAILED};
            send_receipt(jcmd->valuestring,task_id,&rejected,false);cJSON_Delete(root);return;
        }
        if(cloud_replay(jcmd->valuestring,task_id,digest)){cJSON_Delete(root);return;}
    }
    if (!command_deadline_valid(root, jcmd->valuestring, task_id)) {
        cJSON_Delete(root);
        return;
    }

    if (!strcmp(jcmd->valuestring,"get_battery_config")) {
        cloud_receipt_t receipt={.result=PROTO_ERR_OK,.stage=CLOUD_BATTERY_READ};
        direct_battery_config_t config;
        if (!direct_battery_config_get(&config)) send_response(jcmd->valuestring,"NOT_READY",ARM_WRITE_NOT_READY,task_id);
        else send_receipt_data(jcmd->valuestring,task_id,&receipt,false,&config);
        cJSON_Delete(root);return;
    }
    if (battery_write) {
        if (cloud_admit(jcmd->valuestring,task_id,digest)) handle_battery_cmd(&battery_config,task_id);
        cJSON_Delete(root);return;
    }

    /* 查询命令: 无需数值参数 */
    if (handle_query_cmd(jcmd->valuestring, task_id)) {
        cJSON_Delete(root);
        return;
    }

    /* 单设备调试模式: 多参数命令 (args 数组), 独立于单值映射表处理 */
    if (strcmp(jcmd->valuestring, PROTO_CMD_SET_DEBUG_TELEMETRY) == 0) {
        const cJSON *args=cJSON_GetObjectItemCaseSensitive(root,"args");
        int n=cJSON_IsArray(args)?cJSON_GetArraySize(args):0;bool valid=n>=1 && n<=3;
        for(int i=0;i<n && valid;i++){
            const cJSON *v=cJSON_GetArrayItem(args,i);
            valid=cJSON_IsNumber(v) && isfinite(v->valuedouble) && floor(v->valuedouble)==v->valuedouble;
            if(valid)valid=i==0?(v->valuedouble==0 || v->valuedouble==1):
                           i==1?(v->valuedouble>=5 && v->valuedouble<=3600):
                                 (v->valuedouble>=0 && v->valuedouble<=86400);
        }
        if(!valid)send_response(jcmd->valuestring,PROTO_ERR_INVALID_ARGS,-2,task_id);
        else if(cloud_admit(jcmd->valuestring,task_id,digest))handle_debug_cmd(jcmd->valuestring, root, task_id);
        cJSON_Delete(root);
        return;
    }

    if (!cJSON_IsNumber(jval)) {
        ESP_LOGW(TAG, "cmd %s: value must be number", jcmd->valuestring);
        send_response(jcmd->valuestring, PROTO_ERR_INVALID_ARGS, -2, task_id);
        cJSON_Delete(root);
        return;
    }
    const cmd_map_t *entry=cmd_lookup(jcmd->valuestring);
    if(!entry)send_response(jcmd->valuestring,PROTO_ERR_NOT_SUPPORTED,-4,task_id);
    else if(!isfinite(jval->valuedouble) || jval->valuedouble<0 || jval->valuedouble*entry->scale>65535)
        send_response(jcmd->valuestring,"OUT_OF_RANGE",-5,task_id);
    else if(cloud_admit(jcmd->valuestring,task_id,digest))handle_cmd(jcmd->valuestring, jval->valuedouble, task_id);
    cJSON_Delete(root);
}

void cmd_handler_on_mqtt_delivery(const char *topic,const char *payload,unsigned int len,bool retained)
{
    if(!topic || !payload || !len || len>1024)return;
    const char *suffix=strrchr(topic,'/');
    if(!suffix || strcmp(suffix,"/cmd") || strstr(topic,"/ota/cmd"))return;
    portENTER_CRITICAL(&s_cloud_lock);
    if(s_cloud_count==CMD_HANDLER_QUEUE_DEPTH)s_cloud_stats.queue_full++;
    else{
        cloud_input_t *input=&s_cloud_input[(s_cloud_head+s_cloud_count)%CMD_HANDLER_QUEUE_DEPTH];
        memcpy(input->payload,payload,len);input->payload[len]=0;input->len=len;input->retained=retained;
        s_cloud_count++;
    }
    portEXIT_CRITICAL(&s_cloud_lock);
}

void cmd_handler_on_mqtt_message(const char *topic,const char *payload,unsigned int len)
{
    cmd_handler_on_mqtt_delivery(topic,payload,len,false);
}

void cmd_handler_tick(void)
{
    cloud_input_t input;
    portENTER_CRITICAL(&s_cloud_lock);
    if(s_cloud_consumer || !s_cloud_count){portEXIT_CRITICAL(&s_cloud_lock);return;}
    s_cloud_consumer=true;input=s_cloud_input[s_cloud_head];
    s_cloud_head=(s_cloud_head+1U)%CMD_HANDLER_QUEUE_DEPTH;s_cloud_count--;
    portEXIT_CRITICAL(&s_cloud_lock);
    process_cloud_command(input.payload,input.len,input.retained);
    portENTER_CRITICAL(&s_cloud_lock);s_cloud_consumer=false;portEXIT_CRITICAL(&s_cloud_lock);
}

void cmd_handler_get_stats(cmd_handler_stats_t *stats)
{
    if(!stats)return;
    portENTER_CRITICAL(&s_cloud_lock);
    *stats=s_cloud_stats;stats->queue_depth=s_cloud_count;
    stats->static_bytes=(unsigned)(sizeof(s_cloud_replay)+sizeof(s_cloud_input));
    portEXIT_CRITICAL(&s_cloud_lock);
}
