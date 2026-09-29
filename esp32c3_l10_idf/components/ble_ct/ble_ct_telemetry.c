#include "ble_ct_telemetry.h"
#include "ble_ct.h"
#include "esp_log.h"
#include "host/ble_hs.h"
#include <string.h>

static const char *TAG = "ble_ct_telemetry";

#define FRAME_CTRL_FIRST  0x80
#define FRAME_CTRL_LAST   0x40
#define FRAME_CTRL_SINGLE (FRAME_CTRL_FIRST | FRAME_CTRL_LAST)
#define MAX_FRAME_PAYLOAD 509
#define MAX_FRAMES 8

/* 最近一次遥测快照（协议修订①：TELEMETRY 支持 Read，供 App 轮询；
 * 推送时同步更新，避免推送 & 读取两份数据源不一致） */
#define SNAP_MAX 1024
static char s_snapshot[SNAP_MAX];
static uint16_t s_snapshot_len = 0;

void ble_ct_telemetry_set_snapshot(const char *json, uint16_t len)
{
    if (!json || len == 0) return;
    if (len >= SNAP_MAX) len = SNAP_MAX - 1;
    memcpy(s_snapshot, json, len);
    s_snapshot[len] = '\0';
    s_snapshot_len = len;
}

bool ble_ct_telemetry_should_push(void) {
    return ble_ct_is_telemetry_subscribed();
}

static void send_frame(uint16_t conn, uint16_t handle, uint8_t ctrl,
                        const char *data, uint16_t len) {
    struct os_mbuf *om = ble_hs_mbuf_from_flat(&ctrl, 1);
    if (!om) return;
    struct os_mbuf *data_om = ble_hs_mbuf_from_flat(data, len);
    if (!data_om) { os_mbuf_free_chain(om); return; }
    os_mbuf_concat(om, data_om);
    int rc = ble_gattc_notify_custom(conn, handle, om);
    if (rc != 0) ESP_LOGW(TAG, "notify failed rc=%d", rc);
}

void ble_ct_telemetry_notify(const char *json, uint16_t len) {
    if (!ble_ct_telemetry_should_push()) return;
    uint16_t conn = ble_ct_get_conn_handle();
    if (conn == BLE_HS_CONN_HANDLE_NONE) return;
    uint16_t handle = ble_ct_get_telemetry_handle();

    /* 同步缓存快照, 使随后的 TELEMETRY Read 返回同一份数据 */
    ble_ct_telemetry_set_snapshot(json, len);

    if (len <= MAX_FRAME_PAYLOAD) {
        send_frame(conn, handle, FRAME_CTRL_SINGLE, json, len);
        return;
    }

    uint16_t total_frames = (len + MAX_FRAME_PAYLOAD - 1) / MAX_FRAME_PAYLOAD;
    if (total_frames > MAX_FRAMES) {
        ESP_LOGW(TAG, "Telemetry too large (%u bytes, %u frames), skipped", len, total_frames);
        return;
    }

    uint16_t offset = 0;
    uint8_t seq = 0;
    while (offset < len) {
        uint16_t chunk = (len - offset > MAX_FRAME_PAYLOAD) ? MAX_FRAME_PAYLOAD : (len - offset);
        uint8_t ctrl = seq & 0x3F;
        if (seq == 0) ctrl |= FRAME_CTRL_FIRST;
        if (offset + chunk >= len) ctrl |= FRAME_CTRL_LAST;
        send_frame(conn, handle, ctrl, json + offset, chunk);
        offset += chunk;
        seq++;
    }
}

/* TELEMETRY Read：返回最近一次遥测快照（未获取到数据时返回空） */
int ble_ct_telemetry_read_cb(uint16_t conn_handle, uint16_t attr_handle,
                             struct ble_gatt_access_ctxt *ctxt, void *arg) {
    (void)attr_handle; (void)arg; (void)conn_handle;
    /* 2026-09-22 起遥测不再要求鉴权 (鉴权协议已移除, App 侧无调用点) */
    if (ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR) return BLE_ATT_ERR_UNLIKELY;
    if (s_snapshot_len == 0) {
        /* 尚无快照: 返回空对象, App 解析为空数据 */
        os_mbuf_append(ctxt->om, "{}", 2);
        return 0;
    }
    os_mbuf_append(ctxt->om, s_snapshot, s_snapshot_len);
    return 0;
}
