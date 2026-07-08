/**
 * ================================================================
 *  ARM OTA 模块实现
 *
 *  通过 UART 自定义帧协议，ESP32 给 GD32F303 ARM 芯片做固件升级。
 *  使用状态机 + millis() 超时，不阻塞 loop()。
 *
 *  MD5 暂用全零占位，后续可替换为 mbedtls 实现。
 * ================================================================
 */

#include "arm_ota.h"
#include "uart_protocol.h"
#include <Arduino.h>
#include <string.h>

// ==================== 内部状态 ====================
static ArmOtaState       g_ota_state     = ARM_OTA_IDLE;
static ArmOtaCallback    g_ota_callback  = nullptr;
static const uint8_t*    g_firmware      = nullptr;
static size_t            g_firmware_len  = 0;

// 分包参数
static uint16_t          g_total_packets = 0;    // 总包数
static uint16_t          g_current_packet = 0;   // 当前发送的包序号
static int               g_retry_count   = 0;    // 当前包重传计数
static int               g_progress      = 0;    // 进度百分比

// 超时控制
static unsigned long     g_last_send_time  = 0;  // 上次发送时间
static unsigned long     g_state_enter_time = 0; // 进入当前状态的时间

// 状态信息
static char              g_message[128] = {0};

// ARM 固件信息缓存
static ArmFirmwareInfo   g_fw_info;

// ==================== 流式模式变量（用于 ESP8266 等 RAM 有限平台）====================
#define ARM_OTA_STREAM_BUF_SIZE  8192  // 流式缓冲区大小（8KB）
static bool       g_streaming_mode    = false;   // 是否为流式模式
static size_t     g_stream_total      = 0;      // 固件总大小
static size_t     g_stream_sent       = 0;      // 已发送字节数
static size_t     g_stream_buf_len    = 0;      // 缓冲区中有效数据长度
static uint8_t    g_stream_buf[ARM_OTA_STREAM_BUF_SIZE];  // 流式缓冲区

// ==================== 内部辅助函数 ====================

// 更新状态并通知回调
static void set_state(ArmOtaState new_state, const char* msg) {
  g_ota_state = new_state;
  g_state_enter_time = millis();
  if (msg) {
    strncpy(g_message, msg, sizeof(g_message) - 1);
    g_message[sizeof(g_message) - 1] = '\0';
  }
  Serial.printf("[ARM-OTA] State=%d, %s\n", new_state, g_message);
  if (g_ota_callback) {
    g_ota_callback(new_state, g_progress, g_message);
  }
}

// 更新进度
static void update_progress() {
  if (g_total_packets == 0) {
    g_progress = 0;
  } else {
    g_progress = (int)((uint32_t)g_current_packet * 100 / g_total_packets);
    if (g_progress > 100) g_progress = 100;
  }
}

// 构建 little-endian 32 位
static void put_u32_le(uint8_t* buf, uint32_t val) {
  buf[0] = (uint8_t)(val & 0xFF);
  buf[1] = (uint8_t)((val >> 8) & 0xFF);
  buf[2] = (uint8_t)((val >> 16) & 0xFF);
  buf[3] = (uint8_t)((val >> 24) & 0xFF);
}

// 构建 little-endian 16 位
static void put_u16_le(uint8_t* buf, uint16_t val) {
  buf[0] = (uint8_t)(val & 0xFF);
  buf[1] = (uint8_t)((val >> 8) & 0xFF);
}

// 读取 little-endian 16 位
static uint16_t get_u16_le(const uint8_t* buf) {
  return (uint16_t)buf[0] | ((uint16_t)buf[1] << 8);
}

// 读取 little-endian 32 位
static uint32_t get_u32_le(const uint8_t* buf) {
  return (uint32_t)buf[0]
       | ((uint32_t)buf[1] << 8)
       | ((uint32_t)buf[2] << 16)
       | ((uint32_t)buf[3] << 24);
}

// ==================== 发送函数 ====================

// 发送 OTA_START 命令
// DATA: [total_size(4B, LE)] [md5(16B)] [version_len(1B)] [version_string(nB)]
static bool send_ota_start() {
  uint8_t buf[4 + ARM_OTA_MD5_SIZE + 1 + 32];
  int pos = 0;

  // total_size (4B, LE)
  put_u32_le(buf + pos, (uint32_t)g_firmware_len);
  pos += 4;

  // md5 (16B) — 暂用全零
  memset(buf + pos, 0, ARM_OTA_MD5_SIZE);
  pos += ARM_OTA_MD5_SIZE;

  // version_len (1B) + version_string — 暂为空
  buf[pos++] = 0;

  bool ok = uart_send_frame(CMD_ARM_OTA_START, buf, pos);
  if (ok) {
    Serial.printf("[ARM-OTA] START sent: size=%u, packets=%u\n",
                  (unsigned)g_firmware_len, g_total_packets);
  }
  return ok;
}

