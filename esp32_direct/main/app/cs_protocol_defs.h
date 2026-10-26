/*
 * cs_protocol_defs.h — L10 采集器 MQTT 协议常量
 *
 * 主题前缀与信封格式与 esp32c2_idf 保持一致:
 *  - 信封: {"t":<unix秒>,"v":2,"data":{...}}  (统一打时间戳, 补传一致)
 *  - 主题: cs_inv/{sn}/...
 *
 * V2.0 协议 (CS-L10-6K2_MQTT_上报协议设计_V2.0.md):
 *  - 信封版本号 = 2, 所有上行消息统一
 *  - 实时遥测统一 heartbeat 主题 (180s), 不再拆分 data/ac 等旧主题
 *
 * V3 heartbeat (2026-09-22): ESP 不再解析 ARM 运行参数, 只按 pack(1) 结构体
 * 字序原样转发 (87 个 u16), 字段语义/量纲/界限全部由服务端按结构体定义解析
 * (device-communication/internal/telemetry/heartbeat_v3.go)。heartbeat 因此
 * 使用独立信封版本 3; info/config/alarm/status 等仍为 v2。
 */
#ifndef CS_PROTOCOL_DEFS_H
#define CS_PROTOCOL_DEFS_H

#define PROTO_ENVELOPE_VERSION  2
/* heartbeat 专用信封版本: data 为 {"run":[87 个 u16]} 结构体原样转发 */
#define PROTO_HEARTBEAT_ENVELOPE_VERSION  3

/* ---- 主题名 (拼接前缀后使用) ---- */
#define PROTO_TOPIC_HEARTBEAT   "heartbeat"
#define PROTO_TOPIC_INFO        "info"
#define PROTO_TOPIC_ALARM       "alarm"
#define PROTO_TOPIC_CONFIG      "config"
#define PROTO_TOPIC_STATUS      "status"
#define PROTO_TOPIC_CMD_RESP    "cmd/response"
#define PROTO_TOPIC_OTA_STATUS  "ota/status"
/* OTA 过程留痕: 与 ota/status 分开, 避免干扰后端状态机 (后端只认 ota/status) */
#define PROTO_TOPIC_OTA_DEBUG   "ota/debug"
/* 调试模式串口日志上云: 仅 set_debug_telemetry 生效期间上行, 后端不解析 */
#define PROTO_TOPIC_DEBUG_LOG   "debug/log"

/* 主题前缀格式 (用于 snprintf) */
#define PROTO_TOPIC_FMT         "cs_inv/%s/%s"

/* ==================== OTA 协议 ==================== */
#define PROTO_CMD_EXPIRE_S      300    /* 命令过期秒数 */
/* 命令时间戳的"未来"容差: 云端与设备的时钟存在亚秒级偏差, 云端打的 t 会比设备
 * 本地时间晚 1 秒(实测: 命令 t=…207, 设备本地 …206), 旧判据把任何未来时间戳都当
 * 过期, 于是一条刚发出的命令被 EXPIRED 拒掉。留出前向容差, 只拦真正离谱的未来值。 */
#define PROTO_CMD_FUTURE_SLACK_S 60

/* OTA Action 名 */
#define PROTO_OTA_ACTION_START  "start"
#define PROTO_OTA_ACTION_CANCEL "cancel"
#define PROTO_OTA_ACTION_QUERY  "query"

/* OTA 状态字符串 */
#define PROTO_OTA_STATE_ACCEPTED     "accepted"
#define PROTO_OTA_STATE_DOWNLOADING  "downloading"
#define PROTO_OTA_STATE_VERIFYING    "verifying"
#define PROTO_OTA_STATE_INSTALLING   "installing"
#define PROTO_OTA_STATE_REBOOTING    "rebooting"
#define PROTO_OTA_STATE_SUCCEEDED    "succeeded"
#define PROTO_OTA_STATE_FAILED       "failed"
#define PROTO_OTA_STATE_CANCELLED    "cancelled"
#define PROTO_OTA_STATE_ROLLED_BACK  "rolled_back"

/* 命令错误码 */
#define PROTO_ERR_OK            "OK"
#define PROTO_ERR_INVALID_ARGS  "INVALID_ARGS"
#define PROTO_ERR_NOT_SUPPORTED "NOT_SUPPORTED"
#define PROTO_ERR_BUSY          "BUSY"
#define PROTO_ERR_EXPIRED       "EXPIRED"
#define PROTO_ERR_EXEC_FAILED   "EXEC_FAILED"

/* 单设备调试模式: 云端临时切换心跳采样周期, 到期/关闭/重启自动恢复默认 */
#define PROTO_CMD_SET_DEBUG_TELEMETRY "set_debug_telemetry"

/* 查询命令 (无参数, args 为空数组) */
#define PROTO_CMD_QUERY_TELEMETRY "query_telemetry"
#define PROTO_CMD_QUERY_INFO      "query_info"
#define PROTO_CMD_QUERY_CONFIG    "query_config"

#endif /* CS_PROTOCOL_DEFS_H */
