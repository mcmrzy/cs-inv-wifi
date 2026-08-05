/*
 * arm_poll.c — ARM 串口轮询调度
 *
 *  UART1 (9600, 8N1) 连接 ARM 采集器接口 (逆变器侧反相电路已软件补偿):
 *   - 上电读取只读信息 (READ_R_DATA 0x0190, 44B)
 *   - 每 5s 轮询运行参数 (READ_RUNPARAM 0x03E8, 176B)
 *   - 云端命令 (WRITE_CTRLPARAM / WRITE_R_W_DATA) 插队发送
 *   - 接收线程完成 PPP 解封装 → 双重解密 → 帧解析 → 数据分发
 */
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/uart.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "arm_poll.h"
#include "cs_protocol.h"
#include "config.h"
#include "uart_ota.h"
#include "arm_ota.h"

static const char *TAG = "ARMPOLL";

/* 应答超时 (ms): 9600 波特率下 200B 帧 ~200ms, ARM 处理 <100ms */
#define ARM_RESP_TIMEOUT_MS    800
/* 接收字节间隔超时 (ms), 与 ARM 端 1s 一致, 防止残帧堆积 */
#define ARM_RX_GAP_TIMEOUT_MS  1000
/* 断线判定: CollectorConnect=1200 x 100ms = 120s */
#define ARM_DISCONNECT_MS      120000
/* 轮询周期 */
#define ARM_POLL_INTERVAL_MS   5000

#define WRITE_DATA_MAX         176

/* ---- 全局共享参数 (rx 任务写入, 主循环读取, mutex 保护) ---- */
RunParamDef g_run_param;
OnlyReadDataDef g_only_read;
ReadWiteDataDef g_read_write;
arm_link_stat_t g_arm_stat;

static SemaphoreHandle_t s_param_mutex;
static cs_ppp_rx_t s_ppp_rx;
static bool s_rx_active;
static uint32_t s_rx_last_byte_ms;

/* 写命令队列 (单槽) */
static struct {
    bool     active;
    uint8_t  cmd;
    uint16_t param1;
    uint16_t param2;
    uint8_t  data[WRITE_DATA_MAX];
    uint16_t data_len;
} s_write_pending;

/* 调度状态 */
static bool     s_info_requested;      /* 上电需读 info */
static bool     s_info_done;
static bool     s_wait_resp;           /* 有命令等待应答 */
static uint8_t  s_wait_cmd;
static uint16_t s_wait_param1;
static uint16_t s_wait_param2;
static uint32_t s_wait_start_ms;
static uint32_t s_last_poll_ms;
static uint32_t s_last_utc_ms;         /* 最近成功校时时刻 */
static uint32_t s_now_ms;

/* 写命令结果回调 (多路: sntp_sync 校时 + cmd_handler 命令闭环, 各自按 cmd/addr 过滤) */
#define WRITE_CB_MAX  2
static arm_write_result_cb_t s_write_cbs[WRITE_CB_MAX];

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void send_frame_cmd(uint8_t cmd, uint16_t param1, uint16_t param2,
                           const uint8_t *data)
{
    uint8_t out[CS_FRAME_MAX * 2 + 4];
    size_t n = cs_frame_send(out, cmd, param1, param2, data, esp_random());
    if (n == 0) {
        ESP_LOGE(TAG, "frame too large");
        return;
    }
    ESP_LOGI(TAG, "TX cmd=0x%02X p1=0x%04X p2=%u len=%u%s", cmd, param1,
             param2, n, UART_INVERT_TX ? " (inv)" : "");
    ESP_LOG_BUFFER_HEX(TAG, out, MIN(n, 16));
    uart_write_bytes(UART_PORT_NUM, out, n);
    s_wait_resp = true;
    s_wait_cmd = cmd;
    s_wait_param1 = param1;
    s_wait_param2 = param2;
    s_wait_start_ms = s_now_ms;
}

/* ---------------- 接收分发 (UART RX 任务) ---------------- */

