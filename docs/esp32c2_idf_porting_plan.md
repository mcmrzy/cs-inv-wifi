# ESP32-C2 ESP-IDF 移植实施方案

> 源项目: cs-inv-wifi-main (ESP32-C3 + Arduino/PlatformIO)
> 目标: ESP32-C2 + ESP-IDF v5.x
> 日期: 2026-06-30

---

## 1. 项目结构

```
esp32c2_idf/
├── CMakeLists.txt                  # 顶层 CMake
├── sdkconfig.defaults              # ESP-IDF 默认配置
├── sdkconfig.defaults.esp32c2      # C2 特定覆盖
├── partitions.csv                  # OTA 双分区表
├── main/
│   ├── CMakeLists.txt              # main 组件注册
│   ├── Kconfig.projbuild           # menuconfig 自定义选项
│   ├── app_main.c                  # 入口: app_main() + 主循环 FreeRTOS 任务
│   ├── config.h                    # 引脚/常量/协议定义 (纯 C header)
│   ├── mqtt_ca_cert.h              # TLS CA 证书 (原样复用)
│   ├── state_machine.h/.c          # 主状态机 (从 main.cpp 拆出)
│   ├── uart_protocol.h/.c          # UART 帧协议 (uart_driver 封装)
│   ├── mqtt_handler.h/.c           # esp_mqtt 客户端封装
│   ├── mqtt_ota_handler.h/.c       # MQTT 触发的 OTA 处理
│   ├── arm_ota.h/.c                # ARM MCU IAP 状态机
│   ├── ota_manager.h/.c            # ESP OTA (esp_ota API)
│   ├── nvs_storage.h/.c            # NVS 键值存储
│   ├── offline_cache.h/.c          # 离线缓存 (循环缓冲区重写)
│   ├── led_status.h/.c             # LED 驱动 (LEDC)
│   ├── tcp_provision.h/.c          # HTTP 配网服务 (lwIP socket)
│   └── sn_encoder.h/.c             # SN 编码 (纯 C, 直接复制)
└── components/
    └── smartconfig/                # ESP-Touch SmartConfig 封装
        ├── CMakeLists.txt
        ├── smartconfig_manager.h
        └── smartconfig_manager.c
```

**与 Arduino 版本的主要结构差异:**
- `main.cpp` (739行) 拆分为 `app_main.c` (入口/初始化) + `state_machine.c` (状态机逻辑)
- `sn_encoder.cpp/h` 纯 C 代码可直接复制 (无 Arduino 依赖)
- SmartConfig 作为独立 component 封装
- 无 `.ino` 文件，入口是 `app_main()` 中的 FreeRTOS 任务

---

## 2. 构建系统

### 2.1 顶层 CMakeLists.txt

```cmake
cmake_minimum_required(VERSION 3.16)
set(EXTRA_COMPONENT_DIRS "")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(cs_inv_wifi_c2)
```

### 2.2 main/CMakeLists.txt

```cmake
idf_component_register(
    SRCS
        "app_main.c"
        "state_machine.c"
        "uart_protocol.c"
        "mqtt_handler.c"
        "mqtt_ota_handler.c"
        "arm_ota.c"
        "ota_manager.c"
        "nvs_storage.c"
        "offline_cache.c"
        "led_status.c"
        "tcp_provision.c"
        "sn_encoder.c"
    INCLUDE_DIRS "."
    REQUIRES
        driver
        esp_wifi
        esp_netif
        esp_event
        esp_http_client
        esp_https_ota
        mqtt
        nvs_flash
        cjson
        esp_timer
        app_update
        esp_partition
        lwip
        mbedtls
        esp_smartconfig
)
```

### 2.3 partitions.csv (OTA 双分区, 4MB Flash)

```csv
# Name,    Type, SubType, Offset,  Size,     Flags
nvs,       data, nvs,     0x9000,  0x6000,
phy_init,  data, phy,     0xf000,  0x1000,
otadata,   data, ota,     0x10000, 0x2000,
ota_0,     app,  ota_0,   0x20000, 0x1E0000,
ota_1,     app,  ota_1,   0x200000,0x1E0000,
```

每个 OTA 分区 1.9MB，ESP-IDF app 约 800KB-1.2MB，空间充足。

### 2.4 sdkconfig.defaults 关键配置

```ini
CONFIG_IDF_TARGET="esp32c2"
CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"

# WiFi - C2 内存有限，精简 buffer
CONFIG_ESP_WIFI_STATIC_RX_BUFFER_NUM=6
CONFIG_ESP_WIFI_DYNAMIC_RX_BUFFER_NUM=12
CONFIG_ESP_WIFI_DYNAMIC_TX_BUFFER_NUM=8
CONFIG_ESP_WIFI_RX_BA_WIN=6
CONFIG_ESP_WIFI_SOFTAP_SUPPORT=y

# MQTT
CONFIG_MQTT_PROTOCOL_311=y
CONFIG_MQTT_BUFFER_SIZE=1024
CONFIG_MQTT_TASK_STACK_SIZE=4096

# HTTP
CONFIG_ESP_HTTP_CLIENT_ENABLE_HTTPS=y

# FreeRTOS tick
CONFIG_FREERTOS_HZ=1000

# 看门狗
CONFIG_ESP_TASK_WDT_TIMEOUT_S=30

# FreeRTOS 任务栈 (C2 内存紧张，适当缩减)
CONFIG_ESP_MAIN_TASK_STACK_SIZE=4096
CONFIG_ESP_SYSTEM_EVENT_TASK_STACK_SIZE=3072
```

### 2.5 Kconfig.projbuild

