/*
 * arm_poll.c — ARM 串口轮询调度
 *
 *  UART1 (9600, 8N1) 连接 ARM 采集器接口 :
 *   - 每 30s 轮询运行参数 (READ_RUNPARAM 0x03E8, 174B): 保活 + 数据保鲜
 *     (请求长度随 RUN_PARAM_SIZE 自动为 174B)
 *   - 只读信息 (READ_R_DATA 0x0190, 52B) 事件性读取: 上电/MQTT连接/query_info 时,
 *     失败按 30s 节流重试, 成功即停
 *   - MQTT 发布周期 (heartbeat 180s 等) 由 telemetry_tick 统一控制
 *   - 云端命令 (WRITE_CTRLPARAM / WRITE_R_W_DATA) 插队发送
 *   - 接收线程完成 PPP 解封装 → 双重解密 → 帧解析 → 数据分发
 */
#include <string.h>
#ifdef HOST_TEST
#include "arm_poll_platform.h"
#else
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/uart.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "config.h"
#include "mqtt_handler.h"
#include "nvs_storage.h"
#include "arm_ota.h"
#endif
#include "arm_poll.h"
#include "cs_protocol.h"

static const char *TAG = "ARMPOLL";

/* 应答超时 (ms): 9600 波特率下 174B 帧 ~180ms, ARM 处理 <100ms; 800ms 覆盖收帧+处理 */
#define ARM_RESP_TIMEOUT_MS    800
/* 接收字节间隔超时 (ms), 与 ARM 端 1s 一致, 防止残帧堆积 */
#define ARM_RX_GAP_TIMEOUT_MS  1000
/* 断线判定: CollectorConnect=1200 x 100ms = 120s */
#define ARM_DISCONNECT_MS      120000
/* 轮询周期 (0x03 运行参数 / 0x06 只读信息 各自 30s 一次) */
#define ARM_POLL_INTERVAL_MS   30000

#define WRITE_DATA_MAX         176

/* ---- 全局共享参数 (rx 任务写入, 主循环读取, mutex 保护) ---- */
RunParamDef g_run_param;
arm_bms_summary_t g_bms_summary;
OnlyReadDataDef g_only_read;
ReadWiteDataDef g_read_write;
uint16_t g_ctrl_param[CTRL_PARAM_COUNT];
arm_link_stat_t g_arm_stat;

static SemaphoreHandle_t s_param_mutex;
static cs_ppp_rx_t s_ppp_rx;
static bool s_rx_active;
static uint32_t s_rx_last_byte_ms;

/* ARM 只读信息 SN 与 NVS 不一致 → 置位, 主循环检测后重启生效 */
static volatile bool s_sn_changed;

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
static bool     s_info_requested;      /* 置位后立即读 info (上电/连接/query/0x06失败重试) */
static bool     s_wait_resp;           /* 有命令等待应答 */
static uint8_t  s_wait_cmd;
static uint16_t s_wait_param1;
static uint16_t s_wait_param2;
static uint32_t s_wait_start_ms;
static uint32_t s_last_info_req_ms;     /* 0x06 只读信息上次发出时刻 */
static uint32_t s_last_run_req_ms;      /* 0x03 运行参数上次发出时刻 */
static uint32_t s_last_bms_req_ms;
static uint32_t s_last_utc_ms;         /* 最近成功校时时刻 */
static uint32_t s_now_ms;

/* config 读取阶段: 0=idle, 1=待发 0x04 读 CtrlParamAlterTime, 2=待发 0x01 读控制参数 */
static uint8_t s_cfg_phase;

/* 写命令结果回调 (多路: sntp_sync 校时 + cmd_handler 命令闭环, 各自按 cmd/addr 过滤) */
#define WRITE_CB_MAX  2
static arm_write_result_cb_t s_write_cbs[WRITE_CB_MAX];
static arm_ctrl_snapshot_cb_t s_ctrl_snapshot_cb;

/* 连续超时计数 (用于节流 timeout 日志) */
static uint32_t s_timeout_count;

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
    /* ARM 已连接时每帧打印 TX; 断连后降频 (每 10 次打一次, 与 timeout 节流同步) */
    if (g_arm_stat.connected || s_timeout_count <= 1 || s_timeout_count % 10 == 0) {
        ESP_LOGI(TAG, "TX cmd=0x%02X p1=0x%04X p2=%u len=%u%s", cmd, param1,
                 param2, n, UART_INVERT_TX ? " (inv)" : "");
    }
    ESP_LOG_BUFFER_HEX_LEVEL(TAG, out, MIN(n, 16), ESP_LOG_DEBUG);
    uart_write_bytes(UART_PORT_NUM, out, n);
    s_wait_resp = true;
    s_wait_cmd = cmd;
    s_wait_param1 = param1;
    s_wait_param2 = param2;
    s_wait_start_ms = s_now_ms;
}

