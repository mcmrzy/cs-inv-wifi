#ifndef BLE_PROV_H
#define BLE_PROV_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    BLE_PROV_IDLE,
    BLE_PROV_RUNNING,
    BLE_PROV_DONE,
    BLE_PROV_ERROR
} BleProvState;

typedef void (*BleProvCallback)(const char *ssid, const char *password);
typedef void (*BleProvConnCallback)(uint16_t conn_handle);

/**
 * @brief 启动 BLE 配网
 * @param device_sn 设备 SN 号（显示在 BLE 广播中）
 * @param fw_version 固件版本号
 */
bool ble_prov_start(const char *device_sn, const char *fw_version);
/* start/restore true means accepted (or still held by another pause owner),
 * never proof of async host sync/advertising. Poll readiness before presenting
 * local access as available. Both false after acceptance means pending; failed
 * becomes true on heap/init/task/address/advertising failure or host reset.
 * No synchronous sync timeout/deinit is performed on a still-starting task.
 * Reinitialization requires internal8 free>=65536 and largest>=32768; this is
 * a preflight snapshot, so callers must first release competing MQTT memory. */
bool ble_prov_is_ready(void);
bool ble_prov_start_failed(void);
/* Independent-task, LC-serialized cleanup for start_failed==true only.
 * true: failed host is gone (or was never allocated), failure cleared; call
 * start on a later tick, subject to heap admission. false: leave the current
 * instance alone and retry with backoff. Pending/healthy startup and peers are
 * never torn down. Pause owners are never released: if a restore failed while
 * holding an OTA/MQTT owner, retry its matching restore instead.
 * Inactive advertising (EALREADY) still requires a fresh host queue barrier.
 * A deinit failure is logged as restart-required; no new start is admitted.
 * NPL enqueue/host stop retain the SDK blocking boundaries described below. */
bool ble_prov_recover_start_failure(void);

/**
 * @brief 停止 BLE 广播（暂停，保留协议栈已初始化，可用 resume 快速恢复）
 */
void ble_prov_stop(void);

/**
 * @brief 恢复 BLE 广播（不重新初始化协议栈；ble_prov_start 之后可反复调用）
 */
void ble_prov_resume(void);

/* Start and lifecycle calls must run in independent tasks, never NimBLE/GAP or
 * host-reset callbacks. Full lifecycle operations serialize on one mutex, while
 * notification callbacks use only the separate producer gate below.
 *
 * OTA and MQTT each hold an idempotent pause owner (not a nesting counter).
 * Only the last owner's restore restarts BLE; start cannot bypass either owner.
 * Failed suspend acquires no owner. Call the matching restore to recover after
 * a partial teardown failure; it never releases the other owner's pause.
 * Failed restore retains ownership for a later retry.
 * BOTH owners are idle-only: active/racing phone connections reject suspend;
 * remote OTA must defer/fail on false, never force a phone/local OTA offline. */
bool ble_prov_suspend_for_ota(void);
bool ble_prov_restore_after_ota(void);
/* Both OTA and MQTT disable advertising, wait for a default host queue barrier,
 * then recheck the connection. Stop/ACK errors leave the host alive and restore
 * advertising. ACK waiting is bounded to 1s; the SDK NPL enqueue itself can
 * block on a full queue. A timed-out event remains pending; its late ACK cannot
 * admit a later attempt, which must enqueue and acknowledge its own barrier. */
bool ble_prov_suspend_for_mqtt(void);
bool ble_prov_restore_after_mqtt(void);
/* Atomic snapshot of the producer gate. true means host producers are paused,
 * NOT that stop/deinit succeeded or that all BLE RAM has been reclaimed.
 * Suitable for excluding competing MQTT handshakes during lifecycle work;
 * use suspend's return value to establish successful pause ownership. */
bool ble_prov_is_suspended(void);

/* Background producers must hold this gate while accessing the host. A false
 * return means the host is unavailable; do not call release in that case. */
bool ble_prov_host_acquire(void);
void ble_prov_host_release(void);

typedef void (*BleHostResetCb)(void);
void ble_prov_set_host_reset_cb(BleHostResetCb cb);

/**
 * @brief 获取当前配网状态
 */
BleProvState ble_prov_get_state(void);

/**
 * @brief 设置配网成功回调
 */
void ble_prov_set_callback(BleProvCallback cb);

/**
 * @brief 推送配网状态通知给已连接的 App (connecting/connected/failed/not_found)
 * @param status 状态字符串, 见 BLE_Provisioning_Protocol.md 第 5 节; 无连接时仅缓存, 下次连接可读
 */
void ble_prov_notify_status(const char *status);

/**
 * @brief 获取 BLE 配网服务的 service name（用于手机 App 发现）
 */
const char *ble_prov_get_service_name(void);

/**
 * @brief 设置 BLE 连接/断连事件回调
 */
void ble_prov_set_conn_callback(BleProvConnCallback cb);
bool ble_prov_is_connected(void);

/**
 * @brief 获取 OTA 特征的 val handle
 */
uint16_t ble_prov_get_ota_ctrl_handle(void);
uint16_t ble_prov_get_ota_data_handle(void);
uint16_t ble_prov_get_ota_status_handle(void);

/**
 * @brief 设置 OTA GATT 访问回调（由 ble_ota 调用）
 */
void ble_prov_set_ota_access_cb(void *cb);

/**
 * @brief GAP 事件转发回调（由 ble_ct 注册）
 */
struct ble_gap_event;
typedef int (*BleGapEventForwardCb)(struct ble_gap_event *event, void *arg);
void ble_prov_set_gap_event_forward(BleGapEventForwardCb cb);

/**
 * @brief host 启动前回调（用于注册额外 GATT 服务）
 *
 * 在创建 host task 之前调用，此时 NimBLE 已初始化
 * 但 host 尚未启动，可以安全注册 GATT 服务。
 */
typedef void (*BlePreHostStartCb)(void);
void ble_prov_set_pre_host_start_cb(BlePreHostStartCb cb);

#endif // BLE_PROV_H