// 发送 OTA_DATA 命令
// DATA: [packet_index(2B, LE)] [data(最大 480 bytes)]
static bool send_ota_data(uint16_t packet_idx) {
  static uint8_t buf[2 + ARM_OTA_PACKET_SIZE];
  int pos = 0;

  // packet_index (2B, LE)
  put_u16_le(buf + pos, packet_idx);
  pos += 2;

  // 固件数据
  size_t offset = (size_t)packet_idx * ARM_OTA_PACKET_SIZE;
  size_t remaining;
  size_t chunk;

  if (g_streaming_mode) {
    // 流式模式：从缓冲区头部读取，读取后移动数据
    remaining = g_stream_total - offset;
    chunk = (remaining > ARM_OTA_PACKET_SIZE) ? ARM_OTA_PACKET_SIZE : remaining;
    chunk = (chunk > (size_t)g_stream_buf_len) ? (size_t)g_stream_buf_len : chunk;

    // 缓冲区数据不足时，不发送空包
    if (chunk == 0) {
      return false;
    }

    // 从缓冲区头部复制
    memcpy(buf + pos, g_stream_buf, chunk);
    pos += (int)chunk;

    // 移动缓冲区数据（删除已发送的部分）
    g_stream_buf_len -= (int)chunk;
    if (g_stream_buf_len > 0) {
      memmove(g_stream_buf, g_stream_buf + chunk, g_stream_buf_len);
    }
  } else {
    // 内存模式：从固件指针读取
    remaining = g_firmware_len - offset;
    chunk = (remaining > ARM_OTA_PACKET_SIZE) ? ARM_OTA_PACKET_SIZE : remaining;
    memcpy(buf + pos, g_firmware + offset, chunk);
    pos += (int)chunk;
  }

  bool ok = uart_send_frame(CMD_ARM_OTA_DATA, buf, pos);
  if (ok) {
    Serial.printf("[ARM-OTA] DATA[%u/%u] sent, %u bytes\n",
                  packet_idx, g_total_packets, (unsigned)chunk);
  }
  return ok;
}

// 发送 OTA_END 命令
// DATA: [total_packets(2B, LE)] [total_size(4B, LE)] [md5(16B)]
static bool send_ota_end() {
  uint8_t buf[2 + 4 + ARM_OTA_MD5_SIZE];
  int pos = 0;

  // total_packets (2B, LE)
  put_u16_le(buf + pos, g_total_packets);
  pos += 2;

  // total_size (4B, LE)
  put_u32_le(buf + pos, (uint32_t)g_firmware_len);
  pos += 4;

  // md5 (16B) — 暂用全零
  memset(buf + pos, 0, ARM_OTA_MD5_SIZE);
  pos += ARM_OTA_MD5_SIZE;

  bool ok = uart_send_frame(CMD_ARM_OTA_END, buf, pos);
  if (ok) {
    Serial.printf("[ARM-OTA] END sent: packets=%u, size=%u\n",
                  g_total_packets, (unsigned)g_firmware_len);
  }
  return ok;
}

// 发送 OTA_INFO 查询命令
static bool send_ota_info_request() {
  return uart_send_frame(CMD_ARM_OTA_INFO, nullptr, 0);
}

// ==================== 状态机处理 ====================

// 处理 IDLE 状态（无操作）
static void process_idle() {
  // 空闲状态，等待 arm_ota_start() 调用
}

// 处理 WAIT_READY 状态（等待 ARM 进入 IAP 模式）
static void process_wait_ready() {
  unsigned long now = millis();

  // 超时检查
  if (now - g_state_enter_time >= ARM_OTA_START_TIMEOUT) {
    set_state(ARM_OTA_TIMEOUT, "ARM IAP mode timeout");
    return;
  }
}

// 处理 SENDING 状态（发送数据包）
static void process_sending() {
  unsigned long now = millis();

  // 发送间隔控制
  if (now - g_last_send_time < ARM_OTA_SEND_INTERVAL) {
    return;
  }

  // 发送当前包
  if (send_ota_data(g_current_packet)) {
    g_last_send_time = now;
    update_progress();
    set_state(ARM_OTA_WAIT_ACK, "Waiting for ACK");
  } else {
    set_state(ARM_OTA_ERROR, "UART send failed");
  }
}

