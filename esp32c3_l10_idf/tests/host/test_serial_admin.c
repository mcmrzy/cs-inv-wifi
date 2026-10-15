#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#if __has_include("serial_admin.h")
#include "serial_admin.h"
#include "serial_admin_platform.h"
#include "arm_poll.h"
static int failures, starts, releases;
static bool reserved, ota_busy, installed;
static arm_sn_result_t result;
static char rx[1024], requested[17], logs[8192];
static size_t rx_len, rx_offset, log_len;
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); failures++; } } while (0)
bool usb_serial_jtag_is_driver_installed(void) { return installed; }
int usb_serial_jtag_driver_install(usb_serial_jtag_driver_config_t *cfg)
{ CHECK(cfg->rx_buffer_size >= 128); installed = true; return 0; }
int usb_serial_jtag_read_bytes(void *buf, uint32_t size, unsigned timeout)
{
    CHECK(timeout == 0);
    size_t count = rx_len - rx_offset;
    if (count > size) count = size;
    memcpy(buf, rx + rx_offset, count); rx_offset += count;
    return (int)count;
}
bool ota_service_begin_config_change(void)
{ if (ota_busy || reserved) return false; reserved = true; return true; }
void ota_service_end_config_change(void) { CHECK(reserved); reserved = false; releases++; }
int arm_poll_request_sn(const char *sn)
{ CHECK(reserved); starts++; strcpy(requested, sn); result = ARM_SN_PENDING; return 0; }
arm_sn_result_t arm_poll_sn_result(void) { return result; }
void test_serial_log(const char *tag, const char *fmt, ...)
{
    (void)tag; va_list args; va_start(args, fmt);
    int n = vsnprintf(logs + log_len, sizeof(logs) - log_len, fmt, args);
    va_end(args);
    CHECK(n >= 0 && (size_t)n < sizeof(logs) - log_len);
    if (n > 0) log_len += (size_t)n;
}
static void input(const char *line)
{ strcpy(rx, line); rx_len = strlen(rx); rx_offset = 0; serial_admin_tick(); }
int main(void)
{
    serial_admin_init(); CHECK(installed);
    input("set_sn wrong ABC\r\n"); CHECK(starts == 0 && !reserved);
    ota_busy = true;
    input("set_sn test-password ABC\n"); CHECK(starts == 0 && !reserved);
    ota_busy = false;
    input("set_sn test-password ABC\r\n"); CHECK(starts == 1 && reserved);
    CHECK(strcmp(requested, "ABC") == 0);
    input("set_sn test-password SECOND\n"); CHECK(starts == 1);
    result = ARM_SN_NVS_FAILED; serial_admin_tick(); CHECK(!reserved && releases == 1);
    input("set_sn test-password ABC\n"); CHECK(starts == 2 && reserved);
    result = ARM_SN_OK; serial_admin_tick();
    /* Keep reservation through successful identity switch until reboot. */
    CHECK(reserved && releases == 1);
    CHECK(strstr(logs, "OK") != NULL && strstr(logs, "NVS_FAILED") != NULL);
    CHECK(strstr(logs, "test-password") == NULL && strstr(logs, "wrong") == NULL);
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
#else
int main(void) { fputs("FAIL: USB serial SN command integration is missing\n", stderr); return EXIT_FAILURE; }
#endif
