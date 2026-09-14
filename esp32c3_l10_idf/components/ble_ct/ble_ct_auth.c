#include "ble_ct_auth.h"
#include "ble_ct.h"
#include "ble_ct_clock.h"
#include "ble_ct_pin.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"
#include "host/ble_hs.h"
#include "host/ble_gatt.h"
#include "cJSON.h"
#include "mbedtls/base64.h"
#include "mbedtls/md.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "ble_ct_auth";

#define NVS_NAMESPACE "ble_ct"
#define NVS_KEY_DEVICE_KEY "device_key"
#define DEVICE_KEY_LEN 32

static uint8_t s_device_key[DEVICE_KEY_LEN];
static bool s_is_bound = false;
static BleCtAuthState s_auth_state = BLE_CT_AUTH_UNAUTHENTICATED;
static uint32_t s_bind_window_close_ms = 0;
#define BIND_WINDOW_DURATION_MS (10 * 60 * 1000)

static uint32_t now_ms(void) {
    return (uint32_t)(esp_timer_get_time() / 1000);
}

void ble_ct_auth_init(void) {
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) == ESP_OK) {
        size_t len = DEVICE_KEY_LEN;
        if (nvs_get_blob(handle, NVS_KEY_DEVICE_KEY, s_device_key, &len) == ESP_OK
            && len == DEVICE_KEY_LEN) {
            s_is_bound = true;
            ESP_LOGI(TAG, "device_key loaded");
        }
        nvs_close(handle);
    }
    ble_ct_clock_init();
}

bool ble_ct_auth_is_bound(void) { return s_is_bound; }
bool ble_ct_auth_is_authenticated(void) { return s_auth_state == BLE_CT_AUTH_AUTHENTICATED; }

void ble_ct_auth_reset_session(void) {
    s_auth_state = BLE_CT_AUTH_UNAUTHENTICATED;
    ESP_LOGI(TAG, "Auth session reset");
}

bool ble_ct_auth_is_bind_window_open(void) {
    if (!s_is_bound) return true;
    return now_ms() < s_bind_window_close_ms;
}

void ble_ct_auth_open_bind_window(void) {
    if (s_is_bound) {
        ESP_LOGI(TAG, "Bind window not opened: already bound");
        return;
    }
    s_bind_window_close_ms = now_ms() + BIND_WINDOW_DURATION_MS;
    ESP_LOGI(TAG, "Bind window opened");
}

void ble_ct_auth_notify(const char *json, uint16_t len) {
    uint16_t conn = ble_ct_get_conn_handle();
    if (conn == BLE_HS_CONN_HANDLE_NONE) return;
    struct os_mbuf *om = ble_hs_mbuf_from_flat(json, len);
    if (om) ble_gattc_notify_custom(conn, ble_ct_get_auth_handle(), om);
}

