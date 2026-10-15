/**
 * ================================================================
 *  Collector IAP OTA 模块实现 (ESP-IDF) — ARM / DSP / BMS 共用 PULL 协议
 *
 *  与 ARM 端 (external/driver_arm 的 Collector.c / Update.c) 逐字节对齐:
 *    1. App 触发:   ESP 发 0x07/param1=0x0001/param2=4/"ARMU"
 *                   App 回 status 0 后延时复位进 bootloader
 *    2. 信息查询:   boot 发 0x09/param1=1(ARM) 或 2(DSP) 或 3(BMS)
 *                   ESP 回 0x09: data[0]=0 + transport size + 链式 CRC
 *                  或 data[0]=1 (无新固件); 对非本会话 target 一律回"无"
 *    3. 擦除后 0x0A 要第 0 包; ESP 发 0x0B: 包号 LE + TEA 加密 payload
 *    4. 每包 0x0B ACK; 末包 status=0 = 整包 CRC 通过
 *
 *  BMS 差异 (《BMS固件升级通信协议规格说明书》V1.0, ARM boot BmsUpdate.c 中转):
 *    - 0x0A 是握手结果通知: status=1 即 boot 已终止 BMS 任务, 本侧快速失败
 *    - 块失败 (0x0B status=1, ERROR=101/102/103/0xFF): boot 包号不递增,
 *      本侧重发同一包号 (上限 BMS_PACKET_MAX_RETRIES)
 *    - 末包 ACK 覆盖 0xFC+BMS 整体 CRC32, 等待窗口放宽到 30s
 *    - 全程 ~0.4-0.7s/块 (115200 baud), 92KB 约 1 分钟
 *
 *  目标选择 (s_iap_target):
 *    ARM-only: p1=1 有货, p1=2 回"无"
 *    DSP-only: p1=1 回"无", p1=2 有货; 仍要 ARMU 进 IAP
 *
 *  boot 侧任何一步超时都会回到 NoGetFwInfo 重新查询 0x09。
 *  传输在 OTA 任务内同步执行; arm_poll 在 is_streaming 期间暂停 CS 轮询。
 * ================================================================
 */

#include "arm_ota.h"
#include "arm_iap_codec.h"
#include "cs_protocol.h"
#include "config.h"

#include <errno.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "driver/uart.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_spiffs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "offline_cache.h"
#include "mqtt_handler.h"

static const char* TAG = "ARM-OTA";

#define STAGE_PATH_ARM          "/cache/arm_ota.bin"
#define STAGE_PATH_DSP          "/cache/dsp_ota.bin"
#define STAGE_PATH_BMS          "/cache/bms_ota.bin"

/* ARM boot 命令 (Boot/source/Collector/Collector.c InstructionProcess) */
#define IAP_CMD_FW_INFO         0x09    /* 双向: boot 查询 / ESP 应答固件信息 */
#define IAP_CMD_FW_REQ          0x0A    /* boot→ESP: 请求固件数据包 */
#define IAP_CMD_FW_DATA         0x0B    /* ESP→boot 数据包 / boot→ESP ACK */

/* ARM App 触发参数 (App/source/Collector/Collector.c CTRLCMD 分支) */
#define CS_CTRL_ENTER_BOOT      0x0001
#define ARMU_PAYLOAD            "ARMU"

/* 步骤超时。boot 侧窗口: info 查询 3s、每包数据 5s, 均无重试但超时后会
 * 自动回到 NoGetFwInfo 重新查询, 本侧超时即按"重查"处理。 */
#define ARMU_RESP_TIMEOUT_MS    2000    /* App 对 ARMU 的应答 */
#define TRIGGER_ATTEMPTS        3
#define BOOT_INFO_WINDOW_MS     15000   /* 复位后等 boot 0x09 (覆盖 boot 自检) */
/* 每包 ACK 等待: 4112B 帧 @115200bps 单程 ≈0.4s, 加 boot→BMS 转发 + 4KB 擦除
 * + ACK 回传; 窗口 15s 沿用不收紧。 */
#define PACKET_EVENT_WAIT_MS    15000   /* 0x0A / 每包 ACK 等待 */
#define ARM_ERASE_WAIT_MS      45000    /* info 应答后等 boot 擦完 APP 区发 0x0A */
#define DSP_ERASE_WAIT_MS      60000    /* DSP 路径: 握手+擦除 (ARM 侧最长约 24s, 留裕量) */
#define BMS_HANDSHAKE_WAIT_MS  60000    /* BMS: info 应答后等 0x0A 握手通知
                                         * (boot 轮询握手 30×200ms + BMS 可能先静默复位) */
#define BMS_FINAL_ACK_WAIT_MS  30000    /* BMS 末包 ACK = 块写入 + 0xFC + BMS 整体 CRC32 */
#define BMS_PACKET_MAX_RETRIES 3        /* BMS 单块 status=1 重发同包的上限 */
#define DSP_QUERY_WAIT_MS       3000    /* 最终 ACK 后等对端查询 */
#define RETRIGGER_DELAY_MS      1500    /* 重试前等待 ARM 复位回落 */
#define TRANSFER_MAX_RESTARTS   5       /* boot 重查 0x09 的容忍次数 */
#define ARM_OTA_ALLOC_ATTEMPTS  3
#define ARM_OTA_ALLOC_RETRY_MS  50

static ArmOtaState       g_ota_state = ARM_OTA_IDLE;
static ArmOtaCallback    g_ota_callback = NULL;
static ArmOtaCancelCheck g_cancel_check = NULL;
static const volatile bool* g_abort_flag = NULL;
static int               g_progress = 0;
static int64_t           g_state_enter_time = 0;
static char              g_message[128] = {0};
/* 失败原因: 只留在设备日志里时无从判断是暂存问题还是 ARM 协议问题, 需要上报云端 */
static char              g_last_error[96] = {0};
static bool              g_last_storage_error = false;

static bool              g_streaming_mode = false;   /* 会话开启(含 staging 阶段) */
static uint16_t          g_total_packets = 0;
static uint16_t          g_current_packet = 0;
static IapTarget         s_iap_target = IAP_TARGET_ARM;

static FILE*             g_stage_file = NULL;
static size_t            g_stage_written = 0;
static size_t            g_stage_total = 0;
static bool              g_spiffs_ready = false;
static const char*       g_stage_path = STAGE_PATH_ARM;

/* Collector 帧 RX: uart_rx_task 喂字节, OTA worker 消费完整帧 */
static cs_ppp_rx_t       s_col_rx;
static SemaphoreHandle_t s_frame_sem = NULL;
static SemaphoreHandle_t s_frame_mutex = NULL;
static volatile bool     s_frame_ready = false;

/* 收包留痕 → MQTT (cs_inv/{sn}/ota/debug)。
 * 帧是在 UART 任务 (栈仅 4096) 里解析的, 那里绝不能发布 (mqtt_publish 栈上有
 * 2KB 缓冲, 会栈溢出), 所以只把摘要写进静态缓冲, 由 OTA 任务在等待循环里取走发布。 */
