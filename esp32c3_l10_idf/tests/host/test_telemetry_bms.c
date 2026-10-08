#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "arm_poll.h"
#include "cs_protocol_defs.h"
#include "mqtt_envelope.h"
#include "telemetry.h"
#include "cJSON.h"

static int failures;
#define CHECK(x) do { if (!(x)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); \
    failures++; \
} } while (0)

RunParamDef g_run_param;
OnlyReadDataDef g_only_read;
ReadWiteDataDef g_read_write;
uint16_t g_ctrl_param[CTRL_PARAM_COUNT];
arm_link_stat_t g_arm_stat;
arm_bms_summary_t g_bms_summary;
char g_device_sn[32] = "HOST";

static uint64_t clock_ms;
static bool locked;
static unsigned heartbeat_count;
static char envelope[1200];

int64_t esp_timer_get_time(void) { return (int64_t)(clock_ms * 1000); }
uint32_t ble_ct_now(void) { return 0; }
void ble_ct_telemetry_notify(const char *json, uint16_t len)
{
    (void)json;
    (void)len;
}
bool mqtt_is_connected(void) { return true; }
int mqtt_publish(const char *topic, const char *data, int len, int qos, int retain)
{
    (void)topic;
    (void)data;
    (void)len;
    (void)qos;
    (void)retain;
    return 0;
}
int mqtt_publish_heartbeat_raw(const char *topic, const char *data, int len)
{
    char *payload = NULL;
    size_t length = 0;
    CHECK(strcmp(topic, "heartbeat") == 0);
    CHECK(len == (int)strlen(data));
    CHECK(mqtt_envelope_build(data, 1234, PROTO_HEARTBEAT_ENVELOPE_VERSION,
                              &payload, &length) == 0);
    if (payload && length < sizeof(envelope)) {
        memcpy(envelope, payload, length + 1);
        heartbeat_count++;
    } else {
        CHECK(0);
    }
    free(payload);
    return 0;
}
bool arm_poll_take_param(void)
{
    CHECK(!locked);
    locked = true;
    return true;
}
void arm_poll_give_param(void) { CHECK(locked); locked = false; }
void arm_poll_request_info(void) {}
void arm_poll_request_config(void) {}
bool onlyread_sn_to_str(const OnlyReadDataDef *o, char *out, size_t len)
{
    (void)o;
    (void)out;
    (void)len;
    return false;
}

static cJSON *request_heartbeat(void)
{
    unsigned before = heartbeat_count;
    g_arm_stat.last_rx_ms = (uint32_t)clock_ms;
    g_arm_stat.last_run_ms = clock_ms;
    telemetry_request_heartbeat();
    telemetry_tick();
    CHECK(!locked);
    CHECK(heartbeat_count == before + 1);
    cJSON *root = cJSON_Parse(envelope);
    CHECK(cJSON_IsObject(root));
    CHECK(cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(root, "v")) == 3);
    return root;
}

static void check_summary(const cJSON *root, uint32_t expected_age)
{
    const cJSON *data = cJSON_GetObjectItemCaseSensitive(root, "data");
    const cJSON *run = cJSON_GetObjectItemCaseSensitive(data, "run");
    CHECK(cJSON_GetArraySize(run) == RUN_PARAM_WORDS);
    for (int i = 0; i < RUN_PARAM_WORDS; ++i) {
        CHECK(cJSON_GetNumberValue(cJSON_GetArrayItem(run, i)) == UINT16_MAX);
    }
    const cJSON *summary = cJSON_GetObjectItemCaseSensitive(data, "bms_summary");
    CHECK(cJSON_IsObject(summary));
    CHECK(cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(summary, "layout")) == 0);
    CHECK(cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(summary, "age_ms")) == expected_age);
    const cJSON *bytes = cJSON_GetObjectItemCaseSensitive(summary, "bytes");
    CHECK(cJSON_GetArraySize(bytes) == 100);
    for (int i = 0; i < 100; ++i) {
        CHECK(cJSON_GetNumberValue(cJSON_GetArrayItem(bytes, i)) == g_bms_summary.bytes[i]);
    }
}