int ble_ct_auth_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                           struct ble_gatt_access_ctxt *ctxt, void *arg) {
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return BLE_ATT_ERR_UNLIKELY;

    uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
    char buf[512] = {0};
    ble_hs_mbuf_to_flat(ctxt->om, buf, len, NULL);
    ESP_LOGI(TAG, "auth write: len=%u data=%.*s", len, (len < 200 ? len : 200), buf);

    cJSON *root = cJSON_Parse(buf);
    if (!root) { ESP_LOGW(TAG, "JSON parse failed"); return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN; }

    cJSON *j_mode = cJSON_GetObjectItem(root, "mode");
    if (!cJSON_IsString(j_mode)) { ESP_LOGW(TAG, "no mode field"); cJSON_Delete(root); return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN; }
    const char *mode = j_mode->valuestring;
    ESP_LOGI(TAG, "mode=%s", mode);

    /* 获取对端 MAC */
    struct ble_gap_conn_desc desc;
    uint8_t peer_addr[6] = {0};
    if (ble_gap_conn_find(conn_handle, &desc) == 0)
        memcpy(peer_addr, desc.peer_id_addr.val, 6);

    if (strcmp(mode, "pin_check") == 0) {
        cJSON *j_sn = cJSON_GetObjectItem(root, "sn");
        cJSON *j_pin = cJSON_GetObjectItem(root, "pin");
        if (!j_pin || !cJSON_IsString(j_pin)) {
            cJSON_Delete(root); return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        }
        const char *sn = (j_sn && cJSON_IsString(j_sn)) ? j_sn->valuestring : ble_ct_get_sn();
        bool ok = ble_ct_pin_verify(sn, j_pin->valuestring, peer_addr);
        char resp[128];
        int n = snprintf(resp, sizeof(resp),
                         "{\"mode\":\"pin_check\",\"result\":\"%s\"}",
                         ok ? "ok" : "invalid_pin");
        if (n > 0 && n < (int)sizeof(resp))
            ble_ct_auth_notify(resp, (uint16_t)n);
        cJSON_Delete(root); return 0;
    } else if (strcmp(mode, "bind") == 0) {
        cJSON *j_sn = cJSON_GetObjectItem(root, "sn");
        cJSON *j_pin = cJSON_GetObjectItem(root, "pin");
        cJSON *j_key = cJSON_GetObjectItem(root, "device_key");
        if (!j_pin || !j_key) { cJSON_Delete(root); return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN; }
        const char *sn = (j_sn && cJSON_IsString(j_sn)) ? j_sn->valuestring : ble_ct_get_sn();

        if (!ble_ct_pin_verify(sn, j_pin->valuestring, peer_addr)) {
            char resp[] = "{\"mode\":\"bind\",\"result\":\"rejected\",\"reason\":\"invalid_pin\"}";
            ble_ct_auth_notify(resp, strlen(resp));
            cJSON_Delete(root); return 0;
        }
        if (s_is_bound && !ble_ct_auth_is_bind_window_open()) {
            char resp[] = "{\"mode\":\"bind\",\"result\":\"rejected\",\"reason\":\"already_bound\"}";
            ble_ct_auth_notify(resp, strlen(resp));
            cJSON_Delete(root); return 0;
        }

        /* 持久化 device_key（Base64 解码为 32 字节原始密钥）*/
        size_t klen = 0;
        unsigned char keybuf[DEVICE_KEY_LEN];
        int b64rc = mbedtls_base64_decode(keybuf, sizeof(keybuf), &klen,
                                          (const unsigned char *)j_key->valuestring,
                                          strlen(j_key->valuestring));
        if (b64rc != 0 || klen != DEVICE_KEY_LEN) {
            ESP_LOGW(TAG, "bind rejected: invalid device_key base64 (rc=%d len=%u)",
                     b64rc, (unsigned)klen);
            char resp[] = "{\"mode\":\"bind\",\"result\":\"rejected\",\"reason\":\"invalid_key\"}";
            ble_ct_auth_notify(resp, strlen(resp));
            cJSON_Delete(root); return 0;
        }
        memcpy(s_device_key, keybuf, DEVICE_KEY_LEN);

        nvs_handle_t handle;
        if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) == ESP_OK) {
            nvs_set_blob(handle, NVS_KEY_DEVICE_KEY, s_device_key, DEVICE_KEY_LEN);
            nvs_commit(handle);
            nvs_close(handle);
        }
        s_is_bound = true;
        char resp[] = "{\"mode\":\"bind\",\"result\":\"ok\"}";
        ble_ct_auth_notify(resp, strlen(resp));
        ESP_LOGI(TAG, "Device bound (key %u bytes)", (unsigned)klen);
    } else if (strcmp(mode, "auth") == 0) {
        if (!s_is_bound) {
            char resp[] = "{\"mode\":\"auth\",\"result\":\"rejected\",\"reason\":\"not_bound\"}";
            ble_ct_auth_notify(resp, strlen(resp));
            cJSON_Delete(root); return 0;
        }
        /* HMAC-SHA256 challenge-response（协议 §5）
         * digest = HMAC-SHA256(key=device_key, message=nonce_bytes || ":" || ts_ascii)
         * App 端持同样算法重算比对 → 确认双方共享 device_key */
        cJSON *j_nonce = cJSON_GetObjectItem(root, "nonce");
        cJSON *j_ts = cJSON_GetObjectItem(root, "ts");
        if (!cJSON_IsString(j_nonce) || !j_nonce->valuestring[0] ||
            !cJSON_IsNumber(j_ts)) {
            ESP_LOGW(TAG, "auth rejected: missing nonce/ts");
            char resp[] = "{\"mode\":\"auth\",\"result\":\"rejected\",\"reason\":\"bad_request\"}";
            ble_ct_auth_notify(resp, strlen(resp));
            cJSON_Delete(root); return 0;
        }

        /* 时间戳窗口 ±300s（设备未校时 ble_ct_now()==0 时跳过） */
        uint32_t ts = (uint32_t)j_ts->valuedouble;
        uint32_t devnow = ble_ct_now();
        if (devnow > 0) {
            int32_t diff = (int32_t)(devnow - ts);
            if (diff < -300 || diff > 300) {
                ESP_LOGW(TAG, "auth rejected: ts out of window (dev=%u ts=%u)", devnow, ts);
                char resp[] = "{\"mode\":\"auth\",\"result\":\"rejected\",\"reason\":\"bad_ts\"}";
                ble_ct_auth_notify(resp, strlen(resp));
                cJSON_Delete(root); return 0;
            }
        }

        /* 解码 nonce（Base64 16 字节） */
        uint8_t nonce[16];
        size_t nlen = 0;
        int nrc = mbedtls_base64_decode(nonce, sizeof(nonce), &nlen,
                                        (const unsigned char *)j_nonce->valuestring,
                                        strlen(j_nonce->valuestring));
        if (nrc != 0 || nlen != sizeof(nonce)) {
            ESP_LOGW(TAG, "auth rejected: bad nonce base64 (rc=%d len=%u)", nrc, (unsigned)nlen);
            char resp[] = "{\"mode\":\"auth\",\"result\":\"rejected\",\"reason\":\"bad_request\"}";
            ble_ct_auth_notify(resp, strlen(resp));
            cJSON_Delete(root); return 0;
        }

        /* message = nonce_bytes || ":" || ts_ascii */
        uint8_t msg[16 + 1 + 11];   /* nonce(16) + ':' + ts(≤10 位数字) */
        size_t mlen = 0;
        memcpy(msg + mlen, nonce, sizeof(nonce)); mlen += sizeof(nonce);
        msg[mlen++] = ':';
        int t = snprintf((char *)msg + mlen, sizeof(msg) - mlen, "%lu", (unsigned long)ts);
        if (t <= 0) {
            cJSON_Delete(root);
            return 0;
        }
        mlen += (size_t)t;

        /* digest = HMAC-SHA256(device_key, message) */
        uint8_t digest[32];
        const mbedtls_md_info_t *md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
        mbedtls_md_hmac(md, s_device_key, DEVICE_KEY_LEN, msg, mlen, digest);

        /* Base64 编码 digest 回报 */
        char dig_b64[64];
        size_t dlen = 0;
        mbedtls_base64_encode((unsigned char *)dig_b64, sizeof(dig_b64), &dlen,
                              digest, sizeof(digest));
        dig_b64[dlen] = '\0';

        /* 成功后校准时钟 + 置为已鉴权 */
        ble_ct_calibrate(ts);
        s_auth_state = BLE_CT_AUTH_AUTHENTICATED;
        char resp[192];
        int n = snprintf(resp, sizeof(resp),
                         "{\"mode\":\"auth\",\"digest\":\"%s\",\"device_ts\":%lu}",
                         dig_b64, (unsigned long)devnow);
        if (n > 0 && n < (int)sizeof(resp)) {
            ble_ct_auth_notify(resp, (uint16_t)n);
        }
        ESP_LOGI(TAG, "auth OK (ts=%lu)", (unsigned long)ts);
    }

    cJSON_Delete(root);
    return 0;
}