```kconfig
menu "CS_INV WiFi Module"

    config CS_INV_UART_TX_PIN
        int "UART TX GPIO"
        default 0
    config CS_INV_UART_RX_PIN
        int "UART RX GPIO"
        default 1
    config CS_INV_LED1_PIN
        int "LED1 GPIO"
        default 12
    config CS_INV_LED2_PIN
        int "LED2 GPIO"
        default 13
    config CS_INV_BTN_PIN
        int "Button GPIO"
        default 9
    config CS_INV_UART_BAUD
        int "UART baudrate"
        default 115200
    config CS_INV_MQTT_HOST
        string "MQTT broker host"
        default "jiuxiaoyw.online"
    config CS_INV_MQTT_PORT
        int "MQTT broker port"
        default 8883
    config CS_INV_MQTT_USERNAME
        string "MQTT username"
        default "CSKJ-INV-DEVICE-6K2"
    config CS_INV_MQTT_PASSWORD
        string "MQTT password"
        default "CSKJINVDEVICE6K2"
    config CS_INV_CACHE_MAX_ITEMS
        int "Offline cache max items"
        default 500
    config CS_INV_CACHE_TTL_DAYS
        int "Offline cache TTL days"
        default 7

endmenu
```

---

## 3. API 映射表

### 3.1 入口与调度

| Arduino | ESP-IDF | 转换说明 |
|---|---|---|
| `setup()` | `app_main()` 中初始化各模块 | 按顺序调用各 init 函数 |
| `loop()` | `xTaskCreate(main_loop_task, ...)` | FreeRTOS 任务, `while(1)` 循环 |
| `millis()` | `esp_timer_get_time() / 1000` | 返回 uint64_t 毫秒 |
| `delay(ms)` | `vTaskDelay(pdMS_TO_TICKS(ms))` | 让出 CPU |
| `Serial.printf()` | `ESP_LOGI(TAG, ...)` / `ESP_LOGE()` | 分级日志 |
| `ESP.restart()` | `esp_restart()` | |
| `ESP.getFreeHeap()` | `esp_get_free_heap_size()` | |

### 3.2 UART (uart_protocol.c)

| Arduino HardwareSerial | ESP-IDF uart_driver | 转换说明 |
|---|---|---|
| `Serial1.begin(115200, SERIAL_8N1, rx, tx)` | `uart_driver_install(UART_NUM_1, rx_buf, tx_buf, 0, NULL, 0)` + `uart_param_config()` + `uart_set_pin()` | 三步初始化 |
| `Serial1.available()` | `uart_get_buffered_data_len(UART_NUM_1, &len)` | |
| `Serial1.read()` | `uart_read_bytes(UART_NUM_1, &byte, 1, pdMS_TO_TICKS(0))` | 非阻塞 |
| `Serial1.write(b)` | `uart_write_bytes(UART_NUM_1, &b, 1)` | |
| `Serial1.write(buf, len)` | `uart_write_bytes(UART_NUM_1, buf, len)` | |

**实现要点:**
- 接收缓冲区设为 1024 字节 (C2 内存限制)
- 发送函数 `uart_write_escaped()` 中改用 `uart_write_bytes()` 替代 `UART_SERIAL.write()`
- `uart_protocol_process()` 改为从 ring buffer 读取，逻辑不变

### 3.3 WiFi (esp_wifi)

| Arduino WiFi | ESP-IDF esp_wifi | 转换说明 |
|---|---|---|
| `WiFi.mode(WIFI_AP_STA)` | `esp_wifi_set_mode(WIFI_MODE_APSTA)` | |
| `WiFi.begin(ssid, pass)` | `esp_wifi_set_config(WIFI_IF_STA, &cfg)` + `esp_wifi_connect()` | cfg 包含 ssid/password |
| `WiFi.softAP(ssid, NULL, ch, hidden, max)` | `esp_wifi_set_config(WIFI_IF_AP, &ap_cfg)` | ap_cfg 设 authmode=WIFI_AUTH_OPEN |
| `WiFi.softAPConfig(ip, gw, mask)` | `esp_netif_set_ip_info(ap_netif, &ip_info)` | |
| `WiFi.status() == WL_CONNECTED` | 事件标志: `xEventGroupWaitBits(wifi_event_group, CONNECTED_BIT, ...)` | 事件驱动 |
| `WiFi.disconnect()` | `esp_wifi_disconnect()` | |
| `WiFi.scanNetworks()` | `esp_wifi_scan_start(&config, true)` (同步) + `esp_wifi_scan_get_ap_records()` | |
| `WiFi.RSSI()` | `wifi_ap_record_t ap_info; esp_wifi_sta_get_ap_info(&ap_info);` | |
| `WiFi.localIP()` | `esp_netif_get_ip_info(sta_netif, &info)` → `info.ip.addr` | |
| `WiFi.softAPIP()` | `esp_netif_get_ip_info(ap_netif, &info)` | |
| `WiFi.setSleep(false)` | `esp_wifi_set_ps(WIFI_PS_NONE)` | |
| `WiFi.SSID()` | `wifi_config_t cfg; esp_wifi_get_config(WIFI_IF_STA, &cfg);` | |

**WiFi 初始化流程 (替代 Arduino 的 WiFi.mode):**
```
1. esp_netif_init()
2. esp_event_loop_create_default()
3. sta_netif = esp_netif_create_default_wifi_sta()
4. ap_netif  = esp_netif_create_default_wifi_ap()
5. wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT()
6. esp_wifi_init(&cfg)
7. esp_event_handler_register(WIFI_EVENT, ...)
8. esp_event_handler_register(IP_EVENT, ...)
```

**事件处理 (替代 Arduino 的 WiFi.status() 轮询):**
```c
// WIFI_EVENT_STA_CONNECTED    → 已关联AP (非IP层)
// WIFI_EVENT_STA_DISCONNECTED → 断开, 触发重连
// IP_EVENT_STA_GOT_IP         → 获取IP, 设置 CONNECTED_BIT
// WIFI_EVENT_AP_STACONNECTED  → 有客户端连入AP
```

### 3.4 MQTT (esp_mqtt_client)

