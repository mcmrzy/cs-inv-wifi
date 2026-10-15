#include "nvs_storage.h"
#include "nvs_flash.h"
#include "nvs.h"
#include <string.h>
#include "esp_log.h"

static nvs_handle_t s_nvs_handle;
static const char *TAG = "storage";

#define OTA_NAMESPACE       "ota_sec"
#define OTA_KEY_SEC_ESP     "ota_sec_esp"
#define OTA_KEY_SEC_ARM     "ota_sec_arm"
#define OTA_KEY_PENDING     "ota_pending"
#define OTA_PENDING_MAGIC   0x4f544150UL

bool storage_init() {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "nvs_flash_init failed: %s", esp_err_to_name(ret));
        return false;
    }
    ret = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &s_nvs_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(ret));
        return false;
    }
    return true;
}

bool storage_has_wifi_config() {
    size_t len = 0;
    esp_err_t ret = nvs_get_str(s_nvs_handle, KEY_WIFI_SSID, NULL, &len);
    return (ret == ESP_OK && len > 1);
}

bool storage_get_wifi(char* ssid, size_t ssid_len, char* pass, size_t pass_len) {
    if (!storage_has_wifi_config()) return false;

    size_t s_len = ssid_len;
    if (nvs_get_str(s_nvs_handle, KEY_WIFI_SSID, ssid, &s_len) != ESP_OK) return false;

    size_t p_len = pass_len;
    if (nvs_get_str(s_nvs_handle, KEY_WIFI_PASS, pass, &p_len) != ESP_OK) {
        pass[0] = '\0';
    }

    return true;
}

bool storage_set_wifi(const char* ssid, const char* pass) {
    if (!ssid || !ssid[0] || strlen(ssid) > 32) return false;
    if (!pass) pass = "";
    if (strlen(pass) > 64) return false;
    if (nvs_set_str(s_nvs_handle, KEY_WIFI_SSID, ssid) != ESP_OK) return false;
    if (nvs_set_str(s_nvs_handle, KEY_WIFI_PASS, pass) != ESP_OK) return false;
    return nvs_commit(s_nvs_handle) == ESP_OK;
}

bool storage_clear_wifi() {
    esp_err_t err = nvs_erase_key(s_nvs_handle, KEY_WIFI_SSID);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) return false;
    err = nvs_erase_key(s_nvs_handle, KEY_WIFI_PASS);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) return false;
    return nvs_commit(s_nvs_handle) == ESP_OK;
}

bool storage_get_mqtt(char* host, size_t host_len, uint16_t* port) {
    size_t h_len = host_len;
    if (nvs_get_str(s_nvs_handle, KEY_MQTT_HOST, host, &h_len) != ESP_OK) return false;
    if (strlen(host) == 0) return false;

    int32_t port_val = MQTT_DEFAULT_PORT;
    nvs_get_i32(s_nvs_handle, KEY_MQTT_PORT, &port_val);
    *port = (uint16_t)port_val;

    return true;
}

bool storage_set_mqtt(const char* host, uint16_t port) {
    if (!host || !host[0] || strlen(host) > 63 || port == 0) return false;
    if (nvs_set_str(s_nvs_handle, KEY_MQTT_HOST, host) != ESP_OK) return false;
    int32_t port_val = (int32_t)port;
    if (nvs_set_i32(s_nvs_handle, KEY_MQTT_PORT, port_val) != ESP_OK) return false;
    return nvs_commit(s_nvs_handle) == ESP_OK;
}

bool storage_has_sn() {
    size_t len = 0;
    esp_err_t ret = nvs_get_str(s_nvs_handle, KEY_DEVICE_SN, NULL, &len);
    return (ret == ESP_OK && len > 1);
}

bool storage_get_sn(char* sn, size_t sn_len) {
    if (!storage_has_sn()) return false;
    size_t len = sn_len;
    return (nvs_get_str(s_nvs_handle, KEY_DEVICE_SN, sn, &len) == ESP_OK);
}

