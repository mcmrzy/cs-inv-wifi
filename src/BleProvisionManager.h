#ifndef BLE_PROVISION_MANAGER_H
#define BLE_PROVISION_MANAGER_H

#include <WiFi.h>

enum BleProvState {
  BLE_PROV_IDLE,
  BLE_PROV_RUNNING,
  BLE_PROV_DONE,
  BLE_PROV_ERROR
};

typedef void (*BleProvCallback)(const char* ssid, const char* password);

class BleProvisionManager {
public:
  BleProvisionManager();

  // 启动 BLE 配网（传入设备 SN 和固件版本）
  void start(const char* device_sn = nullptr, const char* fw_version = nullptr);
  void stop();
  void process();

  BleProvState getState() const { return _state; }
  void setCallback(BleProvCallback cb) { _callback = cb; }

  // 通知配网状态给手机端
  void notifyStatus(const char* status);

  // 由 NimBLE 回调调用
  void onCredentialsReceived(const char* ssid, const char* password);

  // BLE 回调需要访问的临时缓冲区
  char pending_ssid[33];
  char pending_password[65];

private:
  BleProvState _state;
  BleProvCallback _callback;
  unsigned long _start_time;
  static const unsigned long TIMEOUT_MS = 300000;

  char _ssid[33];
  char _password[65];
  bool _credentials_ready;
  bool _ble_started;
};

#endif
