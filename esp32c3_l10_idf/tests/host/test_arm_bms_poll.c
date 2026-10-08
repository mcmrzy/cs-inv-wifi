#include <stdio.h>
#include <stdlib.h>

/* Exercise the real scheduler and static RX dispatcher; only the platform is stubbed. */
#include "../../main/telemetry/arm_poll.c"

static int failures;
#define CHECK(x) do { if (!(x)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); \
    failures++; \
} } while (0)

char g_device_sn[SN_MAX_LEN] = "HOST";
static uint64_t clock_ms;
static bool locked, ota_active;
static unsigned tx_count;
static uint8_t tx_frame[CS_FRAME_MAX * 2 + 4];
static size_t tx_len;

SemaphoreHandle_t xSemaphoreCreateMutex(void) { return &locked; }
BaseType_t xSemaphoreTake(SemaphoreHandle_t handle, uint32_t timeout)
{
    (void)handle;
    (void)timeout;
    CHECK(!locked);
    locked = true;
    return pdTRUE;
}
BaseType_t xSemaphoreGive(SemaphoreHandle_t handle)
{
    (void)handle;
    CHECK(locked);
    locked = false;
    return pdTRUE;
}
BaseType_t xTaskCreate(TaskFunction_t fn, const char *name, uint32_t stack,
                       void *arg, UBaseType_t priority, void *handle)
{
    (void)fn; (void)name; (void)stack; (void)arg; (void)priority; (void)handle;
    return pdTRUE;
}
int uart_driver_install(int p, int r, int t, int c, void *q, int f)
{ (void)p; (void)r; (void)t; (void)c; (void)q; (void)f; return 0; }
int uart_param_config(int p, const uart_config_t *c) { (void)p; (void)c; return 0; }
int uart_set_pin(int p, int t, int r, int rt, int ct)
{ (void)p; (void)t; (void)r; (void)rt; (void)ct; return 0; }
int uart_set_line_inverse(int p, uint32_t m) { (void)p; (void)m; return 0; }
int uart_write_bytes(int port, const void *data, size_t len)
{
    (void)port;
    CHECK(len <= sizeof(tx_frame));
    if (len <= sizeof(tx_frame)) memcpy(tx_frame, data, len);
    tx_len = len;
    tx_count++;
    return (int)len;
}
int uart_read_bytes(int p, void *d, size_t l, uint32_t t)
{ (void)p; (void)d; (void)l; (void)t; return 0; }
int64_t esp_timer_get_time(void) { return (int64_t)(clock_ms * 1000); }
uint32_t esp_random(void) { return 42; }
int arm_ota_get_state(void) { return ota_active ? 1 : ARM_OTA_IDLE; }
bool arm_ota_is_streaming_mode(void) { return false; }
void arm_ota_rx_reset(void) {}
void arm_ota_rx_byte(uint8_t b) { (void)b; }
void mqtt_clear_retained_status(const char *s) { (void)s; }
bool storage_set_sn(const char *s) { (void)s; return true; }
size_t strlcpy(char *dst, const char *src, size_t cap)
{
    size_t len = strlen(src);
    if (cap) { size_t n = len < cap ? len : cap - 1; memcpy(dst, src, n); dst[n] = 0; }
    return len;
}

static size_t unpack(const uint8_t *frame, size_t length, uint8_t *raw)
{
    cs_ppp_rx_t rx;
    cs_ppp_rx_init(&rx);
    int complete = 0;
    for (size_t i = 0; i < length; ++i) complete = cs_ppp_rx_byte(&rx, frame[i]);
    CHECK(complete);
    memcpy(raw, rx.buf, rx.length);
    return rx.length;
}

static void receive(uint8_t cmd, uint16_t p1, uint16_t p2, const uint8_t *data)
{
    uint8_t frame[CS_FRAME_MAX * 2 + 4], raw[CS_FRAME_MAX];
    size_t length = cs_frame_send(frame, cmd, p1, p2, data, 123);
    size_t raw_length = unpack(frame, length, raw);
    handle_frame(raw, raw_length);
    CHECK(!locked);
}

static void check_tx(uint8_t expected_cmd, uint16_t p1, uint16_t p2)
{
    uint8_t raw[CS_FRAME_MAX], cmd;
    uint16_t param1, param2;
    size_t len = unpack(tx_frame, tx_len, raw);
    CHECK(cs_decrypt_rx(raw, len) == 0);
    CHECK(cs_frame_parse(raw, len, &cmd, &param1, &param2, NULL) == 0);
    CHECK(cmd == expected_cmd && param1 == p1 && param2 == p2);
    if (cmd == CS_CMD_READ_BATTSUM) CHECK(len == 24 && raw[CS_OFF_CMDLEN] == 8);
}

static void receive_bad_header(const uint8_t *data)
{
    uint8_t raw[120] = {0};
    raw[CS_OFF_HEADER] = 0xab;
    raw[CS_OFF_CMDLEN] = 108;
    raw[CS_OFF_CMD] = CS_CMD_READ_BATTSUM;
    raw[CS_OFF_PARAM2] = 100;
    memcpy(raw + CS_OFF_DATA, data, 100);
    uint32_t crc = cs_crc32_words(raw, 27);
    for (int i = 0; i < 4; ++i) raw[108 + i] = (uint8_t)(crc >> (8 * i));
    uint32_t key[4] = {cs_frame_key[0], cs_frame_key[1], ~123u, 123u};
    cs_encrypt_cbc(raw, key, 112);
    memcpy(raw + 112, &key[2], 8);
    cs_encrypt_cbc(raw, cs_frame_key, 120);
    handle_frame(raw, sizeof(raw));
}