bool storage_set_sn(const char* sn) {
    if (!sn || !sn[0] || strlen(sn) >= SN_MAX_LEN ||
        nvs_set_str(s_nvs_handle, KEY_DEVICE_SN, sn) != ESP_OK) return false;
    return nvs_commit(s_nvs_handle) == ESP_OK;
}

bool storage_get_key(char* key, size_t key_len) {
    size_t len = key_len;
    if (nvs_get_str(s_nvs_handle, KEY_DEVICE_KEY, key, &len) != ESP_OK) return false;
    return (strlen(key) > 0);
}

bool storage_set_key(const char* key) {
    if (!key || !key[0] || strlen(key) > 64) return false;
    if (nvs_set_str(s_nvs_handle, KEY_DEVICE_KEY, key) != ESP_OK) return false;
    return nvs_commit(s_nvs_handle) == ESP_OK;
}

static const char* ota_security_key(const char* target) {
    if (target && strcmp(target, "esp") == 0) return OTA_KEY_SEC_ESP;
    if (target && strcmp(target, "arm") == 0) return OTA_KEY_SEC_ARM;
    return NULL;
}

bool storage_get_ota_security_version_for_target(const char* target, uint32_t* version) {
    const char* key = ota_security_key(target);
    if (!key || !version) return false;

    nvs_handle_t ota_handle;
    esp_err_t err = nvs_open(OTA_NAMESPACE, NVS_READONLY, &ota_handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        *version = 0;
        return true;
    }
    if (err != ESP_OK) return false;

    err = nvs_get_u32(ota_handle, key, version);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        /* Legacy releases used one shared key. Treat it as a floor for both
         * targets so migration never weakens anti-rollback protection. */
        err = nvs_get_u32(ota_handle, KEY_OTA_SEC_VER, version);
    }
    nvs_close(ota_handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        *version = 0;
        return true;
    }
    return err == ESP_OK;
}

bool storage_set_ota_security_version_for_target(const char* target, uint32_t version) {
    const char* key = ota_security_key(target);
    if (!key || version == 0) return false;

    nvs_handle_t ota_handle;
    if (nvs_open(OTA_NAMESPACE, NVS_READWRITE, &ota_handle) != ESP_OK) return false;
    esp_err_t err = nvs_set_u32(ota_handle, key, version);
    if (err == ESP_OK) err = nvs_commit(ota_handle);
    nvs_close(ota_handle);
    return err == ESP_OK;
}

bool storage_get_ota_pending(StorageOtaPending* pending) {
    if (!pending) return false;
    memset(pending, 0, sizeof(*pending));

    nvs_handle_t ota_handle;
    esp_err_t err = nvs_open(OTA_NAMESPACE, NVS_READONLY, &ota_handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) return false;
    if (err != ESP_OK) return false;

    size_t length = sizeof(*pending);
    err = nvs_get_blob(ota_handle, OTA_KEY_PENDING, pending, &length);
    nvs_close(ota_handle);
    if (err != ESP_OK || length != sizeof(*pending) || pending->magic != OTA_PENDING_MAGIC) {
        memset(pending, 0, sizeof(*pending));
        return false;
    }
    pending->task_id[sizeof(pending->task_id) - 1] = '\0';
    pending->version[sizeof(pending->version) - 1] = '\0';
    return true;
}

bool storage_set_ota_pending(const StorageOtaPending* pending) {
    if (!pending || pending->security_version == 0 || !pending->task_id[0]) return false;

    StorageOtaPending stored = *pending;
    stored.magic = OTA_PENDING_MAGIC;
    stored.task_id[sizeof(stored.task_id) - 1] = '\0';
    stored.version[sizeof(stored.version) - 1] = '\0';

    nvs_handle_t ota_handle;
    if (nvs_open(OTA_NAMESPACE, NVS_READWRITE, &ota_handle) != ESP_OK) return false;
    esp_err_t err = nvs_set_blob(ota_handle, OTA_KEY_PENDING, &stored, sizeof(stored));
    if (err == ESP_OK) err = nvs_commit(ota_handle);
    nvs_close(ota_handle);
    return err == ESP_OK;
}

