#include "ble_ct.h"
#include "ble_ct_auth.h"
#include "ble_ct_telemetry.h"
#include "ble_ct_cmd.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "host/ble_store.h"
#include "nimble/nimble_port.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include <string.h>

static const char *TAG = "ble_ct";

/* SN 和固件版本（由 ble_ct_start 传入） */
static const char *s_device_sn = "";
static const char *s_fw_version = "";

/* CSIV-CT Service UUID: 43534956-4354-1000-8000-00805f9b34fb */
static const ble_uuid128_t s_ct_svc_uuid = BLE_UUID128_INIT(
    0xfb,0x34,0x9b,0x5f,0x80,0x00,0x00,0x80,
    0x00,0x10,0x54,0x43,0x56,0x49,0x53,0x43);
static const ble_uuid128_t s_auth_uuid = BLE_UUID128_INIT(
    0xfb,0x34,0x9b,0x5f,0x80,0x00,0x00,0x80,
    0x00,0x10,0x55,0x41,0x56,0x49,0x53,0x43);
static const ble_uuid128_t s_telemetry_uuid = BLE_UUID128_INIT(
    0xfb,0x34,0x9b,0x5f,0x80,0x00,0x00,0x80,
    0x00,0x10,0x4c,0x54,0x56,0x49,0x53,0x43);
static const ble_uuid128_t s_cmd_uuid = BLE_UUID128_INIT(
    0xfb,0x34,0x9b,0x5f,0x80,0x00,0x00,0x80,
    0x00,0x10,0x4d,0x43,0x56,0x49,0x53,0x43);
static const ble_uuid128_t s_result_uuid = BLE_UUID128_INIT(
    0xfb,0x34,0x9b,0x5f,0x80,0x00,0x00,0x80,
    0x00,0x10,0x52,0x43,0x56,0x49,0x53,0x43);
static const ble_uuid128_t s_info_uuid = BLE_UUID128_INIT(
    0xfb,0x34,0x9b,0x5f,0x80,0x00,0x00,0x80,
    0x00,0x10,0x4e,0x49,0x56,0x49,0x53,0x43);

static uint16_t s_auth_handle;
static uint16_t s_telemetry_handle;
static uint16_t s_cmd_handle;
static uint16_t s_result_handle;
static uint16_t s_info_handle;

static uint16_t s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static BleCtState s_state = BLE_CT_IDLE;
static bool s_services_registered = false;
static bool s_telemetry_subscribed = false;
static bool s_result_subscribed = false;
static bool s_auth_subscribed = false;

/* g_device_sn 和 FIRMWARE_VERSION 通过 config.h / mqtt_handler.h 引入 */

/* ---- 句柄导出 ---- */
uint16_t ble_ct_get_telemetry_handle(void) { return s_telemetry_handle; }
uint16_t ble_ct_get_result_handle(void) { return s_result_handle; }
uint16_t ble_ct_get_auth_handle(void) { return s_auth_handle; }
bool ble_ct_is_telemetry_subscribed(void) { return s_telemetry_subscribed; }
bool ble_ct_is_result_subscribed(void) { return s_result_subscribed; }
uint16_t ble_ct_get_conn_handle(void) { return s_conn_handle; }
BleCtState ble_ct_get_state(void) { return s_state; }
const char *ble_ct_get_sn(void) { return s_device_sn; }

/* ---- INFO 特征 ---- */
static int info_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                           struct ble_gatt_access_ctxt *ctxt, void *arg) {
    if (ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR) return BLE_ATT_ERR_UNLIKELY;

    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_BT);
    char buf[256];
    int n = snprintf(buf, sizeof(buf),
        "{\"sn\":\"%s\",\"model\":\"CS-L10-6K2\",\"firmware\":\"%s\","
        "\"mac\":\"%02X:%02X:%02X:%02X:%02X:%02X\",\"bound\":%s,\"proto_ver\":1}",
        s_device_sn, s_fw_version,
        mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
        ble_ct_auth_is_bound() ? "true" : "false");
    os_mbuf_append(ctxt->om, buf, n);
    return 0;
}

/* ---- GATT 访问回调 ---- */
static int ct_gatt_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                              struct ble_gatt_access_ctxt *ctxt, void *arg) {
    if (attr_handle == s_auth_handle)
        return ble_ct_auth_access_cb(conn_handle, attr_handle, ctxt, arg);
    if (attr_handle == s_info_handle)
        return info_access_cb(conn_handle, attr_handle, ctxt, arg);
    if (attr_handle == s_cmd_handle)
        return ble_ct_cmd_access_cb(conn_handle, attr_handle, ctxt, arg);
    if (attr_handle == s_telemetry_handle)
        return ble_ct_telemetry_read_cb(conn_handle, attr_handle, ctxt, arg);
    return BLE_ATT_ERR_UNLIKELY;
}