// 处理 WAIT_ACK 状态（等待 ARM 确认）
static void process_wait_ack() {
  unsigned long now = millis();

  // 超时检查
  if (now - g_state_enter_time >= ARM_OTA_ACK_TIMEOUT) {
    if (g_retry_count >= ARM_OTA_MAX_RETRIES) {
      char msg[64];
      snprintf(msg, sizeof(msg), "ACK timeout, packet %u, retries exhausted",
               g_current_packet);
      set_state(ARM_OTA_ERROR, msg);
      return;
    }

    // 重传
    g_retry_count++;
    Serial.printf("[ARM-OTA] ACK timeout, retry %d/%d for packet %u\n",
                  g_retry_count, ARM_OTA_MAX_RETRIES, g_current_packet);
    set_state(ARM_OTA_SENDING, "Retrying packet");
    return;
  }
}

// 处理 VERIFYING 状态（等待 ARM 校验完成）
static void process_verifying() {
  unsigned long now = millis();

  // 发送间隔控制
  if (now - g_last_send_time < ARM_OTA_SEND_INTERVAL) {
    return;
  }

  // 发送 END 命令
  if (send_ota_end()) {
    g_last_send_time = now;
    set_state(ARM_OTA_WAIT_READY, "Waiting for verify result");
  } else {
    set_state(ARM_OTA_ERROR, "Failed to send END command");
  }
}

// ==================== 发送函数 ====================

// 流式模式：发送一包数据（从 g_stream_buf 读取）
static bool stream_send_packet(uint16_t packet_idx, size_t offset, size_t len) {
  uint8_t buf[2 + ARM_OTA_PACKET_SIZE];
  int pos = 0;

  // packet_index (2B, LE)
  put_u16_le(buf + pos, packet_idx);
  pos += 2;

  // 从缓冲区复制数据（offset 是全局偏移，len 是本包长度）
  size_t copy_len = (len > ARM_OTA_PACKET_SIZE) ? ARM_OTA_PACKET_SIZE : len;
  memcpy(buf + pos, g_stream_buf + (offset % ARM_OTA_STREAM_BUF_SIZE), copy_len);
  pos += (int)copy_len;

  bool ok = uart_send_frame(CMD_ARM_OTA_DATA, buf, pos);
  if (ok) {
    Serial.printf("[ARM-OTA:stream] DATA[%u] %u bytes\n", packet_idx, (unsigned)copy_len);
  }
  return ok;
}

// 流式模式：尝试发送缓冲区中的数据
static void stream_process() {
  if (g_ota_state != ARM_OTA_SENDING && g_ota_state != ARM_OTA_WAIT_ACK) {
    return;
  }

  unsigned long now = millis();

  // 如果在 WAIT_ACK 状态，检查超时
  if (g_ota_state == ARM_OTA_WAIT_ACK) {
    if (now - g_state_enter_time >= ARM_OTA_ACK_TIMEOUT) {
      if (g_retry_count >= ARM_OTA_MAX_RETRIES) {
        set_state(ARM_OTA_TIMEOUT, "Stream ACK timeout");
        return;
      }
      g_retry_count++;
      Serial.printf("[ARM-OTA:stream] ACK timeout, retry %d/%d\n",
                   g_retry_count, ARM_OTA_MAX_RETRIES);
      set_state(ARM_OTA_SENDING, "Retrying packet");
    }
    return;  // 等待 ACK 期间不发送新数据
  }

  // 发送间隔控制
  if (now - g_last_send_time < ARM_OTA_SEND_INTERVAL) {
    return;
  }

  // 计算当前包的信息
  size_t packet_offset = (size_t)g_current_packet * ARM_OTA_PACKET_SIZE;
  size_t remaining = g_stream_total - packet_offset;
  if (remaining == 0) {
    return;  // 没有更多数据
  }

  // 缓冲区数据不足时，等待更多数据
  if (g_stream_buf_len == 0) {
    return;
  }

  // 计算本包要发送的数据范围
  size_t packet_len = (remaining > ARM_OTA_PACKET_SIZE) ? ARM_OTA_PACKET_SIZE : remaining;

  if (send_ota_data(g_current_packet)) {
    g_last_send_time = now;
    g_stream_sent += packet_len;
    g_progress = (int)((uint32_t)g_stream_sent * 100 / g_stream_total);
    update_progress();
    set_state(ARM_OTA_WAIT_ACK, "Waiting for ACK");
  }
}

