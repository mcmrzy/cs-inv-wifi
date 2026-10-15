/* The ESP owns the RTC after the ARM removal. SNTP updates the system clock
 * directly; there is no second controller to receive a UTC write. */
#include "sntp_sync.h"
#include "arm_poll.h"
#include "esp_log.h"
#include "esp_sntp.h"
#include <stdlib.h>
#include <time.h>

static bool s_synced;

void sntp_sync_init(void)
{
    s_synced = false;
    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "ntp.aliyun.com");
    esp_sntp_setservername(1, "pool.ntp.org");
    esp_sntp_init();
    setenv("TZ", "CST-8", 1);
    tzset();
}

bool sntp_sync_is_synced(void) { return s_synced; }

void sntp_sync_tick(void)
{
    if (!s_synced && esp_sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED) {
        s_synced = true;
        arm_poll_mark_utc_done();
        ESP_LOGI("SNTP", "ESP clock synced, epoch=%ld", (long)time(NULL));
    }
}
