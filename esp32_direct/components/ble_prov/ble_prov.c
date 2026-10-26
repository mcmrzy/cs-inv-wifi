#include "ble_prov.h"
#include "esp_wifi.h"
#include "esp_mac.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <stdatomic.h>
#include <string.h>

static const char *TAG = "ble_prov";

static BleProvState s_state = BLE_PROV_IDLE;
static BleProvCallback s_callback = NULL;
static BleProvConnCallback s_conn_cb = NULL;
static char s_service_name[32] = "CS_INV";
static char s_device_sn[32] = "";
static char s_fw_version[32] = "";
static char s_pending_ssid[33] = {0};
static char s_pending_password[65] = {0};
static bool s_ssid_written = false;
static atomic_ushort s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static uint16_t s_status_val_handle = 0;
static uint8_t s_own_addr_type;
static char s_status_cache[16] = "waiting";

/* GAP 事件转发回调 */
static BleGapEventForwardCb s_gap_forward_cb = NULL;

/* host 启动前回调 */
static BlePreHostStartCb s_pre_host_start_cb = NULL;
static BleHostResetCb s_host_reset_cb = NULL;
static SemaphoreHandle_t s_host_mutex;
/* Lifecycle callers are independent tasks. Host/GAP callbacks and producers
 * never take this mutex; stop waits for those callbacks to finish. */
static StaticSemaphore_t s_lifecycle_storage;
static SemaphoreHandle_t s_lifecycle_mutex;
static portMUX_TYPE s_lifecycle_init_lock = portMUX_INITIALIZER_UNLOCKED;
static TaskHandle_t s_host_task;
enum { BLE_PAUSE_OTA = 1U, BLE_PAUSE_MQTT = 2U };
static unsigned s_pause_owners;
/* The event/ACK outlive a timed-out caller. Only lifecycle callers modify
 * pending; the host callback only gives the dedicated binary semaphore. */
static struct ble_npl_event s_mqtt_barrier_event;
static bool s_mqtt_barrier_initialized;
static bool s_mqtt_barrier_pending;
static StaticSemaphore_t s_mqtt_barrier_ack_storage;
static SemaphoreHandle_t s_mqtt_barrier_ack;
static atomic_bool s_host_initialized;
static atomic_bool s_host_started;
static atomic_bool s_host_ready;
static atomic_bool s_host_sync_complete;
static atomic_bool s_start_failed;
static bool s_restore_needed;
static bool s_advertising_before_suspend;
static atomic_bool s_ota_suspended;
static atomic_bool s_advertising_enabled;

/* 服务 UUID: 43534956-5052-1000-8000-00805f9b34fb */
static const ble_uuid128_t s_svc_uuid = BLE_UUID128_INIT(
    0xfb, 0x34, 0x9b, 0x5f, 0x80, 0x00, 0x00, 0x80,
    0x00, 0x10, 0x52, 0x50, 0x56, 0x49, 0x53, 0x43);

/* SN: 43534956-534e-1000-8000-00805f9b34fb */
static const ble_uuid128_t s_sn_uuid = BLE_UUID128_INIT(
    0xfb, 0x34, 0x9b, 0x5f, 0x80, 0x00, 0x00, 0x80,
    0x00, 0x10, 0x4e, 0x53, 0x56, 0x49, 0x53, 0x43);

/* FW: 43534956-4657-1000-8000-00805f9b34fb */
static const ble_uuid128_t s_fw_uuid = BLE_UUID128_INIT(
    0xfb, 0x34, 0x9b, 0x5f, 0x80, 0x00, 0x00, 0x80,
    0x00, 0x10, 0x57, 0x46, 0x56, 0x49, 0x53, 0x43);

/* MAC: 43534956-4d41-1000-8000-00805f9b34fb */
static const ble_uuid128_t s_mac_uuid = BLE_UUID128_INIT(
    0xfb, 0x34, 0x9b, 0x5f, 0x80, 0x00, 0x00, 0x80,
    0x00, 0x10, 0x41, 0x4d, 0x56, 0x49, 0x53, 0x43);

/* SSID: 43534956-5353-1000-8000-00805f9b34fb */
static const ble_uuid128_t s_ssid_uuid = BLE_UUID128_INIT(
    0xfb, 0x34, 0x9b, 0x5f, 0x80, 0x00, 0x00, 0x80,
    0x00, 0x10, 0x53, 0x53, 0x56, 0x49, 0x53, 0x43);

/* PASS: 43534956-5057-1000-8000-00805f9b34fb */
static const ble_uuid128_t s_pass_uuid = BLE_UUID128_INIT(
    0xfb, 0x34, 0x9b, 0x5f, 0x80, 0x00, 0x00, 0x80,
    0x00, 0x10, 0x57, 0x50, 0x56, 0x49, 0x53, 0x43);

