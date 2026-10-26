#include "ble_ct.h"
#include "ble_ct_telemetry.h"
#include "ble_ct_cmd.h"
#include "ble_prov.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "host/ble_store.h"
#include "nimble/nimble_port.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include <string.h>
#include <stdatomic.h>

static const char *TAG = "ble_ct";

/* SN 和固件版本（由 ble_ct_start 传入） */
static const char *s_device_sn = "";
static const char *s_fw_version = "";
static char s_session_id[33] = "";

/* CSIV-CT Service UUID: 43534956-4354-1000-8000-00805f9b34fb */
static const ble_uuid128_t s_ct_svc_uuid = BLE_UUID128_INIT(
    0xfb,0x34,0x9b,0x5f,0x80,0x00,0x00,0x80,
    0x00,0x10,0x54,0x43,0x56,0x49,0x53,0x43);
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

static uint16_t s_telemetry_handle;
static uint16_t s_cmd_handle;
static uint16_t s_result_handle;
static uint16_t s_info_handle;

static uint16_t s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static BleCtState s_state = BLE_CT_IDLE;
static bool s_services_registered = false;
static bool s_telemetry_subscribed = false;
static bool s_result_subscribed = false;
static atomic_bool s_framing_enabled = false;

/* g_device_sn 和 FIRMWARE_VERSION 通过 config.h / mqtt_handler.h 引入 */

/* ---- 句柄导出 ---- */
uint16_t ble_ct_get_telemetry_handle(void) { return s_telemetry_handle; }
uint16_t ble_ct_get_result_handle(void) { return s_result_handle; }
bool ble_ct_is_telemetry_subscribed(void) { return s_telemetry_subscribed; }
bool ble_ct_is_result_subscribed(void) { return s_result_subscribed; }
uint16_t ble_ct_get_conn_handle(void) { return s_conn_handle; }
BleCtState ble_ct_get_state(void) { return s_state; }
const char *ble_ct_get_sn(void) { return s_device_sn; }
const char *ble_ct_get_session_id(void) { return s_session_id; }
bool ble_ct_framing_enabled(void) { return atomic_load(&s_framing_enabled); }
void ble_ct_set_framing_enabled(bool enabled) { atomic_store(&s_framing_enabled, enabled); }

static void new_session_id(void) {
    uint8_t random_bytes[16];
    esp_fill_random(random_bytes, sizeof(random_bytes));
    for (size_t i = 0; i < sizeof(random_bytes); ++i)
        snprintf(s_session_id + i * 2, 3, "%02x", random_bytes[i]);
}

/* ---- INFO 特征 ---- */
static int info_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                           struct ble_gatt_access_ctxt *ctxt, void *arg) {
    if (ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR) return BLE_ATT_ERR_UNLIKELY;

    char buf[512];
    int n = snprintf(buf, sizeof(buf),
        "{\"v\":2,\"type\":\"info\",\"message_id\":\"info-current\",\"session_id\":\"%s\",\"body\":{"
        "\"device_sn\":\"%s\",\"proto_version\":2,\"model\":\"CS-L10-6K2\","
        "\"firmware_version\":\"%s\","
        "\"capabilities\":[\"info\",\"telemetry\",\"control\",\"ota\",\"ota_binary_v1\","
        "\"framing_v1\",\"direct_control_v1\"],"
        "\"supported_upgrade_modules\":[\"communication_module\","
        "\"dsp_controller\",\"bms\"],"
        "\"max_message_bytes\":512,"
        /* bound: 2026-09-22 起设备端不再有绑定/鉴权协议(App 侧 checkPin 已无调用点),
         * 这里恒为 true —— "无绑定限制"。字段保留是为了不动 App 的 INFO(§8)契约。 */
        "\"bound\":true,\"production_approved\":false}}",
        s_session_id, s_device_sn, s_fw_version);
    if (n <= 0 || n >= (int)sizeof(buf)) return BLE_ATT_ERR_UNLIKELY;
    return os_mbuf_append(ctxt->om, buf, (uint16_t)n) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

/* ---- GATT 访问回调 ---- */
static int ct_gatt_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                              struct ble_gatt_access_ctxt *ctxt, void *arg) {
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
            /* AUTH 特征已随鉴权协议一并移除 (2026-09-22, 省内存): App 侧 checkPin/
             * verifyPin 已无调用点, 旧 App 写该 UUID 会得到 Attribute Not Found。 */
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
void ble_ct_reset_host(void) {
    ble_ct_cmd_reset_connection();
    ble_ct_telemetry_reset_connection();
    s_services_registered = false;
    s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
    s_telemetry_handle = s_cmd_handle = s_result_handle = s_info_handle = 0;
    s_telemetry_subscribed = s_result_subscribed = false;
    s_session_id[0] = '\0';
    s_framing_enabled = false;
}

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
            new_session_id();
            ble_ct_cmd_reset_connection();
            ble_ct_telemetry_reset_connection();
            s_framing_enabled = false;
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
            ble_ct_cmd_reset_connection();
            ble_ct_telemetry_reset_connection();
            s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
            s_session_id[0] = '\0';
            s_framing_enabled = false;
            s_telemetry_subscribed = false;
            s_result_subscribed = false;
        }
        break;

    case BLE_GAP_EVENT_SUBSCRIBE:
        if (event->subscribe.attr_handle == s_telemetry_handle)
            s_telemetry_subscribed = event->subscribe.cur_notify;
        else if (event->subscribe.attr_handle == s_result_handle)
            s_result_subscribed = event->subscribe.cur_notify;
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
    ble_prov_set_host_reset_cb(ble_ct_reset_host);

    ESP_LOGI(TAG, "CSIV-CT init (SN=%s)", s_device_sn);

    s_state = BLE_CT_RUNNING;
    return true;
}

void ble_ct_stop(void) {
    if (!ble_prov_host_acquire()) return;
    if (s_conn_handle != BLE_HS_CONN_HANDLE_NONE)
        ble_gap_terminate(s_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    s_state = BLE_CT_IDLE;
    ble_prov_host_release();
}
