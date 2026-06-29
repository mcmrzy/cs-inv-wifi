#ifndef SMART_CONFIG_MANAGER_H
#define SMART_CONFIG_MANAGER_H

#include <WiFi.h>

// SmartConfig 配网状态
enum SmartConfigState {
  SC_STATE_IDLE,        // 未启动
  SC_STATE_RUNNING,     // 正在等待手机发送配网信息
  SC_STATE_DONE,        // 配网成功，已拿到 WiFi 信息
  SC_STATE_ERROR        // 配网失败
};

// 配网成功回调
typedef void (*SmartConfigCallback)(const char* ssid, const char* password);

class SmartConfigManager {
public:
  SmartConfigManager();

  // 启动 SmartConfig 配网
  void start();

  // 停止 SmartConfig
  void stop();

  // 在 loop() 中调用，检测配网结果
  void process();

  // 获取当前状态
  SmartConfigState getState() const { return _state; }

  // 设置配网成功回调
  void setCallback(SmartConfigCallback cb) { _callback = cb; }

private:
  SmartConfigState _state;
  SmartConfigCallback _callback;
  unsigned long _start_time;
  static const unsigned long TIMEOUT_MS = 300000;  // 5 分钟超时
};

#endif