bool storage_clear_ota_pending(void) {
    nvs_handle_t ota_handle;
    esp_err_t err = nvs_open(OTA_NAMESPACE, NVS_READWRITE, &ota_handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) return true;
    if (err != ESP_OK) return false;

    err = nvs_erase_key(ota_handle, OTA_KEY_PENDING);
    if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
    if (err == ESP_OK) err = nvs_commit(ota_handle);
    nvs_close(ota_handle);
    return err == ESP_OK;
}

bool storage_get_ota_security_version(uint32_t* version) {
    return storage_get_ota_security_version_for_target("esp", version);
}

bool storage_set_ota_security_version(uint32_t version) {
    return storage_set_ota_security_version_for_target("esp", version);
}

bool storage_load_cmd_result(unsigned slot, char* task_id, size_t task_len,
                             char* response, size_t response_len) {
    if (slot >= 8 || !task_id || !response) return false;
    char tk[8], rk[8];
    snprintf(tk, sizeof(tk), "ct%u", slot);
    snprintf(rk, sizeof(rk), "cr%u", slot);
    size_t tl = task_len, rl = response_len;
    return nvs_get_str(s_nvs_handle, tk, task_id, &tl) == ESP_OK &&
           nvs_get_str(s_nvs_handle, rk, response, &rl) == ESP_OK;
}

bool storage_save_cmd_result(unsigned slot, const char* task_id, const char* response) {
    if (slot >= 8 || !task_id || !response) return false;
    char tk[8], rk[8];
    snprintf(tk, sizeof(tk), "ct%u", slot);
    snprintf(rk, sizeof(rk), "cr%u", slot);
    return nvs_set_str(s_nvs_handle, tk, task_id) == ESP_OK &&
           nvs_set_str(s_nvs_handle, rk, response) == ESP_OK &&
           nvs_commit(s_nvs_handle) == ESP_OK;
}

unsigned storage_get_cmd_next(void) {
    uint32_t next = 0;
    nvs_get_u32(s_nvs_handle, "cmd_next", &next);
    return next % 8;
}

bool storage_set_cmd_next(unsigned next) {
    return nvs_set_u32(s_nvs_handle, "cmd_next", next % 8) == ESP_OK &&
           nvs_commit(s_nvs_handle) == ESP_OK;
}

bool storage_has_ap_ssid() {
    size_t len = 0;
    esp_err_t ret = nvs_get_str(s_nvs_handle, KEY_AP_SSID, NULL, &len);
    return (ret == ESP_OK && len > 1);
}

bool storage_get_ap_ssid(char* ssid, size_t ssid_len) {
    if (!storage_has_ap_ssid()) return false;
    size_t len = ssid_len;
    return (nvs_get_str(s_nvs_handle, KEY_AP_SSID, ssid, &len) == ESP_OK);
}

bool storage_set_ap_ssid(const char* ssid) {
    if (!ssid || strlen(ssid) < 2 || strlen(ssid) > 31) return false;
    for (const char* p = ssid; *p; ++p) {
        if ((unsigned char)*p < 0x20 || (unsigned char)*p > 0x7e) return false;
    }
    if (nvs_set_str(s_nvs_handle, KEY_AP_SSID, ssid) != ESP_OK) return false;
    return nvs_commit(s_nvs_handle) == ESP_OK;
}

bool storage_clear_ap_ssid() {
    esp_err_t err = nvs_erase_key(s_nvs_handle, KEY_AP_SSID);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) return false;
    return nvs_commit(s_nvs_handle) == ESP_OK;
}

void storage_factory_reset() {
    nvs_erase_all(s_nvs_handle);
    nvs_commit(s_nvs_handle);
    ESP_LOGW(TAG, "Factory reset preserved ota_sec anti-rollback state");
}