| Arduino PubSubClient | ESP-IDF esp_mqtt | 转换说明 |
|---|---|---|
| `new PubSubClient(wifiClient)` | `esp_mqtt_client_init(&mqtt_cfg)` | 事件驱动, 无需 loop() |
| `client.setServer(host, port)` | `mqtt_cfg.host`, `mqtt_cfg.port` | 在 config 中设置 |
| `client.connect(id, user, pass, will_topic, will_qos, will_retain, will_msg)` | `mqtt_cfg.client_id`, `.username`, `.password`, `.lwt_topic`, `.lwt_msg`, `.lwt_qos`, `.lwt_retain` | LWT 在 config 中 |
| `client.publish(topic, data, len, retain)` | `esp_mqtt_client_publish(client, topic, data, len, qos, retain)` | 返回 msg_id |
| `client.subscribe(topic, qos)` | `esp_mqtt_client_subscribe(client, topic, qos)` | |
| `client.setCallback(cb)` | `MQTT_EVENT_DATA` 事件: `event->topic`, `event->data`, `event->data_len` | |
| `client.connected()` | `esp_mqtt_client_is_connected(client)` | |
| `client.loop()` | **不需要** (内部任务自动管理) | |
| `client.disconnect()` | `esp_mqtt_client_stop(client)` | |
| `client.state()` | `MQTT_EVENT_ERROR` 事件获取错误码 | |
| `client.setBufferSize(1024)` | `mqtt_cfg.buffer_size = 1024` | |

**TLS 配置 (修复 setInsecure 问题):**
```c
esp_mqtt_client_config_t mqtt_cfg = {
    .broker.address.hostname = MQTT_HOST,
    .broker.address.port = MQTT_PORT,
    .broker.address.transport = MQTT_TRANSPORT_OVER_SSL,
    .broker.verification.certificate = MQTT_CA_CERT,  // 使用 CA 证书验证
    .credentials.username = MQTT_USERNAME,
    .credentials.authentication.password = MQTT_PASSWORD,
    .session.last_will = {
        .topic = lwt_topic,
        .msg = lwt_payload,
        .qos = 1,
        .retain = true,
    },
    .buffer.size = 1024,
};
```

### 3.5 NVS (nvs_flash)

| Arduino Preferences | ESP-IDF nvs_flash | 转换说明 |
|---|---|---|
| `prefs.begin("inv_cfg", false)` | `nvs_open("inv_cfg", NVS_READWRITE, &handle)` | |
| `prefs.getString(key, "")` | `nvs_get_str(handle, key, buf, &len)` | 先 `nvs_get_str(handle, key, NULL, &len)` 获取长度 |
| `prefs.putString(key, val)` | `nvs_set_str(handle, key, val)` | |
| `prefs.getInt(key, 0)` | `nvs_get_i32(handle, key, &val)` | |
| `prefs.getUInt(key, 0)` | `nvs_get_u32(handle, key, &val)` | |
| `prefs.getUShort(key, def)` | `nvs_get_u16(handle, key, &val)` | |
| `prefs.putUShort(key, val)` | `nvs_set_u16(handle, key, val)` | |
| `prefs.isKey(key)` | `nvs_get_str(handle, key, NULL, &len) == ESP_OK` | |
| `prefs.remove(key)` | `nvs_erase_key(handle, key)` | |
| `prefs.clear()` | `nvs_erase_all(handle)` | |
| `prefs.end()` | `nvs_commit(handle); nvs_close(handle);` | |

**初始化:** `nvs_flash_init()` 在 `app_main()` 最开头调用。

### 3.6 HTTP Client (esp_http_client)

| Arduino HTTPClient | ESP-IDF esp_http_client | 转换说明 |
|---|---|---|
| `HTTPClient http` | `esp_http_client_init(&config)` | |
| `http.begin(url)` | `config.url = url` | |
| `http.GET()` | `esp_http_client_open(client, 0)` + `esp_http_client_fetch_headers(client)` | 流式读取 |
| `http.getSize()` | `esp_http_client_get_content_length(client)` | |
| `http.getStreamPtr()->readBytes()` | `esp_http_client_read(client, buf, len)` | |
| `http.setFollowRedirects()` | `config.disable_auto_redirect = false` | |
| `http.end()` | `esp_http_client_cleanup(client)` | |

### 3.7 OTA Flash (esp_ota)

| Arduino Update | ESP-IDF esp_ota | 转换说明 |
|---|---|---|
| `Update.begin(size)` | `const esp_partition_t* part = esp_ota_get_next_update_partition(NULL)` + `esp_ota_begin(part, size, &handle)` | |
| `Update.write(buf, len)` | `esp_ota_write(handle, buf, len)` | |
| `Update.end(true)` | `esp_ota_end(handle)` + `esp_ota_set_boot_partition(part)` | |
| `Update.abort()` | `esp_ota_abort(handle)` | |
| `Update.errorString()` | 错误码转字符串 (自定义映射) | |

### 3.8 TCP Server (lwIP socket)

| Arduino WiFiServer/WiFiClient | ESP-IDF lwIP | 转换说明 |
|---|---|---|
| `WiFiServer(80)` | `socket(AF_INET, SOCK_STREAM, 0)` + `bind()` + `listen()` | |
| `server.hasClient()` / `server.available()` | `accept(fd, ...)` 配合 `select()` 或非阻塞 | |
| `client.read(buf, len)` | `read(client_fd, buf, len)` | |
| `client.write(buf, len)` | `write(client_fd, buf, len)` | |
| `client.available()` | `ioctl(client_fd, FIONREAD, &avail)` | |
| `client.connected()` | `getsockopt()` 或检测 `read` 返回 0 | |
| `client.stop()` | `close(client_fd)` | |
| `String` 操作 | `snprintf()` + 固定 `char[]` 缓冲区 | 避免 Arduino String |

### 3.9 LED (LEDC PWM)

| Arduino ledcWrite | ESP-IDF LEDC | 转换说明 |
|---|---|---|
| `ledcSetup(channel, 5000, 8)` | `ledc_timer_config({.speed_mode=LOW_SPEED, .duty_resolution=8, .freq_hz=5000, .timer_num=LEDC_TIMER_0})` | C2 只有 LOW_SPEED 模式 |
| `ledcAttachPin(pin, channel)` | `ledc_channel_config({.gpio_num=pin, .speed_mode=LOW_SPEED, .channel=ch, .timer_sel=LEDC_TIMER_0})` | |
| `ledcWrite(channel, duty)` | `ledc_set_duty(LOW_SPEED, ch, duty)` + `ledc_update_duty(LOW_SPEED, ch)` | |
| `digitalWrite(pin, HIGH/LOW)` | `gpio_set_level(pin, 1/0)` | 普通 GPIO |