static void handle_frame(uint8_t *raw, size_t len)
{
    uint8_t cmd;
    uint16_t param1, param2;
    const uint8_t *data;
    uint32_t now = now_ms();

    if (cs_decrypt_rx(raw, len) != 0) {
        g_arm_stat.err_count++;
        ESP_LOGW(TAG, "rx decrypt fail len=%u", len);
        return;
    }
    if (cs_frame_parse(raw, len, &cmd, &param1, &param2, &data) != 0) {
        g_arm_stat.err_count++;
        ESP_LOGW(TAG, "rx parse fail len=%u", len);
        return;
    }

    g_arm_stat.last_rx_ms = now;
    g_arm_stat.connected = true;
    g_arm_stat.err_count = 0;

    if (xSemaphoreTake(s_param_mutex, pdMS_TO_TICKS(50)) != pdTRUE) {
        return;
    }

    switch (cmd) {
    case CS_CMD_READ_RUNPARAM:            /* 0x03 运行参数应答 */
        if (param2 == RUN_PARAM_SIZE && len >= 8 + RUN_PARAM_SIZE) {
            memcpy(&g_run_param, data, RUN_PARAM_SIZE);
            g_arm_stat.poll_count++;
            if (s_wait_resp && s_wait_cmd == cmd) {
                s_wait_resp = false;
            }
        }
        break;
    case CS_CMD_READ_R_DATA:              /* 0x06 只读信息应答 */
        if (param2 == ONLYREAD_DATA_SIZE && len >= 8 + ONLYREAD_DATA_SIZE) {
            memcpy(&g_only_read, data, ONLYREAD_DATA_SIZE);
            s_info_done = true;
            g_arm_stat.last_info_ms = now;
            if (s_wait_resp && s_wait_cmd == cmd) {
                s_wait_resp = false;
            }
        }
        break;
    case CS_CMD_READ_R_W_DATA:            /* 0x04 可读写数据应答 */
        if (param2 == 4 && len >= 12) {
            memcpy(&g_read_write, data, 4);
            if (s_wait_resp && s_wait_cmd == cmd) {
                s_wait_resp = false;
            }
        }
        break;
    case CS_CMD_WRITE_CTRLPARAM:          /* 0x02 写控制参数应答 */
    case CS_CMD_WRITE_R_W_DATA:           /* 0x05 写可读写应答 */
        /* 写响应: CmdLen=12, param2=1(0x02)/回显(0x05), data[0]=err */
        if (s_wait_resp && s_wait_cmd == cmd && param2 >= 1) {
            if (data[0] != 0) {
                g_arm_stat.ctrl_err++;
                ESP_LOGW(TAG, "write cmd=0x%02X addr=0x%04X err=%u",
                         cmd, param1, data[0]);
            }
            for (int i = 0; i < WRITE_CB_MAX; i++) {
                if (s_write_cbs[i]) {
                    s_write_cbs[i](s_wait_cmd, s_wait_param1, s_wait_param2,
                                   data[0]);
                }
            }
            s_wait_resp = false;
        }
        break;
    default:
        break;
    }

    xSemaphoreGive(s_param_mutex);
}

static bool arm_ota_active(void)
{
    return arm_ota_get_state() != ARM_OTA_IDLE || arm_ota_is_streaming_mode();
}

static void uart_rx_task(void *arg)
{
    uint8_t buf[128];
    (void)arg;
    for (;;) {
        int n = uart_read_bytes(UART_PORT_NUM, buf, sizeof(buf), pdMS_TO_TICKS(100));
        if (n > 0) {
            uint8_t raw[CS_FRAME_MAX];
            int i;
            ESP_LOGI(TAG, "RX %d bytes%s", n, UART_INVERT_RX ? " (inv)" : "");
            ESP_LOG_BUFFER_HEX(TAG, buf, MIN(n, 16));
            for (i = 0; i < n; i++) {
                /* ARM OTA 激活时 (IAP 模式): 帧字节交给 OTA 解析器, 不干扰 CS PPP */
                if (arm_ota_active()) {
                    uart_ota_rx_byte(buf[i]);
                    continue;
                }
                s_rx_active = true;
                s_rx_last_byte_ms = now_ms();
                if (cs_ppp_rx_byte(&s_ppp_rx, buf[i])) {
                    /* 一帧完成: 拷贝后处理, 状态机继续接收下一帧 */
                    memcpy(raw, s_ppp_rx.buf, s_ppp_rx.length);
                    handle_frame(raw, s_ppp_rx.length);
                    cs_ppp_rx_init(&s_ppp_rx);
                    s_rx_active = false;
                }
            }
        }
    }
}

/* ---------------- 调度 (主循环每 100ms 调用) ---------------- */

