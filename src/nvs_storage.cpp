#include "nvs_storage.h"
#include <string.h>

#ifdef USE_PREFERENCES
  #include <Preferences.h>
  static Preferences prefs;
#else
  #include <EEPROM.h>
  #define EEPROM_SIZE 512
  // 简单键值偏移（EEPROM 前 64 字节存索引）
  #define EEPROM_SSID_OFFSET    64
  #define EEPROM_PASS_OFFSET    (EEPROM_SSID_OFFSET + 33)
  #define EEPROM_HOST_OFFSET    (EEPROM_PASS_OFFSET + 65)
  #define EEPROM_PORT_OFFSET    (EEPROM_HOST_OFFSET + 65)
  #define EEPROM_SN_OFFSET      (EEPROM_PORT_OFFSET + 2)
  #define EEPROM_KEY_OFFSET     (EEPROM_SN_OFFSET + 33)
  #define EEPROM_FLAG_OFFSET    0   // 标记位：0xAA=已配置
#endif

// ==================== 初始化 ====================
void storage_init() {
#ifdef USE_PREFERENCES
  prefs.begin(NVS_NAMESPACE, false);
#else
  EEPROM.begin(EEPROM_SIZE);
#endif
}

// ==================== XOR 加密 ====================
void xor_crypt(const char* key, const uint8_t* input, size_t len, uint8_t* output) {
  size_t key_len = strlen(key);
  for (size_t i = 0; i < len; i++) {
    output[i] = input[i] ^ key[i % key_len];
  }
}

// ==================== WiFi 凭据 ====================
bool storage_has_wifi_config() {
#ifdef USE_PREFERENCES
  return prefs.isKey(KEY_WIFI_SSID) && prefs.getString(KEY_WIFI_SSID).length() > 0;
#else
  return EEPROM.read(EEPROM_FLAG_OFFSET) == 0xAA;
#endif
}

bool storage_get_wifi(char* ssid, size_t ssid_len, char* pass, size_t pass_len) {
  if (!storage_has_wifi_config()) return false;

#ifdef USE_PREFERENCES
  String s = prefs.getString(KEY_WIFI_SSID, "");
  strncpy(ssid, s.c_str(), ssid_len - 1);
  ssid[ssid_len - 1] = '\0';

  String p = prefs.getString(KEY_WIFI_PASS, "");
  strncpy(pass, p.c_str(), pass_len - 1);
  pass[pass_len - 1] = '\0';
  Serial.printf("[NVS] GET pass: len=%d, raw=", p.length());
  for (int i = 0; i < min((int)p.length(), 16); i++) {
    Serial.printf("%02X ", (uint8_t)p[i]);
  }
  Serial.println();
#else
  for (size_t i = 0; i < ssid_len - 1; i++) {
    ssid[i] = EEPROM.read(EEPROM_SSID_OFFSET + i);
  }
  ssid[ssid_len - 1] = '\0';

  // EEPROM 密码（加密存储）
  String sn;
  for (size_t i = 0; i < 32; i++) {
    char c = EEPROM.read(EEPROM_SN_OFFSET + i);
    if (c == 0) break;
    sn += c;
  }
  for (size_t i = 0; i < pass_len - 1; i++) {
    pass[i] = EEPROM.read(EEPROM_PASS_OFFSET + i) ^ sn[i % sn.length()];
  }
  pass[pass_len - 1] = '\0';
#endif
  return true;
}

bool storage_set_wifi(const char* ssid, const char* pass) {
#ifdef USE_PREFERENCES
  Serial.printf("[NVS] SET wifi: ssid='%s' pass='%s' (len=%d)\n", ssid, pass, strlen(pass));
  // 先清除旧数据，避免新旧格式共存
  prefs.remove(KEY_WIFI_SSID);
  prefs.remove(KEY_WIFI_PASS);
  prefs.putString(KEY_WIFI_SSID, ssid);
  prefs.putString(KEY_WIFI_PASS, pass);
  prefs.end();  // 确保数据写入 Flash
#else
  for (size_t i = 0; i < strlen(ssid) && i < 32; i++) {
    EEPROM.write(EEPROM_SSID_OFFSET + i, ssid[i]);
  }
  EEPROM.write(EEPROM_SSID_OFFSET + strlen(ssid), 0);

  String sn;
  for (size_t i = 0; i < 32; i++) {
    char c = EEPROM.read(EEPROM_SN_OFFSET + i);
    if (c == 0) break;
    sn += c;
  }
  for (size_t i = 0; i < strlen(pass) && i < 64; i++) {
    EEPROM.write(EEPROM_PASS_OFFSET + i, pass[i] ^ sn[i % sn.length()]);
  }
  EEPROM.write(EEPROM_PASS_OFFSET + strlen(pass), 0);

  EEPROM.write(EEPROM_FLAG_OFFSET, 0xAA);
  EEPROM.commit();
#endif
  return true;
}

void storage_clear_wifi() {
#ifdef USE_PREFERENCES
  prefs.remove(KEY_WIFI_SSID);
  prefs.remove(KEY_WIFI_PASS);
#else
  EEPROM.write(EEPROM_FLAG_OFFSET, 0x00);
  EEPROM.commit();
#endif
}

// ==================== MQTT 配置 ====================
bool storage_get_mqtt(char* host, size_t host_len, uint16_t* port) {
#ifdef USE_PREFERENCES
  String h = prefs.getString(KEY_MQTT_HOST, "");
  if (h.length() == 0) return false;
  strncpy(host, h.c_str(), host_len - 1);
  host[host_len - 1] = '\0';
  *port = prefs.getUShort(KEY_MQTT_PORT, MQTT_DEFAULT_PORT);
  return true;
#else
  if (!storage_has_wifi_config()) return false;
  for (size_t i = 0; i < host_len - 1; i++) {
    host[i] = EEPROM.read(EEPROM_HOST_OFFSET + i);
  }
  host[host_len - 1] = '\0';
  *port = EEPROM.read(EEPROM_PORT_OFFSET) | (EEPROM.read(EEPROM_PORT_OFFSET + 1) << 8);
  return true;
#endif
}