// ==================== 公共 API（流式模式）====================

bool arm_ota_start_streaming(size_t total_size) {
  if (total_size == 0) {
    Serial.println("[ARM-OTA] Error: invalid total size");
    return false;
  }

  if (g_ota_state != ARM_OTA_IDLE && g_ota_state != ARM_OTA_DONE
      && g_ota_state != ARM_OTA_ERROR && g_ota_state != ARM_OTA_TIMEOUT) {
    Serial.printf("[ARM-OTA] Cannot start streaming in state %d\n", g_ota_state);
    return false;
  }

  // 初始化流式模式
  g_streaming_mode = true;
  g_stream_total = total_size;
  g_stream_sent = 0;
  g_stream_buf_len = 0;
  g_current_packet = 0;
  g_retry_count = 0;
  g_progress = 0;
  g_total_packets = (uint16_t)((total_size + ARM_OTA_PACKET_SIZE - 1) / ARM_OTA_PACKET_SIZE);

  Serial.printf("[ARM-OTA] Streaming mode: size=%u, packets=%u\n",
                (unsigned)total_size, g_total_packets);

  // 发送 START 命令
  g_firmware_len = total_size;  // 复用 g_firmware_len 用于计算
  if (send_ota_start()) {
    g_last_send_time = millis();
    set_state(ARM_OTA_WAIT_READY, "Waiting for ARM IAP ready");
    return true;
  } else {
    g_streaming_mode = false;
    set_state(ARM_OTA_ERROR, "Failed to send START");
    return false;
  }
}

bool arm_ota_feed(const uint8_t* data, size_t len) {
  if (!g_streaming_mode || len == 0) {
    return false;
  }

  // 将数据追加到缓冲区（带反压：缓冲区满时等待状态机消费）
  for (size_t i = 0; i < len; i++) {
    // 缓冲区满时，驱动状态机发送数据包并等待 ACK，腾出空间
    unsigned long wait_start = millis();
    while (g_stream_buf_len >= ARM_OTA_STREAM_BUF_SIZE) {
      // 超时保护：30 秒内缓冲区仍未腾出空间
      if (millis() - wait_start > 30000) {
        Serial.printf("[ARM-OTA] feed() timeout: buffer stuck at %u bytes\n",
                      (unsigned)g_stream_buf_len);
        return false;
      }
      // 驱动状态机：发送数据包 + 等待 ACK
      arm_ota_process();
      // 处理 UART 接收 ACK
      uart_protocol_process();
      delay(5);
    }
    g_stream_buf[g_stream_buf_len++] = data[i];
  }

  return true;
}

bool arm_ota_end_streaming() {
  if (!g_streaming_mode) {
    return false;
  }

  // 等待状态机发送完所有剩余数据包（带 ACK 确认）
  unsigned long start_wait = millis();
  const unsigned long MAX_WAIT_TIME = 60000;  // 最多等待 60 秒

  Serial.printf("[ARM-OTA] Waiting for %u packets to send (buffer: %u bytes)...\n",
                g_total_packets - g_current_packet, (unsigned)g_stream_buf_len);

  while (g_current_packet < g_total_packets) {
    // 超时检查
    if (millis() - start_wait > MAX_WAIT_TIME) {
      Serial.printf("[ARM-OTA] Timeout waiting for packets: sent %u/%u\n",
                    g_current_packet, g_total_packets);
      set_state(ARM_OTA_TIMEOUT, "End streaming timeout");
      return false;
    }

    // 驱动状态机发送数据包（带 ACK 确认）
    arm_ota_process();

    // 处理 UART 接收 ACK
    uart_protocol_process();

    // 短暂延迟，避免 CPU 占用过高
    delay(10);
  }

  Serial.printf("[ARM-OTA] All %u packets sent, sending END...\n", g_total_packets);

  // 所有包发送完毕，发送 END 命令
  if (send_ota_end()) {
    g_last_send_time = millis();
    set_state(ARM_OTA_WAIT_READY, "Waiting for verify result");
    return true;
  }
  return false;
}

bool arm_ota_is_streaming_mode() {
  return g_streaming_mode;
}

// ==================== 公共 API ====================

