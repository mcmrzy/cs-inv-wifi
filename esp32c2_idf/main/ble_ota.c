#include "ble_ota.h"
#include "ble_prov.h"
#include "ota_service.h"
#include "config.h"

#include "esp_log.h"
#include "esp_ota_ops.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "nimble/nimble_port.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <string.h>

static const char *TAG = "ble_ota";

/* ── Ring buffer for streaming data ─────────────────────────── */
#define RING_BUF_SIZE   2048
#define RING_BUF_MASK   (RING_BUF_SIZE - 1)

static uint8_t s_ring_buf[RING_BUF_SIZE];
static volatile uint32_t s_ring_head;
static volatile uint32_t s_ring_tail;
static SemaphoreHandle_t s_data_sem;
static SemaphoreHandle_t s_space_sem;
static volatile bool s_transfer_active;

/* ── State ──────────────────────────────────────────────────── */
static uint16_t s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static BleOtaState s_state = BLE_OTA_IDLE;
static uint32_t s_file_size;
static uint32_t s_received;
static uint16_t s_expected_seq;
static uint8_t  s_target;
static char     s_version[32];
static uint8_t  s_sha256[32];
static bool     s_active;
static BleOtaFinishCallback s_finish_cb;

/* ── Forward declarations ───────────────────────────────────── */
static void notify_status(void);
static void notify_data_ack(uint8_t status, uint16_t seq);
static void reset_state(void);
static void finish(bool success, const char *msg);

/* ── Ring buffer helpers ────────────────────────────────────── */
static uint32_t ring_available(void)
{
    return (s_ring_head - s_ring_tail) & RING_BUF_MASK;
}

static uint32_t ring_free(void)
{
    return RING_BUF_SIZE - 1 - ring_available();
}

static size_t ring_write(const uint8_t *data, size_t len)
{
    size_t written = 0;
    while (written < len && ring_free() > 0) {
        s_ring_buf[s_ring_head & RING_BUF_MASK] = data[written++];
        s_ring_head = (s_ring_head + 1) & RING_BUF_MASK;
    }
    return written;
}

static size_t ring_read(uint8_t *buf, size_t max_len)
{
    size_t rd = 0;
    while (rd < max_len && ring_available() > 0) {
        buf[rd++] = s_ring_buf[s_ring_tail & RING_BUF_MASK];
        s_ring_tail = (s_ring_tail + 1) & RING_BUF_MASK;
    }
    return rd;
}

static void ring_reset(void)
{
    s_ring_head = 0;
    s_ring_tail = 0;
    if (s_data_sem) xSemaphoreGive(s_data_sem);
}

/* ── OtaStreamReader: reads from ring buffer ────────────────── */
static int ble_ota_reader(void *context, uint8_t *buffer, size_t capacity)
{
    (void)context;
    if (!s_transfer_active) return -1;

    while (ring_available() == 0 && s_transfer_active) {
        if (xSemaphoreTake(s_data_sem, pdMS_TO_TICKS(100)) == pdTRUE)
            break;
    }
    if (!s_transfer_active) return -1;

    size_t got = ring_read(buffer, capacity);
    if (got > 0 && s_space_sem) xSemaphoreGive(s_space_sem);
    return (int)got;
}

/* ── OTA worker task ────────────────────────────────────────── */
static void ota_worker_task(void *arg)
{
    (void)arg;
    OtaManifest manifest = {0};
    manifest.source = OTA_SOURCE_LOCAL;
    manifest.target = s_target == 0 ? OTA_TARGET_ESP : OTA_TARGET_ARM;
    manifest.file_size = s_file_size;
    memcpy(manifest.sha256, s_sha256, 32);
    strlcpy(manifest.version, s_version, sizeof(manifest.version));
    manifest.security_version = 0;

    ESP_LOGI(TAG, "OTA start: target=%s size=%lu ver=%s",
             s_target == 0 ? "ESP" : "ARM",
             (unsigned long)s_file_size, s_version);

    OtaResult result = ota_service_start_local(&manifest, ble_ota_reader, NULL);

    if (result == OTA_RESULT_OK)
        finish(true, "OTA completed successfully");
    else {
        char msg[64];
        snprintf(msg, sizeof(msg), "OTA failed: %s", ota_result_code(result));
        finish(false, msg);
    }
    vTaskDelete(NULL);
}

