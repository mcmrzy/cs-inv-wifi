#ifndef BLE_CT_CMD_H
#define BLE_CT_CMD_H

#include <stdbool.h>
#include <stdint.h>

struct ble_gatt_access_ctxt;

int ble_ct_cmd_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                          struct ble_gatt_access_ctxt *ctxt, void *arg);
void ble_ct_cmd_notify_result(const char *json, uint16_t len);

/* ARM 写应答/参数回读闭环。仅完成当前等待中的 BLE 写命令。 */
void ble_ct_cmd_on_write_ack(const char *param_id, int err);
void ble_ct_cmd_on_write_readback(const char *param_id, double actual,
                                  uint32_t parameter_revision);

/*
 * 命令执行回调（由 main 层注入，解耦 ble_ct 与 main/cmd_handler 的编译依赖）
 * 返回值对应下列 BLE_CMD_* 结果码。
 */
typedef int (*ble_cmd_write_fn)(const char *param_id, double value);
typedef int (*ble_cmd_read_fn)(const char *param_id, double *out);

/* 命令执行结果码（与 main 层 cmd_handler 对齐） */
enum {
    BLE_CMD_OK = 0,          /* 已排队下发 ARM */
    BLE_CMD_UNKNOWN,         /* 未知命令名/param_id */
    BLE_CMD_OUT_OF_RANGE,    /* 值越界 */
    BLE_CMD_BUSY,            /* ARM 链路忙 */
    BLE_CMD_NOT_READABLE,    /* 参数不可读 (get_param) */
};

void ble_ct_cmd_set_executors(ble_cmd_write_fn write_fn, ble_cmd_read_fn read_fn);

#endif
