#include "SmartConfigManager.h"
#include <esp_smartconfig.h>

SmartConfigManager::SmartConfigManager()
  : _state(SC_STATE_IDLE), _callback(nullptr), _start_time(0) {}

void SmartConfigManager::start() {
  if (_state == SC_STATE_RUNNING) return;

  log_i("[SC] Starting SmartConfig (ESPTouch v2)...");
  _state = SC_STATE_RUNNING;
  _start_time = millis();

  // SmartConfig 事件由 WiFi 事件循环处理，这里只需启动
  WiFi.beginSmartConfig();
}

void SmartConfigManager::stop() {
  if (_state == SC_STATE_IDLE) return;

  log_i("[SC] Stopping SmartConfig");
  WiFi.stopSmartConfig();
  _state = SC_STATE_IDLE;
}

void SmartConfigManager::process() {
  if (_state != SC_STATE_RUNNING) return;

  // 检查超时
  if (millis() - _start_time > TIMEOUT_MS) {
    log_i("[SC] Timeout after 5 minutes");
    WiFi.stopSmartConfig();
    _state = SC_STATE_ERROR;
    return;
  }

  // 检查 SmartConfig 是否完成
  if (WiFi.smartConfigDone()) {
    String ssid = WiFi.SSID();
    String password = WiFi.psk();
    log_i("[SC] Got WiFi: %s", ssid.c_str());

    _state = SC_STATE_DONE;

    if (_callback) {
      _callback(ssid.c_str(), password.c_str());
    }
  }
}