/* STATUS: 43534956-5354-1000-8000-00805f9b34fb */
static const ble_uuid128_t s_status_uuid = BLE_UUID128_INIT(
    0xfb, 0x34, 0x9b, 0x5f, 0x80, 0x00, 0x00, 0x80,
    0x00, 0x10, 0x54, 0x53, 0x56, 0x49, 0x53, 0x43);

/* OTA CTRL: 43534f54-4354-1000-8000-00805f9b34fb */
static const ble_uuid128_t s_ota_ctrl_uuid = BLE_UUID128_INIT(
    0xfb, 0x34, 0x9b, 0x5f, 0x80, 0x00, 0x00, 0x80,
    0x00, 0x10, 0x54, 0x43, 0x54, 0x4f, 0x53, 0x43);

/* OTA DATA: 43534f54-4441-1000-8000-00805f9b34fb */
static const ble_uuid128_t s_ota_data_uuid = BLE_UUID128_INIT(
    0xfb, 0x34, 0x9b, 0x5f, 0x80, 0x00, 0x00, 0x80,
    0x00, 0x10, 0x41, 0x44, 0x54, 0x4f, 0x53, 0x43);

/* OTA STATUS: 43534f54-5354-1000-8000-00805f9b34fb */
static const ble_uuid128_t s_ota_status_uuid = BLE_UUID128_INIT(
    0xfb, 0x34, 0x9b, 0x5f, 0x80, 0x00, 0x00, 0x80,
    0x00, 0x10, 0x54, 0x53, 0x54, 0x4f, 0x53, 0x43);

/* OTA GATT access callback (set by ble_ota) */
typedef int (*BleGattAccessCb)(uint16_t conn_handle, uint16_t attr_handle,
                               struct ble_gatt_access_ctxt *ctxt, void *arg);
static BleGattAccessCb s_ota_access_cb;

static int gatt_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                          struct ble_gatt_access_ctxt *ctxt, void *arg);
static void start_advertising(void);

static uint16_t s_sn_val_handle;
static uint16_t s_fw_val_handle;
static uint16_t s_mac_val_handle;
static uint16_t s_ssid_val_handle;
static uint16_t s_pass_val_handle;
static uint16_t s_ota_ctrl_handle;
static uint16_t s_ota_data_handle;
static uint16_t s_ota_status_handle;

bool ble_prov_host_acquire(void)
{
    if (atomic_load(&s_ota_suspended) || !s_host_mutex) return false;
    if (xSemaphoreTake(s_host_mutex, portMAX_DELAY) != pdTRUE) return false;
    if (atomic_load(&s_ota_suspended) || !s_host_initialized ||
        !s_host_started || !ble_hs_synced()) {
        xSemaphoreGive(s_host_mutex);
        return false;
    }
    return true;
}

void ble_prov_host_release(void)
{
    xSemaphoreGive(s_host_mutex);
}

static void reset_host_handles(void)
{
    atomic_store(&s_host_ready, false);
    atomic_store(&s_host_sync_complete, false);
    s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
    s_sn_val_handle = s_fw_val_handle = s_mac_val_handle = 0;
    s_ssid_val_handle = s_pass_val_handle = s_status_val_handle = 0;
    s_ota_ctrl_handle = s_ota_data_handle = s_ota_status_handle = 0;
    if (s_host_reset_cb) s_host_reset_cb();
    if (s_conn_cb) s_conn_cb(BLE_HS_CONN_HANDLE_NONE);
}

static const struct ble_gatt_svc_def s_gatt_svcs[] = {
    /* CSIV-PR 配网服务 */
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &s_svc_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]){
            {
                .uuid = &s_sn_uuid.u,
                .access_cb = gatt_access_cb,
                .flags = BLE_GATT_CHR_F_READ,
                .val_handle = &s_sn_val_handle,
            },
            {
                .uuid = &s_fw_uuid.u,
                .access_cb = gatt_access_cb,
                .flags = BLE_GATT_CHR_F_READ,
                .val_handle = &s_fw_val_handle,
            },
            {
                .uuid = &s_mac_uuid.u,
                .access_cb = gatt_access_cb,
                .flags = BLE_GATT_CHR_F_READ,
                .val_handle = &s_mac_val_handle,
            },
            {
                .uuid = &s_ssid_uuid.u,
                .access_cb = gatt_access_cb,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE,
                .val_handle = &s_ssid_val_handle,
            },
            {
                .uuid = &s_pass_uuid.u,
                .access_cb = gatt_access_cb,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE,
                .val_handle = &s_pass_val_handle,
            },
            {
                .uuid = &s_status_uuid.u,
                .access_cb = gatt_access_cb,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &s_status_val_handle,
            },
            {
                .uuid = &s_ota_ctrl_uuid.u,
                .access_cb = gatt_access_cb,
                .flags = BLE_GATT_CHR_F_WRITE,
                .val_handle = &s_ota_ctrl_handle,
            },
            {
                .uuid = &s_ota_data_uuid.u,
                .access_cb = gatt_access_cb,
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &s_ota_data_handle,
            },
            {
                .uuid = &s_ota_status_uuid.u,
                .access_cb = gatt_access_cb,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &s_ota_status_handle,
            },
            {0},
        },
    },
    {0},
};