/* ---- GATT 服务定义（无 ENC 标志）---- */
static const struct ble_gatt_svc_def s_ct_gatt_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &s_ct_svc_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]){
            { .uuid = &s_auth_uuid.u, .access_cb = ct_gatt_access_cb,
              .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_NOTIFY,
              .val_handle = &s_auth_handle },
            { .uuid = &s_telemetry_uuid.u, .access_cb = ct_gatt_access_cb,
              .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
              .val_handle = &s_telemetry_handle },
            { .uuid = &s_cmd_uuid.u, .access_cb = ct_gatt_access_cb,
              .flags = BLE_GATT_CHR_F_WRITE,
              .val_handle = &s_cmd_handle },
            { .uuid = &s_result_uuid.u, .access_cb = ct_gatt_access_cb,
              .flags = BLE_GATT_CHR_F_NOTIFY,
              .val_handle = &s_result_handle },
            { .uuid = &s_info_uuid.u, .access_cb = ct_gatt_access_cb,
              .flags = BLE_GATT_CHR_F_READ,
              .val_handle = &s_info_handle },
            {0},
        },
    },
    {0},
};

/* ---- 注册 GATT 服务（由 ble_prov pre-host-start 回调调用）---- */
void ble_ct_register_services(void) {
    if (s_services_registered) return;

    int rc = ble_gatts_count_cfg(s_ct_gatt_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "gatts_count failed: %d", rc);
        return;
    }
    rc = ble_gatts_add_svcs(s_ct_gatt_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "gatts_add failed: %d", rc);
        return;
    }

    s_services_registered = true;
    ESP_LOGI(TAG, "CSIV-CT GATT services registered");
}

/* ---- GAP 事件处理 ---- */
int ble_ct_on_gap_event(struct ble_gap_event *event, void *arg) {
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            /* MAX_CONNECTIONS=1: 新连接能建立即说明旧连接已失效,
             * 无条件接受新连接, 否则 Android 配对时的探测/重连
             * 会被误判为"第二连接"而主动断开, 表现为"配对失败" */
            if (s_conn_handle != BLE_HS_CONN_HANDLE_NONE &&
                s_conn_handle != event->connect.conn_handle) {
                ESP_LOGW(TAG, "New conn=%d while old conn=%d tracked; accepting new",
                         event->connect.conn_handle, s_conn_handle);
            }
            s_conn_handle = event->connect.conn_handle;
            ESP_LOGI(TAG, "CT connected (conn=%d)", s_conn_handle);
            /* 不主动发起安全请求（参考 Arduino 版本）：
             * 配对由手机端在需要访问加密特征时发起 */
        }
        break;

    case BLE_GAP_EVENT_ENC_CHANGE:
        /* 加密协商完成（配对成功后） */
        if (event->enc_change.status == 0) {
            ESP_LOGI(TAG, "Encryption established (conn=%d)", event->enc_change.conn_handle);
        } else {
            ESP_LOGW(TAG, "Encryption failed (status=%d)", event->enc_change.status);
        }
        break;

    case BLE_GAP_EVENT_DISCONNECT:
        if (event->disconnect.conn.conn_handle == s_conn_handle) {
            ESP_LOGI(TAG, "CT disconnected");
            s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
            s_telemetry_subscribed = false;
            s_result_subscribed = false;
            s_auth_subscribed = false;
            ble_ct_auth_reset_session();
        }
        break;

    case BLE_GAP_EVENT_SUBSCRIBE:
        if (event->subscribe.attr_handle == s_telemetry_handle)
            s_telemetry_subscribed = event->subscribe.cur_notify;
        else if (event->subscribe.attr_handle == s_result_handle)
            s_result_subscribed = event->subscribe.cur_notify;
        else if (event->subscribe.attr_handle == s_auth_handle)
            s_auth_subscribed = event->subscribe.cur_notify;
        break;

    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "MTU: %d", event->mtu.value);
        break;

    case BLE_GAP_EVENT_REPEAT_PAIRING:
        /* 已绑定设备再次请求配对（例如手机端取消配对后重配，
         * 或换手机绑定）。必须删除旧绑定记录再返回 RETRY，
         * 否则 NimBLE 会静默忽略配对请求，表现为"一直配对不上"。
         * 参考官方 bleprph 示例。 */
        ESP_LOGW(TAG, "Repeat pairing request; wiping old bond and retrying");
        {
            struct ble_gap_conn_desc desc;
            int rc = ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc);
            if (rc == 0) {
                rc = ble_store_util_delete_peer(&desc.peer_id_addr);
                if (rc != 0) {
                    ESP_LOGE(TAG, "delete old bond failed rc=%d", rc);
                }
            } else {
                ESP_LOGE(TAG, "conn_find failed rc=%d", rc);
            }
        }
        return BLE_GAP_REPEAT_PAIRING_RETRY;

    default:
        break;
    }
    return 0;
}

/* ---- 启动/停止 ---- */
bool ble_ct_start(const char *device_sn, const char *fw_version) {
    if (s_state == BLE_CT_RUNNING) return true;

    s_device_sn = device_sn ? device_sn : "";
    s_fw_version = fw_version ? fw_version : "";

    ESP_LOGI(TAG, "CSIV-CT init (SN=%s)", s_device_sn);

    ble_ct_auth_init();
    s_state = BLE_CT_RUNNING;
    return true;
}

void ble_ct_stop(void) {
    if (s_conn_handle != BLE_HS_CONN_HANDLE_NONE)
        ble_gap_terminate(s_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    s_state = BLE_CT_IDLE;
}
