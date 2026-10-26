#include "direct_protocol.h"
#include <assert.h>
#include <setjmp.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

typedef uint32_t TickType_t;
#define BMS_PORT 1
#define DIRECT_IAP_BMS 3
#define CONFIG_DIRECT_BMS_7C 1
#define pdMS_TO_TICKS(ms) (ms)
#define pdTRUE 1
#define ESP_LOGW(...) ((void)0)
static atomic_uint s_bms_tick;
static atomic_bool s_quiet;
static direct_bms_measurement_t s_bms;
static uint32_t s_bms_last_ms;
static direct_bms_summary_t s_bms_summary;
static int s_lock;
static jmp_buf finished;
static unsigned tick, count, mode, owner_calls;
static unsigned timestamps[6];
static bool delivered;
static uint8_t reply[110];

static uint32_t now_ms(void) { return tick; }
static int64_t esp_timer_get_time(void) { return (int64_t)tick * 1000; }
static TickType_t xTaskGetTickCount(void) { return tick; }
static bool iap_owner_step(int target) {
    assert(target == DIRECT_IAP_BMS);
    if (mode == 2 && ++owner_calls == 2) { tick += 5000; return true; }
    return false;
}
static void vTaskDelay(TickType_t duration) {
    tick += duration;
    if (atomic_load(&s_quiet)) atomic_store(&s_quiet, false);
}
static void vTaskDelayUntil(TickType_t *previous, TickType_t period) {
    *previous += period;
    if (tick < *previous) tick = *previous;
}
static void uart_flush_input(int port) { assert(port == BMS_PORT); }
static int uart_write_bytes(int port, const void *bytes, size_t len) {
    static const uint8_t expected[] = {0x7c,0x78,0x24,0x80,1,0,0};
    assert(port == BMS_PORT && len == sizeof(expected));
    assert(memcmp(bytes, expected, len) == 0);
    timestamps[count++] = tick;
    if (count == 6) longjmp(finished, 1);
    if (mode == 3 && count == 2) atomic_store(&s_quiet, true);
    delivered = false;
    return (int)len;
}
static int uart_read_bytes(int port, void *bytes, size_t len, unsigned timeout) {
    assert(port == BMS_PORT);
    tick += timeout;
    if (mode == 4 && !delivered) tick += 1500;
    if (mode != 1 && !delivered) {
        assert(len >= sizeof(reply));
        memcpy(bytes, reply, sizeof(reply));
        if(mode==5 && count>1)((uint8_t *)bytes)[1]^=1;
        delivered = true;
        return sizeof(reply);
    }
    return 0;
}
static int xSemaphoreTake(int lock, unsigned timeout) { (void)lock;(void)timeout;return pdTRUE; }
static void xSemaphoreGive(int lock) { (void)lock; }
static void bms_identity_step(void) { }
#include "../tmp/bms_poll_under_test.inc"

int main(void) {
    /* A complete CRC-valid V1.6 reply: 16 cells, 4 temperatures, 61-byte tail. */
    reply[0]=0x7c;reply[3]=0x80;reply[4]=1;reply[5]=103;
    reply[7]=16;reply[40]=4;reply[51]=0x08;reply[52]=2;
    uint16_t crc=direct_crc16(reply+3,sizeof(reply)-3);
    reply[1]=(uint8_t)crc;reply[2]=(uint8_t)(crc>>8);
    for (mode=0;mode<6;mode++) {
        memset(&s_bms_summary,0,sizeof(s_bms_summary));
        tick=count=owner_calls=0;atomic_store(&s_quiet,false);
        if (!setjmp(finished)) bms_task(NULL);
        for (unsigned i=1;i<count;i++) {
            unsigned interval=timestamps[i]-timestamps[i-1];
            if ((mode<=1 && interval!=500) || (mode>=2 && interval<500)) {
                printf("FAIL mode=%u query=%u interval=%u ms (expected 500 ms, no catch-up bursts)\n",mode,i,interval);
                return 1;
            }
        }
        assert(s_bms_summary.acquired == (mode != 1));
        if (mode != 1) {
            assert(s_bms_summary.received_ms > 0 && s_bms_summary.received_ms <= tick);
            assert(s_bms_summary.bytes[0] == 16);
            assert(s_bms_summary.bytes[1] == 0x50 && s_bms_summary.bytes[2] == 0x14);
            if(mode==5)assert(s_bms_summary.received_ms < timestamps[1]);
        }
    }
    puts("PASS real BMS task: 500ms start-to-start with complete replies/timeouts; no catch-up after OTA/quiet/stall; exact read-only query");
    return 0;
}