static char              s_rx_trace[224];
static volatile bool     s_rx_trace_pending = false;

/* ---- ARM IAP 工作区: 只在 ARM/DSP/BMS 传输期使用, 按需整块分配 ----
 * ESP 自升级完全用不到这 ~20KB; 而整机堆只有 ~131KB, OTA 窗口还要拿接收缓冲
 * 和 (https 时的)TLS 上下文, 常驻等于挤占。整块一次 malloc、显式一次 free,
 * 避免多处分配/释放泄漏:
 *   - 分配: 固件已暂存完成、真正进入 IAP 传输前才懒分配，避免与下载传输
 *     缓冲同时占堆；OOM 时清理暂存会话并干净失败；
 *   - 释放: 每次 ARM/DSP/BMS 会话结束后由 ota_service 的统一出口释放；ESP
 *     会话入口再做一次防御性释放，确保该约 20KB 工作区绝不跨会话常驻。
 * RX 路径由 arm_poll 的 arm_ota_active() 门控 (会话外不会有字节路由进来),
 * TX/打包路径只在会话内; 所有使用点仍带 s_ws 空指针护栏, 不命中即失败不崩溃。 */
typedef struct {
    /* TX 上限: 4KB IAP 包封装后 ~4136B, ×2 容纳 PPP 7E 转义最坏膨胀 */
    uint8_t  tx[CS_FRAME_TX_MAX * 2 + 4];
    uint8_t  iap_data[2 + ARM_IAP_MAX_PAYLOAD_SIZE];  /* 0x0B data: 包号(u16)+payload */
    uint8_t  plain[CS_FRAME_MAX];
    uint8_t  raw_rx[CS_FRAME_MAX];   /* 解密前的原始(加密)帧字节, 仅用于失败留痕 */
    uint8_t  scratch[CS_FRAME_MAX];  /* 合并帧尾部恢复用 */
    struct {
        uint8_t  raw[CS_FRAME_MAX];
        size_t   len;
        uint8_t  cmd;
        uint16_t p1;
        uint16_t p2;
    }        frame;
} ArmOtaWorkspace;

static ArmOtaWorkspace*  s_ws;

static int64_t get_time_ms(void)
{
    return esp_timer_get_time() / 1000;
}

/* 记录失败原因: 同时进设备日志与上报 message。ARM 升级失败的现场此前只剩一个
 * 通用错误码, 无法区分"暂存写不进去"和"ARM 侧协议没走通"。 */
static void record_error(const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    vsnprintf(g_last_error, sizeof(g_last_error), fmt, args);
    va_end(args);
}

/* 工作区懒分配: 只在 OTA worker 即将进入 IAP PULL 传输时调用。 */
static bool ws_ensure(void)
{
    if (s_ws) return true;
    for (unsigned attempt = 1; attempt <= ARM_OTA_ALLOC_ATTEMPTS; ++attempt) {
        s_ws = malloc(sizeof(*s_ws));
        if (s_ws) return true;
        ESP_LOGW(TAG, "workspace allocation failed (%u/%u, %u bytes)",
                 attempt, ARM_OTA_ALLOC_ATTEMPTS, (unsigned)sizeof(*s_ws));
        if (attempt < ARM_OTA_ALLOC_ATTEMPTS)
            vTaskDelay(pdMS_TO_TICKS(ARM_OTA_ALLOC_RETRY_MS));
    }
    record_error("ARM OTA workspace malloc failed after %u attempts (%u bytes)",
                 ARM_OTA_ALLOC_ATTEMPTS, (unsigned)sizeof(*s_ws));
    return false;
}

/* 见工作区注释: 只应在没有 ARM 会话时调用。 */
void arm_ota_workspace_release(void)
{
    if (g_streaming_mode || g_ota_state != ARM_OTA_IDLE) {
        ESP_LOGE(TAG, "workspace release refused: ARM session active");
        return;
    }
    /* 与 RX 路径串行: uart_rx_task 可能刚通过 arm_ota_active() 判断、正持锁在用
     * s_ws (TOCTOU)。取同一把 s_frame_mutex 再 free, 保证没有在飞的使用者。 */
    if (s_frame_mutex && xSemaphoreTake(s_frame_mutex, portMAX_DELAY) != pdTRUE) {
        return;
    }
    if (s_ws) {
        free(s_ws);
        s_ws = NULL;
        ESP_LOGI(TAG, "ARM OTA workspace released (+%u bytes heap)",
                 (unsigned)sizeof(ArmOtaWorkspace));
    }
    if (s_frame_mutex) xSemaphoreGive(s_frame_mutex);
}

static const char* iap_target_name(IapTarget target)
{
    return target == IAP_TARGET_DSP ? "DSP"
         : target == IAP_TARGET_BMS ? "BMS" : "ARM";
}

static const char* stage_path_for(IapTarget target)
{
    return target == IAP_TARGET_DSP ? STAGE_PATH_DSP
         : target == IAP_TARGET_BMS ? STAGE_PATH_BMS : STAGE_PATH_ARM;
}

static int64_t erase_wait_ms_for(IapTarget target)
{
    return target == IAP_TARGET_BMS ? BMS_HANDSHAKE_WAIT_MS
         : target == IAP_TARGET_DSP ? DSP_ERASE_WAIT_MS : ARM_ERASE_WAIT_MS;
}

static size_t iap_packet_size_for(IapTarget target)
{
    return target == IAP_TARGET_BMS
        ? ARM_IAP_BMS_PACKET_SIZE : ARM_IAP_LEGACY_PACKET_SIZE;
}

/* 释放异常收尾留下的会话。不走 arm_ota_cancel(): 那条路径会触发状态回调,
 * 暂存阶段被上层当成一次"取消"上报。释放后 g_ota_state 回 IDLE, arm_poll
 * 才会恢复 CS 轮询与串口 RX 路由。 */
static void release_stale_session(void)
{
    if (g_stage_file) {
        fclose(g_stage_file);
        g_stage_file = NULL;
    }
    remove(g_stage_path);
    g_stage_written = 0;
    g_stage_total = 0;
    g_streaming_mode = false;
    g_total_packets = 0;
    g_current_packet = 0;
    g_progress = 0;
    g_ota_state = ARM_OTA_IDLE;
}

static void set_state(ArmOtaState new_state, const char* msg)
{
    g_ota_state = new_state;
    g_state_enter_time = get_time_ms();
    if (msg) {
        strncpy(g_message, msg, sizeof(g_message) - 1);
        g_message[sizeof(g_message) - 1] = '\0';
    }
    ESP_LOGI(TAG, "State=%d, %s", new_state, g_message);
    if (g_ota_callback) {
        g_ota_callback(new_state, g_progress, g_message);
    }
}

static void update_progress(void)
{
    if (g_total_packets == 0) {
        g_progress = 0;
    } else {
        g_progress = (int)((uint32_t)g_current_packet * 100 / g_total_packets);
        if (g_progress > 100) g_progress = 100;
    }
}

