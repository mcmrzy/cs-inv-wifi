#include "nvs_storage.h"
#include "nvs_flash.h"
#include "nvs.h"
#include <string.h>

static nvs_handle_t s_nvs_handle;

void storage_init() {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
    nvs_open(NVS_NAMESPACE, NVS_READWRITE, &s_nvs_handle);
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
    nvs_erase_key(s_nvs_handle, KEY_WIFI_SSID);
    nvs_erase_key(s_nvs_handle, KEY_WIFI_PASS);
    nvs_set_str(s_nvs_handle, KEY_WIFI_SSID, ssid);
    nvs_set_str(s_nvs_handle, KEY_WIFI_PASS, pass);
    nvs_commit(s_nvs_handle);
    return true;
}

void storage_clear_wifi() {
    nvs_erase_key(s_nvs_handle, KEY_WIFI_SSID);
    nvs_erase_key(s_nvs_handle, KEY_WIFI_PASS);
    nvs_commit(s_nvs_handle);
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
    nvs_set_str(s_nvs_handle, KEY_MQTT_HOST, host);
    int32_t port_val = (int32_t)port;
    nvs_set_i32(s_nvs_handle, KEY_MQTT_PORT, port_val);
    nvs_commit(s_nvs_handle);
    return true;
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
    nvs_set_str(s_nvs_handle, KEY_DEVICE_SN, sn);
    nvs_commit(s_nvs_handle);
    return true;
}

bool storage_get_key(char* key, size_t key_len) {
    size_t len = key_len;
    if (nvs_get_str(s_nvs_handle, KEY_DEVICE_KEY, key, &len) != ESP_OK) return false;
    return (strlen(key) > 0);
}

bool storage_set_key(const char* key) {
    nvs_set_str(s_nvs_handle, KEY_DEVICE_KEY, key);
    nvs_commit(s_nvs_handle);
    return true;
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
    nvs_set_str(s_nvs_handle, KEY_AP_SSID, ssid);
    nvs_commit(s_nvs_handle);
    return true;
}

void storage_clear_ap_ssid() {
    nvs_erase_key(s_nvs_handle, KEY_AP_SSID);
    nvs_commit(s_nvs_handle);
}

void storage_factory_reset() {
    nvs_erase_all(s_nvs_handle);
    nvs_commit(s_nvs_handle);
}