int main(void)
{
    memset(&g_run_param, 0xff, sizeof(g_run_param));
    g_arm_stat.poll_count = 1;
    telemetry_init();
    clock_ms = 1000;
    cJSON *root = request_heartbeat();
    const cJSON *data = cJSON_GetObjectItemCaseSensitive(root, "data");
    CHECK(!cJSON_GetObjectItemCaseSensitive(data, "bms_summary"));
    cJSON_Delete(root);

    for (int i = 0; i < CS_BATTSUM_DATA_SIZE; ++i) {
        g_bms_summary.bytes[i] = (uint8_t)(255 - i);
    }
    g_bms_summary.acquired = true;
    g_bms_summary.received_ms = clock_ms;
    root = request_heartbeat();
    check_summary(root, 0);
    cJSON_Delete(root);

    /* Keep the stale bytes and their real age even while ARM/run replies continue. */
    clock_ms += 120001;
    root = request_heartbeat();
    check_summary(root, 120001);
    cJSON_Delete(root);

    unsigned before = heartbeat_count;
    telemetry_on_mqtt_connected();
    CHECK(heartbeat_count == before + 1 && !locked);
    root = cJSON_Parse(envelope);
    check_summary(root, 120001);
    cJSON_Delete(root);

    clock_ms = g_bms_summary.received_ms + UINT32_MAX + 5000ULL;
    root = request_heartbeat();
    check_summary(root, UINT32_MAX);
    cJSON_Delete(root);

    memset(g_bms_summary.bytes, 0xff, sizeof(g_bms_summary.bytes));
    root = request_heartbeat();
    check_summary(root, UINT32_MAX);
    CHECK(strlen(envelope) < sizeof(envelope));
    cJSON_Delete(root);

    /* Fresh BMS receipt resets only BMS age; raw run words remain unchanged. */
    g_bms_summary.received_ms = clock_ms;
    root = request_heartbeat();
    check_summary(root, 0);
    cJSON_Delete(root);

    /* Valid BMS traffic must not keep publishing inverter data older than 240s. */
    clock_ms += 239999;
    g_arm_stat.last_rx_ms = (uint32_t)clock_ms;
    g_bms_summary.received_ms = clock_ms;
    before = heartbeat_count;
    telemetry_request_heartbeat();
    telemetry_tick();
    CHECK(heartbeat_count == before + 1 && !locked);
    root = cJSON_Parse(envelope);
    check_summary(root, 0);
    cJSON_Delete(root);

    clock_ms++;
    g_arm_stat.last_rx_ms = (uint32_t)clock_ms;
    g_bms_summary.received_ms = clock_ms;
    arm_bms_summary_t saved = g_bms_summary;
    before = heartbeat_count;
    telemetry_request_heartbeat();
    telemetry_tick();
    CHECK(heartbeat_count == before && !locked);
    telemetry_on_mqtt_connected();
    CHECK(heartbeat_count == before && !locked);
    CHECK(memcmp(&saved, &g_bms_summary, sizeof(saved)) == 0);

    clock_ms += 30000;
    g_arm_stat.last_rx_ms = (uint32_t)clock_ms;
    g_bms_summary.received_ms = clock_ms;
    telemetry_request_heartbeat();
    telemetry_tick();
    CHECK(heartbeat_count == before && !locked);

    /* A new accepted run sample restores publishing without clearing BMS data. */
    g_arm_stat.last_run_ms = clock_ms;
    telemetry_request_heartbeat();
    telemetry_tick();
    CHECK(heartbeat_count == before + 1 && !locked);
    root = cJSON_Parse(envelope);
    check_summary(root, 0);
    cJSON_Delete(root);
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
