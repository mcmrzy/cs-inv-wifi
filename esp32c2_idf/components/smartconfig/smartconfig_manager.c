#include "smartconfig_manager.h"
#include "esp_smartconfig.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "smartconfig";

static SmartConfigState s_state = SC_STATE_IDLE;
static SmartConfigCallback s_callback = NULL;

void smartconfig_set_callback(SmartConfigCallback cb)
{
    s_callback = cb;
}

SmartConfigState smartconfig_get_state(void)
{
    return s_state;
}

void smartconfig_start(void)
{
    if (s_state == SC_STATE_RUNNING) return;

    ESP_LOGI(TAG, "Starting SmartConfig (ESPTouch)");
    s_state = SC_STATE_RUNNING;

    esp_smartconfig_set_type(SC_TYPE_ESPTOUCH);
    smartconfig_start_config_t cfg = SMARTCONFIG_START_CONFIG_DEFAULT();
    esp_smartconfig_start(&cfg);
}

void smartconfig_stop(void)
{
    if (s_state == SC_STATE_IDLE) return;

    ESP_LOGI(TAG, "Stopping SmartConfig");
    esp_smartconfig_stop();
    s_state = SC_STATE_IDLE;
}

void smartconfig_event_handler(void *arg, esp_event_base_t base,
                               int32_t event_id, void *data)
{
    switch (event_id) {
    case SC_EVENT_SCAN_DONE:
        ESP_LOGI(TAG, "Scan done");
        break;
    case SC_EVENT_FOUND_CHANNEL:
        ESP_LOGI(TAG, "Found channel");
        break;
    case SC_EVENT_GOT_SSID_PSWD: {
        smartconfig_event_got_ssid_pswd_t *evt =
            (smartconfig_event_got_ssid_pswd_t *)data;

        char ssid[33] = {0};
        char password[65] = {0};
        memcpy(ssid, evt->ssid, sizeof(evt->ssid));
        memcpy(password, evt->password, sizeof(evt->password));

        ESP_LOGI(TAG, "Got SSID: %s", ssid);

        s_state = SC_STATE_DONE;
        esp_smartconfig_stop();

        if (s_callback) {
            s_callback(ssid, password);
        }
        break;
    }
    case SC_EVENT_SEND_ACK_DONE:
        ESP_LOGI(TAG, "ACK sent");
        break;
    default:
        break;
    }
}