void ble_prov_notify_status(const char *status)
{
    strlcpy(s_status_cache, status, sizeof(s_status_cache));
    if (!ble_prov_host_acquire()) return;
    if (s_conn_handle == BLE_HS_CONN_HANDLE_NONE || s_status_val_handle == 0) {
        ble_prov_host_release();
        return;
    }
    struct os_mbuf *om = ble_hs_mbuf_from_flat(status, strlen(status));
    if (om) {
        /* 失败时 NimBLE 内部已释放 om, 此处只记日志 */
        int rc = ble_gattc_notify_custom(s_conn_handle, s_status_val_handle, om);
        if (rc != 0) {
            ESP_LOGW(TAG, "notify '%s' failed rc=%d (client may not subscribe)",
                     status, rc);
        } else {
            ESP_LOGI(TAG, "Status: %s", status);
        }
    } else {
        ESP_LOGE(TAG, "notify mbuf alloc failed: %s", status);
    }
    ble_prov_host_release();
}

static int gatt_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                          struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    /* Forward OTA characteristic events to ble_ota */
    if (s_ota_access_cb &&
        (attr_handle == s_ota_ctrl_handle ||
         attr_handle == s_ota_data_handle ||
         attr_handle == s_ota_status_handle)) {
        return s_ota_access_cb(conn_handle, attr_handle, ctxt, arg);
    }

    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        if (attr_handle == s_sn_val_handle) {
            os_mbuf_append(ctxt->om, s_device_sn, strlen(s_device_sn));
            return 0;
        }
        if (attr_handle == s_fw_val_handle) {
            os_mbuf_append(ctxt->om, s_fw_version, strlen(s_fw_version));
            return 0;
        }
        if (attr_handle == s_mac_val_handle) {
            uint8_t mac[6];
            esp_read_mac(mac, ESP_MAC_BT);
            char mac_str[18];
            snprintf(mac_str, sizeof(mac_str), "%02X:%02X:%02X:%02X:%02X:%02X",
                     mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
            os_mbuf_append(ctxt->om, mac_str, strlen(mac_str));
            return 0;
        }
        if (attr_handle == s_ssid_val_handle) {
            os_mbuf_append(ctxt->om, s_pending_ssid, strlen(s_pending_ssid));
            return 0;
        }
        if (attr_handle == s_pass_val_handle) {
            os_mbuf_append(ctxt->om, s_pending_password, strlen(s_pending_password));
            return 0;
        }
        if (attr_handle == s_status_val_handle) {
            os_mbuf_append(ctxt->om, s_status_cache, strlen(s_status_cache));
            return 0;
        }
        return BLE_ATT_ERR_UNLIKELY;
    }

    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
        if (len > 64) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;

        char buf[65] = {0};
        ble_hs_mbuf_to_flat(ctxt->om, buf, len, NULL);

        if (attr_handle == s_ssid_val_handle) {
            strlcpy(s_pending_ssid, buf, sizeof(s_pending_ssid));
            s_ssid_written = true;
            ESP_LOGI(TAG, "SSID: %s", s_pending_ssid);
            return 0;
        }
        if (attr_handle == s_pass_val_handle) {
            strlcpy(s_pending_password, buf, sizeof(s_pending_password));
            ESP_LOGI(TAG, "PASS: %d bytes", len);
            if (s_ssid_written && s_pending_ssid[0] && s_callback) {
                ble_prov_notify_status("connecting");
                s_state = BLE_PROV_DONE;
                s_callback(s_pending_ssid, s_pending_password);
            }
            return 0;
        }
        return BLE_ATT_ERR_UNLIKELY;
    }

    return BLE_ATT_ERR_UNLIKELY;
}

void ble_prov_set_gap_event_forward(BleGapEventForwardCb cb) {
    s_gap_forward_cb = cb;
}