### 3.10 SmartConfig

| Arduino (WiFi.beginSmartConfig) | ESP-IDF | 转换说明 |
|---|---|---|
| `WiFi.beginSmartConfig()` | `esp_smartconfig_set_type(SC_TYPE_ESPTOUCH)` + `smartconfig_start(&sc_config)` | |
| `WiFi.smartConfigDone()` | `SC_EVENT` 事件: `SMARTCONFIG_EVENT_GET_RESULT` | 事件驱动 |
| `WiFi.SSID()` / `WiFi.psk()` | 从 `smartconfig_event_got_ssid_pswd_t` 结构体获取 | |
| `WiFi.stopSmartConfig()` | `smartconfig_stop()` | |

### 3.11 SNTP (时间同步)

| Arduino | ESP-IDF | 转换说明 |
|---|---|---|
| `configTime(8*3600, 0, "ntp.aliyun.com", "pool.ntp.org")` | `esp_sntp_setoperatingmode(SNTP_OPMODE_POLL)` + `esp_sntp_setservername(0, "ntp.aliyun.com")` + `esp_sntp_init()` | 时区通过 `setenv("TZ", "CST-8", 1)` + `tzset()` |

### 3.12 JSON (cJSON 替代 ArduinoJson)

| ArduinoJson | cJSON | 转换说明 |
|---|---|---|
| `StaticJsonDocument<512> doc` | `cJSON *root = cJSON_CreateObject()` | 堆分配 |
| `doc["key"] = val` | `cJSON_AddStringToObject(root, "key", val)` | 类型化 API |
| `deserializeJson(doc, str)` | `cJSON_Parse(str)` | |
| `serializeJson(doc, str)` | `char *str = cJSON_PrintUnformatted(root)` | **用完必须 free** |
| `doc["key"] \| ""` | `cJSON_GetObjectItem(root, "key")` + null check | |

**注意:** cJSON 使用堆分配，用完必须 `cJSON_Delete(root)` 和 `free(str)`。

---

## 4. 模块逐个转换计划

### 4.1 app_main.c — 入口与调度

**来源:** `main.cpp` (setup/loop)

**转换内容:**
```c
// app_main.c
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_wifi.h"

static const char *TAG = "APP";

static void main_loop_task(void *pvParameters) {
    while (1) {
        uart_protocol_process();
        check_button();
        led_tick_process();
        state_machine_run();
        tcp_provision_process();
        // delay(2) → vTaskDelay
        vTaskDelay(pdMS_TO_TICKS(2));
    }
}

void app_main(void) {
    // NVS 初始化 (必须最先)
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    // 网络初始化
    esp_netif_init();
    esp_event_loop_create_default();

    // 各模块初始化 (顺序与原 setup() 一致)
    storage_init();
    uart_protocol_init();
    led_init();
    offline_cache_init();
    mqtt_init(on_mqtt_message);
    ota_manager_init();
    mqtt_ota_init();

    // WiFi + SN 初始化
    wifi_system_init();  // 封装 esp_wifi_init + AP/STA 配置
    sn_init();           // 从 NVS 加载或 MAC 生成

    // SNTP
    sntp_init_custom();

    // 创建主循环任务 (栈 4KB, 优先级 5)
    xTaskCreate(main_loop_task, "main_loop", 4096, NULL, 5, NULL);
}
```

### 4.2 config.h — 配置头文件

**来源:** `src/config.h`

**转换要点:**
- 移除所有 `#include <Arduino.h>` 和 `#ifdef CHIP_ESP32C3` 分支
- 引脚定义改为 `#include "sdkconfig.h"` + `CONFIG_CS_INV_xxx` 宏
- 移除 ESP8266 相关代码
- 命令码、帧协议常量原样保留

```c
#ifndef CONFIG_H
#define CONFIG_H

#include "sdkconfig.h"
#include <stdint.h>

// 引脚 (从 Kconfig 读取)
#define PIN_UART_TX  CONFIG_CS_INV_UART_TX_PIN
#define PIN_UART_RX  CONFIG_CS_INV_UART_RX_PIN
#define PIN_LED1     CONFIG_CS_INV_LED1_PIN
#define PIN_LED2     CONFIG_CS_INV_LED2_PIN
#define PIN_BTN      CONFIG_CS_INV_BTN_PIN
#define UART_BAUDRATE CONFIG_CS_INV_UART_BAUD

// 帧协议 (不变)
#define FRAME_HEADER     0xAA
#define FRAME_ESCAPE     0x55
// ... 其余命令码不变

// MQTT (从 Kconfig 读取)
#define MQTT_DEFAULT_HOST  CONFIG_CS_INV_MQTT_HOST
#define MQTT_DEFAULT_PORT  CONFIG_CS_INV_MQTT_PORT

// 固件版本
#define FIRMWARE_VERSION "V2.0.0.ESP-IDF"

#endif
```

### 4.3 uart_protocol.c — UART 帧协议

**来源:** `src/uart_protocol.cpp` (161行)

**转换要点:**
1. `#include <Arduino.h>` → `#include "driver/uart.h"`
2. `Serial1.begin()` → `uart_driver_install(UART_NUM_1, 1024, 512, 0, NULL, 0)` + `uart_param_config()` + `uart_set_pin()`
3. `UART_SERIAL.available()` → `uart_get_buffered_data_len(UART_NUM_1, &len)`
4. `UART_SERIAL.read()` → `uart_read_bytes(UART_NUM_1, &byte, 1, 0)`
5. `UART_SERIAL.write(b)` → `uart_write_bytes(UART_NUM_1, &b, 1)`

