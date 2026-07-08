#include "BleProvisionManager.h"
#include <NimBLEDevice.h>
#include <esp_mac.h>

// ============================================================
//  CS Inverter BLE Provisioning Service
//  自定义 UUID，包含产品标识和设备信息
// ============================================================

// 产品基础 UUID: CSINV-xxxx-0000-1000-8000-00805f9b34fb
// 前 4 字节标识产品，后续为标准 UUID 后缀
#define CSINV_BASE_UUID       "43534956-0000-1000-8000-00805f9b34fb"

// 配网服务 UUID: CSINV-PROV
#define PROV_SERVICE_UUID     "43534956-5052-1000-8000-00805f9b34fb"

// 特征 UUID（基于服务 UUID，修改第3段区分）
#define CHAR_DEVICE_SN_UUID   "43534956-534e-1000-8000-00805f9b34fb"  // SN 号
#define CHAR_FW_VERSION_UUID  "43534956-4657-1000-8000-00805f9b34fb"  // 固件版本
#define CHAR_MAC_ADDR_UUID    "43534956-4d41-1000-8000-00805f9b34fb"  // MAC 地址
#define CHAR_SSID_UUID        "43534956-5353-1000-8000-00805f9b34fb"  // WiFi SSID（写入）
#define CHAR_PASS_UUID        "43534956-5057-1000-8000-00805f9b34fb"  // WiFi 密码（写入）
#define CHAR_PROV_STATUS_UUID "43534956-5354-1000-8000-00805f9b34fb"  // 配网状态

// 状态值
#define STATUS_WAITING    "waiting"
#define STATUS_CONNECTING "connecting"
#define STATUS_CONNECTED  "connected"
#define STATUS_FAILED     "failed"

static BleProvisionManager* g_prov_instance = nullptr;
static NimBLEServer* g_ble_server = nullptr;
static NimBLECharacteristic* g_status_char = nullptr;

// 设备信息（从外部获取）
extern char g_device_sn[];

// ---- BLE 回调类 ----

class ProvSsidCallback : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* pChar) {
    std::string val = pChar->getValue();
    if (g_prov_instance && val.length() > 0) {
      strncpy(g_prov_instance->pending_ssid, val.c_str(), 32);
      Serial.printf("[BLE-PROV] SSID received: %s\n", g_prov_instance->pending_ssid);
    }
  }
};

class ProvPassCallback : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* pChar) {
    std::string val = pChar->getValue();
    if (g_prov_instance) {
      if (val.length() > 0) {
        strncpy(g_prov_instance->pending_password, val.c_str(), 64);
      } else {
        g_prov_instance->pending_password[0] = '\0';
      }
      Serial.printf("[BLE-PROV] Password received (%d bytes)\n", (int)val.length());

      // 收到密码后触发配网
      if (strlen(g_prov_instance->pending_ssid) > 0) {
        g_prov_instance->onCredentialsReceived(
          g_prov_instance->pending_ssid, g_prov_instance->pending_password);
      }
    }
  }
};

class ProvServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* pServer) {
    Serial.println("[BLE-PROV] Client connected");
  }

  void onDisconnect(NimBLEServer* pServer) {
    Serial.println("[BLE-PROV] Client disconnected");
    if (g_prov_instance && g_prov_instance->getState() == BLE_PROV_RUNNING) {
      NimBLEAdvertising* adv = pServer->getAdvertising();
      adv->start();
    }
  }
};

// ---- 构造函数 ----

BleProvisionManager::BleProvisionManager()
  : _state(BLE_PROV_IDLE), _callback(nullptr), _start_time(0),
    _credentials_ready(false), _ble_started(false) {
  memset(_ssid, 0, sizeof(_ssid));
  memset(_password, 0, sizeof(_password));
  memset(pending_ssid, 0, sizeof(pending_ssid));
  memset(pending_password, 0, sizeof(pending_password));
  g_prov_instance = this;
}

// ---- 启动 BLE 配网 ----