void arm_poll_init(void)
{
    s_param_mutex = xSemaphoreCreateMutex();
    cs_ppp_rx_init(&s_ppp_rx);
    uart_ota_set_callback(arm_ota_handle_uart);
    uart_ota_rx_reset();
    memset(&g_run_param, 0, sizeof(g_run_param));
    memset(&g_only_read, 0, sizeof(g_only_read));
    memset(&g_read_write, 0, sizeof(g_read_write));
    memset(&g_arm_stat, 0, sizeof(g_arm_stat));
    s_info_requested = true;
    s_info_done = false;
    s_wait_resp = false;
    s_last_poll_ms = 0;
    s_last_utc_ms = 0;
    s_rx_active = false;
    s_write_pending.active = false;
    memset(s_write_cbs, 0, sizeof(s_write_cbs));

    uart_config_t cfg = {
        .baud_rate = UART_BAUDRATE,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_driver_install(UART_PORT_NUM, 512, 512, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(UART_PORT_NUM, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(UART_PORT_NUM, PIN_UART_TX, PIN_UART_RX, -1, -1));
    /* 硬件极性反转: 抵消逆变器反相电路 (空闲/起始/停止位一并反转) */
    ESP_ERROR_CHECK(uart_set_line_inverse(UART_PORT_NUM,
                        (UART_INVERT_TX ? UART_SIGNAL_TXD_INV : 0) |
                        (UART_INVERT_RX ? UART_SIGNAL_RXD_INV : 0)));

    xTaskCreate(uart_rx_task, "arm_uart_rx", 4096, NULL, 10, NULL);
    ESP_LOGI(TAG, "init done (baud=%d tx=%d rx=%d inv_tx=%d inv_rx=%d)",
             UART_BAUDRATE, PIN_UART_TX, PIN_UART_RX,
             UART_INVERT_TX, UART_INVERT_RX);
}

void arm_poll_request_info(void)
{
    s_info_requested = true;
}

int arm_poll_send_write(uint8_t cmd, uint16_t param1, uint16_t param2,
                        const uint8_t *data)
{
    if (s_write_pending.active) {
        return -1;    /* 忙 */
    }
    s_write_pending.active = true;
    s_write_pending.cmd = cmd;
    s_write_pending.param1 = param1;
    s_write_pending.param2 = param2;
    s_write_pending.data_len = (param2 > WRITE_DATA_MAX) ? WRITE_DATA_MAX : param2;
    memcpy(s_write_pending.data, data, s_write_pending.data_len);
    return 0;
}

bool arm_poll_write_pending(void)
{
    return s_write_pending.active || s_wait_resp;
}

void arm_poll_mark_utc_done(void)
{
    s_last_utc_ms = s_now_ms;
}

uint32_t arm_poll_last_utc_ms(void)
{
    return s_last_utc_ms;
}

bool arm_poll_take_param(void)
{
    return xSemaphoreTake(s_param_mutex, pdMS_TO_TICKS(50)) == pdTRUE;
}

void arm_poll_give_param(void)
{
    xSemaphoreGive(s_param_mutex);
}

void arm_poll_set_write_cb(arm_write_result_cb_t cb)
{
    if (!cb) {
        return;
    }
    /* 去重后填入空槽位 (最多 WRITE_CB_MAX 路, 供 sntp_sync/cmd_handler 并存) */
    for (int i = 0; i < WRITE_CB_MAX; i++) {
        if (s_write_cbs[i] == cb) {
            return;
        }
    }
    for (int i = 0; i < WRITE_CB_MAX; i++) {
        if (s_write_cbs[i] == NULL) {
            s_write_cbs[i] = cb;
            return;
        }
    }
    ESP_LOGW(TAG, "write cb slots full (%d)", WRITE_CB_MAX);
}

void arm_poll_tick(void)
{
    s_now_ms = now_ms();

    /* 1. 接收残帧超时重置 (与 ARM 端 1s 超时一致) */
    if (s_rx_active && (s_now_ms - s_rx_last_byte_ms) > ARM_RX_GAP_TIMEOUT_MS) {
        cs_ppp_rx_init(&s_ppp_rx);
        s_rx_active = false;
    }

    /* 2. 写命令应答超时 */
    if (s_wait_resp && (s_now_ms - s_wait_start_ms) > ARM_RESP_TIMEOUT_MS) {
        s_wait_resp = false;
        g_arm_stat.err_count++;
        ESP_LOGW(TAG, "resp timeout cmd=0x%02X", s_wait_cmd);
        for (int i = 0; i < WRITE_CB_MAX; i++) {
            if (s_write_cbs[i]) {
                s_write_cbs[i](s_wait_cmd, s_wait_param1, s_wait_param2, -1);
            }
        }
    }

    /* 3. 断线判定 (120s 无有效应答) */
    if (g_arm_stat.connected &&
        (s_now_ms - g_arm_stat.last_rx_ms) > ARM_DISCONNECT_MS) {
        g_arm_stat.connected = false;
        ESP_LOGW(TAG, "ARM disconnected (120s no response)");
    }

    /* 3.5 ARM OTA 进行中: 暂停 CS 轮询/写命令, 避免干扰 IAP 帧 */
    if (arm_ota_active()) {
        return;
    }

    /* 4. 空闲且有写命令 → 立即发送 */
    if (!s_wait_resp && s_write_pending.active) {
        send_frame_cmd(s_write_pending.cmd, s_write_pending.param1,
                       s_write_pending.param2, s_write_pending.data);
        s_write_pending.active = false;
        return;
    }

    /* 5. 周期性任务 */
    if ((s_now_ms - s_last_poll_ms) < ARM_POLL_INTERVAL_MS) {
        return;
    }
    if (s_wait_resp) {
        return;    /* 等上一命令应答 */
    }

    if (!s_info_done || s_info_requested) {
        s_info_requested = false;
        send_frame_cmd(CS_CMD_READ_R_DATA, 0x0190, ONLYREAD_DATA_SIZE, NULL);
    } else {
        send_frame_cmd(CS_CMD_READ_RUNPARAM, RUN_PARAM_START_INDEX,
                       RUN_PARAM_SIZE, NULL);
    }
    s_last_poll_ms = s_now_ms;
}
