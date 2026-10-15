#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

typedef int esp_err_t;
typedef const char *esp_event_base_t;
#define ESP_OK 0
#define ESP_ERR_WIFI_NOT_STARTED 1
#define ESP_ERR_WIFI_STATE 3
#define WIFI_IF_STA 0
#define WIFI_AUTH_WPA2_PSK 3
#define WIFI_RESET_AFTER_FAILS 5
#define WIFI_EVENT_STA_START 1
#define WIFI_EVENT_STA_CONNECTED 2
#define WIFI_EVENT_STA_DISCONNECTED 3
#define IP_EVENT_STA_GOT_IP 4
#define WIFI_EVENT_STA_STOP 5
#define IPSTR "%u.%u.%u.%u"
#define IP2STR(addr) 1U,2U,3U,4U
static void (*info_hook)(void);
#define ESP_LOGI(...) do { if(info_hook){void (*hook)(void)=info_hook;info_hook=NULL;hook();} } while(0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define BIT0 1
static const char *WIFI_EVENT="wifi",*IP_EVENT="ip";
static int s_wifi_event_group=1;
static const int WIFI_CONNECTED_BIT=BIT0;
static uint8_t s_last_disconnect_reason;
static int s_disconnect_count;
static char s_ip_str[32];
static atomic_bool s_connect_requested,s_driver_started,s_connect_inflight;
static unsigned starts,stops,connects,disconnects,configures,event_bits;
static bool associated,fail_config,reject_connecting_config;
static uint8_t ap_channel=6;
static esp_err_t connect_result;
typedef struct {struct {uint8_t ssid[32],password[64];struct {int authmode;} threshold;int scan_method;bool bssid_set;uint8_t bssid[6],channel;} sta;} wifi_config_t;
typedef struct {uint8_t ssid[33],primary;} wifi_ap_record_t;
typedef struct {uint8_t reason;} wifi_event_sta_disconnected_t;
typedef struct {struct {int ip;} ip_info;void *esp_netif;} ip_event_got_ip_t;
static wifi_config_t driver_config;
static void xEventGroupClearBits(int group,int bits){assert(group==1);event_bits&=~bits;}
static void xEventGroupSetBits(int group,int bits){assert(group==1);event_bits|=bits;}
static esp_err_t esp_wifi_connect(void){connects++;return atomic_load(&s_driver_started)?connect_result:ESP_ERR_WIFI_NOT_STARTED;}
static esp_err_t esp_wifi_stop(void){stops++;atomic_store(&s_driver_started,false);return ESP_OK;}
static esp_err_t esp_wifi_start(void){starts++;return ESP_OK;}
static esp_err_t esp_wifi_disconnect(void){disconnects++;associated=false;return ESP_OK;}
static esp_err_t esp_wifi_set_config(int mode,const wifi_config_t *config){assert(mode==WIFI_IF_STA);configures++;if(fail_config)return 2;if(reject_connecting_config && atomic_load(&s_connect_inflight) && !associated)return ESP_ERR_WIFI_STATE;driver_config=*config;return ESP_OK;}
static esp_err_t esp_wifi_get_config(int mode,wifi_config_t *config){assert(mode==WIFI_IF_STA);*config=driver_config;return ESP_OK;}
static esp_err_t esp_wifi_sta_get_ap_info(wifi_ap_record_t *ap){memcpy(ap->ssid,driver_config.sta.ssid,32);ap->primary=ap_channel;return associated?ESP_OK:2;}
static bool wifi_sta_is_connected(void){return event_bits&WIFI_CONNECTED_BIT;}
const char *esp_err_to_name(esp_err_t err){(void)err;return "FAKE";}
const char *esp_netif_get_desc(void *netif){(void)netif;return "sta";}
static void configure_dns_servers(void *netif){(void)netif;}
static void log_dns_servers(void *netif){(void)netif;}
#include "../tmp/wifi_reconnect_under_test.inc"
static void reset(void){
    starts=stops=connects=disconnects=configures=event_bits=0;
    s_disconnect_count=0;associated=fail_config=reject_connecting_config=false;
    ap_channel=6;connect_result=ESP_OK;
    atomic_store(&s_connect_requested,false);atomic_store(&s_driver_started,false);
    atomic_store(&s_connect_inflight,false);
    memset(&driver_config,0,sizeof(driver_config));info_hook=NULL;
}
static void fire(int id){wifi_event_sta_disconnected_t disconnected={.reason=205};wifi_event_handler(NULL,WIFI_EVENT,id,&disconnected);}
static void configure_during_start(void){assert(wifi_sta_connect("race-network","test-password"));}
int main(void){
    reset();info_hook=configure_during_start;fire(WIFI_EVENT_STA_START);
    assert(connects==1); /* START and caller must share a single connection attempt. */
    reset();assert(wifi_sta_connect("before-start","test-password"));
    assert(connects==0);fire(WIFI_EVENT_STA_START);assert(connects==1);
    reset();fire(WIFI_EVENT_STA_START);assert(connects==0);
    assert(wifi_sta_connect("same-network","test-password"));assert(connects==1);
    unsigned attempt_configures=configures;
    assert(wifi_sta_connect("same-network","test-password"));
    assert(connects==1 && configures==attempt_configures);
    for(unsigned i=0;i<5;i++)fire(WIFI_EVENT_STA_DISCONNECTED);
    assert(stops==1 && starts==1 && s_disconnect_count==0);
    unsigned before=connects;
    fire(WIFI_EVENT_STA_START);
    assert(connects==before+1); /* start() never auto-connects by itself */
    assert(!strcmp((char*)driver_config.sta.ssid,"same-network"));
    assert(!strcmp((char*)driver_config.sta.password,"test-password"));
    wifi_sta_force_disconnect();before=connects;
    fire(WIFI_EVENT_STA_DISCONNECTED);fire(WIFI_EVENT_STA_START);
    assert(connects==before);
    assert(wifi_sta_connect("new-network","new-password"));
    assert(!strcmp((char*)driver_config.sta.ssid,"new-network"));
    associated=true;
    ip_event_got_ip_t ip={0};wifi_event_handler(NULL,IP_EVENT,IP_EVENT_STA_GOT_IP,&ip);
    assert(wifi_sta_is_connected() && s_disconnect_count==0);
    assert(driver_config.sta.channel==6 && !driver_config.sta.bssid_set);
    unsigned configured_before=configures,connected_before=connects;
    assert(wifi_sta_connect("new-network","new-password"));
    assert(wifi_sta_is_connected() && configures==configured_before && connects==connected_before);
    wifi_event_handler(NULL,IP_EVENT,IP_EVENT_STA_GOT_IP,&ip);
    assert(configures==configured_before); /* No extra NVS writes on unchanged IP renewals. */
    fail_config=true;
    assert(!wifi_sta_connect("replacement-network","replacement-password"));
    assert(wifi_sta_is_connected()); /* Rejected config must preserve the live IP. */
    fail_config=false;
    wifi_config_t saved=driver_config;
    reset();driver_config=saved;
    driver_config.sta.bssid_set=true;memset(driver_config.sta.bssid,0x11,6);
    assert(wifi_sta_connect("new-network","new-password"));
    assert(driver_config.sta.channel==6 && !driver_config.sta.bssid_set);
    for(unsigned i=0;i<6;i++)assert(driver_config.sta.bssid[i]==0);
    fire(WIFI_EVENT_STA_START);assert(connects==1);
    fire(WIFI_EVENT_STA_DISCONNECTED);assert(connects==2);
    associated=true;ap_channel=11;
    wifi_event_handler(NULL,IP_EVENT,IP_EVENT_STA_GOT_IP,&ip);
    assert(driver_config.sta.channel==11); /* A moved AP updates the next boot hint. */
    reset();driver_config=saved;
    assert(wifi_sta_connect("new-network","changed-password"));
    assert(driver_config.sta.channel==0 && !driver_config.sta.bssid_set);
    reset();driver_config=saved;
    assert(wifi_sta_connect("other-network","new-password"));
    assert(driver_config.sta.channel==0 && !driver_config.sta.bssid_set);
    reset();fire(WIFI_EVENT_STA_START);connect_result=2;
    assert(!wifi_sta_connect("retry-network","test-password"));
    assert(!atomic_load(&s_connect_inflight));
    connect_result=ESP_OK;
    assert(wifi_sta_connect("retry-network","test-password"));assert(connects==2);
    reject_connecting_config=true;
    assert(!wifi_sta_connect("replacement-network","replacement-password"));
    assert(!strcmp((char*)driver_config.sta.ssid,"retry-network") && connects==2);
    assert(atomic_load(&s_connect_requested) && atomic_load(&s_connect_inflight));
    wifi_sta_force_disconnect();
    assert(!atomic_load(&s_connect_requested) && !wifi_sta_is_connected());
    unsigned before_stale=configures;
    wifi_event_handler(NULL,IP_EVENT,IP_EVENT_STA_GOT_IP,&ip);
    assert(!wifi_sta_is_connected() && configures==before_stale);
    fire(WIFI_EVENT_STA_DISCONNECTED);
    assert(!atomic_load(&s_connect_inflight) && connects==2);
    reset();fire(WIFI_EVENT_STA_START);
    assert(wifi_sta_connect("live-network","test-password"));associated=true;
    wifi_event_handler(NULL,IP_EVENT,IP_EVENT_STA_GOT_IP,&ip);
    assert(wifi_sta_is_connected());
    wifi_sta_force_disconnect();assert(!wifi_sta_is_connected());
    wifi_event_handler(NULL,IP_EVENT,IP_EVENT_STA_GOT_IP,&ip);
    fire(WIFI_EVENT_STA_DISCONNECTED);
    assert(!wifi_sta_is_connected() && !atomic_load(&s_connect_inflight) && connects==1);
    reset();fire(WIFI_EVENT_STA_START);
    assert(wifi_sta_connect("cancel-during-ip","test-password"));associated=true;
    info_hook=wifi_sta_force_disconnect;
    wifi_event_handler(NULL,IP_EVENT,IP_EVENT_STA_GOT_IP,&ip);
    assert(!wifi_sta_is_connected() && !atomic_load(&s_connect_requested));
    reset();fire(WIFI_EVENT_STA_START);
    assert(wifi_sta_connect("hint-fail-network","test-password"));associated=true;fail_config=true;
    wifi_event_handler(NULL,IP_EVENT,IP_EVENT_STA_GOT_IP,&ip);
    assert(wifi_sta_is_connected() && driver_config.sta.channel==0 && connects==1 && disconnects==0);
    fire(WIFI_EVENT_STA_STOP);
    wifi_event_handler(NULL,IP_EVENT,IP_EVENT_STA_GOT_IP,&ip);
    assert(!wifi_sta_is_connected() && !atomic_load(&s_connect_inflight));
    reset();fail_config=true;assert(!wifi_sta_connect("bad-config","test-password"));
    fire(WIFI_EVENT_STA_START);assert(connects==0);
    puts("PASS actual WiFi: START/caller share one attempt; repeated setup preserves IP; reset/explicit-stop/config-error recover; successful channel survives reboot without BSSID pinning; changed credentials discard hint");
    return 0;
}
