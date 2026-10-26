#ifndef BLE_CT_CMD_H
#define BLE_CT_CMD_H

#include <stdbool.h>
#include <stdint.h>

struct ble_gatt_access_ctxt;

int ble_ct_cmd_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                          struct ble_gatt_access_ctxt *ctxt, void *arg);
void ble_ct_cmd_notify_result(const char *json, uint16_t len);

/* Run on the main loop together with the injected executors and callbacks. */
void ble_ct_cmd_tick(void);
/* Reset only unaccepted connection work; preserve cache and accepted writes. */
void ble_ct_cmd_reset_connection(void);
uint32_t ble_ct_cmd_write_token(void);
/* Capture token at enqueue. Direct completion proves save+UART submission,
 * never DSP observation. A stale token cannot finish a newer operation. */
void ble_ct_cmd_on_write_ack(uint32_t token, const char *param_id, int err);
void ble_ct_cmd_on_write_saved(uint32_t token, const char *param_id, double actual,
                               uint32_t parameter_revision);

/*
 * 命令执行回调（由 main 层注入，解耦 ble_ct 与 main/cmd_handler 的编译依赖）
 * 返回值对应下列 BLE_CMD_* 结果码。
 */
typedef int (*ble_cmd_write_fn)(const char *param_id, double value);
typedef int (*ble_cmd_read_fn)(const char *param_id, double *out);
struct cJSON;
/* JSON remains owned by the caller for writes; read returns an owned object.
 * validate_only MUST have no storage/UART side effects. Native admission codes
 * -10..-16/-20/-21 cross this boundary without depending on main headers. */
typedef int (*ble_cmd_battery_write_fn)(const struct cJSON *config, bool validate_only);
typedef struct cJSON *(*ble_cmd_battery_read_fn)(void);
void ble_ct_cmd_set_battery_executors(ble_cmd_battery_write_fn write_fn,
                                      ble_cmd_battery_read_fn read_fn);

/* 命令执行结果码（与 main 层 cmd_handler 对齐） */
enum {
    BLE_CMD_OK = 0,          /* 已排队下发 ARM */
    BLE_CMD_UNKNOWN,         /* 未知命令名/param_id */
    BLE_CMD_OUT_OF_RANGE,    /* 值越界 */
    BLE_CMD_BUSY,            /* ARM 链路忙 */
    BLE_CMD_NOT_READABLE,    /* 参数不可读 (get_param) */
    BLE_CMD_UNSUPPORTED = 5,
    BLE_CMD_UPGRADE_BUSY = 6,
    BLE_CMD_NOT_READY = 7,
    BLE_CMD_REVISION_CONFLICT = -14,
    BLE_CMD_PROTOCOL_UNSUPPORTED = -15,
    BLE_CMD_PROFILE_REQUIRED = -16,
};

/* Injection boundary: keep main/arm_poll dependencies out of this component. */
enum { BLE_WRITE_SAVE_FAILED = -20, BLE_WRITE_SUBMIT_FAILED = -21 };

void ble_ct_cmd_set_executors(ble_cmd_write_fn write_fn, ble_cmd_read_fn read_fn);

#endif