static int gap_event_cb(struct ble_gap_event *event, void *arg)
{
    /* 转发给 ble_ct */
    if (s_gap_forward_cb) {
        s_gap_forward_cb(event, arg);
    }

    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            s_conn_handle = event->connect.conn_handle;
            if (s_conn_cb) s_conn_cb(s_conn_handle);
            ESP_LOGI(TAG, "BLE connected");
        } else {
            ESP_LOGW(TAG, "Connect failed status=%d", event->connect.status);
        }
        break;
    case BLE_GAP_EVENT_ENC_CHANGE:
        /* 配对/加密结果: 0 成功, 其他为 SMP 错误码 */
        ESP_LOGW(TAG, "ENC_CHANGE status=%d", event->enc_change.status);
        break;
    case BLE_GAP_EVENT_REPEAT_PAIRING:
        ESP_LOGW(TAG, "REPEAT_PAIRING from peer");
        /* ble_ct 回调已删除旧绑定; 这里必须返回 RETRY,
         * 否则返回值覆盖 ble_ct 的 RETRY 为 IGNORE,
         * NimBLE 会静默忽略配对请求导致配对卡死 */
        return BLE_GAP_REPEAT_PAIRING_RETRY;
    case BLE_GAP_EVENT_DISCONNECT:
        /* reason 0x13=对端主动断开, 0x08/0x3e=连接超时, 0x16=本端断开,
         * 0x22=LL响应超时(controller/host 未及时响应) */
        ESP_LOGW(TAG, "BLE disconnected reason=0x%02x", event->disconnect.reason);
        s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        if (s_conn_cb) s_conn_cb(BLE_HS_CONN_HANDLE_NONE);
        s_ssid_written = false;
        s_pending_ssid[0] = '\0';
        s_pending_password[0] = '\0';
        strlcpy(s_status_cache, "waiting", sizeof(s_status_cache));
        if (!atomic_load(&s_advertising_enabled) || atomic_load(&s_ota_suspended)) break;
        s_state = BLE_PROV_RUNNING;
        start_advertising();
        break;
    default:
        break;
    }
    return 0;
}

static void host_start_error(void)
{
    atomic_store(&s_host_ready, false);
    s_state = BLE_PROV_ERROR;
    atomic_store(&s_start_failed, true);
}

static void start_advertising(void)
{
    if (!atomic_load(&s_advertising_enabled) || atomic_load(&s_ota_suspended)) return;

    /* 主广播包: Flags + 设备名 */
    struct ble_hs_adv_fields adv_fields = {0};

    /* Flags: LE General Discoverable + BR/EDR Not Supported */
    adv_fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;

    /* 设备名 */
    adv_fields.name = (uint8_t *)s_service_name;
    adv_fields.name_len = strlen(s_service_name);
    adv_fields.name_is_complete = 1;

    int rc = ble_gap_adv_set_fields(&adv_fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv fields failed: %d", rc);
        host_start_error();
        return;
    }

    /* Scan Response: 128-bit 服务 UUID。
     * 普通 BLE 外设（网关/采集器），不声明 HID 等特殊服务，
     * 避免 Android 系统蓝牙走 HID 配对流程导致配对失败。 */
    struct ble_hs_adv_fields rsp_fields = {0};
    rsp_fields.uuids128 = &s_svc_uuid;
    rsp_fields.num_uuids128 = 1;
    rsp_fields.uuids128_is_complete = 1;

    rc = ble_gap_adv_rsp_set_fields(&rsp_fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "scan response failed: %d", rc);
        host_start_error();
        return;
    }

    /* 广播参数 */
    struct ble_gap_adv_params adv_params = {0};
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    adv_params.itvl_min = 0x20;
    adv_params.itvl_max = 0x40;

    rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER,
                           &adv_params, gap_event_cb, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "advertising start failed: %d", rc);
        host_start_error();
        return;
    }
    s_state = BLE_PROV_RUNNING;
    atomic_store(&s_start_failed, false);
    atomic_store(&s_host_ready, true);
    ESP_LOGW(TAG, "BLE actual ready; advertising: %s", s_service_name);
}

static void nimble_host_reset(int reason)
{
    atomic_store(&s_host_sync_complete, false);
    ESP_LOGE(TAG, "NimBLE host reset: %d", reason);
    host_start_error();
}

static void nimble_host_sync(void)
{
    atomic_store(&s_host_sync_complete, false);
    atomic_store(&s_host_ready, false);
    int rc = ble_hs_util_ensure_addr(0);
    if (rc != 0) {
        ESP_LOGE(TAG, "BLE address setup failed: %d", rc);
        host_start_error();
        return;
    }

    rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "BLE address type failed: %d", rc);
        host_start_error();
        return;
    }

    atomic_store(&s_host_sync_complete, true);
    start_advertising();
}