int main(void)
{
    uint8_t info[ONLYREAD_DATA_SIZE] = {0}, run[RUN_PARAM_SIZE], bytes[104];
    memset(run, 0x5a, sizeof(run));
    for (size_t i = 0; i < sizeof(bytes); ++i) bytes[i] = (uint8_t)i;
    arm_poll_init();
    CHECK(!g_bms_summary.acquired);
    clock_ms = 29999;
    arm_poll_tick();
    CHECK(tx_count == 0);
    clock_ms = 30000;
    arm_poll_tick();
    check_tx(CS_CMD_READ_R_DATA, 0x0190, ONLYREAD_DATA_SIZE);
    receive(CS_CMD_READ_R_DATA, 0x0190, sizeof(info), info);
    clock_ms++;
    arm_poll_tick();
    check_tx(CS_CMD_READ_RUNPARAM, RUN_PARAM_START_INDEX, RUN_PARAM_SIZE);
    receive(CS_CMD_READ_RUNPARAM, RUN_PARAM_START_INDEX, sizeof(run), run);
    CHECK(g_arm_stat.last_run_ms == clock_ms);
    clock_ms++;
    arm_poll_tick();
    check_tx(CS_CMD_READ_BATTSUM, 0, 0);
    receive(CS_CMD_READ_BATTSUM, 0, 100, bytes);
    CHECK(g_bms_summary.acquired && g_bms_summary.received_ms == clock_ms);
    CHECK(memcmp(g_bms_summary.bytes, bytes, 100) == 0);
    CHECK(g_arm_stat.poll_count == 1 && memcmp(&g_run_param, run, sizeof(run)) == 0);
    CHECK(g_arm_stat.last_run_ms == clock_ms - 1 && g_arm_stat.last_rx_ms == clock_ms);

    arm_bms_summary_t saved = g_bms_summary;
    clock_ms = 60001;
    arm_poll_tick();
    check_tx(CS_CMD_READ_RUNPARAM, RUN_PARAM_START_INDEX, RUN_PARAM_SIZE);
    receive(CS_CMD_READ_RUNPARAM, RUN_PARAM_START_INDEX, sizeof(run), run);
    clock_ms++;
    arm_poll_tick();
    check_tx(CS_CMD_READ_BATTSUM, 0, 0);
    receive(CS_CMD_READ_RUNPARAM, RUN_PARAM_START_INDEX, sizeof(run) - 2, run);
    CHECK(g_arm_stat.last_run_ms == clock_ms - 1);
    receive(CS_CMD_READ_BATTSUM, 1, 100, bytes);
    receive(CS_CMD_READ_BATTSUM, 0, 99, bytes);
    receive(CS_CMD_READ_BATTSUM, 0, 104, bytes);
    receive_bad_header(bytes);
    uint8_t frame[512], raw[CS_FRAME_MAX];
    size_t frame_len = cs_frame_send(frame, CS_CMD_READ_BATTSUM, 0, 100, bytes, 0);
    size_t raw_len = unpack(frame, frame_len, raw);
    handle_frame(raw, raw_len - 8);
    CHECK(memcmp(&saved, &g_bms_summary, sizeof(saved)) == 0);
    CHECK(s_wait_resp && g_arm_stat.poll_count == 2);
    CHECK(memcmp(&g_run_param, run, sizeof(run)) == 0);

    unsigned sent = tx_count;
    clock_ms += 801;
    arm_poll_tick();
    CHECK(!s_wait_resp && tx_count == sent);
    CHECK(memcmp(&saved, &g_bms_summary, sizeof(saved)) == 0);
    clock_ms = 90002;
    ota_active = true;
    arm_poll_tick();
    CHECK(tx_count == sent);
    ota_active = false;
    arm_poll_set_quiet(true);
    arm_poll_tick();
    CHECK(tx_count == sent);
    arm_poll_set_quiet(false);
    arm_poll_tick();
    receive(CS_CMD_READ_RUNPARAM, RUN_PARAM_START_INDEX, sizeof(run), run);
    clock_ms++;
    arm_poll_tick();
    check_tx(CS_CMD_READ_BATTSUM, 0, 0);
    receive(CS_CMD_READ_BATTSUM, 0, 100, bytes);
    CHECK(g_bms_summary.received_ms == clock_ms);
    uint64_t last_run_ms = g_arm_stat.last_run_ms;
    clock_ms += 240000;
    receive(CS_CMD_READ_BATTSUM, 0, 100, bytes);
    CHECK(g_arm_stat.last_rx_ms == clock_ms && g_arm_stat.last_run_ms == last_run_ms);
    CHECK(g_bms_summary.received_ms == clock_ms);
    arm_poll_init();
    CHECK(!g_bms_summary.acquired && g_bms_summary.received_ms == 0);
    CHECK(g_arm_stat.last_run_ms == 0);
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
