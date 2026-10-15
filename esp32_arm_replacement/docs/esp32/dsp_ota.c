/*=============================================================================
 * dsp_ota.c -- ESP32 side of the F280039C IAP frame protocol
 *
 * Frame formats (authoritative; taken from host/dsp_iap_core.py, which drove
 * the proven F28335 production path and is unchanged for the F280039C):
 *
 *   host -> DSP : 01 05 42 00 00 00 00 73 9A                  (9 bytes)
 *   DSP  -> host: 48 52                                       "HR"  ready
 *
 *   host -> DSP : 01 10 04 38 00 02 04 00 00 00 00 C3 DD      (13 bytes) erase App area
 *   DSP  -> host: 01 10 04 38 00 02 04 00 00 00 11 03 D1      (13 bytes) accepted
 *   DSP  -> host: 43 52                                       "CR"  erase finished
 *
 *   host -> DSP : 02 AddrH AddrL <1024 bytes> CrcH CrcL       data packet
 *                 Addr = (0x01FE + 0x00FF * index) & 0xFFFF
 *                 Crc  = CRC16-Modbus (poly 0xA001, init 0xFFFF) over head+addr+data
 *   DSP  -> host: 43 52                                       "CR"  packet stored
 *
 *   host -> DSP : 02 00 FF 45 4F 54                           (6 bytes) EOT, no CRC
 *   (no answer: the DSP writes the remaining blocks, then jumps to the App)
 *
 * Timeouts/retries mirror the proven PC driver so the DSP's own window logic
 * behaves the same way.
 *===========================================================================*/
#include "dsp_ota.h"

#include <string.h>

#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "dsp_ota";

/* ---- protocol constants ------------------------------------------------- */
static const uint8_t CMD_UPGRADE[9] = {0x01, 0x05, 0x42, 0x00, 0x00, 0x00, 0x00, 0x73, 0x9A};
static const uint8_t CMD_ERASE[13] = {0x01, 0x10, 0x04, 0x38, 0x00, 0x02, 0x04,
                                      0x00, 0x00, 0x00, 0x00, 0xC3, 0xDD};
static const uint8_t ERASE_ACK[13] = {0x01, 0x10, 0x04, 0x38, 0x00, 0x02, 0x04,
                                      0x00, 0x00, 0x00, 0x11, 0x03, 0xD1};
static const uint8_t EOT[6] = {0x02, 0x00, 0xFF, 0x45, 0x4F, 0x54};
static const uint8_t TOK_HR[2] = {0x48, 0x52};   /* "HR" */
static const uint8_t TOK_CR[2] = {0x43, 0x52};   /* "CR" */

#define PKT_DATA_BYTES   1024        /* payload per packet */
#define ADDR_BASE        0x01FEu
#define ADDR_STEP        0x00FFu
#define ADDR_EOT         0x00FFu

#define T_HR_MS          2000
#define T_HR_RETRY       5
#define T_ERASE_ACK_MS   3000
#define T_ERASE_ACK_RETRY 5
#define T_CR_MS          12000       /* the erase itself takes seconds */
#define T_PKT_MS         3000
#define T_PKT_RETRY      10
#define T_EOT_DRAIN_MS   1000

static dsp_ota_progress_fn s_progress;

void dsp_ota_set_progress(dsp_ota_progress_fn cb) { s_progress = cb; }

/* ---- logging ------------------------------------------------------------ */
#include <stdarg.h>
static void logf_(dsp_ota_log_fn log, const char *fmt, ...)
{
    char buf[192];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (log) {
        log("%s", buf);
    } else {
        ESP_LOGI(TAG, "%s", buf);
    }
}

/* ---- CRC16-Modbus (poly 0xA001, init 0xFFFF) ---------------------------- */
static uint16_t crc16_modbus(const uint8_t *d, size_t n)
{
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < n; i++) {
        crc ^= d[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc & 1) ? (uint16_t)((crc >> 1) ^ 0xA001) : (uint16_t)(crc >> 1);
        }
    }
    return crc;
}