static void nimble_host_task(void *param)
{
    (void)param;
    nimble_port_run();
    /* The lifecycle owner deletes our task after nimble_port_stop().
     * Keeping ownership there avoids racing a self-deleted task handle. */
    vTaskSuspend(NULL);
}

static bool lifecycle_acquire(void)
{
    portENTER_CRITICAL(&s_lifecycle_init_lock);
    if (!s_lifecycle_mutex)
        s_lifecycle_mutex = xSemaphoreCreateMutexStatic(&s_lifecycle_storage);
    portEXIT_CRITICAL(&s_lifecycle_init_lock);
    return s_lifecycle_mutex &&
           xSemaphoreTake(s_lifecycle_mutex, portMAX_DELAY) == pdTRUE;
}

static void lifecycle_release(void)
{
    xSemaphoreGive(s_lifecycle_mutex);
}

static void mqtt_barrier_ack(struct ble_npl_event *event)
{
    (void)event;
    /* Never acquire the lifecycle or producer mutex from the host task. */
    xSemaphoreGive(s_mqtt_barrier_ack);
}

/* Called with the lifecycle mutex, after successful advertising disable. HCI
 * work already queued on the default host queue must run before this ACK. */
static bool mqtt_host_barrier(void)
{
    if (!s_host_started) return false;
    if (!s_mqtt_barrier_ack)
        s_mqtt_barrier_ack = xSemaphoreCreateBinaryStatic(&s_mqtt_barrier_ack_storage);
    if (!s_mqtt_barrier_ack) return false;
    if (s_mqtt_barrier_pending) {
        /* A timeout does not cancel or reuse the outstanding event. Consume
         * its late ACK first, then require a NEW event/ACK for this attempt. */
        if (xSemaphoreTake(s_mqtt_barrier_ack, 0) != pdTRUE) return false;
        s_mqtt_barrier_pending = false;
    }
    if (!s_mqtt_barrier_initialized) {
        ble_npl_event_init(&s_mqtt_barrier_event, mqtt_barrier_ack, NULL);
        s_mqtt_barrier_initialized = true;
    }
    s_mqtt_barrier_pending = true;
    /* NPL put itself may block on a full SDK queue (portMAX_DELAY). The 1s
     * bound below applies to ACK waiting, not this SDK enqueue operation. */
    ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &s_mqtt_barrier_event);
    if (xSemaphoreTake(s_mqtt_barrier_ack, pdMS_TO_TICKS(1000)) != pdTRUE)
        return false;
    s_mqtt_barrier_pending = false;
    return true;
}

/* Host has stopped and our task has been deleted (or never created). Release
 * NPL event storage BEFORE nimble_port_deinit destroys its allocation pool. */
static void mqtt_barrier_cleanup(void)
{
    if (s_mqtt_barrier_initialized) {
        ble_npl_eventq_remove(nimble_port_get_dflt_eventq(), &s_mqtt_barrier_event);
        ble_npl_event_deinit(&s_mqtt_barrier_event);
        memset(&s_mqtt_barrier_event, 0, sizeof(s_mqtt_barrier_event));
        s_mqtt_barrier_initialized = false;
    }
    s_mqtt_barrier_pending = false;
    if (s_mqtt_barrier_ack)
        while (xSemaphoreTake(s_mqtt_barrier_ack, 0) == pdTRUE) {}
}

/* Caller holds the lifecycle mutex. Only restore_owned may call this while
 * retaining the final pause owner, so a failed restore remains retryable. */