void BleProvisionManager::start(const char* device_sn, const char* fw_version) {
  if (_state == BLE_PROV_RUNNING) return;

  Serial.println("[BLE-PROV] Starting BLE provisioning...");
  _state = BLE_PROV_RUNNING;
  _start_time = millis();
  _credentials_ready = false;

  // 构建设备名: CS_INV_完整SN（BLE 名称上限 31 字节，CS_INV_ + SN 足够放）
  char dev_name[40] = "CS_INV";
  if (device_sn && strlen(device_sn) > 0) {
    snprintf(dev_name, sizeof(dev_name), "CS_INV_%s", device_sn);
  }

  if (!_ble_started) {
    // 初始化 NimBLE（广播名用 CS_INV_后6位，方便用户识别）
    NimBLEDevice::init(dev_name);
    NimBLEDevice::setPower(ESP_PWR_LVL_P9);

    // GAP Device Name 设为完整 SN（App 可通过 0x2A00 直接读取）
    NimBLEDevice::setDeviceName(device_sn ? device_sn : "UNKNOWN");

    // 创建 BLE 服务器
    g_ble_server = NimBLEDevice::createServer();
    g_ble_server->setCallbacks(new ProvServerCallbacks());

    // ---- 配网服务 ----
    NimBLEService* pService = g_ble_server->createService(PROV_SERVICE_UUID);

    // 设备 SN（只读，自定义特征）
    NimBLECharacteristic* pSnChar = pService->createCharacteristic(
      CHAR_DEVICE_SN_UUID, NIMBLE_PROPERTY::READ);
    {
      std::string sn_str(device_sn ? device_sn : "UNKNOWN");
      pSnChar->setValue((const uint8_t*)sn_str.c_str(), sn_str.length());
      Serial.printf("[BLE-PROV] SN char set: '%s' (%d bytes)\n",
                    sn_str.c_str(), (int)sn_str.length());
    }

    // 固件版本（只读）
    NimBLECharacteristic* pFwChar = pService->createCharacteristic(
      CHAR_FW_VERSION_UUID, NIMBLE_PROPERTY::READ);
    pFwChar->setValue(fw_version ? fw_version : "0.0.0");

    // MAC 地址（只读）
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_BT);
    char mac_str[18];
    snprintf(mac_str, sizeof(mac_str), "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    NimBLECharacteristic* pMacChar = pService->createCharacteristic(
      CHAR_MAC_ADDR_UUID, NIMBLE_PROPERTY::READ);
    pMacChar->setValue(mac_str);

    // WiFi SSID（读写）
    NimBLECharacteristic* pSsidChar = pService->createCharacteristic(
      CHAR_SSID_UUID, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE);
    pSsidChar->setCallbacks(new ProvSsidCallback());

    // WiFi 密码（读写）
    NimBLECharacteristic* pPassChar = pService->createCharacteristic(
      CHAR_PASS_UUID, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE);
    pPassChar->setCallbacks(new ProvPassCallback());

    // 配网状态（读 + 通知）
    g_status_char = pService->createCharacteristic(
      CHAR_PROV_STATUS_UUID, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
    g_status_char->setValue(STATUS_WAITING);

    pService->start();
    _ble_started = true;

    Serial.printf("[BLE-PROV] Service created, device: %s\n", dev_name);
    Serial.printf("[BLE-PROV]   SN:  %s\n", device_sn ? device_sn : "N/A");
    Serial.printf("[BLE-PROV]   FW:  %s\n", fw_version ? fw_version : "N/A");
    Serial.printf("[BLE-PROV]   MAC: %s\n", mac_str);
  }

  // 广播策略：
  //   主广播包: 服务 UUID（用于 App 过滤）
  //   Scan Response: 完整设备名（含 SN，用于用户识别）
  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  adv->addServiceUUID(PROV_SERVICE_UUID);
  adv->setScanResponse(true);
  adv->start();

  Serial.printf("[BLE-PROV] Advertising as '%s'\n", dev_name);
  Serial.println("[BLE-PROV] Scan response carries full SN");
  Serial.println("[BLE-PROV] Use 'CS INV' App to provision WiFi");
}

// ---- 停止 ----

void BleProvisionManager::stop() {
  if (_state == BLE_PROV_IDLE) return;
  Serial.println("[BLE-PROV] Stopping BLE provisioning");
  if (_ble_started) {
    NimBLEDevice::getAdvertising()->stop();
  }
  _state = BLE_PROV_IDLE;
}

// ---- 主循环处理 ----

void BleProvisionManager::process() {
  if (_state != BLE_PROV_RUNNING) return;

  // BLE 持续广播，无超时限制

  // 收到凭据
  if (_credentials_ready) {
    _credentials_ready = false;

    if (g_status_char) {
      g_status_char->setValue(STATUS_CONNECTING);
      g_status_char->notify();
    }

    Serial.printf("[BLE-PROV] Credentials ready, connecting to: %s\n", _ssid);

    if (_callback) {
      _callback(_ssid, _password);
    }
    // BLE 继续广播，不停止
  }
}

// ---- 供外部回调调用 ----

void BleProvisionManager::onCredentialsReceived(const char* ssid, const char* password) {
  strncpy(_ssid, ssid, sizeof(_ssid) - 1);
  strncpy(_password, password, sizeof(_password) - 1);
  _credentials_ready = true;
}

// ---- 通知状态（供外部调用） ----

void BleProvisionManager::notifyStatus(const char* status) {
  if (g_status_char && _ble_started) {
    size_t conns = g_ble_server ? g_ble_server->getConnectedCount() : 0;
    size_t subs = g_status_char->getSubscribedCount();
    Serial.printf("[BLE-PROV] notifyStatus('%s'): %d conn(s), %d subscriber(s)\n",
                  status, (int)conns, (int)subs);
    if (subs > 0) {
      // 直接传数据给 notify，避免 WiFi+BLE 共存时 setValue 缓冲区竞争
      g_status_char->setValue(status);
      g_status_char->notify((const uint8_t*)status, strlen(status));
    } else {
      Serial.println("[BLE-PROV] No BLE subscribers, notification skipped");
    }
  } else {
    Serial.printf("[BLE-PROV] notifyStatus('%s'): skipped (char=%d, started=%d)\n",
                  status, g_status_char ? 1 : 0, _ble_started ? 1 : 0);
  }
}