bool storage_set_mqtt(const char* host, uint16_t port) {
#ifdef USE_PREFERENCES
  prefs.putString(KEY_MQTT_HOST, host);
  prefs.putUShort(KEY_MQTT_PORT, port);
#else
  for (size_t i = 0; i < strlen(host) && i < 64; i++) {
    EEPROM.write(EEPROM_HOST_OFFSET + i, host[i]);
  }
  EEPROM.write(EEPROM_HOST_OFFSET + strlen(host), 0);
  EEPROM.write(EEPROM_PORT_OFFSET, port & 0xFF);
  EEPROM.write(EEPROM_PORT_OFFSET + 1, (port >> 8) & 0xFF);
  EEPROM.commit();
#endif
  return true;
}

// ==================== 设备信息 ====================
bool storage_has_sn() {
#ifdef USE_PREFERENCES
  return prefs.isKey(KEY_DEVICE_SN) && prefs.getString(KEY_DEVICE_SN).length() > 0;
#else
  return EEPROM.read(EEPROM_SN_OFFSET) != 0;
#endif
}

bool storage_get_sn(char* sn, size_t sn_len) {
  if (!storage_has_sn()) return false;
#ifdef USE_PREFERENCES
  String s = prefs.getString(KEY_DEVICE_SN, "");
  strncpy(sn, s.c_str(), sn_len - 1);
  sn[sn_len - 1] = '\0';
#else
  for (size_t i = 0; i < sn_len - 1; i++) {
    sn[i] = EEPROM.read(EEPROM_SN_OFFSET + i);
  }
  sn[sn_len - 1] = '\0';
#endif
  return true;
}

bool storage_set_sn(const char* sn) {
#ifdef USE_PREFERENCES
  prefs.putString(KEY_DEVICE_SN, sn);
#else
  for (size_t i = 0; i < strlen(sn) && i < 32; i++) {
    EEPROM.write(EEPROM_SN_OFFSET + i, sn[i]);
  }
  EEPROM.write(EEPROM_SN_OFFSET + strlen(sn), 0);
  EEPROM.commit();
#endif
  return true;
}

bool storage_get_key(char* key, size_t key_len) {
#ifdef USE_PREFERENCES
  String s = prefs.getString(KEY_DEVICE_KEY, "");
  strncpy(key, s.c_str(), key_len - 1);
  key[key_len - 1] = '\0';
  return s.length() > 0;
#else
  for (size_t i = 0; i < key_len - 1; i++) {
    key[i] = EEPROM.read(EEPROM_KEY_OFFSET + i);
  }
  key[key_len - 1] = '\0';
  return key[0] != 0;
#endif
}

bool storage_set_key(const char* key) {
#ifdef USE_PREFERENCES
  prefs.putString(KEY_DEVICE_KEY, key);
#else
  for (size_t i = 0; i < strlen(key) && i < 64; i++) {
    EEPROM.write(EEPROM_KEY_OFFSET + i, key[i]);
  }
  EEPROM.write(EEPROM_KEY_OFFSET + strlen(key), 0);
  EEPROM.commit();
#endif
  return true;
}

// ==================== AP 热点名称 ====================
bool storage_has_ap_ssid() {
#ifdef USE_PREFERENCES
  return prefs.isKey(KEY_AP_SSID) && prefs.getString(KEY_AP_SSID).length() > 0;
#else
  // EEPROM: AP SSID 存在 SN 之前的位置
  return EEPROM.read(EEPROM_SN_OFFSET - 33) != 0;
#endif
}

bool storage_get_ap_ssid(char* ssid, size_t ssid_len) {
  if (!storage_has_ap_ssid()) return false;
#ifdef USE_PREFERENCES
  String s = prefs.getString(KEY_AP_SSID, "");
  strncpy(ssid, s.c_str(), ssid_len - 1);
  ssid[ssid_len - 1] = '\0';
#else
  for (size_t i = 0; i < ssid_len - 1; i++) {
    ssid[i] = EEPROM.read(EEPROM_SN_OFFSET - 33 + i);
  }
  ssid[ssid_len - 1] = '\0';
#endif
  return true;
}

bool storage_set_ap_ssid(const char* ssid) {
#ifdef USE_PREFERENCES
  prefs.putString(KEY_AP_SSID, ssid);
#else
  for (size_t i = 0; i < strlen(ssid) && i < 32; i++) {
    EEPROM.write(EEPROM_SN_OFFSET - 33 + i, ssid[i]);
  }
  EEPROM.write(EEPROM_SN_OFFSET - 33 + strlen(ssid), 0);
  EEPROM.commit();
#endif
  return true;
}

void storage_clear_ap_ssid() {
#ifdef USE_PREFERENCES
  prefs.remove(KEY_AP_SSID);
#else
  for (size_t i = 0; i < 33; i++) {
    EEPROM.write(EEPROM_SN_OFFSET - 33 + i, 0);
  }
  EEPROM.commit();
#endif
}

// ==================== 恢复出厂 ====================
void storage_factory_reset() {
#ifdef USE_PREFERENCES
  prefs.clear();
#else
  for (int i = 0; i < EEPROM_SIZE; i++) {
    EEPROM.write(i, 0);
  }
  EEPROM.commit();
#endif
}