static void put_u32_le(uint8_t* buf, uint32_t val)
{
    buf[0] = (uint8_t)(val & 0xFF);
    buf[1] = (uint8_t)((val >> 8) & 0xFF);
    buf[2] = (uint8_t)((val >> 16) & 0xFF);
    buf[3] = (uint8_t)((val >> 24) & 0xFF);
}

static bool iap_cancelled(void)
{
    if (g_cancel_check && g_cancel_check()) return true;
    if (g_abort_flag && *g_abort_flag) return true;
    return false;
}

/* ---------------- 帧收发 ---------------- */

static bool iap_send_frame(uint8_t cmd, uint16_t param1, uint16_t param2,
                           const uint8_t* data)
{
    if (!s_ws) return false;   /* 只在会话内被调用; 护栏兜底 */
    size_t n = cs_frame_send(s_ws->tx, cmd, param1, param2, data, esp_random());
    if (n == 0) {
        ESP_LOGE(TAG, "frame build failed cmd=0x%02X p2=%u", cmd, param2);
        return false;
    }
    uart_write_bytes(UART_PORT_NUM, s_ws->tx, n);
    ESP_LOGI(TAG, "TX cmd=0x%02X p1=0x%04X p2=%u (%u bytes)", cmd, param1,
             param2, (unsigned)n);
    return true;
}

typedef struct {
    uint8_t  cmd;
    uint16_t p1;
    uint16_t p2;
    uint8_t  payload[16];
    uint16_t payload_len;
} iap_event_t;

/* 取走 UART 任务记录的失败帧摘要 (在 OTA 任务里发布, 见 s_rx_trace 说明) */
static bool take_rx_trace(char* out, size_t capacity)
{
    bool has = false;
    if (xSemaphoreTake(s_frame_mutex, portMAX_DELAY) == pdTRUE) {
        if (s_rx_trace_pending) {
            strncpy(out, s_rx_trace, capacity - 1);
            out[capacity - 1] = '\0';
            s_rx_trace_pending = false;
            has = true;
        }
        xSemaphoreGive(s_frame_mutex);
    }
    return has;
}

/* 1 帧暂存: 当前等待不匹配的帧不再直接丢弃 —— boot 的 0x09 可能在等 ACK 的
 * 2 秒窗口内先到, 丢弃会让接下来的 0x09 等待永远落空。暂存只保留最新一帧,
 * 再次不匹配时丢弃, 保证循环有界。 */
static iap_event_t s_stashed_event;
static bool s_stashed_valid = false;
/* 留痕上报计数 (节流用) */
static unsigned s_trace_pub_count = 0;

/* 等待下一帧 (deadline 之前无帧返回 false; 与过滤条件不符的帧暂存) */
static bool iap_wait_event(int64_t deadline, iap_event_t* ev)
{
    for (;;) {
        if (iap_cancelled()) return false;
        const int64_t remain = deadline - get_time_ms();
        if (remain <= 0) return false;
        const TickType_t wait =
            (remain > 50) ? pdMS_TO_TICKS(50) : (TickType_t)remain;

        /* 把 UART 任务记下的失败帧摘要发到 MQTT (在此任务发布, 栈够)。
         * 节流: 数据阶段可能每帧都 CRC 不符, 全量上报会刷屏。 */
        {
            char trace[sizeof(s_rx_trace)];
            if (take_rx_trace(trace, sizeof(trace))) {
                if (s_trace_pub_count < 5 || (s_trace_pub_count % 20) == 0) {
                    mqtt_publish_ota_trace("arm-rx", trace);
                } else if (s_trace_pub_count == 5) {
                    mqtt_publish_ota_trace("arm-rx", "后续同类帧不再逐条上报(每20条一次)");
                }
                s_trace_pub_count++;
            }
        }

        bool valid = false;
        bool from_stash = false;
        if (s_stashed_valid) {
            *ev = s_stashed_event;
            s_stashed_valid = false;
            valid = true;
            from_stash = true;
        } else {
            if (xSemaphoreTake(s_frame_sem, wait) != pdTRUE) continue;
            if (xSemaphoreTake(s_frame_mutex, portMAX_DELAY) == pdTRUE) {
                valid = s_frame_ready;
                if (valid) {
                    const size_t plen =
                        (s_ws->frame.len > CS_OFF_DATA) ? (s_ws->frame.len - CS_OFF_DATA) : 0;
                    const uint16_t n = (plen > sizeof(ev->payload))
                        ? (uint16_t)sizeof(ev->payload) : (uint16_t)plen;
                    ev->cmd = s_ws->frame.cmd;
                    ev->p1 = s_ws->frame.p1;
                    ev->p2 = s_ws->frame.p2;
                    memcpy(ev->payload, s_ws->frame.raw + CS_OFF_DATA, n);
                    ev->payload_len = n;
                }
                s_frame_ready = false;
                xSemaphoreGive(s_frame_mutex);
            }
            if (!valid) {
                ESP_LOGW(TAG, "RX 事件为空, 继续等待");
                continue;
            }
        }
        if (ev->cmd == IAP_CMD_FW_INFO || ev->cmd == IAP_CMD_FW_REQ ||
            ev->cmd == IAP_CMD_FW_DATA || ev->cmd == CS_CMD_CTRLCMD) {
            /* 关键帧留痕 (0x0B 数据包太多, 不进 MQTT: 进度由 progress 表达) */
            if (ev->cmd != IAP_CMD_FW_DATA) {
                char line[80];
                snprintf(line, sizeof(line), "cmd=0x%02X p1=0x%04X p2=%u",
                         ev->cmd, ev->p1, ev->p2);
                mqtt_publish_ota_trace("arm-rx", line);
            }
            return true;
        }
        if (from_stash) {
            ESP_LOGW(TAG, "暂存帧仍不匹配, 丢弃 cmd=0x%02X p1=0x%04X p2=%u",
                     ev->cmd, ev->p1, ev->p2);
        } else {
            s_stashed_event = *ev;
            s_stashed_valid = true;
            ESP_LOGI(TAG, "帧暂存 cmd=0x%02X p1=0x%04X p2=%u (等待其他帧)",
                     ev->cmd, ev->p1, ev->p2);
        }
    }
}

/* ---- staged 文件 → FwKey 加密数据包 ---- */

/* 第 packet_id 包的加密 payload。传输头**只在第 0 包**:
 *   ARM boot (Update.c InvFwUpdatingProcess) 对首包收取并跳过前 16B 写 APP
 *   (databuf+4), 其余包直写, 末包按 (Size-16)%包大小 收。
 *   若每包都加 16B 头: ① 链式 CRC 从第 1 包起就与 boot 不一致 → 末包报 CRC 错;
 *   ② 写进 flash 的镜像每包错位 16B (即使 CRC 对上也是坏镜像)。
 *   与 arm_iap_codec.c 的 arm_iap_build_packet() 保持同一语义。
 * 包大小按目标选择: ARM/DSP=1024B，匹配 boot 的 1088B 接收帧及 DSP 协议；
 * BMS=4096B，匹配 UPDATE_FW_BLOCK_LEN 与 flash block 编址。
 * 镜像按 8 字节对齐补零, 补零部分不在 staged 文件里, 由 memset 兜底。 */