**核心帧解析逻辑 (状态机) 完全不变** — 它是纯 C 逻辑，不依赖任何平台 API。

### 4.4 mqtt_handler.c — MQTT 客户端

**来源:** `src/mqtt_handler.cpp` (352行)

**重写为事件驱动模型:**

```c
#include "mqtt_client.h"

static esp_mqtt_client_handle_t s_client = NULL;
static MQTTMessageHandler s_msg_handler = NULL;

// 事件处理 (替代 Arduino 的 callback)
static void mqtt_event_handler(void *args, esp_event_base_t base,
                               int32_t event_id, void *event_data) {
    esp_mqtt_event_handle_t event = event_data;
    switch (event->event_id) {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG, "MQTT connected");
            // 订阅 cmd 和 ota/cmd 主题
            esp_mqtt_client_subscribe(s_client, cmd_topic, 1);
            esp_mqtt_client_subscribe(s_client, ota_topic, 1);
            // 发送上线消息
            esp_mqtt_client_publish(s_client, status_topic, online_payload, 0, 1, true);
            // 刷新离线缓存
            offline_cache_flush_all(offline_send_callback);
            break;
        case MQTT_EVENT_DATA:
            // topic 和 data 不是 null-terminated, 需要手动处理
            char topic[128] = {0};
            char payload[1024] = {0};
            strncpy(topic, event->topic, MIN(event->topic_len, sizeof(topic)-1));
            strncpy(payload, event->data, MIN(event->data_len, sizeof(payload)-1));
            if (s_msg_handler) s_msg_handler(topic, payload, event->data_len);
            break;
        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGW(TAG, "MQTT disconnected");
            break;
        case MQTT_EVENT_ERROR:
            ESP_LOGE(TAG, "MQTT error");
            break;
    }
}

bool mqtt_init(MQTTMessageHandler handler) {
    s_msg_handler = handler;

    esp_mqtt_client_config_t cfg = {
        .broker.address.hostname = MQTT_DEFAULT_HOST,
        .broker.address.port = MQTT_DEFAULT_PORT,
        .broker.address.transport = MQTT_TRANSPORT_OVER_SSL,
        .broker.verification.certificate = MQTT_CA_CERT,  // 修复: 使用 CA 证书
        .credentials.username = MQTT_USERNAME,
        .credentials.authentication.password = MQTT_PASSWORD,
        .credentials.client_id = g_device_sn,
        .session.last_will = {
            .topic = g_status_topic,
            .msg = g_lwt_payload,
            .qos = 1,
            .retain = true,
        },
        .buffer.size = 1024,
    };

    s_client = esp_mqtt_client_init(&cfg);
    esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_mqtt_client_start(s_client);
    return true;
}
```

**关键差异:**
- 无需 `mqtt_loop()` — esp_mqtt 内部任务自动处理
- 重连由 esp_mqtt 内部管理，无需手动 `mqtt_connect()`
- TLS 使用 CA 证书验证 (修复原有 `setInsecure` 问题)

### 4.5 mqtt_ota_handler.c — MQTT OTA 处理

**来源:** `src/mqtt_ota_handler.cpp` (591行)

**转换要点:**
1. `ArduinoJson` → `cJSON`
2. `HTTPClient` → `esp_http_client`
3. `Update` → `esp_ota` API
4. `String` → `char[]` + `snprintf()`
5. `WiFiClient* stream = http->getStreamPtr()` → `esp_http_client_read(client, buf, len)` 循环读取

**ESP OTA 下载流程 (流式):**
```c
esp_http_client_handle_t client = esp_http_client_init(&config);
esp_http_client_open(client, 0);
int content_length = esp_http_client_fetch_headers(client);

const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
esp_ota_handle_t ota_handle;
esp_ota_begin(part, content_length, &ota_handle);

uint8_t buf[2048];  // 缩小缓冲区 (C2 内存限制)
int downloaded = 0;
while (downloaded < content_length) {
    int read = esp_http_client_read(client, buf, sizeof(buf));
    if (read > 0) {
        esp_ota_write(ota_handle, buf, read);
        downloaded += read;
    }
}

esp_ota_end(ota_handle);
esp_ota_set_boot_partition(part);
esp_restart();
```

### 4.6 arm_ota.c — ARM MCU IAP

**来源:** `src/arm_ota.cpp` (724行)

**转换要点:**
1. `#include <Arduino.h>` → 标准 C 头文件 + ESP-IDF API
2. `millis()` → `esp_timer_get_time() / 1000`
3. `Serial.printf()` → `ESP_LOGI(TAG, ...)`
4. `g_stream_buf[8192]` → `g_stream_buf[4096]` (C2 内存优化, 详见 §6)
5. 核心状态机逻辑完全不变 (纯 C)

### 4.7 nvs_storage.c — NVS 存储

**来源:** `src/nvs_storage.cpp` (285行)

**完全重写为 ESP-IDF nvs_flash API:**
```c
#include "nvs_flash.h"
#include "nvs.h"

static nvs_handle_t s_handle;

void storage_init(void) {
    nvs_open("inv_cfg", NVS_READWRITE, &s_handle);
}

bool storage_get_wifi(char *ssid, size_t ssid_len, char *pass, size_t pass_len) {
    size_t sl = ssid_len, pl = pass_len;
    if (nvs_get_str(s_handle, "ws", ssid, &sl) != ESP_OK) return false;
    if (nvs_get_str(s_handle, "wp", pass, &pl) != ESP_OK) return false;
    return true;
}

bool storage_set_wifi(const char *ssid, const char *pass) {
    nvs_set_str(s_handle, "ws", ssid);
    nvs_set_str(s_handle, "wp", pass);
    nvs_commit(s_handle);
    return true;
}
// ... 其余函数类似
```

移除所有 EEPROM/ESP8266 分支代码。

### 4.8 offline_cache.c — 离线缓存 (重写)

**来源:** `src/offline_cache.cpp` (341行)