/* ---------------- 接收分发 (UART RX 任务) ---------------- */

/* ---------------- 调试打印: 0x06/0x03 应答字段 (按地址顺序, 输出到 USB 虚拟串口) ----
 * ⚠ 本表仅供本地串口调试时人读 (ESP_LOGD 级), **不在上报数据路径上**:
 * heartbeat 已改为按 pack(1) 字序原样转发 87 个 u16 (见 telemetry.c
 * publish_heartbeat), ESP 零解析。
 * 字段语义/单位/界限的**权威在服务端** device-communication/internal/telemetry/
 * heartbeat_v3.go (与 arm_param.h 布局同源; 单位以 ARM UsartDsp.c 的 //单位X
 * 注释为准)。若本表与服务端不一致, 以服务端为准 —— 切勿照本表反向改服务端解析。*/
typedef struct {
    const char *name;
    uint16_t    off;    /* 结构体内字节偏移 */
    uint8_t     sz;     /* 字段宽度: 2/4/8 */
} arm_dbg_field_t;

/* 只读信息 0x0190-0x01A9 (OnlyReadDataDef, 52B; SN 为 4×u32) */
static const arm_dbg_field_t s_onlyread_fields[] = {
    { "ARMFirmwareVersion",   0,  2 }, { "DSPFirmwareVersion",  2,  2 },
    { "InverterModuleL",      4,  2 }, { "InverterModuleH",     6,  2 },
    { "HardwareVersion",      8,  2 },
    { "Inverter_SN1",        10,  4 }, { "Inverter_SN2",       14,  4 },
    { "Inverter_SN3",        18,  4 }, { "Inverter_SN4",       22,  4 },
    { "DeviceTypeCodeL",     26,  2 }, { "DeviceTypeCodeH",    28,  2 },
    { "RateWatt",            30,  2 }, { "RateVA",             32,  2 },
    { "NomGridVolt",         34,  2 }, { "NomGridFreq",        36,  2 },
    { "NomBatVolt",          38,  2 }, { "NomPvCurr",          40,  2 },
    { "NomAcChgCurr",        42,  2 }, { "NomOpVolt",          44,  2 },
    { "NomOpFreq",           46,  2 }, { "NomOpPow",           48,  2 },
    { "BLVersion",           50,  2 },
};

/* 运行参数 0x03E8 起 (RunParamDef, 174B App(10) 直传布局, 全 u16/s16 + 4 处 u32/u64) */
static const arm_dbg_field_t s_runparam_fields[] = {
    { "SysStatus",          0,  2 }, { "Vpv1",               2,  2 },
    { "Vpv2",               4,  2 }, { "Ppv1",               6,  2 },
    { "Ppv2",               8,  2 }, { "Ppv",               10,  2 },
    { "Buck1Curr",         12,  2 }, { "Buck2Curr",         14,  2 },
    { "BoostTemp",         16,  2 }, { "InvertTemp",        18,  2 },
    { "OutputWatt",        20,  2 }, { "OutputVA",          22,  2 },
    { "OutputCurr",        24,  2 }, { "ACChrWatt",         26,  2 },
    { "ACChrVA",           28,  2 }, { "GridVolt",          30,  2 },
    { "GridFreq",          32,  2 }, { "ACOutputVolt",      34,  2 },
    { "ACOutputFreq",      36,  2 }, { "ACInWatt",          38,  2 },
    { "ACInVA",            40,  2 }, { "ACDisChrWatt",      42,  2 },
    { "ACDisChrVA",        44,  2 }, { "ACChrCurr",         46,  2 },
    { "BatVolt",           48,  2 }, { "BatterySOC",        50,  2 },
    { "BatDisChrWatt",     52,  2 }, { "BatChrWatt",        54,  2 },
    { "BatChgCurr",        56,  2 }, { "BatDischgCurr",     58,  2 },
    { "BatOverCharge",     60,  2 }, { "BusVolt",           62,  2 },
    { "Ibus",              64,  2 }, { "PvTemp",            66,  2 },
    { "InvCurr",           68,  2 }, { "TransformerTemp",   70,  2 },
    { "LoadPercent",       72,  2 }, { "GridPercent",       74,  2 },
    { "ParaChgCurr",       76,  2 }, { "WorkTimeTotal",     78,  4 },
    { "MpptFanSpeed",      82,  2 }, { "InvFanSpeed",       84,  2 },
    { "PairedSocket",      86,  2 }, { "OnlineSocket",      88,  2 },
    { "OnSocket",          90,  2 }, { "Warning",           92,  8 },
    { "BmsWarning",       100,  2 }, { "FaultValue",       102,  4 },
    { "EGen_today",       106,  4 }, { "EGen_total",       110,  4 },
    { "Epv_today",        114,  4 }, { "Epv_total",        118,  4 },
    { "Eac_chrToday",     122,  4 }, { "Eac_chrTotal",     126,  4 },
    { "Ebat_dischrToday", 130,  4 }, { "Ebat_dischrTotal", 134,  4 },
    { "Ebat_chrToday",    138,  4 }, { "Ebat_chrTotal",    142,  4 },
    { "Eac_dischrToday",  146,  4 }, { "Eac_dischrTotal",  150,  4 },
    { "Eop_dischrToday",  154,  4 }, { "Eop_dischrTotal",  158,  4 },
    { "VinvtA",           162,  2 }, { "IinvtA",           164,  2 },
    { "InvtFreq",         166,  2 }, { "Pinvt",            168,  2 },
    { "Sinvt",            170,  2 }, { "InvtPercent",      172,  2 },
};

