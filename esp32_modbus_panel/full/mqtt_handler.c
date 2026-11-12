/* Keep the original direct MQTT implementation intact. Register a separate
 * listener on EVERY new client, including BLE/local-mode client recreation. */
#include "mqtt_client.h"
#include "full_alarm_service.h"
#include "panel_firmware_catalog.h"
#include <stdint.h>
#include <stdatomic.h>
static esp_err_t full_register_events(esp_mqtt_client_handle_t,esp_mqtt_event_id_t,esp_event_handler_t,void *);
#define esp_mqtt_client_register_event full_register_events
#include "../../esp32_direct/main/mqtt/mqtt_handler.c"
#undef esp_mqtt_client_register_event

bool full_mqtt_client_resident(void){return g_mqtt_client!=NULL;}

static atomic_uint s_alarm_listener;
static atomic_bool s_alarm_hook_ready;
static void alarm_mqtt_event(void *arg,esp_event_base_t base,int32_t id,void *data)
{
    (void)base;
    if((uint32_t)(uintptr_t)arg!=atomic_load(&s_alarm_listener))return;
    if(id==MQTT_EVENT_CONNECTED || id==MQTT_EVENT_DISCONNECTED){
        full_alarm_transport_reset();
        panel_firmware_catalog_network(id==MQTT_EVENT_CONNECTED);
    }
    else if(id==MQTT_EVENT_PUBLISHED && data)full_alarm_puback(((esp_mqtt_event_handle_t)data)->msg_id);
    else if(id==MQTT_EVENT_DELETED && data)full_alarm_expired(((esp_mqtt_event_handle_t)data)->msg_id);
}
static esp_err_t full_register_events(esp_mqtt_client_handle_t client,esp_mqtt_event_id_t id,
                                      esp_event_handler_t callback,void *arg)
{
    atomic_store(&s_alarm_hook_ready,false);
    uint32_t listener=atomic_fetch_add(&s_alarm_listener,1)+1U;
    full_alarm_transport_reset();
    panel_firmware_catalog_network(false);
    esp_err_t err=esp_mqtt_client_register_event(client,id,callback,arg);
    if(err==ESP_OK)err=esp_mqtt_client_register_event(client,ESP_EVENT_ANY_ID,alarm_mqtt_event,(void *)(uintptr_t)listener);
    atomic_store(&s_alarm_hook_ready,err==ESP_OK);
    if(err!=ESP_OK)ESP_LOGE("mb_alarm","MQTT PUBACK listener unavailable: %s",esp_err_to_name(err));
    return err;
}
int full_alarm_mqtt_enqueue(const char *envelope)
{
    if(!envelope || !atomic_load(&s_alarm_hook_ready))return -2;
    if(atomic_load(&g_local_paused) || !g_mqtt_client || !atomic_load(&g_mqtt_connected))return -1;
    char topic[96];int n=snprintf(topic,sizeof(topic),"cs_inv/%s/alarm",g_device_sn);
    if(n<=0 || n>=(int)sizeof(topic))return -3;
    /* Nonblocking queue insertion: SDK owns QoS1 retransmission until PUBACK.
     * The independent NVS journal remains authoritative until that PUBACK. */
    int id=esp_mqtt_client_enqueue(g_mqtt_client,topic,envelope,(int)strlen(envelope),1,0,true);
    return id<0?-4:id; /* Distinguish SDK/outbox failure from disconnected. */
}

bool full_panel_rpc_publish(const char *payload,size_t length)
{
    if(!payload || length>512 || !atomic_load(&s_alarm_hook_ready) ||
       atomic_load(&g_local_paused) || !g_mqtt_client || !atomic_load(&g_mqtt_connected))return false;
    char topic[96];int n=snprintf(topic,sizeof(topic),"cs_inv/%s/ota/status",g_device_sn);
    return n>0 && n<(int)sizeof(topic) &&
        esp_mqtt_client_enqueue(g_mqtt_client,topic,payload,(int)length,1,0,true)>=0;
}
