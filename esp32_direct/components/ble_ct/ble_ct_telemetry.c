#include "ble_ct_telemetry.h"
#include "ble_ct.h"
#include "ble_ct_frames.h"
#include "ble_prov.h"
#include "esp_log.h"
#include "host/ble_hs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include <string.h>

static const char *TAG = "ble_ct_telemetry";

#define LEGACY_MAX_FRAMES 8u

/* 最近一次遥测快照（协议修订①：TELEMETRY 支持 Read，供 App 轮询；
 * 推送时同步更新，避免推送 & 读取两份数据源不一致） */
#define SNAP_MAX BLE_CT_FRAME_MAX_MESSAGE_BYTES
static char s_snapshot[SNAP_MAX + 1u];
static uint16_t s_snapshot_len = 0;
static portMUX_TYPE s_snapshot_lock = portMUX_INITIALIZER_UNLOCKED;
/* Freeze one value across a Read/Read Blob sequence on the single BLE link. */
static char s_read_snapshot[SNAP_MAX];
static uint16_t s_read_len, s_read_conn;
static bool s_read_valid;

void ble_ct_telemetry_reset_connection(void) {
    portENTER_CRITICAL(&s_snapshot_lock);
    s_read_valid = false;
    portEXIT_CRITICAL(&s_snapshot_lock);
}

void ble_ct_telemetry_set_snapshot(const char *json, uint16_t len)
{
    if (!json || len == 0 || len > SNAP_MAX) return;
    /* Copy only under the bounded critical section; never call NimBLE here. */
    portENTER_CRITICAL(&s_snapshot_lock);
    memcpy(s_snapshot, json, len);
    s_snapshot[len] = '\0';
    s_snapshot_len = len;
    portEXIT_CRITICAL(&s_snapshot_lock);
}

bool ble_ct_telemetry_should_push(void) {
    return ble_ct_is_telemetry_subscribed();
}

static bool send_frame(uint16_t conn, uint16_t handle,
                       const uint8_t *data, uint16_t len) {
    struct os_mbuf *om = ble_hs_mbuf_from_flat(data, len);
    if (!om) return false;
    int rc = ble_gattc_notify_custom(conn, handle, om);
    if (rc != 0) ESP_LOGW(TAG, "notify failed rc=%d", rc);
    return rc == 0;
}

void ble_ct_telemetry_notify(const char *json, uint16_t len) {
    if (!json || !len || len > SNAP_MAX) return;
    /* Reads must remain fresh even without a connection, subscription or host. */
    ble_ct_telemetry_set_snapshot(json, len);
    if (!ble_prov_host_acquire()) return;
    if (!ble_ct_telemetry_should_push()) {
        ble_prov_host_release();
        return;
    }
    uint16_t conn = ble_ct_get_conn_handle();
    if (conn == BLE_HS_CONN_HANDLE_NONE) {
        ble_prov_host_release();
        return;
    }
    uint16_t handle = ble_ct_get_telemetry_handle();

    const uint16_t mtu = ble_att_mtu(conn);
    const size_t payload = ble_ct_frame_payload_capacity(mtu);
    const size_t limit = ble_ct_framing_enabled() ? BLE_CT_FRAME_MAX_FRAMES : LEGACY_MAX_FRAMES;
    const size_t total_frames = payload ? (len + payload - 1u) / payload : 0;
    if (!total_frames || total_frames > limit) {
        ESP_LOGW(TAG, "Telemetry too large (%u bytes, %u frames), skipped", len, (unsigned)total_frames);
        ble_prov_host_release();
        return;
    }

    size_t offset = 0;
    uint8_t seq = 0;
    uint8_t frame[512];
    while (offset < len) {
        size_t frame_len = 0;
        if (ble_ct_frames_next((const uint8_t *)json, len, mtu, &offset, &seq,
                               frame, sizeof(frame), &frame_len) < 0 ||
            !send_frame(conn, handle, frame, (uint16_t)frame_len)) break;
    }
    ble_prov_host_release();
}

/* TELEMETRY Read：返回最近一次遥测快照（未获取到数据时返回空） */
int ble_ct_telemetry_read_cb(uint16_t conn_handle, uint16_t attr_handle,
                             struct ble_gatt_access_ctxt *ctxt, void *arg) {
    (void)attr_handle; (void)arg;
    /* 2026-09-22 起遥测不再要求鉴权 (鉴权协议已移除, App 侧无调用点) */
    if (ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR) return BLE_ATT_ERR_UNLIKELY;
    char snapshot[SNAP_MAX];
    portENTER_CRITICAL(&s_snapshot_lock);
    if (ctxt->offset == 0) {
        s_read_len = s_snapshot_len ? s_snapshot_len : 2;
        memcpy(s_read_snapshot, s_snapshot_len ? s_snapshot : "{}", s_read_len);
        s_read_conn = conn_handle;
        s_read_valid = true;
    }
    if (!s_read_valid || s_read_conn != conn_handle || ctxt->offset > s_read_len) {
        portEXIT_CRITICAL(&s_snapshot_lock);
        return BLE_ATT_ERR_INVALID_OFFSET;
    }
    const uint16_t len = s_read_len;
    memcpy(snapshot, s_read_snapshot, len);
    portEXIT_CRITICAL(&s_snapshot_lock);
    /* Append after unlocking: radio allocation or another producer cannot
     * block an atomic snapshot update, and both length and bytes agree. */
    /* NimBLE applies ctxt->offset itself: append the entire frozen value. */
    int rc = os_mbuf_append(ctxt->om, snapshot, len);
    return rc == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}