/* 按地址顺序打印字段表 (调试用) */
static void dump_fields(const char *title, uint16_t start_addr,
                        const uint8_t *base, const arm_dbg_field_t *tbl,
                        int count)
{
    ESP_LOGD(TAG, "== %s (%d fields) ==", title, count);
    for (int i = 0; i < count; i++) {
        uint64_t v = 0;
        memcpy(&v, base + tbl[i].off, tbl[i].sz);
        ESP_LOGD(TAG, "0x%04X  %-24s = 0x%llX (%llu)",
                 start_addr + tbl[i].off, tbl[i].name,
                 (unsigned long long)v, (unsigned long long)v);
    }
}

/* Inverter_SN1..4 (4×u32, 偏移 10~25) = 16 字节 ASCII (小端自然字节序, 与 ARM
   端 pOnlyReadData->Inverter_SN1 = ch0 | ch1<<8 | ch2<<16 | ch3<<24 一致);
   跳过 0x00 填充; 含不可打印字节/全 0x00 → 非法返回 false */
bool onlyread_sn_to_str(const OnlyReadDataDef *o, char *out, size_t len)
{
    const uint8_t *b;
    char *p = out;

    if (!o || !out || len < 17) {
        return false;
    }
    b = (const uint8_t *)&o->Inverter_SN1;
    for (int i = 0; i < 16; i++) {
        uint8_t c = b[i];
        if (c == 0x00) {
            continue;
        }
        if (c < 0x20 || c > 0x7E) {
            return false;   /* 非法字符 */
        }
        *p++ = (char)c;
    }
    if (p == out) {
        return false;       /* 全 0x00 */
    }
    *p = '\0';
    return true;
}

/* 0x06 只读信息应答携带的真实 SN (Inverter_SN1..4, 16B ASCII): 与 NVS 不一致时
   更新并置重启标志; 一致时零写入 (保护 NVS 擦写寿命) */
static void sync_sn_from_onlyread(void)
{
    char arm_sn[32];
    if (!onlyread_sn_to_str(&g_only_read, arm_sn, sizeof(arm_sn))) {
        return;
    }
    if (strcmp(arm_sn, g_device_sn) != 0) {
        char old_sn[SN_MAX_LEN];
        strlcpy(old_sn, g_device_sn, sizeof(old_sn));
        /* 换 SN 前清掉旧 SN 的 retained status: 否则旧主题会作为"幽灵设备"
         * 永久留在 broker/设备列表里 (见 mqtt_clear_retained_status 注释)。 */
        mqtt_clear_retained_status(old_sn);
        if (!storage_set_sn(arm_sn)) {
            /* NVS 写失败(分区满/碎片)时绝不能置重启标志: 否则每次开机都同步一次
             * 又一次重启, 设备永远起不来, 现场表现得像"配网/配置全丢"。 */
            ESP_LOGE(TAG, "SN from ARM: %s but NVS save FAILED; keeping %s",
                     arm_sn, old_sn);
            return;
        }
        strlcpy(g_device_sn, arm_sn, sizeof(g_device_sn));
        s_sn_changed = true;
        ESP_LOGW(TAG, "SN from ARM: %s (was %s), restart to apply",
                 arm_sn, old_sn);
    }
}