**重写为循环缓冲区 (修复 Bug #1):**

```c
// 使用 head/tail 指针 + NVS 存储
// 每个 item 有 NVS key: "cache_N" (N = (head + i) % MAX_ITEMS)
// 无需 remove_item_at() 的 O(n) shift

static int s_head = 0;      // 最老数据的索引
static int s_count = 0;     // 当前缓存数量
static int s_next_seq = 0;  // 全局递增序号 (用于 TTL 判断)

bool offline_cache_add(const char *topic, const char *payload, uint32_t ts) {
    if (s_count >= MAX_ITEMS) {
        // 覆盖最老的 (head 位置)
        s_head = (s_head + 1) % MAX_ITEMS;
        s_count--;
    }
    int write_idx = (s_head + s_count) % MAX_ITEMS;
    save_item(write_idx, topic, payload, ts, s_next_seq++);
    s_count++;
    save_metadata();  // 持久化 head, count, next_seq
    return true;
}

int offline_cache_flush_all(bool (*send_func)(...)) {
    int sent = 0;
    while (s_count > 0) {
        OfflineCacheItem item;
        load_item(s_head, &item);
        if (send_func(item.topic, item.payload, item.timestamp)) {
            s_head = (s_head + 1) % MAX_ITEMS;
            s_count--;
            sent++;
        } else {
            break;  // 发送失败，保留剩余
        }
    }
    save_metadata();
    return sent;
}
```

**关键改进:**
- `remove_item_at()` O(n) shift → O(1) head 移动
- 缓存数量从 1000 降至 500 (C2 NVS 空间限制)
- JSON 序列化时对 payload 中的特殊字符做转义 (修复 Bug #2)

### 4.9 led_status.c — LED 驱动

**来源:** `src/led_status.cpp` (196行)

**转换要点:**
1. `pinMode()` / `digitalWrite()` → `gpio_config()` / `gpio_set_level()`
2. `ledcWrite(channel, duty)` → LEDC API (`ledc_timer_config` + `ledc_channel_config` + `ledc_set_duty`)
3. ESP32-C2 只有 LOW_SPEED LEDC 模式 (不影响 LED 控制)
4. 核心状态机逻辑不变

### 4.10 tcp_provision.c — HTTP 配网服务

**来源:** `src/tcp_provision.cpp` (1447行)

**这是最大的转换工作量。** Arduino String + WiFiServer/WiFiClient → lwIP BSD socket。

**架构重设计:**
```c
// 使用 lwIP socket API
static int s_server_fd = -1;

void tcp_provision_init(void) {
    s_server_fd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(s_server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(80),
        .sin_addr.s_addr = INADDR_ANY,
    };
    bind(s_server_fd, (struct sockaddr*)&addr, sizeof(addr));
    listen(s_server_fd, MAX_HTTP_CLIENTS);

    // 设为非阻塞
    fcntl(s_server_fd, F_SETFL, O_NONBLOCK);
}

void tcp_provision_process(void) {
    // accept 新连接 (非阻塞)
    int client_fd = accept(s_server_fd, NULL, NULL);
    if (client_fd < 0) return;

    // 设置超时
    struct timeval tv = {.tv_sec = 5};
    setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    // 读取 HTTP 请求
    char buf[2048];
    int len = read(client_fd, buf, sizeof(buf) - 1);
    if (len > 0) {
        buf[len] = '\0';
        process_http_request(client_fd, buf, len);
    }

    close(client_fd);
}
```

**HTML 模板替换:**
- `String.replace()` → `snprintf()` 拼接或手动 `memcpy` 替换占位符
- HTML 内容存储为 `const char[]` (不用 PROGMEM，ESP-IDF 没有这个概念)
- 所有 JSON 响应用 `snprintf()` 生成固定缓冲区

### 4.11 sn_encoder.c — SN 编码

**来源:** `src/sn_encoder.cpp` (359行)

**直接复制，无需修改。** 这是纯 C 代码，不依赖任何 Arduino API。只需:
1. 文件扩展名 `.cpp` → `.c`
2. 检查 `#include` 头文件兼容性 (`<stdint.h>`, `<string.h>`, `<ctype.h>`, `<stdio.h>` 均为标准 C)

### 4.12 SmartConfigManager — ESP-Touch

**来源:** `src/SmartConfigManager.cpp` (49行)

**重写为 ESP-IDF 事件驱动:**
```c
// components/smartconfig/smartconfig_manager.c
#include "esp_smartconfig.h"
#include "esp_wifi.h"
#include "esp_event.h"

static void sc_event_handler(void *arg, esp_event_base_t base,
                             int32_t id, void *data) {
    if (base == SC_EVENT && id == SC_EVENT_SCAN_DONE) {
        ESP_LOGI(TAG, "SmartConfig scan done");
    } else if (base == SC_EVENT && id == SC_EVENT_FOUND_CHANNEL) {
        ESP_LOGI(TAG, "SmartConfig found channel");
    } else if (base == SC_EVENT && id == SC_EVENT_GOT_SSID_PSWD) {
        smartconfig_event_got_ssid_pswd_t *evt = data;
        wifi_config_t wifi_cfg;
        memset(&wifi_cfg, 0, sizeof(wifi_cfg));
        memcpy(wifi_cfg.sta.ssid, evt->ssid, sizeof(wifi_cfg.sta.ssid));
        memcpy(wifi_cfg.sta.password, evt->password, sizeof(wifi_cfg.sta.password));

        esp_wifi_disconnect();
        esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg);
        esp_wifi_connect();

        // 回调通知
        if (s_callback) s_callback((char*)evt->ssid, (char*)evt->password);
    }
}

void smartconfig_start(smartconfig_callback_t cb) {
    s_callback = cb;
    esp_event_handler_register(SC_EVENT, ESP_EVENT_ANY_ID, sc_event_handler, NULL);
    smartconfig_start_config_t cfg = SMARTCONFIG_START_CONFIG_DEFAULT();
    esp_smartconfig_set_type(SC_TYPE_ESPTOUCH);
    smartconfig_start(&cfg);
}
```

---

## 5. Bug 修复与设计改进

### Bug #1: offline_cache O(n) shift → 循环缓冲区

**原问题:** `remove_item_at()` 中的 for 循环逐条移动 NVS 数据，flush 时每删一条都要 shift 全部后续项。

**修复:** 使用 head/tail 环形指针，删除只需移动 head 索引。详见 §4.8。

**收益:** flush 1000 条从 ~5000 次 NVS 操作降至 ~1000 次。

### Bug #2: offline_cache JSON 无转义

**原问题:** `save_item()` 中 `snprintf` 直接拼接 payload 到 JSON 字符串，如果 payload 包含 `"` 或 `\` 会破坏 JSON 格式。

**修复:**
```c
// 方案 A: 使用 cJSON 构建 (推荐)
cJSON *root = cJSON_CreateObject();
cJSON_AddStringToObject(root, "topic", item->topic);
cJSON_AddRawToObject(root, "payload", item->payload);  // payload 本身是 JSON
cJSON_AddNumberToObject(root, "ts", item->timestamp);
char *json_str = cJSON_PrintUnformatted(root);
// 保存到 NVS
cJSON_Delete(root);
free(json_str);

// 方案 B: 手动转义
// 对 payload 中的 " 和 \ 做转义
```

### Bug #3: TLS setInsecure → 使用 CA 证书

**原问题:** `mqtt_handler.cpp:124` 中 `g_wifi_client->setInsecure()` 跳过了 TLS 证书验证。

**修复:** esp_mqtt 配置中设置 `broker.verification.certificate = MQTT_CA_CERT`，使用已有的 CA 证书 (`mqtt_ca_cert.h`) 进行验证。

**如果证书过期或不匹配:** 作为 fallback，可在 Kconfig 中添加 "skip cert verify" 选项，但默认启用证书验证。

### Bug #4: arm_ota g_stream_buf 8KB → 4KB

**原问题:** `arm_ota.cpp:45` 中 `g_stream_buf[8192]` 占用 8KB 静态 RAM，C2 的 272KB SRAM 紧张。

**修复:** 将 `ARM_OTA_STREAM_BUF_SIZE` 从 8192 降至 4096。ARM OTA 固件包大小 480 字节/包，4KB 缓冲区可容纳 8 包数据，流水线足够。

### Bug #5: WiFi 重连阻塞 delay() → 事件驱动

**原问题:** `main.cpp:353-354` 中 `WiFi.disconnect(true); WiFi.mode(WIFI_OFF); delay(200);` 使用阻塞延时。

**修复:** ESP-IDF 中 WiFi 状态变化通过事件通知，无需 delay。断开重连：
```c
esp_wifi_disconnect();
// 无需 delay — WiFi 事件会在适当时机触发
esp_wifi_set_config(WIFI_IF_STA, &new_cfg);
esp_wifi_connect();
```

重连逻辑改为事件驱动: 在 `WIFI_EVENT_STA_DISCONNECTED` 处理中启动指数退避重连定时器 (`esp_timer_create`)。

### Bug #6: tcp_provision 阻塞 + Arduino String → lwIP + snprintf

**原问题:** `tcp_provision.cpp` 中使用 Arduino `String` 的 `+=` / `replace()` 操作频繁堆分配，HTTP 服务器阻塞主循环。

**修复:**
- 全部 `String` 替换为 `char[]` + `snprintf()` / `memcpy()`
- HTML 模板使用 `const char[]` + 占位符替换函数
- HTTP 请求处理使用固定缓冲区，避免动态分配

### Bug #7: ota_manager ARM OTA 256KB 全量下载到 RAM → 流式

**原问题:** `ota_manager.cpp:368` 中 `malloc(OTA_ARM_MAX_SIZE)` 分配 256KB 内存，C2 上可能失败。

**修复:** 使用已有的 `arm_ota_start_streaming()` + `arm_ota_feed()` 流式模式（原 ESP8266 分支已有此逻辑）。边 HTTP 下载边喂给 ARM OTA 模块。

```c
// 替代 arm_ota_download_and_flash 中的 malloc(256KB)
arm_ota_start_streaming(content_length);
while (downloaded < content_length) {
    int read = esp_http_client_read(client, buf, sizeof(buf));
    if (read > 0) {
        arm_ota_feed(buf, read);
        downloaded += read;
    }
}
arm_ota_end_streaming();
```

---

## 6. 内存优化策略

### 6.1 ESP32-C2 内存预算 (272KB SRAM)

| 项目 | Arduino 版估算 | ESP-IDF C2 目标 | 优化措施 |
|---|---|---|---|
| WiFi 驱动 | ~40KB | ~35KB | 减少 WiFi buffer 数量 |
| TCP/IP 栈 | ~20KB | ~18KB | 减少 socket 数 |
| MQTT 任务 | ~8KB | ~6KB | 减小 task stack |
| 主循环任务 | ~8KB | ~4KB | 栈大小 4096 |
| UART 缓冲 | ~2KB | ~1.5KB | rx_buf=1024 |
| HTTP Server | ~4KB | ~3KB | 固定缓冲区 |
| NVS cache | ~4KB | ~4KB | |
| 系统保留 | ~50KB | ~50KB | |
| **可用剩余** | ~136KB | **~150KB** | |

### 6.2 具体优化措施

1. **arm_ota stream buffer:** 8KB → 4KB (省 4KB)
2. **offline_cache:** 1000 条 → 500 条 (NVS 空间节省，内存无直接影响)
3. **mqtt_ota_handler g_ota_buffer:** 4096 → 2048 (省 2KB)
4. **tcp_provision rx_buf:** 每 slot 1024 × 4 = 4KB → 1024 × 2 = 2KB (省 2KB, 最大并发 2)
5. **JSON 处理:** ArduinoJson 的 `StaticJsonDocument<512>` (栈上 512B) → cJSON (堆分配，用完释放)
6. **OTA ARM 下载:** 不再 malloc 256KB，改用流式 (省 ~250KB 峰值)
7. **HTML 模板:** 不在运行时拼接 String，直接 `write()` 分段发送

### 6.3 编译优化

```
# sdkconfig
CONFIG_COMPILER_OPTIMIZATION_SIZE=y     # 优化体积而非速度
CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_120=y   # C2 最高 120MHz
```

---

## 7. 转换顺序与依赖图

### 推荐转换顺序 (从底层到上层)

```
阶段 1: 基础设施 (无外部依赖)
  ├── sn_encoder.c      ← 直接复制 (纯 C)
  ├── config.h           ← 改为 Kconfig + sdkconfig.h
  ├── nvs_storage.c      ← Preferences → nvs_flash
  └── led_status.c       ← GPIO/LEDC 驱动

阶段 2: 通信层
  ├── uart_protocol.c    ← HardwareSerial → uart_driver
  └── mqtt_handler.c     ← PubSubClient → esp_mqtt_client

阶段 3: 应用层 (依赖阶段 2)
  ├── mqtt_ota_handler.c ← ArduinoJson→cJSON, HTTPClient→esp_http_client, Update→esp_ota
  ├── arm_ota.c          ← millis→esp_timer, Serial→ESP_LOG
  └── ota_manager.c      ← HTTPClient→esp_http_client, Update→esp_ota

阶段 4: 网络服务
  ├── offline_cache.c    ← 重写为循环缓冲区
  ├── tcp_provision.c    ← WiFiServer→lwIP socket (工作量最大)
  └── SmartConfigManager ← WiFi.beginSmartConfig→esp_smartconfig

阶段 5: 集成
  ├── state_machine.c    ← 从 main.cpp 拆出状态机
  └── app_main.c         ← 入口 + FreeRTOS 任务
```

### 依赖关系

```
app_main.c
  ├── state_machine.c
  │     ├── wifi_system (esp_wifi 事件)
  │     ├── mqtt_handler.c
  │     │     ├── nvs_storage.c
  │     │     ├── sn_encoder.c
  │     │     └── offline_cache.c
  │     ├── tcp_provision.c
  │     └── mqtt_ota_handler.c
  │           ├── mqtt_handler.c
  │           ├── ota_manager.c
  │           │     └── arm_ota.c
  │           │           └── uart_protocol.c
  │           └── uart_protocol.c
  ├── uart_protocol.c
  ├── led_status.c
  └── nvs_storage.c
```

---

## 8. 测试验证计划

### 8.1 单元级验证 (每个模块转换后)

| 模块 | 验证方法 |
|---|---|
| sn_encoder | 直接复用原项目的测试用例 |
| nvs_storage | 写入→重启→读取→校验 |
| uart_protocol | 用串口工具发送已知帧，验证解析 |
| led_status | 目视确认各 LED 模式 |
| mqtt_handler | 连接公共 MQTT broker，pub/sub 验证 |

### 8.2 集成验证

| 场景 | 验证步骤 |
|---|---|
| 冷启动配网 | 无 WiFi 凭据 → AP 热点 → 手机配网 → 连接成功 |
| 自动重连 | 断开 WiFi → 自动重连 → MQTT 恢复 |
| MQTT 数据流 | ARM 发 PUBLISH → MQTT broker 收到 |
| MQTT 命令转发 | MQTT cmd → UART 转发给 ARM |
| 离线缓存 | 断网 → 缓存数据 → 恢复后自动发送 |
| ESP OTA | 通过 MQTT 触发 OTA → 下载 → 重启 → 新版本 |
| ARM OTA | 通过 MQTT 触发 → HTTP 下载 → UART 流式转发 |
| 恢复出厂 | 长按按键 → NVS 清除 → 重启 → AP 配网 |

### 8.3 内存压力测试

```
1. 运行 10 分钟，每分钟记录 heap:
   ESP_LOGI(TAG, "Free heap: %lu", esp_get_free_heap_size());
2. 执行 OTA + 离线缓存 flush 同时进行，检查是否 OOM
3. 运行 24 小时，检查内存泄漏
```

### 8.4 GPIO 引脚映射确认

ESP32-C2 GPIO 与 C3 不完全相同，需要确认:
- GPIO 0/1 用于 UART1 (TX/RX) — C2 上需确认是否可用
- GPIO 12/13 用于 LED — C2 上需确认
- GPIO 9 用于按键 — C2 上 BOOT 按键可能在不同 GPIO

**建议:** 参考 ESP32-C2 datasheet 的 GPIO 功能矩阵，必要时调整 Kconfig 默认值。

---

## 附录: 关键转换决策记录

| 决策 | 选择 | 理由 |
|---|---|---|
| MQTT 库 | esp_mqtt (IDF 原生) | 无需外部依赖, 事件驱动, 原生 TLS |
| JSON 库 | cJSON (IDF 组件) | 无需外部依赖, 运行时堆分配更灵活 |
| HTTP Server | lwIP socket 直接实现 | 避免引入额外组件, 控制内存 |
| OTA | esp_ota API | 官方推荐, 支持 rollback |
| SmartConfig | esp_smartconfig 组件 | ESP-IDF 原生支持 |
| 状态机调度 | FreeRTOS 单任务 + 非阻塞 | 简单, 与原 Arduino loop() 行为一致 |
| 事件系统 | esp_event | WiFi/MQTT 状态变化的统一处理机制 |

---

**Files touched**: `docs/esp32c2_idf_porting_plan.md` (new)
**Findings worth promoting**:
- The offline_cache circular buffer redesign eliminates O(n) shift on every removal
- ARM OTA should always use streaming mode (arm_ota_feed) to avoid 256KB malloc on constrained devices
- TLS certificate verification should use the existing CA cert instead of setInsecure
- ESP-IDF's esp_mqtt client eliminates the need for manual mqtt_loop() and reconnection logic
- The tcp_provision module is the largest conversion effort (1447 lines, Arduino String + WiFiServer → lwIP socket)
- ESP32-C2 only has LOW_SPEED LEDC mode (no high-speed)
- cJSON requires explicit free() for both the cJSON tree and printed strings (unlike ArduinoJson's stack allocation)
