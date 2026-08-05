#include "ble_prov.h"
#include "esp_wifi.h"
#include "esp_mac.h"
#include "esp_log.h"
#include "esp_err.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
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
static uint16_t s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static uint16_t s_status_val_handle = 0;
static uint8_t s_own_addr_type;
static char s_status_cache[16] = "waiting";

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

static const struct ble_gatt_svc_def s_gatt_svcs[] = {
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
    if (s_conn_handle == BLE_HS_CONN_HANDLE_NONE || s_status_val_handle == 0) {
        ESP_LOGI(TAG, "Status cached: %s (no BLE client)", status);
        return;
    }
    struct os_mbuf *om = ble_hs_mbuf_from_flat(status, strlen(status));
    if (om) {
        ble_gattc_notify_custom(s_conn_handle, s_status_val_handle, om);
    }
    ESP_LOGI(TAG, "Status: %s", status);
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

static int gap_event_cb(struct ble_gap_event *event, void *arg)
{
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            s_conn_handle = event->connect.conn_handle;
            if (s_conn_cb) s_conn_cb(s_conn_handle);
            ESP_LOGI(TAG, "BLE connected");
        }
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "BLE disconnected");
        s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        if (s_conn_cb) s_conn_cb(BLE_HS_CONN_HANDLE_NONE);
        s_ssid_written = false;
        s_pending_ssid[0] = '\0';
        s_pending_password[0] = '\0';
        strlcpy(s_status_cache, "waiting", sizeof(s_status_cache));
        s_state = BLE_PROV_RUNNING;
        start_advertising();
        break;
    default:
        break;
    }
    return 0;
}

static void start_advertising(void)
{
    struct ble_hs_adv_fields adv_fields = {0};
    adv_fields.uuids128 = &s_svc_uuid;
    adv_fields.num_uuids128 = 1;
    adv_fields.uuids128_is_complete = 1;
    int rc = ble_gap_adv_set_fields(&adv_fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv fields failed: %d", rc);
        return;
    }

    struct ble_hs_adv_fields rsp_fields = {0};
    rsp_fields.name = (uint8_t *)s_service_name;
    rsp_fields.name_len = strlen(s_service_name);
    rsp_fields.name_is_complete = 1;
    rc = ble_gap_adv_rsp_set_fields(&rsp_fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "scan response failed: %d", rc);
        return;
    }

    struct ble_gap_adv_params adv_params = {0};
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    adv_params.itvl_min = 0x20;
    adv_params.itvl_max = 0x40;

    rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER,
                           &adv_params, gap_event_cb, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "advertising start failed: %d", rc);
        return;
    }
    ESP_LOGI(TAG, "Advertising: %s", s_service_name);
}

static void nimble_host_reset(int reason)
{
    ESP_LOGE(TAG, "NimBLE host reset: %d", reason);
}

static void nimble_host_sync(void)
{
    int rc = ble_hs_util_ensure_addr(0);
    if (rc != 0) {
        ESP_LOGE(TAG, "BLE address setup failed: %d", rc);
        return;
    }

    rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "BLE address type failed: %d", rc);
        return;
    }

    start_advertising();
}

static void nimble_host_task(void *param)
{
    nimble_port_run();
    nimble_port_freertos_deinit();
}

bool ble_prov_start(const char *device_sn, const char *fw_version)
{
    if (s_state == BLE_PROV_RUNNING) return true;

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
        s_state = BLE_PROV_ERROR;
        return false;
    }
    ble_hs_cfg.reset_cb = nimble_host_reset;
    ble_hs_cfg.sync_cb = nimble_host_sync;
    ble_svc_gap_init();
    ble_svc_gatt_init();
    int rc = ble_svc_gap_device_name_set(s_service_name);
    if (rc != 0) {
        ESP_LOGE(TAG, "device name failed: %d", rc);
        s_state = BLE_PROV_ERROR;
        return false;
    }

    rc = ble_gatts_count_cfg(s_gatt_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "gatts_count failed: %d", rc);
        s_state = BLE_PROV_ERROR;
        return false;
    }
    rc = ble_gatts_add_svcs(s_gatt_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "gatts_add failed: %d", rc);
        s_state = BLE_PROV_ERROR;
        return false;
    }

    nimble_port_freertos_init(nimble_host_task);

    s_state = BLE_PROV_RUNNING;
    return true;
}

void ble_prov_stop(void)
{
    if (s_conn_handle != BLE_HS_CONN_HANDLE_NONE) {
        ble_gap_terminate(s_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    }
    ble_gap_adv_stop();
    s_state = BLE_PROV_IDLE;
}

void ble_prov_resume(void)
{
    /* Stack stays initialized after ble_prov_stop(); just restart advertising.
     * Guard against re-init and against resuming before the host has synced. */
    if (s_state == BLE_PROV_RUNNING) return;
    if (!ble_hs_synced()) return;
    s_state = BLE_PROV_RUNNING;
    start_advertising();
}

void ble_prov_set_callback(BleProvCallback cb) { s_callback = cb; }
void ble_prov_set_conn_callback(BleProvConnCallback cb) { s_conn_cb = cb; }
BleProvState ble_prov_get_state(void) { return s_state; }
const char *ble_prov_get_service_name(void) { return s_service_name; }

uint16_t ble_prov_get_ota_ctrl_handle(void) { return s_ota_ctrl_handle; }
uint16_t ble_prov_get_ota_data_handle(void) { return s_ota_data_handle; }
uint16_t ble_prov_get_ota_status_handle(void) { return s_ota_status_handle; }

void ble_prov_set_ota_access_cb(void *cb) { s_ota_access_cb = (BleGattAccessCb)cb; }