/* ---- raw UART helpers --------------------------------------------------- */
static void rx_flush(int uart_num)
{
    uart_flush_input(uart_num);
}

/* Read exactly n bytes within timeout_ms.  Returns the number read. */
static size_t read_exact(int uart_num, uint8_t *dst, size_t n, int timeout_ms)
{
    size_t got = 0;
    int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    while (got < n) {
        int64_t left_us = deadline - esp_timer_get_time();
        if (left_us <= 0) break;
        int chunk = uart_read_bytes(uart_num, dst + got, n - got,
                                    (TickType_t)(left_us / 1000 / portTICK_PERIOD_MS + 1));
        if (chunk > 0) got += (size_t)chunk;
    }
    return got;
}

/* Wait for a 2-byte token, discarding anything else in front of it. */
static bool wait_token(int uart_num, const uint8_t tok[2], int timeout_ms)
{
    int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    uint8_t prev_valid = 0, prev = 0;

    while (esp_timer_get_time() < deadline) {
        uint8_t c;
        int64_t left_us = deadline - esp_timer_get_time();
        if (left_us <= 0) break;
        if (uart_read_bytes(uart_num, &c, 1,
                            (TickType_t)(left_us / 1000 / portTICK_PERIOD_MS + 1)) != 1) {
            continue;
        }
        if (prev_valid && prev == tok[0] && c == tok[1]) return true;
        prev = c;
        prev_valid = 1;
    }
    return false;
}

/* ---- steps -------------------------------------------------------------- */
bool dsp_ota_probe(int uart_num, int timeout_ms, int retries, dsp_ota_log_fn log)
{
    if (timeout_ms <= 0) timeout_ms = T_HR_MS;
    if (retries <= 0) retries = T_HR_RETRY;

    for (int i = 1; i <= retries; i++) {
        rx_flush(uart_num);
        uart_write_bytes(uart_num, (const char *)CMD_UPGRADE, sizeof(CMD_UPGRADE));
        uart_wait_tx_done(uart_num, pdMS_TO_TICKS(200));
        if (wait_token(uart_num, TOK_HR, timeout_ms)) {
            logf_(log, "got \"HR\" on attempt %d", i);
            return true;
        }
        logf_(log, "attempt %d: no \"HR\"", i);
    }
    return false;
}

static dsp_ota_result_t do_erase(int uart_num, dsp_ota_log_fn log)
{
    for (int i = 1; i <= T_ERASE_ACK_RETRY; i++) {
        uint8_t ack[sizeof(ERASE_ACK)];
        rx_flush(uart_num);
        uart_write_bytes(uart_num, (const char *)CMD_ERASE, sizeof(CMD_ERASE));
        uart_wait_tx_done(uart_num, pdMS_TO_TICKS(200));

        if (read_exact(uart_num, ack, sizeof(ack), T_ERASE_ACK_MS) != sizeof(ack) ||
            memcmp(ack, ERASE_ACK, sizeof(ack)) != 0) {
            logf_(log, "erase ack attempt %d failed", i);
            continue;
        }
        logf_(log, "erase accepted, waiting for \"CR\" (sectors 2..47 are being erased)");

        if (wait_token(uart_num, TOK_CR, T_CR_MS)) {
            logf_(log, "erase finished (\"CR\")");
            return DSP_OTA_OK;
        }
        logf_(log, "attempt %d: no \"CR\" after erase", i);
    }
    return DSP_OTA_ERR_ERASE;
}