void arm_ota_init() {
  g_ota_state = ARM_OTA_IDLE;
  g_ota_callback = nullptr;
  g_firmware = nullptr;
  g_firmware_len = 0;
  g_total_packets = 0;
  g_current_packet = 0;
  g_retry_count = 0;
  g_progress = 0;
  g_last_send_time = 0;
  g_state_enter_time = 0;
  g_message[0] = '\0';
  memset(&g_fw_info, 0, sizeof(g_fw_info));
  g_fw_info.valid = false;
  g_streaming_mode = false;
  g_stream_total = 0;
  g_stream_sent = 0;
  g_stream_buf_len = 0;
  memset(g_stream_buf, 0, sizeof(g_stream_buf));

  Serial.println("[ARM-OTA] Module initialized");
}

void arm_ota_set_callback(ArmOtaCallback cb) {
  g_ota_callback = cb;
}

bool arm_ota_start(const uint8_t* firmware, size_t length) {
  if (!firmware || length == 0) {
    Serial.println("[ARM-OTA] Error: invalid firmware data");
    return false;
  }

  if (g_ota_state != ARM_OTA_IDLE && g_ota_state != ARM_OTA_DONE
      && g_ota_state != ARM_OTA_ERROR && g_ota_state != ARM_OTA_TIMEOUT) {
    Serial.printf("[ARM-OTA] Error: cannot start in state %d\n", g_ota_state);
    return false;
  }

  g_firmware = firmware;
  g_firmware_len = length;
  g_current_packet = 0;
  g_retry_count = 0;
  g_progress = 0;

  // 计算总包数
  g_total_packets = (uint16_t)((length + ARM_OTA_PACKET_SIZE - 1) / ARM_OTA_PACKET_SIZE);

  Serial.printf("[ARM-OTA] Starting OTA: size=%u, packets=%u\n",
                (unsigned)length, g_total_packets);

  // 发送 START 命令
  if (send_ota_start()) {
    g_last_send_time = millis();
    set_state(ARM_OTA_WAIT_READY, "Waiting for ARM IAP ready");
    return true;
  } else {
    set_state(ARM_OTA_ERROR, "Failed to send START command");
    return false;
  }
}

void arm_ota_process() {
  switch (g_ota_state) {
    case ARM_OTA_IDLE:
      process_idle();
      break;

    case ARM_OTA_WAIT_READY:
      process_wait_ready();
      break;

    case ARM_OTA_SENDING:
      if (g_streaming_mode) {
        stream_process();
      } else {
        process_sending();
      }
      break;

    case ARM_OTA_WAIT_ACK:
      if (g_streaming_mode) {
        stream_process();
      } else {
        process_wait_ack();
      }
      break;

    case ARM_OTA_VERIFYING:
      process_verifying();
      break;

    case ARM_OTA_DONE:
    case ARM_OTA_ERROR:
    case ARM_OTA_TIMEOUT:
      // 终态，不做处理
      break;

    default:
      break;
  }
}

ArmOtaState arm_ota_get_state() {
  return g_ota_state;
}

int arm_ota_get_progress() {
  return g_progress;
}

const char* arm_ota_get_message() {
  return g_message;
}

bool arm_ota_is_ready() {
  return (g_ota_state == ARM_OTA_IDLE);
}

bool arm_ota_request_info() {
  if (g_ota_state != ARM_OTA_IDLE) {
    Serial.printf("[ARM-OTA] Cannot request info in state %d\n", g_ota_state);
    return false;
  }

  g_fw_info.valid = false;
  bool ok = send_ota_info_request();
  if (ok) {
    Serial.println("[ARM-OTA] INFO request sent");
  }
  return ok;
}

const ArmFirmwareInfo* arm_ota_get_firmware_info() {
  return &g_fw_info;
}

void arm_ota_cancel() {
  if (g_ota_state == ARM_OTA_IDLE) {
    return;
  }
  Serial.printf("[ARM-OTA] Cancelled from state %d\n", g_ota_state);
  g_firmware = nullptr;
  g_firmware_len = 0;
  g_progress = 0;
  set_state(ARM_OTA_IDLE, "OTA cancelled");
}

// ==================== UART 帧处理 ====================
// 在 main.cpp 的 on_uart_frame 回调中调用此函数

