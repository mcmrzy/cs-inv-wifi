/*
 * CS-I10-6k2 协议常量定义
 * 版本: V1.0.26.713
 *
 * 集中管理 MQTT 主题名、信封版本号、命令错误码、OTA 状态等字符串常量，
 * 消除代码中的 magic string。
 *
 * Arduino 版与 IDF 版共用此文件（内容一致）。
 */

#ifndef CS_PROTOCOL_DEFS_H
#define CS_PROTOCOL_DEFS_H

/* ==================== 协议版本 ==================== */
#define PROTO_ENVELOPE_VERSION  1

/* ==================== MQTT 主题名 ==================== */
#define PROTO_TOPIC_HEARTBEAT   "heartbeat"
#define PROTO_TOPIC_INFO        "info"
#define PROTO_TOPIC_CONFIG      "config"
#define PROTO_TOPIC_ALARM       "alarm"
#define PROTO_TOPIC_PARALLEL    "parallel"
#define PROTO_TOPIC_THREE_PHASE "three_phase"
#define PROTO_TOPIC_STATUS      "status"
#define PROTO_TOPIC_CMD_RESP    "cmd/response"
#define PROTO_TOPIC_OTA_STATUS  "ota/status"

/* 主题前缀格式（用于 snprintf） */
#define PROTO_TOPIC_FMT         "cs_inv/%s/%s"

/* ==================== 命令时限 ==================== */
#define PROTO_CMD_EXPIRE_S      300    /* 命令过期秒数 */
#define PROTO_CMD_ACK_MS        2000   /* ACK 时限毫秒 */

/* ==================== OTA Action 名 ==================== */
#define PROTO_OTA_ACTION_START  "start"
#define PROTO_OTA_ACTION_CANCEL "cancel"
#define PROTO_OTA_ACTION_QUERY  "query"

/* ==================== OTA 状态字符串（9 个） ==================== */
#define PROTO_OTA_STATE_ACCEPTED     "accepted"
#define PROTO_OTA_STATE_DOWNLOADING  "downloading"
#define PROTO_OTA_STATE_VERIFYING    "verifying"
#define PROTO_OTA_STATE_INSTALLING   "installing"
#define PROTO_OTA_STATE_REBOOTING    "rebooting"
#define PROTO_OTA_STATE_SUCCEEDED    "succeeded"
#define PROTO_OTA_STATE_FAILED       "failed"
#define PROTO_OTA_STATE_CANCELLED    "cancelled"
#define PROTO_OTA_STATE_ROLLED_BACK  "rolled_back"

/* ==================== 命令错误码字符串（9 个） ==================== */
#define PROTO_ERR_OK            "OK"
#define PROTO_ERR_INVALID_ARGS  "INVALID_ARGS"
#define PROTO_ERR_OUT_OF_RANGE  "OUT_OF_RANGE"
#define PROTO_ERR_NOT_SUPPORTED "NOT_SUPPORTED"
#define PROTO_ERR_BUSY          "BUSY"
#define PROTO_ERR_INTERLOCKED   "INTERLOCKED"
#define PROTO_ERR_EXPIRED       "EXPIRED"
#define PROTO_ERR_EXEC_FAILED   "EXEC_FAILED"
#define PROTO_ERR_TIMEOUT       "TIMEOUT"

/* ==================== 命令回复阶段（3 阶段） ==================== */
#define PROTO_CMD_STAGE_ACK      "acknowledged"
#define PROTO_CMD_STAGE_EXEC     "executing"
#define PROTO_CMD_STAGE_DONE     "completed"

/* ==================== 告警 source 枚举（0-based） ==================== */
#define PROTO_SOURCE_PCS   0
#define PROTO_SOURCE_BMS   1
#define PROTO_SOURCE_MPPT  2
#define PROTO_SOURCE_COMM  3

/* ==================== 告警 level ==================== */
#define PROTO_LEVEL_WARNING  1
#define PROTO_LEVEL_FAULT    2

/* ==================== 告警 state ==================== */
#define PROTO_STATE_RECOVERED  0
#define PROTO_STATE_ACTIVE     1

#endif /* CS_PROTOCOL_DEFS_H */