static size_t iap_build_packet(FILE* f, uint16_t packet_id, size_t padded,
                               uint8_t* packet, size_t capacity)
{
    if (!packet) return 0;
    const size_t packet_size = iap_packet_size_for(s_iap_target);
    const size_t offset = (size_t)packet_id * packet_size;
    if (offset >= padded) return 0;
    size_t len = padded - offset;
    if (len > packet_size) len = packet_size;

    size_t want = len;   /* 应用字节数 */
    const size_t available = (g_stage_total > offset) ? (g_stage_total - offset) : 0;
    if (want > available) want = available;
    const size_t header = (packet_id == 0) ? ARM_IAP_HEADER_SIZE : 0u;
    const size_t total = len + header;
    if (total > capacity) return 0;
    memset(packet, 0, total);
    if (fseek(f, (long)offset, SEEK_SET) != 0) return 0;
    if (want > 0 && fread(packet + header, 1, want, f) != want) {
        return 0;
    }

    arm_iap_crypt_payload(packet, total, 1);
    return total;
}

/* 链式 CRC: 对每包加密后的 payload 链算 (与 ARM 端 DataCRC 一致) */
static bool iap_compute_chain_crc(uint16_t packets, size_t padded, uint32_t* out)
{
    if (!s_ws) return false;
    FILE* f = fopen(g_stage_path, "rb");
    if (!f) return false;
    uint32_t crc = ARM_IAP_CRC_INIT;
    bool ok = true;
    for (uint16_t k = 0; k < packets && ok; ++k) {
        const size_t len = iap_build_packet(
            f, k, padded, s_ws->iap_data + 2, sizeof(s_ws->iap_data) - 2);
        if (len == 0 || (len & 3u) != 0) {
            ok = false;
            break;
        }
        crc = arm_iap_crc32_update(s_ws->iap_data + 2, len, crc);
    }
    fclose(f);
    if (ok) *out = crc;
    return ok;
}

static bool iap_send_packet(FILE* f, uint16_t packet_id, size_t padded)
{
    if (!s_ws) return false;

    /* data = 包号(u16 LE) + payload；直接构建到最终帧数据区，避免再保留/复制
     * 一份 4112B packet 缓冲。 */
    s_ws->iap_data[0] = (uint8_t)(packet_id & 0xFF);
    s_ws->iap_data[1] = (uint8_t)((packet_id >> 8) & 0xFF);
    const size_t payload_len = iap_build_packet(
        f, packet_id, padded, s_ws->iap_data + 2, sizeof(s_ws->iap_data) - 2);
    if (payload_len == 0) return false;
    return iap_send_frame(IAP_CMD_FW_DATA, s_iap_target,
                          (uint16_t)(2 + payload_len), s_ws->iap_data);
}

static bool iap_send_info(size_t transport, uint32_t chain_crc)
{
    uint8_t info[9] = {0};   /* data[0]=0 有新固件; [1..4]=size; [5..8]=CRC */
    put_u32_le(info + 1, (uint32_t)transport);
    put_u32_le(info + 5, chain_crc);
    return iap_send_frame(IAP_CMD_FW_INFO, s_iap_target, sizeof(info), info);
}

static bool iap_send_no_fw(IapTarget target)
{
    const uint8_t no_fw = 1;
    return iap_send_frame(IAP_CMD_FW_INFO, target, 1, &no_fw);
}

/* ---- PULL 传输主流程 ---- */

/* Wait for the 0x09 matching this session target. The boot queries every IAP
 * target in its chain (ARM→DSP→BMS) before reaching ours; reply "none" to the
 * others immediately or the peer wastes its query window and times out. */
static bool wait_target_fw_query(iap_event_t* ev)
{
    const int64_t deadline = get_time_ms() + BOOT_INFO_WINDOW_MS;
    const uint16_t want = (uint16_t)s_iap_target;
    while (iap_wait_event(deadline, ev)) {
        if (ev->cmd != IAP_CMD_FW_INFO) continue;
        if (ev->p1 == want) return true;
        if (ev->p1 == IAP_TARGET_ARM || ev->p1 == IAP_TARGET_DSP ||
            ev->p1 == IAP_TARGET_BMS) {
            ESP_LOGI(TAG, "%s-only: reject 0x09/p1=%u (no image this session)",
                     iap_target_name(s_iap_target), (unsigned)ev->p1);
            {
                char line[64];
                snprintf(line, sizeof(line),
                         "%s-only: no p1=%u firmware this session",
                         iap_target_name(s_iap_target), (unsigned)ev->p1);
                mqtt_publish_ota_trace("iap-0x09", line);
            }
            iap_send_no_fw((IapTarget)ev->p1);
            continue;
        }
    }
    return false;
}

