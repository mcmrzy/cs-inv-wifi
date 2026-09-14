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
 */
#ifndef CS_PROTOCOL_DEFS_H
#define CS_PROTOCOL_DEFS_H

#define PROTO_ENVELOPE_VERSION  2

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

/* 主题前缀格式 (用于 snprintf) */
#define PROTO_TOPIC_FMT         "cs_inv/%s/%s"

/* ==================== OTA 协议 ==================== */
#define PROTO_CMD_EXPIRE_S      300    /* 命令过期秒数 */

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

/* 查询命令 (无参数, args 为空数组) */
#define PROTO_CMD_QUERY_TELEMETRY "query_telemetry"
#define PROTO_CMD_QUERY_INFO      "query_info"
#define PROTO_CMD_QUERY_CONFIG    "query_config"

#endif /* CS_PROTOCOL_DEFS_H */