/* ── CTRL command handlers ──────────────────────────────────── */
static void handle_start(const uint8_t *data, uint16_t len)
{
    if (len < 37) { ESP_LOGE(TAG, "START too short: %d", len); return; }
    if (s_active) { ESP_LOGW(TAG, "OTA already active"); return; }

    s_target = data[0];
    s_file_size = data[1] | (data[2] << 8) | (data[3] << 16) | (data[4] << 24);
    memcpy(s_sha256, &data[5], 32);

    if (len > 37) {
        size_t vlen = len - 37;
        if (vlen >= sizeof(s_version)) vlen = sizeof(s_version) - 1;
        memcpy(s_version, &data[37], vlen);
        s_version[vlen] = '\0';
    } else {
        strcpy(s_version, "unknown");
    }

    if (s_file_size == 0 || s_file_size > 2 * 1024 * 1024) {
        ESP_LOGE(TAG, "Bad size: %lu", (unsigned long)s_file_size); return;
    }

    ring_reset();
    s_received = 0;
    s_expected_seq = 0;
    s_transfer_active = true;
    s_active = true;
    s_state = BLE_OTA_READY;
    notify_status();

    if (xTaskCreate(ota_worker_task, "ble_ota_wr", 4096, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Task create failed");
        finish(false, "Task creation failed");
    }
}

static void handle_cancel(void)
{
    if (s_active) {
        s_transfer_active = false;
        ota_service_cancel(NULL);
        finish(false, "Cancelled by user");
    }
}

/* ── GATT access callback (called from ble_prov) ────────────── */
static int ota_gatt_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                              struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)arg;
    uint16_t ctrl_h = ble_prov_get_ota_ctrl_handle();
    uint16_t data_h = ble_prov_get_ota_data_handle();
    uint16_t status_h = ble_prov_get_ota_status_handle();

    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
        uint8_t buf[256];
        if (len > sizeof(buf)) len = sizeof(buf);
        ble_hs_mbuf_to_flat(ctxt->om, buf, len, NULL);

        if (attr_handle == ctrl_h) {
            if (len < 1) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
            if (buf[0] == 0x01) handle_start(buf + 1, len - 1);
            else if (buf[0] == 0x02) handle_cancel();
            return 0;
        }

        if (attr_handle == data_h) {
            if (!s_transfer_active || !s_active) {
                notify_data_ack(3, 0);
                return 0;
            }
            if (len < 3) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;

            uint16_t seq = buf[0] | (buf[1] << 8);
            if (seq != s_expected_seq) {
                notify_data_ack(2, s_expected_seq);
                return 0;
            }

            size_t payload_len = len - 2;
            int waited = 0;
            while (ring_free() < payload_len && s_transfer_active) {
                if (xSemaphoreTake(s_space_sem, pdMS_TO_TICKS(100)) == pdTRUE) break;
                if (++waited > 50) { notify_data_ack(1, seq); return 0; }
            }
            if (!s_transfer_active) return 0;

            if (ring_write(buf + 2, payload_len) != payload_len) {
                notify_data_ack(1, seq); return 0;
            }

            s_received += payload_len;
            s_expected_seq = seq + 1;
            if (s_data_sem) xSemaphoreGive(s_data_sem);
            notify_data_ack(0, seq);
            if (s_state == BLE_OTA_READY) s_state = BLE_OTA_RECEIVING;
            return 0;
        }
    }

    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR && attr_handle == status_h) {
        uint8_t st[10];
        uint8_t progress = s_file_size > 0 ? (uint8_t)((s_received * 100) / s_file_size) : 0;
        st[0] = (uint8_t)s_state; st[1] = progress;
        st[2] = s_received; st[3] = s_received >> 8;
        st[4] = s_received >> 16; st[5] = s_received >> 24;
        st[6] = s_file_size; st[7] = s_file_size >> 8;
        st[8] = s_file_size >> 16; st[9] = s_file_size >> 24;
        os_mbuf_append(ctxt->om, st, 10);
        return 0;
    }

    return BLE_ATT_ERR_UNLIKELY;
}