static void handle_frame(uint8_t *raw, size_t len)
{
    uint8_t cmd;
    uint16_t param1, param2;
    const uint8_t *data;
    uint64_t received_ms = (uint64_t)(esp_timer_get_time() / 1000);
    uint32_t now = (uint32_t)received_ms;

    if (cs_decrypt_rx(raw, len) != 0) {
        g_arm_stat.err_count++;
        ESP_LOGW(TAG, "rx decrypt fail len=%u, raw:", len);
        ESP_LOG_BUFFER_HEX(TAG, raw, len);
        return;
    }
    if (cs_frame_parse(raw, len, &cmd, &param1, &param2, &data) != 0 ||
        (cmd == CS_CMD_READ_BATTSUM &&
         cs_frame_parse_bms_summary(raw, len, &data) != 0)) {
        g_arm_stat.err_count++;
        ESP_LOGW(TAG, "rx parse fail len=%u, decrypted:", len);
        ESP_LOG_BUFFER_HEX(TAG, raw, len);
        return;
    }

    g_arm_stat.last_rx_ms = now;
    g_arm_stat.connected = true;
    g_arm_stat.err_count = 0;
    s_timeout_count = 0;  /* 收到有效应答, 重置超时计数 */

    if (xSemaphoreTake(s_param_mutex, pdMS_TO_TICKS(50)) != pdTRUE) {
        return;
    }

    switch (cmd) {
    case CS_CMD_READ_RUNPARAM:            /* 0x03 运行参数应答 */
        if (param2 == RUN_PARAM_SIZE && len >= 8 + RUN_PARAM_SIZE) {
            memcpy(&g_run_param, data, RUN_PARAM_SIZE);
            g_arm_stat.last_run_ms = received_ms;
            dump_fields("RUNPARAM", RUN_PARAM_START_INDEX,
                        (const uint8_t *)&g_run_param, s_runparam_fields,
                        (int)(sizeof(s_runparam_fields) / sizeof(s_runparam_fields[0])));
            g_arm_stat.poll_count++;
            /* 心跳/遥测由 telemetry_tick 按 180s 周期发布 (数据保持 30s 内新鲜) */
            if (s_wait_resp && s_wait_cmd == cmd) {
                s_wait_resp = false;
            }
        } else {
            ESP_LOGW(TAG, "RUNPARAM rejected: param2=%u(expected %u) len=%u(need %u)",
                     param2, (unsigned)RUN_PARAM_SIZE, (unsigned)len,
                     (unsigned)(8 + RUN_PARAM_SIZE));
        }
        break;
    case CS_CMD_READ_BATTSUM:
        memcpy(g_bms_summary.bytes, data, CS_BATTSUM_DATA_SIZE);
        g_bms_summary.received_ms = received_ms;
        g_bms_summary.acquired = true;
        if (s_wait_resp && s_wait_cmd == cmd) {
            s_wait_resp = false;
        }
        break;
    case CS_CMD_READ_R_DATA:              /* 0x06 只读信息应答 */
        if (param2 == ONLYREAD_DATA_SIZE && len >= 8 + ONLYREAD_DATA_SIZE) {
            memcpy(&g_only_read, data, ONLYREAD_DATA_SIZE);
            dump_fields("ONLYREAD", ONLYREAD_START_INDEX,
                        (const uint8_t *)&g_only_read, s_onlyread_fields,
                        (int)(sizeof(s_onlyread_fields) / sizeof(s_onlyread_fields[0])));
            sync_sn_from_onlyread();
            g_arm_stat.last_info_ms = now;
            if (s_wait_resp && s_wait_cmd == cmd) {
                s_wait_resp = false;
            }
        }
        break;
    case CS_CMD_READ_CTRLPARAM:           /* 0x01 控制参数应答 (84B / 42 个 u16) */
        if (param2 == CTRL_PARAM_SIZE && len >= 8 + CTRL_PARAM_SIZE) {
            memcpy(g_ctrl_param, data, CTRL_PARAM_SIZE);
            g_arm_stat.last_ctrl_ms = now;
            if (s_ctrl_snapshot_cb) {
                s_ctrl_snapshot_cb(g_ctrl_param, CTRL_PARAM_COUNT,
                                   g_read_write.CtrlParamAlterTime);
            }
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
            ESP_LOG_BUFFER_HEX_LEVEL(TAG, buf, n, ESP_LOG_DEBUG);
            for (i = 0; i < n; i++) {
                /* ARM OTA 激活时 (IAP 模式): 帧字节交给 OTA 解析器, 不干扰 CS PPP */
                if (arm_ota_active()) {
                    arm_ota_rx_byte(buf[i]);
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

bool arm_poll_sn_changed(void)
{
    return s_sn_changed;
}

void arm_poll_init(void)
{
    s_param_mutex = xSemaphoreCreateMutex();
    cs_ppp_rx_init(&s_ppp_rx);
    arm_ota_rx_reset();
    memset(&g_run_param, 0, sizeof(g_run_param));
    memset(&g_bms_summary, 0, sizeof(g_bms_summary));
    memset(&g_only_read, 0, sizeof(g_only_read));
    memset(&g_read_write, 0, sizeof(g_read_write));
    memset(&g_arm_stat, 0, sizeof(g_arm_stat));
    s_info_requested = true;
    s_cfg_phase = 0;
    s_wait_resp = false;
    s_last_info_req_ms = 0;
    s_last_run_req_ms = 0;
    s_last_bms_req_ms = 0;
    s_last_utc_ms = 0;
    s_rx_active = false;
    s_write_pending.active = false;
    s_timeout_count = 0;
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

void arm_poll_request_config(void)
{
    /* 空闲才接受请求 (避免多路触发重复); 完成后自动归零 */
    if (s_cfg_phase == 0) {
        s_cfg_phase = 1;
    }
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

void arm_poll_set_ctrl_snapshot_cb(arm_ctrl_snapshot_cb_t cb)
{
    s_ctrl_snapshot_cb = cb;
}

/* OTA 静默窗口: 停发周期轮询(0x03), 但保留残帧超时/应答超时等状态维护,
 * 使恢复后不会因为计数或残帧而误判。仅 ESP 自升级用 (ARM/DSP/BMS 的传输
 * 通道就是这条 UART, 绝不能停)。 */
static volatile bool s_quiet = false;

void arm_poll_set_quiet(bool quiet)
{
    s_quiet = quiet;
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
        s_timeout_count++;
        if (s_wait_cmd == CS_CMD_READ_R_DATA) {
            s_info_requested = true;   /* 0x06 未获应答 → 置位, 由 30s 节流重试 */
        }
        /* 首次超时 + 每 10 次打一次 WARN, 其余静默 */
        if (s_timeout_count <= 1 || s_timeout_count % 10 == 0) {
            ESP_LOGW(TAG, "resp timeout cmd=0x%02X (#%lu)", s_wait_cmd,
                     (unsigned long)s_timeout_count);
        }
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

    /* 5. config 请求 (query_config 触发, 插队于轮询之前) */
    if (s_cfg_phase != 0) {
        ESP_LOGI(TAG, "cfg_phase=%d wait_resp=%d", s_cfg_phase, s_wait_resp);
    }
    if (!s_wait_resp && s_cfg_phase != 0) {
        if (s_cfg_phase == 1) {
            s_cfg_phase = 2;
            send_frame_cmd(CS_CMD_READ_R_W_DATA, 0x00C8, 4, NULL);
        } else if (s_cfg_phase == 2) {
            s_cfg_phase = 0;
            send_frame_cmd(CS_CMD_READ_CTRLPARAM, CTRL_PARAM_START_INDEX,
                           CTRL_PARAM_SIZE, NULL);
        }
        return;
    }

    /* 6. 周期/事件轮询: 0x03 每 30s 保活; 0x06 事件性 (请求置位, 30s 节流防失败轰炸)。
     * 静默窗口内只跳过发送, 其它状态维护(残帧/应答超时)照常。 */
    if (!s_wait_resp && !s_quiet) {
        if (s_info_requested &&
            (s_now_ms - s_last_info_req_ms) >= ARM_POLL_INTERVAL_MS) {
            s_info_requested = false;
            s_last_info_req_ms = s_now_ms;
            send_frame_cmd(CS_CMD_READ_R_DATA, 0x0190, ONLYREAD_DATA_SIZE, NULL);
        } else if ((s_now_ms - s_last_run_req_ms) >= ARM_POLL_INTERVAL_MS) {
            s_last_run_req_ms = s_now_ms;
            send_frame_cmd(CS_CMD_READ_RUNPARAM, RUN_PARAM_START_INDEX,
                           RUN_PARAM_SIZE, NULL);
        } else if ((s_now_ms - s_last_bms_req_ms) >= ARM_POLL_INTERVAL_MS) {
            s_last_bms_req_ms = s_now_ms;
            send_frame_cmd(CS_CMD_READ_BATTSUM, 0, 0, NULL);
        }
    }
}