static dsp_ota_result_t send_packets(int uart_num, const uint8_t *bin, size_t len,
                                     dsp_ota_log_fn log)
{
    size_t off = 0;
    uint32_t index = 0;

    while (off < len) {
        size_t n = len - off;
        if (n > PKT_DATA_BYTES) n = PKT_DATA_BYTES;

        uint16_t addr = (uint16_t)((ADDR_BASE + ADDR_STEP * (index & 0xFFFFu)) & 0xFFFFu);
        if (addr == ADDR_EOT) {
            /* 0x00FF is reserved for the EOT frame; the DSP's decoder never
             * produces it for index < 256, but guard anyway. */
            logf_(log, "packet index %u maps to the reserved EOT address -> abort", index);
            return DSP_OTA_ERR_ARG;
        }

        uint8_t frame[3 + PKT_DATA_BYTES + 2];
        frame[0] = 0x02;
        frame[1] = (uint8_t)(addr >> 8);
        frame[2] = (uint8_t)(addr & 0xFF);
        memcpy(&frame[3], bin + off, n);
        uint16_t crc = crc16_modbus(frame, 3 + n);
        frame[3 + n] = (uint8_t)(crc >> 8);
        frame[4 + n] = (uint8_t)(crc & 0xFF);

        bool acked = false;
        for (int r = 1; r <= T_PKT_RETRY; r++) {
            rx_flush(uart_num);
            uart_write_bytes(uart_num, (const char *)frame, 3 + n + 2);
            uart_wait_tx_done(uart_num, pdMS_TO_TICKS(200));
            if (wait_token(uart_num, TOK_CR, T_PKT_MS)) { acked = true; break; }
            logf_(log, "packet %u retry %d", (unsigned)index, r);
        }
        if (!acked) {
            logf_(log, "packet %u never acknowledged", (unsigned)index);
            return DSP_OTA_ERR_PACKET;
        }

        off += n;
        index++;
        if (s_progress) s_progress(off, len);
        if ((index % 16) == 0) {
            logf_(log, "  %u/%u bytes", (unsigned)off, (unsigned)len);
        }
    }
    logf_(log, "all %u packet(s) acknowledged (%u bytes)", (unsigned)index, (unsigned)off);
    return DSP_OTA_OK;
}

/* ---- public entry point ------------------------------------------------- */
dsp_ota_result_t dsp_ota_flash(int uart_num, const uint8_t *bin, size_t len,
                               dsp_ota_log_fn log)
{
    if (!bin || len < 64 || (len % 2) != 0) {
        logf_(log, "bad image (len=%u)", (unsigned)len);
        return DSP_OTA_ERR_ARG;
    }

    /* A quick sanity check on the packed image: the first word must be the LB
     * opcode (0x0048 | bits) and the word after the entry is the header's magic
     * high half.  Refusing early beats erasing the App and then failing. */
    uint16_t w0 = (uint16_t)((bin[0] << 8) | bin[1]);
    if ((w0 & 0xFFFCu) != 0x0048u) {
        logf_(log, "first word 0x%04X is not an F28003x LB opcode -- wrong image?", w0);
        return DSP_OTA_ERR_ARG;
    }
    if (len < 20 + 2 || bin[4] != 0x5A || bin[5] != 0x5A) {
        logf_(log, "image header magic missing -- run host/pack_f28003x.py first");
        return DSP_OTA_ERR_ARG;
    }

    logf_(log, "handshake: sending CMD_UPGRADE, waiting for \"HR\"");
    if (!dsp_ota_probe(uart_num, T_HR_MS, T_HR_RETRY, log)) {
        return DSP_OTA_ERR_NO_HR;
    }

    logf_(log, "erase: the DSP will wipe App sectors 2..47 (bootloader is safe)");
    dsp_ota_result_t r = do_erase(uart_num, log);
    if (r != DSP_OTA_OK) return r;

    logf_(log, "data: %u bytes in %u packet(s)", (unsigned)len,
          (unsigned)((len + PKT_DATA_BYTES - 1) / PKT_DATA_BYTES));
    r = send_packets(uart_num, bin, len, log);
    if (r != DSP_OTA_OK) return r;

    logf_(log, "EOT: telling the DSP to finish and jump");
    uart_write_bytes(uart_num, (const char *)EOT, sizeof(EOT));
    uart_wait_tx_done(uart_num, pdMS_TO_TICKS(200));
    vTaskDelay(pdMS_TO_TICKS(T_EOT_DRAIN_MS));

    logf_(log, "done: the DSP is now running the new App");
    return DSP_OTA_OK;
}