static bool pull_transfer(void)
{
    g_last_error[0] = '\0';
    /* 下载收尾刚经历 flash 连续写 + WiFi 收包功耗尖峰, 立即触发 ARMU 会让
     * ARM 复位浪涌与之叠加——现场实测第二轮任务起在下载尾段/触发段间歇性把
     * 共电轨拉穿 brownout 阈值(档7=2.51V, 已是最低档), ESP 猝死重启。先静默
     * 600ms 让电源回稳, 再触发 ARM 复位进 boot。 */
    vTaskDelay(pdMS_TO_TICKS(600));
    const size_t padded = (g_stage_total + 7u) & ~(size_t)7u;
    const size_t transport = padded + ARM_IAP_HEADER_SIZE;
    const size_t packet_size = iap_packet_size_for(s_iap_target);
    const uint16_t packets = arm_iap_packet_count(padded, packet_size);
    const char* tname = iap_target_name(s_iap_target);
    if (packets == 0) {
        record_error("image size not storable: %u bytes", (unsigned)g_stage_total);
        ESP_LOGE(TAG, "image size not storable: %u", (unsigned)g_stage_total);
        return false;
    }
    g_total_packets = packets;
    g_current_packet = 0;

    uint32_t chain_crc = 0;
    if (!iap_compute_chain_crc(packets, padded, &chain_crc)) {
        record_error("chain CRC computation failed on staged image");
        ESP_LOGE(TAG, "chain CRC computation failed");
        return false;
    }
    ESP_LOGI(TAG, "transfer start target=%s image=%u padded=%u transport=%u chunk=%u packets=%u crc=0x%08lx",
             tname, (unsigned)g_stage_total, (unsigned)padded, (unsigned)transport,
             (unsigned)packet_size, packets, (unsigned long)chain_crc);
    {
        char line[96];
        snprintf(line, sizeof(line),
                 "target=%s image=%u transport=%u packets=%u crc=0x%08lx",
                 tname, (unsigned)g_stage_total, (unsigned)transport, packets,
                 (unsigned long)chain_crc);
        mqtt_publish_ota_trace("transfer-start", line);
    }

    FILE* f = fopen(g_stage_path, "rb");
    if (!f) {
        record_error("reopen staged firmware failed (errno=%d)", errno);
        ESP_LOGE(TAG, "reopen staged firmware failed");
        return false;
    }

    const uint16_t want_p1 = (uint16_t)s_iap_target;
    const int64_t first_erase_wait = erase_wait_ms_for(s_iap_target);
    bool success = false;
    bool trigger_acked = false;
    bool info_seen = false;
    bool handshake_fatal = false;   /* BMS 握手身份不匹配: 重试无意义 */
    for (unsigned attempt = 1;
         attempt <= TRIGGER_ATTEMPTS && !success && !iap_cancelled();
         ++attempt) {
        iap_send_frame(CS_CMD_CTRLCMD, CS_CTRL_ENTER_BOOT,
                       (uint16_t)strlen(ARMU_PAYLOAD), (const uint8_t*)ARMU_PAYLOAD);
        iap_event_t ev;
        if (iap_wait_event(get_time_ms() + ARMU_RESP_TIMEOUT_MS, &ev) &&
            ev.cmd == CS_CMD_CTRLCMD && ev.p1 == CS_CTRL_ENTER_BOOT &&
            ev.p2 == 1 && ev.payload_len >= 1 && ev.payload[0] == 0) {
            trigger_acked = true;
            ESP_LOGI(TAG, "ARM accepted OTA trigger");
            mqtt_publish_ota_trace("arm-trigger", "ARMU acked (App online, will reset to boot)");
        } else {
            ESP_LOGW(TAG, "no ARMU ack (attempt %u), ARM may already be in bootloader", attempt);
        }

        if (!wait_target_fw_query(&ev)) {
            record_error("no %s bootloader 0x09 within %us (attempt %u/%u, ARMU ack=%d)",
                         tname, BOOT_INFO_WINDOW_MS / 1000, attempt, TRIGGER_ATTEMPTS,
                         (int)trigger_acked);
            ESP_LOGW(TAG, "bootloader 0x09/%u not seen (attempt %u)", want_p1, attempt);
            mqtt_publish_ota_trace("arm-boot-0x09", "NOT seen (ARM not in bootloader or no 0x09)");
            vTaskDelay(pdMS_TO_TICKS(RETRIGGER_DELAY_MS));
            continue;
        }
        info_seen = true;
        {
            char line[80];
            snprintf(line, sizeof(line), "seen: p1=%u in IAP, reply firmware info now",
                     (unsigned)want_p1);
            mqtt_publish_ota_trace("arm-boot-0x09", line);
        }

        if (!iap_send_info(transport, chain_crc)) {
            record_error("send info frame failed (first reply)");
            ESP_LOGE(TAG, "send info frame failed");
            break;
        }
        mqtt_publish_ota_trace("arm-info", "info sent (transport/chain crc)");

        uint16_t sent = 0;
        unsigned restarts = 0;
        unsigned bms_retries = 0;
        int64_t wait_ms = first_erase_wait;
        while (!success && !iap_cancelled()) {
            /* BMS 末包 ACK 覆盖 块写入+0xFC+BMS 整体 CRC32, 窗口放宽 */
            const int64_t iter_wait =
                (s_iap_target == IAP_TARGET_BMS && sent >= packets)
                    ? BMS_FINAL_ACK_WAIT_MS : wait_ms;
            if (!iap_wait_event(get_time_ms() + iter_wait, &ev)) {
                record_error("no %s frame within %us (packet %u/%u)",
                             tname, (unsigned)(iter_wait / 1000),
                             (unsigned)g_current_packet, packets);
                ESP_LOGW(TAG, "no frame in %.1fs (attempt %u)",
                         iter_wait / 1000.0, attempt);
                break;
            }
            wait_ms = PACKET_EVENT_WAIT_MS;
            if (ev.cmd == IAP_CMD_FW_INFO && ev.p1 == want_p1) {
                if (++restarts > TRANSFER_MAX_RESTARTS) {
                    record_error("%s boot re-queried 0x09 over %u times",
                                 tname, TRANSFER_MAX_RESTARTS);
                    ESP_LOGE(TAG, "boot re-query loop exceeds %u", TRANSFER_MAX_RESTARTS);
                    break;
                }
                ESP_LOGW(TAG, "boot re-queried 0x09/%u, restarting transfer", want_p1);
                {
                    /* MQTT 侧留痕: boot 中途重查 0x09 意味着传输将从 packet 0
                     * 重爬(进度回退), 失败记录里的 block 号属于重启后的那一遍
                     * ——不看这条 trace 会把两遍传输的进度混为一谈。 */
                    char line[80];
                    snprintf(line, sizeof(line), "boot re-queried 0x09, restart #%u at packet %u/%u",
                             restarts, (unsigned)sent, (unsigned)packets);
                    mqtt_publish_ota_trace("iap-restart", line);
                }
                if (!iap_send_info(transport, chain_crc)) {
                    record_error("send info frame failed (packet restart)");
                    break;
                }
                wait_ms = first_erase_wait;
                continue;
            }
            if (ev.cmd == IAP_CMD_FW_REQ && ev.p1 == want_p1) {
                if (s_iap_target == IAP_TARGET_BMS && ev.payload_len >= 1 &&
                    ev.payload[0] != 0) {
                    /* BMS 的 0x0A 是握手结果通知 (status=1 即握手失败, boot 已
                     * 终止本次 BMS 任务): 0xFF=无应答, 202~205=型号不匹配 */
                    const uint8_t bms_err =
                        ev.payload_len > 4 ? ev.payload[4] : 0u;
                    record_error("BMS handshake failed: ERROR=%u%s", bms_err,
                                 bms_err == 0xFF ? " (no BMS response)" : "");
                    ESP_LOGE(TAG, "BMS handshake failed ERROR=%u", bms_err);
                    /* 协议书 §5: 握手失败即终止 BMS 任务, 不分错误码一律不再重试。
                     * 实测 1.7.4: boot 的 WaitTimeOut 会 3s 后重查 0x09 (无上限),
                     * 本侧若再答 info / 再触发 ARMU, 就和 boot 形成"查询→握手→
                     * BMS 复位→失败"的无限循环。立即失败回 IDLE 后, boot 的重查
                     * 询无人应答, 它自己的 WaitFwInfor 1s tick 到期即跳到 PC 阶段
                     * 退出, BMS 停止被反复复位。 */
                    handshake_fatal = true;
                    break;
                }
                if (!iap_send_packet(f, 0, padded)) {
                    record_error("send packet 0 failed");
                    break;
                }
                sent = 1;
                g_current_packet = sent;
                update_progress();
                set_state(ARM_OTA_WAIT_ACK, "Waiting for packet 0 ACK");
                continue;
            }
            if (ev.cmd == IAP_CMD_FW_DATA && ev.p1 == want_p1) {
                if (ev.payload_len < 1) continue;
                if (ev.payload[0] != 0) {
                    const uint8_t bms_err =
                        ev.payload_len > 4 ? ev.payload[4] : 0u;
                    if (s_iap_target == IAP_TARGET_BMS && sent > 0) {
                        /* BMS 块失败 (101/102/103/0xFF): boot 的包号不递增,
                         * 协议要求重发"同一包号"的数据包 (协议书 §5) */
                        if (++bms_retries > BMS_PACKET_MAX_RETRIES) {
                            record_error("BMS block %u failed %u times (ERROR=%u)",
                                         (unsigned)(sent - 1), bms_retries,
                                         bms_err);
                            ESP_LOGE(TAG, "BMS block %u keeps failing (ERROR=%u)",
                                     (unsigned)(sent - 1), bms_err);
                            break;
                        }
                        ESP_LOGW(TAG, "BMS block %u ACK status=1 ERROR=%u, resend %u/%u",
                                 (unsigned)(sent - 1), bms_err, bms_retries,
                                 BMS_PACKET_MAX_RETRIES);
                        if (!iap_send_packet(f, sent - 1, padded)) {
                            record_error("resend BMS block %u failed",
                                         (unsigned)(sent - 1));
                            break;
                        }
                        set_state(ARM_OTA_WAIT_ACK, "Resending BMS block");
                        continue;
                    }
                    record_error("%s boot reported CRC failure (packet %u/%u)",
                                 tname, (unsigned)g_current_packet, packets);
                    ESP_LOGE(TAG, "boot reported CRC failure");
                    break;
                }
                bms_retries = 0;
                if (sent == 0) continue;
                if (sent < packets) {
                    if (!iap_send_packet(f, sent, padded)) {
                        record_error("send packet %u failed", (unsigned)sent);
                        break;
                    }
                    ++sent;
                    g_current_packet = sent;
                    update_progress();
                    set_state(ARM_OTA_SENDING, "Sending next packet");
                    continue;
                }
                success = true;
                break;
            }
        }

        if (success) break;
        if (handshake_fatal) break;
        if (attempt < TRIGGER_ATTEMPTS) {
            vTaskDelay(pdMS_TO_TICKS(RETRIGGER_DELAY_MS));
        }
    }
    fclose(f);

    if (success) {
        /* 收尾: boot 升级链若还查其他 target 的 0x09, 回"无固件"放行 */
        iap_event_t ev;
        if (iap_wait_event(get_time_ms() + DSP_QUERY_WAIT_MS, &ev) &&
            ev.cmd == IAP_CMD_FW_INFO &&
            ev.p1 != (uint16_t)s_iap_target &&
            (ev.p1 == IAP_TARGET_ARM || ev.p1 == IAP_TARGET_DSP ||
             ev.p1 == IAP_TARGET_BMS)) {
            iap_send_no_fw((IapTarget)ev.p1);
        }
        remove(g_stage_path);
        {
            char line[64];
            snprintf(line, sizeof(line), "OK: %s transfer complete (chain CRC pass)", tname);
            mqtt_publish_ota_trace("arm-result", line);
        }
        return true;
    }
    if (g_last_error[0] == '\0') {
        record_error("%s transfer incomplete: ARMU ack=%d, boot 0x09=%d, packets=%u",
                     tname, (int)trigger_acked, (int)info_seen, (unsigned)packets);
    }
    ESP_LOGE(TAG, "%s transfer aborted: %s", tname, g_last_error);
    mqtt_publish_ota_trace("arm-result", g_last_error);
    return false;
}