static bool start_host_locked(const char *device_sn, const char *fw_version)
{
    if (s_host_initialized) {
        if (atomic_load(&s_ota_suspended)) return false;
        if (atomic_load(&s_start_failed)) return false;
        /* Accepted host startup is asynchronous. Do not attempt advertising
         * or claim actual readiness before its sync callback has completed. */
        if (atomic_load(&s_host_sync_complete)) ble_prov_resume();
        return !atomic_load(&s_start_failed);
    }
    atomic_store(&s_host_ready, false);
    atomic_store(&s_start_failed, false);
    /* BLE used >50KB on site. Keep at least 12KB headroom and a large internal
     * contiguous block before any NimBLE/controller/task allocations begin.
     * Main must first release MQTT TLS/client/stack; this is admission, not a
     * heap reservation against unrelated concurrent allocations. */
    const unsigned caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    const size_t free_bytes = heap_caps_get_free_size(caps);
    const size_t largest = heap_caps_get_largest_free_block(caps);
    if (free_bytes < 65536U || largest < 32768U) {
        ESP_LOGW(TAG, "BLE start denied: internal8 free=%u largest=%u (need 65536/32768)",
                 (unsigned)free_bytes, (unsigned)largest);
        host_start_error();
        return false;
    }
    if (!s_host_mutex) s_host_mutex = xSemaphoreCreateMutex();
    if (!s_host_mutex) {
        host_start_error();
        return false;
    }

    s_ssid_written = false;
    s_pending_ssid[0] = '\0';
    s_pending_password[0] = '\0';

    strlcpy(s_device_sn, device_sn ? device_sn : "", sizeof(s_device_sn));
    strlcpy(s_fw_version, fw_version ? fw_version : "", sizeof(s_fw_version));

    if (device_sn && device_sn[0]) {
        snprintf(s_service_name, sizeof(s_service_name), "CS_INV_%s", device_sn);
    }

    ESP_LOGI(TAG, "BLE prov start: %s", s_service_name);

    esp_err_t ret = nimble_port_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "nimble init failed: %s", esp_err_to_name(ret));
        host_start_error();
        return false;
    }
    s_host_initialized = true;
    reset_host_handles();
    ble_hs_cfg.reset_cb = nimble_host_reset;
    ble_hs_cfg.sync_cb = nimble_host_sync;

    /* 安全配置（参考 Arduino NimBLE 默认行为）：
     * - 不强制 MITM：sm_io_cap=NO_IO 只能用 Just Works，不支持 MITM
     * - 允许绑定：手机端发起配对时保存密钥（需 NVS_PERSIST 持久化）
     * - 不主动发起安全请求：由手机端在需要时触发
     */
    ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_NO_IO;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 0;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;

    ble_svc_gap_init();
    ble_svc_gatt_init();
    int rc;

    rc = ble_svc_gap_device_name_set(s_service_name);
    if (rc != 0) {
        ESP_LOGE(TAG, "device name failed: %d", rc);
        goto init_failed;
    }

    rc = ble_gatts_count_cfg(s_gatt_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "gatts_count failed: %d", rc);
        goto init_failed;
    }
    rc = ble_gatts_add_svcs(s_gatt_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "gatts_add failed: %d", rc);
        goto init_failed;
    }

    /* host 启动前回调：注册额外 GATT 服务（如 ble_ct） */
    if (s_pre_host_start_cb) {
        s_pre_host_start_cb();
    }

    atomic_store(&s_advertising_enabled,
                 s_restore_needed ? s_advertising_before_suspend : true);
    s_state = BLE_PROV_IDLE; /* task acceptance is not advertising readiness */
    /* The SDK wrapper discards task creation failure. Own the handle and check
     * the allocation before claiming a running host (same stack/priority). */
    if (xTaskCreate(nimble_host_task, "nimble_host",
                    CONFIG_BT_NIMBLE_HOST_TASK_STACK_SIZE, NULL,
                    configMAX_PRIORITIES - 4, &s_host_task) != pdPASS) {
        ESP_LOGE(TAG, "NimBLE host task creation failed");
        s_host_task = NULL;
        goto init_failed;
    }
    s_host_started = true;
    ESP_LOGW(TAG, "BLE host task accepted; actual_ready=%d (async sync/advertising)",
             (int)atomic_load(&s_host_ready));
    return true;

init_failed:
    mqtt_barrier_cleanup();
    if (nimble_port_deinit() != ESP_OK)
        ESP_LOGE(TAG, "NimBLE init rollback failed");
    s_host_started = false;
    s_host_initialized = false;
    reset_host_handles();
    host_start_error();
    return false;
}

bool ble_prov_start(const char *device_sn, const char *fw_version)
{
    if (!lifecycle_acquire()) return false;
    bool result = false;
    if (!s_pause_owners && !atomic_load(&s_ota_suspended))
        result = start_host_locked(device_sn, fw_version);
    lifecycle_release();
    return result;
}