/* ── Notify helpers ─────────────────────────────────────────── */
static void notify_status(void)
{
    if (s_conn_handle == BLE_HS_CONN_HANDLE_NONE) return;
    uint8_t st[10];
    uint8_t progress = s_file_size > 0 ? (uint8_t)((s_received * 100) / s_file_size) : 0;
    st[0] = (uint8_t)s_state; st[1] = progress;
    st[2] = s_received; st[3] = s_received >> 8;
    st[4] = s_received >> 16; st[5] = s_received >> 24;
    st[6] = s_file_size; st[7] = s_file_size >> 8;
    st[8] = s_file_size >> 16; st[9] = s_file_size >> 24;
    struct os_mbuf *om = ble_hs_mbuf_from_flat(st, sizeof(st));
    if (om) ble_gattc_notify_custom(s_conn_handle, ble_prov_get_ota_status_handle(), om);
}

static void notify_data_ack(uint8_t status, uint16_t seq)
{
    if (s_conn_handle == BLE_HS_CONN_HANDLE_NONE) return;
    uint8_t ack[3] = { status, seq & 0xFF, (seq >> 8) & 0xFF };
    struct os_mbuf *om = ble_hs_mbuf_from_flat(ack, sizeof(ack));
    if (om) ble_gattc_notify_custom(s_conn_handle, ble_prov_get_ota_data_handle(), om);
}

/* ── State management ───────────────────────────────────────── */
static void reset_state(void)
{
    s_state = BLE_OTA_IDLE;
    s_active = false;
    s_transfer_active = false;
    s_file_size = 0;
    s_received = 0;
    s_expected_seq = 0;
    s_target = 0;
    s_version[0] = '\0';
    ring_reset();
}

static void finish(bool success, const char *msg)
{
    s_state = success ? BLE_OTA_DONE : BLE_OTA_ERROR;
    s_transfer_active = false;
    ESP_LOGI(TAG, "Finish: %s - %s", success ? "OK" : "FAIL", msg ? msg : "");
    notify_status();
    if (s_finish_cb) s_finish_cb(success, msg);
    s_active = false;
}

/* ── Public API ─────────────────────────────────────────────── */
bool ble_ota_init(void)
{
    s_data_sem = xSemaphoreCreateBinary();
    s_space_sem = xSemaphoreCreateBinary();
    if (!s_data_sem || !s_space_sem) {
        ESP_LOGE(TAG, "Semaphore creation failed");
        return false;
    }

    /* Register OTA access callback with ble_prov (no separate GATT service) */
    ble_prov_set_ota_access_cb((void *)ota_gatt_access_cb);
    reset_state();
    ESP_LOGI(TAG, "BLE OTA registered (shared GATT service)");
    return true;
}

bool ble_ota_is_active(void) { return s_active; }

void ble_ota_cancel(void) { if (s_active) handle_cancel(); }

void ble_ota_set_finish_callback(BleOtaFinishCallback cb) { s_finish_cb = cb; }

void ble_ota_on_conn_change(uint16_t conn_handle)
{
    if (conn_handle == BLE_HS_CONN_HANDLE_NONE) {
        if (s_active) {
            s_transfer_active = false;
            ota_service_cancel(NULL);
            finish(false, "BLE disconnected");
        }
        reset_state();
    }
    s_conn_handle = conn_handle;
}