/* ---- staged 数据写入 (HTTP 下载路径) ---- */

static bool ensure_stage_fs(void)
{
    if (g_spiffs_ready) return true;
    const esp_vfs_spiffs_conf_t conf = {
        .base_path = "/cache",
        .partition_label = "cache",
        .max_files = 2,
        .format_if_mount_failed = true,
    };
    esp_err_t err = esp_vfs_spiffs_register(&conf);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        /* offline_cache_init 已有"挂不上就格式化"的兜底, 这里同样兜一次:
         * 否则 cache 分区一旦损坏, ARM 升级会一直失败且无法自行恢复。 */
        ESP_LOGW(TAG, "Mount OTA cache failed: %s, formatting", esp_err_to_name(err));
        esp_spiffs_format(conf.partition_label);
        err = esp_vfs_spiffs_register(&conf);
    }
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        record_error("mount '/cache' failed: %s (partition 'cache' 缺失或不可用)",
                     esp_err_to_name(err));
        ESP_LOGE(TAG, "Failed to mount OTA cache: %s", esp_err_to_name(err));
        return false;
    }
    /* 断电/panic 中断会把暂存固件留在 /cache, 而会话标志在 RAM: 重启后
     * stale-session 清理永不触发, offline_cache_reserve_space 又只逐遥测
     * 批次文件, 残留会把之后所有中继升级堵死在空间检查上。下载只以 "wb"
     * 截断写、无跨重启续传, 故重启后挂载成功的残留必是垃圾, 直接清掉。 */
    static bool s_stage_swept;
    if (!s_stage_swept) {
        s_stage_swept = true;
        int swept = 0;
        if (remove(STAGE_PATH_ARM) == 0) swept++;
        if (remove(STAGE_PATH_DSP) == 0) swept++;
        if (remove(STAGE_PATH_BMS) == 0) swept++;
        if (swept > 0) {
            ESP_LOGW(TAG, "Swept %d stale OTA stage file(s) from /cache", swept);
        }
    }
    g_spiffs_ready = true;
    return true;
}

bool arm_ota_start_streaming_for(IapTarget target, size_t total_size)
{
    g_last_storage_error = false;
    s_iap_target = target;
    g_stage_path = stage_path_for(target);
    if (total_size == 0 || total_size > ARM_OTA_MAX_IMAGE_SIZE) {
        record_error("bad %s image size: %u bytes (max %u)",
                     iap_target_name(target),
                     (unsigned)total_size, (unsigned)ARM_OTA_MAX_IMAGE_SIZE);
        return false;
    }
    if (g_stage_file || g_streaming_mode) {
        /* 上次会话异常收尾留下的标志: 清理后继续。否则本次及后续所有 ARM 推送
         * 都会在这里静默失败, 同时 CS 轮询一直被挂起。 */
        ESP_LOGW(TAG, "Stale ARM OTA session (file=%d streaming=%d), releasing",
                 g_stage_file != NULL, (int)g_streaming_mode);
        release_stale_session();
        if (g_stage_file || g_streaming_mode) {
            record_error("stale ARM OTA session could not be released");
            return false;
        }
    }
    g_last_error[0] = '\0';
    s_trace_pub_count = 0;
    if (!ensure_stage_fs()) return false;
    if (!offline_cache_reserve_space(total_size + 16 * 1024)) {
        g_last_storage_error = true;
        const size_t need = total_size + 16 * 1024;
        size_t total = 0, used = 0;
        const esp_err_t info = esp_spiffs_info("cache", &total, &used);
        const size_t free_bytes = (info == ESP_OK && total > used) ? (total - used) : 0;
        record_error("not enough /cache space: need=%u free=%u (total=%u used=%u, info=%s)",
                     (unsigned)need, (unsigned)free_bytes, (unsigned)total, (unsigned)used,
                     esp_err_to_name(info));
        ESP_LOGE(TAG, "Insufficient cache space for ARM OTA staging");
        return false;
    }
    g_stage_file = fopen(g_stage_path, "wb");
    if (!g_stage_file) {
        record_error("open %s failed (errno=%d)", g_stage_path, errno);
        return false;
    }
    g_stage_total = total_size;
    g_stage_written = 0;
    g_streaming_mode = true;
    g_total_packets = 0;
    g_current_packet = 0;
    g_progress = 0;
    ESP_LOGI(TAG, "Staging %s firmware: %u bytes",
             iap_target_name(target), (unsigned)total_size);
    return true;
}