void ble_prov_stop(void)
{
    if (atomic_load(&s_ota_suspended)) return;
    atomic_store(&s_host_ready, false);
    atomic_store(&s_advertising_enabled, false);
    if (!ble_prov_host_acquire()) return;
    if (s_conn_handle != BLE_HS_CONN_HANDLE_NONE) {
        ble_gap_terminate(s_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    }
    ble_gap_adv_stop();
    s_state = BLE_PROV_IDLE;
    ble_prov_host_release();
}

void ble_prov_resume(void)
{
    /* Stack stays initialized after ble_prov_stop(); just restart advertising.
     * Guard against re-init and against resuming before the host has synced. */
    if (s_state == BLE_PROV_RUNNING || !ble_prov_host_acquire()) return;
    atomic_store(&s_advertising_enabled, true);
    s_state = BLE_PROV_RUNNING;
    start_advertising();
    ble_prov_host_release();
}

static bool suspend_owned(unsigned owner)
{
    if (!lifecycle_acquire()) return false;
    bool result = false;
    if (s_pause_owners) {
        /* Ownership is published only after successful teardown. */
        s_pause_owners |= owner;
        result = true;
        goto done;
    }
    if (!s_host_initialized) {
        /* A failed deinit is not proof that the controller was reclaimed.
         * Recover it through restore before granting any new pause owner. */
        if (s_restore_needed) goto done;
        atomic_store(&s_ota_suspended, true);
        s_pause_owners |= owner;
        result = true;
        goto done;
    }
    if (ble_prov_is_connected()) goto done;
    if (xSemaphoreTake(s_host_mutex, portMAX_DELAY) != pdTRUE) goto done;
    if (!s_restore_needed) {
        s_restore_needed = true;
        s_advertising_before_suspend = atomic_load(&s_advertising_enabled);
    }
    /* Drain producers, but never hold their mutex across host callbacks. */
    atomic_store(&s_ota_suspended, true);
    atomic_store(&s_advertising_enabled, false);
    xSemaphoreGive(s_host_mutex);

    {
        /* Both cloud reconnect and remote OTA are idle-only: neither may
         * terminate an active phone or a local BLE OTA session. */
        /* Stop connectable advertising before the final peer check. Treat
         * every error (including EALREADY after a connection won the race) as
         * uncertain: leave the host alive and acquire no pause owner. After a
         * successful disable, drain earlier host events through a barrier
         * before reading the peer handle and deciding whether to stop. */
        const int rc = ble_gap_adv_stop();
        if (rc != 0 || !mqtt_host_barrier() || ble_prov_is_connected()) {
            atomic_store(&s_advertising_enabled, s_advertising_before_suspend);
            atomic_store(&s_ota_suspended, false);
            s_restore_needed = false;
            if (s_advertising_before_suspend && !ble_prov_is_connected())
                start_advertising();
            goto done;
        }
    }
    if (s_host_started) {
        const int stop_result = nimble_port_stop();
        if (stop_result != 0) {
            /* No owner is acquired; restore can repair the producer/adv gate. */
            ESP_LOGE(TAG, "BLE host stop failed: %d", stop_result);
            goto done;
        }
        vTaskDelete(s_host_task);
        s_host_task = NULL;
        s_host_started = false;
        atomic_store(&s_host_ready, false);
    }
    reset_host_handles();
    mqtt_barrier_cleanup();
    const esp_err_t deinit_result = nimble_port_deinit();
    /* IDF may have freed the host before a controller deinit error. Never call
     * that host again; keep the producer gate closed until a successful init. */
    s_host_initialized = false;
    s_state = BLE_PROV_IDLE;
    if (deinit_result != ESP_OK) ESP_LOGE(TAG, "BLE deinit failed: %s", esp_err_to_name(deinit_result));
    if (deinit_result == ESP_OK) {
        s_pause_owners |= owner;
        result = true;
    }
done:
    lifecycle_release();
    return result;
}

static bool restore_owned(unsigned owner)
{
    if (!lifecycle_acquire()) return false;
    bool result = true;
    const unsigned remaining = s_pause_owners & ~owner;
    if (remaining) {
        s_pause_owners = remaining;
        goto done;
    }
    if (!s_restore_needed) {
        s_pause_owners = 0;
        atomic_store(&s_ota_suspended, false);
        goto done;
    }
    if (s_host_initialized) {
        /* stop failed before deinit, so the original host is still alive. */
        atomic_store(&s_advertising_enabled, s_advertising_before_suspend);
        atomic_store(&s_ota_suspended, false);
        if (s_advertising_before_suspend && !ble_prov_is_connected()) start_advertising();
        s_restore_needed = false;
        s_pause_owners = 0;
        goto done;
    }
    char device_sn[sizeof(s_device_sn)], version[sizeof(s_fw_version)];
    strlcpy(device_sn, s_device_sn, sizeof(device_sn));
    strlcpy(version, s_fw_version, sizeof(version));
    atomic_store(&s_ota_suspended, false);
    if (!start_host_locked(device_sn, version)) {
        atomic_store(&s_ota_suspended, true);
        result = false;
        goto done;
    }
    s_restore_needed = false;
    s_pause_owners = 0;
done:
    lifecycle_release();
    return result;
}

bool ble_prov_suspend_for_ota(void) { return suspend_owned(BLE_PAUSE_OTA); }
bool ble_prov_restore_after_ota(void) { return restore_owned(BLE_PAUSE_OTA); }
bool ble_prov_suspend_for_mqtt(void) { return suspend_owned(BLE_PAUSE_MQTT); }
bool ble_prov_restore_after_mqtt(void) { return restore_owned(BLE_PAUSE_MQTT); }

void ble_prov_set_callback(BleProvCallback cb) { s_callback = cb; }
void ble_prov_set_conn_callback(BleProvConnCallback cb) { s_conn_cb = cb; }
bool ble_prov_is_connected(void)
{
    return atomic_load(&s_conn_handle) != BLE_HS_CONN_HANDLE_NONE;
}
bool ble_prov_is_suspended(void)
{
    return atomic_load(&s_ota_suspended);
}

bool ble_prov_is_ready(void)
{
    return atomic_load(&s_host_ready) && !atomic_load(&s_start_failed) &&
           !atomic_load(&s_ota_suspended) && atomic_load(&s_host_started);
}

bool ble_prov_start_failed(void)
{
    return atomic_load(&s_start_failed);
}

bool ble_prov_recover_start_failure(void)
{
    if (!lifecycle_acquire()) return false;
    bool result = false;
    /* Never tear down pending/healthy startup, release another owner's pause,
     * or disconnect a peer just because main wants to retry local mode. */
    if (!atomic_load(&s_start_failed) || s_pause_owners ||
        ble_prov_is_connected()) goto done;
    if (!s_host_initialized) {
        /* Partial deinit cannot be safely repeated against a destroyed NPL
         * pool. Keep its recovery debt and reject a fresh start. */
        if (s_restore_needed) goto done;
        goto recovered;
    }
    if (!s_host_started || !s_host_task) goto done;
    if (xSemaphoreTake(s_host_mutex, portMAX_DELAY) != pdTRUE) goto done;
    const bool was_suspended = atomic_load(&s_ota_suspended);
    const bool was_advertising = atomic_load(&s_advertising_enabled);
    atomic_store(&s_ota_suspended, true);
    atomic_store(&s_advertising_enabled, false);
    xSemaphoreGive(s_host_mutex);

    /* Failed address/advertising startup often has no active advertising.
     * EALREADY is acceptable HERE only: require a fresh host barrier and a
     * final peer check exactly as for an idle pause before stopping anything. */
    const int adv_result = ble_gap_adv_stop();
    if ((adv_result != 0 && adv_result != BLE_HS_EALREADY) ||
        !mqtt_host_barrier() || ble_prov_is_connected()) goto keep_host;
    const int stop_result = nimble_port_stop();
    if (stop_result != 0) {
        ESP_LOGW(TAG, "Failed BLE startup cleanup: host stop=%d; retry later", stop_result);
        goto keep_host;
    }
    vTaskDelete(s_host_task);
    s_host_task = NULL;
    s_host_started = false;
    reset_host_handles();
    mqtt_barrier_cleanup();
    const esp_err_t deinit_result = nimble_port_deinit();
    s_host_initialized = false;
    if (deinit_result != ESP_OK) {
        s_restore_needed = true;
        host_start_error();
        ESP_LOGE(TAG, "Failed BLE startup cleanup: deinit=%s; restart required",
                 esp_err_to_name(deinit_result));
        goto done;
    }
recovered:
    s_restore_needed = false;
    atomic_store(&s_advertising_enabled, false);
    atomic_store(&s_ota_suspended, false);
    atomic_store(&s_host_ready, false);
    atomic_store(&s_host_sync_complete, false);
    atomic_store(&s_start_failed, false);
    s_state = BLE_PROV_IDLE;
    ESP_LOGW(TAG, "Failed BLE startup cleaned up; a fresh start may be requested");
    result = true;
    goto done;
keep_host:
    /* Preserve failure status for the caller's next retry. Do not attempt
     * advertising again with the address/startup error still unresolved. */
    atomic_store(&s_advertising_enabled, was_advertising);
    atomic_store(&s_ota_suspended, was_suspended);
done:
    lifecycle_release();
    return result;
}

BleProvState ble_prov_get_state(void) { return s_state; }
const char *ble_prov_get_service_name(void) { return s_service_name; }

uint16_t ble_prov_get_ota_ctrl_handle(void) { return s_ota_ctrl_handle; }
uint16_t ble_prov_get_ota_data_handle(void) { return s_ota_data_handle; }
uint16_t ble_prov_get_ota_status_handle(void) { return s_ota_status_handle; }

void ble_prov_set_ota_access_cb(void *cb) { s_ota_access_cb = (BleGattAccessCb)cb; }

void ble_prov_set_pre_host_start_cb(BlePreHostStartCb cb) {
    s_pre_host_start_cb = cb;
}

void ble_prov_set_host_reset_cb(BleHostResetCb cb) { s_host_reset_cb = cb; }