void arm_ota_handle_uart(uint8_t cmd, const uint8_t* data, uint16_t len) {
  switch (cmd) {
    case CMD_ARM_OTA_ACK: {
      // ARM 确认收到
      Serial.printf("[ARM-OTA] ACK received, len=%u\n", len);

      if (g_ota_state == ARM_OTA_WAIT_READY) {
        // ARM 已进入 IAP 模式，开始发送数据
        g_current_packet = 0;
        g_retry_count = 0;
        set_state(ARM_OTA_SENDING, "Sending firmware data");
      }
      else if (g_ota_state == ARM_OTA_WAIT_ACK) {
        // 数据包确认，发送下一包
        g_retry_count = 0;  // 重置重传计数
        g_current_packet++;

        if (g_current_packet >= g_total_packets) {
          // 所有包发送完毕，进入校验阶段
          g_progress = 100;
          set_state(ARM_OTA_VERIFYING, "All packets sent, verifying");
        } else {
          update_progress();
          set_state(ARM_OTA_SENDING, "Sending next packet");
        }
      }
      else if (g_ota_state == ARM_OTA_VERIFYING) {
        // VERIFYING 状态下收到 ACK 表示 ARM 校验完成且成功
        // （此时 WAIT_READY 状态下的 ACK 表示校验成功）
        // 注意：VERIFYING 发送 END 后会转到 WAIT_READY
      }
      // WAIT_READY 状态下收到 ACK（END 校验成功）
      else if (g_ota_state == ARM_OTA_WAIT_READY && g_progress == 100) {
        // 这是 END 命令的 ACK，表示 ARM 校验成功
        set_state(ARM_OTA_DONE, "OTA completed successfully");
        g_firmware = nullptr;
        g_firmware_len = 0;
      }
      break;
    }

    case CMD_ARM_OTA_NACK: {
      // ARM 请求重传
      uint16_t nack_packet = 0;
      if (len >= 2) {
        nack_packet = get_u16_le(data);
      }

      Serial.printf("[ARM-OTA] NACK received, packet=%u\n", nack_packet);

      if (g_ota_state == ARM_OTA_WAIT_ACK) {
        if (g_retry_count >= ARM_OTA_MAX_RETRIES) {
          char msg[64];
          snprintf(msg, sizeof(msg), "NACK max retries for packet %u", nack_packet);
          set_state(ARM_OTA_ERROR, msg);
        } else {
          g_retry_count++;
          // 跳转到 NACK 指定的包序号重传
          if (nack_packet < g_total_packets) {
            g_current_packet = nack_packet;
          }
          char msg[64];
          snprintf(msg, sizeof(msg), "NACK retry %d/%d for packet %u",
                   g_retry_count, ARM_OTA_MAX_RETRIES, g_current_packet);
          set_state(ARM_OTA_SENDING, msg);
        }
      }
      else if (g_ota_state == ARM_OTA_WAIT_READY) {
        // END 命令后 ARM 校验失败，发 NACK
        char msg[64];
        snprintf(msg, sizeof(msg), "ARM verify NACK, packet=%u", nack_packet);
        set_state(ARM_OTA_ERROR, msg);
      }
      break;
    }

    case CMD_ARM_OTA_INFO: {
      // ARM 上报固件信息
      // DATA: [version_len(1B)] [version_string(nB)] [fw_size(4B, LE)] [md5(16B)]
      Serial.printf("[ARM-OTA] INFO received, len=%u\n", len);

      memset(&g_fw_info, 0, sizeof(g_fw_info));
      g_fw_info.valid = false;

      if (len < 1 + 4 + ARM_OTA_MD5_SIZE) {
        Serial.println("[ARM-OTA] INFO data too short");
        break;
      }

      int pos = 0;

      // version_len (1B)
      uint8_t ver_len = data[pos++];
      if (ver_len > sizeof(g_fw_info.version) - 1) {
        ver_len = sizeof(g_fw_info.version) - 1;
      }

      // version_string (nB)
      if (pos + ver_len + 4 + ARM_OTA_MD5_SIZE > len) {
        Serial.println("[ARM-OTA] INFO data length mismatch");
        break;
      }
      memcpy(g_fw_info.version, data + pos, ver_len);
      g_fw_info.version[ver_len] = '\0';
      pos += ver_len;

      // fw_size (4B, LE)
      g_fw_info.fw_size = get_u32_le(data + pos);
      pos += 4;

      // md5 (16B)
      memcpy(g_fw_info.md5, data + pos, ARM_OTA_MD5_SIZE);
      pos += ARM_OTA_MD5_SIZE;

      g_fw_info.valid = true;

      Serial.printf("[ARM-OTA] ARM firmware: version=\"%s\", size=%u\n",
                    g_fw_info.version, (unsigned)g_fw_info.fw_size);
      break;
    }

    default:
      // 非 OTA 命令，忽略
      break;
  }
}