bool arm_ota_start_streaming(size_t total_size)
{
    return arm_ota_start_streaming_for(IAP_TARGET_ARM, total_size);
}

bool arm_ota_feed(const uint8_t* data, size_t len)
{
    if (!g_stage_file || !data || len == 0 || g_stage_written + len > g_stage_total) {
        record_error("stage feed rejected: written=%u/%u len=%u",
                     (unsigned)g_stage_written, (unsigned)g_stage_total, (unsigned)len);
        return false;
    }
    if (fwrite(data, 1, len, g_stage_file) != len) {
        record_error("stage write failed at %u/%u bytes (errno=%d)",
                     (unsigned)g_stage_written, (unsigned)g_stage_total, errno);
        arm_ota_cancel();
        return false;
    }
    g_stage_written += len;
    return true;
}

/* ---- 会话收尾 ---- */

bool arm_ota_end_streaming_for(IapTarget target, const volatile bool* cancel_flag)
{
    s_iap_target = target;
    g_stage_path = stage_path_for(target);
    if (!g_stage_file) {
        record_error("end_streaming called without an open stage file");
        return false;
    }
    if (g_stage_written != g_stage_total) {
        record_error("staged size mismatch: %u/%u bytes",
                     (unsigned)g_stage_written, (unsigned)g_stage_total);
        arm_ota_cancel();
        return false;
    }
    /* 暂存下载已结束，4.5KB 传输缓冲已由 ota_service 释放；此刻才申请约
     * 20KB IAP 工作区，避免两块大内存在下载阶段叠加。 */
    if (!ws_ensure()) {
        arm_ota_cancel();
        return false;
    }
    fclose(g_stage_file);
    g_stage_file = NULL;

    g_abort_flag = cancel_flag;
    set_state(ARM_OTA_WAIT_READY, "Triggering ARM IAP (0x07 ARMU)");
    const bool ok = pull_transfer();
    g_abort_flag = NULL;
    if (!ok) {
        const bool cancelled = iap_cancelled();
        release_stale_session();
        ESP_LOGE(TAG, "Collector IAP transfer failed: %s",
                 g_last_error[0] ? g_last_error : "unknown");
        if (cancelled) {
            set_state(ARM_OTA_IDLE, "OTA cancelled");
        }
        return false;
    }
    g_progress = 100;
    set_state(ARM_OTA_DONE, "Collector IAP transfer complete");
    return true;
}

bool arm_ota_end_streaming_cancelable(const volatile bool* cancel_flag)
{
    return arm_ota_end_streaming_for(s_iap_target, cancel_flag);
}

bool arm_ota_end_streaming(void)
{
    return arm_ota_end_streaming_for(s_iap_target, NULL);
}

IapTarget arm_ota_get_iap_target(void)
{
    return s_iap_target;
}

bool arm_ota_is_streaming_mode(void)
{
    return g_streaming_mode || g_stage_file != NULL;
}

/* ---- 公共 API ---- */

void arm_ota_init(void)
{
    g_ota_state = ARM_OTA_IDLE;
    g_ota_callback = NULL;
    g_cancel_check = NULL;
    g_abort_flag = NULL;
    g_progress = 0;
    g_state_enter_time = 0;
    g_message[0] = '\0';
    g_streaming_mode = false;
    g_total_packets = 0;
    g_current_packet = 0;
    g_stage_file = NULL;
    g_stage_written = 0;
    g_stage_total = 0;
    s_iap_target = IAP_TARGET_ARM;
    g_stage_path = STAGE_PATH_ARM;
    s_frame_ready = false;
    cs_ppp_rx_init(&s_col_rx);
    if (!s_frame_sem) s_frame_sem = xSemaphoreCreateBinary();
    if (!s_frame_mutex) s_frame_mutex = xSemaphoreCreateMutex();
    ESP_LOGI(TAG, "Module initialized (Collector PULL protocol)");
}

void arm_ota_set_callback(ArmOtaCallback cb)
{
    g_ota_callback = cb;
}

void arm_ota_set_cancel_check(ArmOtaCancelCheck cb)
{
    g_cancel_check = cb;
}

/* 兼容保留: PULL 流程在 end_streaming 内同步完成, 无需外部驱动 */
void arm_ota_process(void)
{
}

ArmOtaState arm_ota_get_state(void)
{
    return g_ota_state;
}

int arm_ota_get_progress(void)
{
    return g_progress;
}

const char* arm_ota_get_message(void)
{
    return g_message;
}

const char* arm_ota_get_last_error(void)
{
    return g_last_error;
}

bool arm_ota_last_error_is_storage(void)
{
    return g_last_storage_error;
}

void arm_ota_reset(void)
{
    g_progress = 0;
    g_total_packets = 0;
    g_current_packet = 0;
    g_streaming_mode = false;
    if (g_ota_state != ARM_OTA_IDLE) {
        set_state(ARM_OTA_IDLE, "OTA session closed");
    }
}

void arm_ota_cancel(void)
{
    if (g_stage_file) {
        fclose(g_stage_file);
        g_stage_file = NULL;
        remove(g_stage_path);
        g_stage_written = 0;
        g_stage_total = 0;
    }
    if (g_ota_state == ARM_OTA_IDLE && !g_streaming_mode) {
        return;
    }
    ESP_LOGW(TAG, "Cancelled from state %d", g_ota_state);
    g_streaming_mode = false;
    g_progress = 0;
    g_total_packets = 0;
    g_current_packet = 0;
    set_state(ARM_OTA_IDLE, "OTA cancelled");
}

void arm_ota_rx_reset(void)
{
    cs_ppp_rx_init(&s_col_rx);
    s_frame_ready = false;
    s_stashed_valid = false;
}

static void dispatch_frame(const uint8_t* buf, size_t len,
                           uint8_t cmd, uint16_t p1, uint16_t p2)
{
    if (!s_ws) return;
    memcpy(s_ws->frame.raw, buf, len);
    s_ws->frame.len = len;
    s_ws->frame.cmd = cmd;
    s_ws->frame.p1 = p1;
    s_ws->frame.p2 = p2;
    s_frame_ready = true;
    ESP_LOGI(TAG, "RX cmd=0x%02X p1=0x%04X p2=%u len=%u", cmd, p1, p2,
             (unsigned)len);
}

/* 合并帧恢复: ARM 复位过快时, 半截 ACK(缺结尾 7E)会与紧随其后的完整帧在
 * PPP 层拼进同一个缓冲。每一帧的外层 CBC 都以自身首字节为零 IV 起点, 因此
 * 从缓冲尾部按 8 字节对齐切出的完整子帧可以独立解密校验。 */
