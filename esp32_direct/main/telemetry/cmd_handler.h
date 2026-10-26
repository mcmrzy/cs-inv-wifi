/*
 * cmd_handler.h — 云端命令处理
 *
 * 订阅主题: cs_inv/{sn}/cmd (QoS1, mqtt_handler 回调进入)
 * 命令格式: {"cmd":"set_charge_limit","value":20}  (兼容 name/action 键)
 * 响应主题: cs_inv/{sn}/cmd/response
 */
#ifndef CMD_HANDLER_H
#define CMD_HANDLER_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 初始化 (由 app_main 调用) */
void cmd_handler_init(void);

/* MQTT 消息回调入口 (mqtt_handler 转发) */
void cmd_handler_on_mqtt_message(const char *topic, const char *payload,
                                 unsigned int len);
#define CMD_HANDLER_QUEUED 1
#define CMD_HANDLER_QUEUE_DEPTH 2
/* Callback path copies only; the main loop owns parsing and execution. */
void cmd_handler_on_mqtt_delivery(const char *topic, const char *payload,
                                  unsigned int len, bool retained);
void cmd_handler_tick(void);
typedef struct {
    uint32_t queue_full, retained_rejected, expired, cache_full;
    unsigned queue_depth;
    unsigned static_bytes;
} cmd_handler_stats_t;
void cmd_handler_get_stats(cmd_handler_stats_t *stats);

/*
 * BLE 本地通道命令执行入口（不发布 MQTT 回报，由调用方构造 CMD_RESULT）
 * 复用同一 s_cmd_map（参数名/地址/缩放单一来源），保证 BLE 与云端行为一致。
 */

typedef enum {
    CMD_HANDLE_OK = 0,         /* 已排队下发 ARM */
    CMD_HANDLE_UNKNOWN,        /* 未知命令名/param_id */
    CMD_HANDLE_OUT_OF_RANGE,   /* 值越界 */
    CMD_HANDLE_BUSY,           /* ARM 链路忙 */
    CMD_HANDLE_NOT_READABLE,   /* 该参数不在可读快照范围内 (get_param) */
    CMD_HANDLE_UNSUPPORTED,
    CMD_HANDLE_UPGRADE_BUSY,
    CMD_HANDLE_NOT_READY,
    CMD_HANDLE_REVISION_CONFLICT = -14,
    CMD_HANDLE_PROTOCOL_UNSUPPORTED = -15,
    CMD_HANDLE_PROFILE_REQUIRED = -16,
} cmd_handle_result_t;

struct cJSON;
/* Full native battery group: 14 integer fields + REQUIRED uint32 revision.
 * No scalar collector type conversion or engineering-unit scaling here.
 * Read returns an owned JSON object; NULL means unavailable/allocation failure.
 * validate_only performs schema/policy checks without changing ownership/state.
 * Write returns native ARM admission errors (including -14/-15/-16). */
struct cJSON *cmd_handler_get_battery_config(void);
int cmd_handler_write_battery_config(const struct cJSON *config, bool validate_only);

/* 写控制参数：name 为 s_cmd_map 命令名（BLE set_param 的 param_id 即此名），
 * value 为工程单位值（内部按 scale 缩放后排队写入 ARM）。
 * 可在 NimBLE host 任务上下文安全调用（arm_poll_send_write 有忙位保护）。 */
cmd_handle_result_t cmd_handler_write_param(const char *name, double value);

/* 读控制参数：返回最近一次快照中该命令的工程单位值。
 * out    输出的工程单位值
 * 返回 true=可读且已填充；false=未知命令或不在快照范围 */
bool cmd_handler_get_param(const char *name, double *out);

#ifdef __cplusplus
}
#endif

#endif /* CMD_HANDLER_H */