static bool recover_suffix_frame(size_t len)
{
    if (!s_ws) return false;
    for (size_t take = 24; take < len; take += 8) {
        memcpy(s_ws->scratch, s_ws->plain + len - take, take);
        uint8_t cmd;
        uint16_t p1, p2;
        if (cs_decrypt_rx(s_ws->scratch, take) != 0) continue;
        if (cs_frame_parse(s_ws->scratch, take, &cmd, &p1, &p2, NULL) != 0) continue;
        ESP_LOGW(TAG, "从合并帧尾部恢复子帧 len=%u", (unsigned)take);
        dispatch_frame(s_ws->scratch, take, cmd, p1, p2);
        return true;
    }
    return false;
}

/* 前 max 字节的十六进制 (供留痕), 越界即截断 */
static void hex_prefix(char* out, size_t capacity, const uint8_t* data,
                       size_t len, size_t max)
{
    static const char digits[] = "0123456789abcdef";
    size_t n = 0;
    if (len > max) len = max;
    for (size_t i = 0; i < len && n + 3 <= capacity; ++i) {
        out[n++] = digits[data[i] >> 4];
        out[n++] = digits[data[i] & 0x0f];
    }
    out[n] = '\0';
}

/* 把帧的失败细节写进 s_rx_trace (由 OTA 任务取走发布)。
 * 必须在持有 s_frame_mutex 时调用。raw 为解密前的原始字节。 */
static void record_bad_frame(const char* why, const uint8_t* raw, size_t len,
                             const uint8_t* plain)
{
    char rawhex[52];
    char plainhex[52];
    hex_prefix(rawhex, sizeof(rawhex), raw, len, 24);
    hex_prefix(plainhex, sizeof(plainhex), plain, len, 24);
    snprintf(s_rx_trace, sizeof(s_rx_trace),
             "%s len=%u hdr=0x%02X cmdlen=%u raw=%s%s%s plain=%s",
             why, (unsigned)len, plain[0],
             (size_t)len > 3 ? (unsigned)(plain[1] | (plain[2] << 8)) : 0u,
             rawhex, len > 24 ? ".." : "", "", plainhex);
    s_rx_trace_pending = true;
}

void arm_ota_rx_byte(uint8_t byte)
{
    if (!s_frame_sem) {
        /* 兜底: init 未被调用时也能建同步原语 (正常路径 app_main 已建) */
        arm_ota_init();
    }
    if (xSemaphoreTake(s_frame_mutex, portMAX_DELAY) != pdTRUE) {
        return;
    }
    if (!s_ws) {
        /* 会话外不会有字节路由进来 (arm_ota_active() 门控); 万一进来, 丢弃。
         * 持锁后再判: 释放工作区的路径也持这把锁, 故此处不会摸到已 free 的指针。 */
        xSemaphoreGive(s_frame_mutex);
        return;
    }
    if (cs_ppp_rx_byte(&s_col_rx, byte)) {
        const size_t len = s_col_rx.length;
        memcpy(s_ws->plain, s_col_rx.buf, len);
        memcpy(s_ws->raw_rx, s_col_rx.buf, len);   /* 解密是就地改写, 先留原始 */
        cs_ppp_rx_init(&s_col_rx);

        /* 每一帧完整留痕: 原始(加密)字节 —— 用于判读 ARM 侧到底发了什么 */
        ESP_LOGI(TAG, "RX raw len=%u", (unsigned)len);
        ESP_LOG_BUFFER_HEX_LEVEL(TAG, s_ws->plain, len, ESP_LOG_INFO);

        uint8_t cmd = 0;
        uint16_t p1 = 0, p2 = 0;
        bool valid = false;
        if (cs_decrypt_rx(s_ws->plain, len) != 0) {
            ESP_LOGW(TAG, "帧解密失败 len=%u", (unsigned)len);
            record_bad_frame("decrypt-fail", s_ws->raw_rx, len, s_ws->plain);
        } else {
            const int rc = cs_frame_parse(s_ws->plain, len, &cmd, &p1, &p2, NULL);
            if (rc == 0) {
                dispatch_frame(s_ws->plain, len, cmd, p1, p2);
                valid = true;
            } else if (rc == -2) {
                /* rc == -2 只在 header/CmdLen/长度 全部合法、仅 CRC 不符时返回。
                 * 现场 boot 的 0x09 实测就是这种: 帧头 cmd=0x09/p1=1/p2=0 完全正确,
                 * 但 CRC 槽位填的不是任何常规约定算出的值 (疑似 boot 发送侧实现的
                 * 缺陷)。IAP 控制帧不携带固件数据, 整包完整性另由链式 CRC + 镜像
                 * SHA-256 + Ed25519 签名把关, 故放行并留痕 —— 否则整条 ARM 升级
                 * 会被这一处 CRC 卡死。结构不合法的帧 (rc == -1) 仍然拒绝。 */
                /* rc == -2 时 cs_frame_parse 在回填 cmd/p1/p2 之前就返回了,
                 * 必须自己从帧里取 —— 否则按 cmd=0 分发会被状态机过滤掉,
                 * 表现为"帧收到了却永远等不到 0x09"(1.5.7 的实测教训)。 */
                cmd    = s_ws->plain[CS_OFF_CMD];
                p1     = (uint16_t)(s_ws->plain[CS_OFF_PARAM1] |
                                    (s_ws->plain[CS_OFF_PARAM1 + 1] << 8));
                p2     = (uint16_t)(s_ws->plain[CS_OFF_PARAM2] |
                                    (s_ws->plain[CS_OFF_PARAM2 + 1] << 8));
                const uint16_t cmdlen =
                    (uint16_t)(s_ws->plain[1] | (s_ws->plain[2] << 8));
                const uint32_t crc_got =
                    (uint32_t)s_ws->plain[cmdlen] |
                    ((uint32_t)s_ws->plain[cmdlen + 1] << 8) |
                    ((uint32_t)s_ws->plain[cmdlen + 2] << 16) |
                    ((uint32_t)s_ws->plain[cmdlen + 3] << 24);
                ESP_LOGW(TAG, "帧 CRC 不符但结构合法, 放行 cmd=0x%02X p1=0x%04X "
                         "p2=%u crc_calc=0x%08lx crc_got=0x%08lx",
                         cmd, p1, p2,
                         (unsigned long)cs_crc32_words(s_ws->plain, cmdlen >> 2),
                         (unsigned long)crc_got);
                record_bad_frame("crc-mismatch", s_ws->raw_rx, len, s_ws->plain);
                dispatch_frame(s_ws->plain, len, cmd, p1, p2);
                valid = true;
            } else {
                ESP_LOGW(TAG, "帧结构非法 len=%u header=0x%02X", (unsigned)len,
                         s_ws->plain[0]);
                record_bad_frame("parse-fail", s_ws->raw_rx, len, s_ws->plain);
                valid = recover_suffix_frame(len);
            }
        }
        xSemaphoreGive(s_frame_mutex);
        if (valid) {
            xSemaphoreGive(s_frame_sem);
        }
    } else {
        xSemaphoreGive(s_frame_mutex);
    }
}
