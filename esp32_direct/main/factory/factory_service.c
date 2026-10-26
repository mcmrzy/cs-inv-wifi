#include "factory_service.h"
#include "factory_wire.h"
#include "factory_identity.h"
#include "factory_json.h"
#include "direct_link.h"
#include "direct_buzzer.h"
#include "ota_service.h"
#include "config.h"
#include "mqtt_handler.h"
#include "cmd_handler.h"
#include "esp_heap_caps.h"
#include "firmware_identity.h"
#include "ota_image_identity.h"
#include "admin_sn_command.h"
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_mac.h"
#include "esp_app_desc.h"
#include "esp_flash.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
static atomic_uint keys;
static atomic_bool display_test;
static atomic_uint diagnostic_tick;
static atomic_bool usb_started;
static atomic_uint usb_tick;
static factory_parser_t parser;
/* Requests borrow the parser bytes. Process them synchronously before encoding
 * the reply into the same buffer, avoiding a second resident 4KB payload. */
static factory_frame_view_t request;
static admin_sn_input_t admin_input;
extern bool admin_sn_password_valid(const char *);
static bool provision_sn_valid(const char *sn){
 if(strlen(sn)!=16)return false;
 for(unsigned i=0;i<16;i++)if(!((sn[i]>='0'&&sn[i]<='9')||(sn[i]>='A'&&sn[i]<='Z')))return false;
 return true;
}
static void factory_print_dsp_observation(void)
{
 direct_config_observation_t observed;
 bool have=direct_config_observed_snapshot_wire(&observed);
 if(!have)printf("[DSP_OBSERVED] available=0 source=25\n");
 else{
  printf("[DSP_OBSERVED] available=1 source=25 seq=%lu received_ms=%llu age_ms=%llu\n",
   (unsigned long)observed.sequence,(unsigned long long)observed.received_ms,(unsigned long long)observed.age_ms);
  for(unsigned i=0;i<DIRECT_DSP_CONFIG_COUNT;i++)if(observed.valid[i])
   printf("[DSP_OBSERVED_PARAM] index=%u wire_raw=%u\n",i,observed.values[i]);
 }
 direct_dsp_status_t status;uint32_t received_ms;
 if(direct_dsp_get_status_sample(&status,&received_ms))
  printf("[DSP_STATUS_RAW] received_ms=%lu fault=0x%08lX status0=0x%04X status1=0x%04X status2=0x%04X mode=%u\n",
   (unsigned long)received_ms,(unsigned long)status.fault,status.inverter_status[0],
   status.inverter_status[1],status.inverter_status[2],status.mode);
}

static void serial_result(admin_sn_result_t result,const char *sn){
 if(result==ADMIN_SN_NONE)return;
 flockfile(stdout);
 if(result==ADMIN_SN_DSP_BOOT || result==ADMIN_SN_DSP_RUN || result==ADMIN_SN_DSP_REBOOT){
  direct_iap_result_t code=DIRECT_IAP_INCOMPATIBLE;
  bool accepted=ota_service_admin_begin();
  if(accepted){
   direct_dsp_control_t operation=result==ADMIN_SN_DSP_BOOT?DIRECT_DSP_ENTER_BOOT:
       result==ADMIN_SN_DSP_RUN?DIRECT_DSP_EXIT_BOOT:DIRECT_DSP_REBOOT;
   code=direct_link_dsp_control(operation);ota_service_admin_end();
  }
  printf("[DSP_CONTROL] %s code=%d boot_hold=%u; App execution unconfirmed\n",
    !accepted?"BUSY":code!=DIRECT_IAP_OK?"FAILED":result==ADMIN_SN_DSP_BOOT?"BOOT_HR_RECEIVED":"EOT_SENT",
    (int)code,direct_dsp_boot_active());
 }else if(result==ADMIN_SN_DSP_VALID){
  int code=-1;
  if(ota_service_admin_begin()){
   code=direct_dsp_params_install(admin_input.dsp_bundle,sizeof(admin_input.dsp_bundle),sn);
   ota_service_admin_end();
  }
  memset(admin_input.dsp_bundle,0,sizeof(admin_input.dsp_bundle));
  printf("[SET_DSP] %s sn=%s; UART sync queued only after durable save\n",
    code==0?"OK":code==-2?"INVALID_PARAMS_OR_SN":code==-3?"SAVE_FAILED":"BUSY",sn);
 }else if(result==ADMIN_SN_DSP_STATUS){
  direct_dsp_params_t params;bool durable=false;
  bool ready=direct_dsp_params_export(&params,&durable);
  printf("[DSP_PARAMS] ready=%u durable=%u sn=%s\n",ready,durable,g_device_sn);
  if(ready){
   static const char hex[]="0123456789ABCDEF";
   for(unsigned cmd=0;cmd<3;cmd++){
    uint8_t frame[25];char text[51];size_t n=direct_dsp_params_reply(&params,(uint8_t)cmd,frame);
    for(size_t i=0;i<n;i++){text[2*i]=hex[frame[i]>>4];text[2*i+1]=hex[frame[i]&15];}
    text[2*n]=0;printf("[DSP_PARAMS] cmd=%02u frame=%s\n",cmd,text);
   }
  }
 }else if(result==ADMIN_SN_STATUS){
  RunParamDef dsp={0};uint32_t tick=0;bool online=direct_dsp_get_measurement(&dsp,&tick);
  printf("[STATUS] firmware=%s sn=%s mqtt=%s dsp=%s heap=%lu reset=%d\n",FIRMWARE_VERSION,g_device_sn,mqtt_is_connected()?"CONNECTED":"DISCONNECTED",online?"ONLINE":"NO_FRESH_DATA",(unsigned long)esp_get_free_heap_size(),(int)esp_reset_reason());
  printf("[MEMORY] minimum_heap=%lu largest_block=%lu\n",(unsigned long)esp_get_minimum_free_heap_size(),
   (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
  cmd_handler_stats_t commands;cmd_handler_get_stats(&commands);
  printf("[CMD] queued=%u queue_full=%lu retained_rejected=%lu expired=%lu cache_full=%lu control_static_bytes=%u\n",
   commands.queue_depth,(unsigned long)commands.queue_full,(unsigned long)commands.retained_rejected,
   (unsigned long)commands.expired,(unsigned long)commands.cache_full,commands.static_bytes);
  direct_dsp_diagnostics_t link={0};
  if(direct_dsp_get_diagnostics(&link)){
   uint32_t now=(uint32_t)(esp_timer_get_time()/1000);
   printf("[DSP] task=%s quiet=%u stack_min_free=%lu bytes\n",link.task_running?"RUNNING":"STOPPED",
    link.quiet,(unsigned long)link.stack_min_free_bytes);
   printf("[DSP_IO] tx=%lu rx_bytes=%lu frames=%lu tx_err=%lu rx_err=%lu\n",(unsigned long)link.tx_frames,
    (unsigned long)link.rx.rx_bytes,(unsigned long)link.rx.frames,(unsigned long)link.tx_errors,(unsigned long)link.rx_errors);
   printf("[DSP_TX_HEALTH] age_ms=%lu max_gap_ms=%lu gaps_over_500=%lu lock_misses=%lu hold=0x%02X\n",
    (unsigned long)(link.last_tx_ms?now-link.last_tx_ms:UINT32_MAX),(unsigned long)link.max_tx_gap_ms,
    (unsigned long)link.tx_gap_over_500_ms,(unsigned long)link.poll_lock_misses,link.poll_hold_reason);
   printf("[DSP_FRAMES] 03=%lu 04=%lu 25=%lu age04_ms=%lu\n",(unsigned long)link.cmd03,
    (unsigned long)link.cmd04,(unsigned long)link.cmd25,(unsigned long)(link.last_measurement_ms?now-link.last_measurement_ms:UINT32_MAX));
   printf("[DSP_ERRORS] crc=%lu len=%lu timeout=%lu rejected=%lu unknown=%lu\n",(unsigned long)link.rx.crc_errors,
    (unsigned long)link.rx.length_errors,(unsigned long)link.rx.timeouts,(unsigned long)link.rejected_frames,(unsigned long)link.unhandled_frames);
   printf("[DSP_LAST] sync=%lu last_cmd=0x%02X last_len=%u usb_stack_min_free=%lu bytes\n",
    (unsigned long)link.sync_requests,link.last_command,link.last_length,(unsigned long)uxTaskGetStackHighWaterMark(NULL));
   printf("[DSP_UART] fifo_overflow=%lu buffer_full=%lu frame_error=%lu\n",
     (unsigned long)link.uart_fifo_overflows,(unsigned long)link.uart_buffer_full,(unsigned long)link.uart_frame_errors);
   printf("[DSP_UART] parity_error=%lu break=%lu queue_full_seen=%lu\n",
     (unsigned long)link.uart_parity_errors,(unsigned long)link.uart_breaks,(unsigned long)link.uart_queue_full_observations);
   printf("[DSP_SYNC] ready=%u durable=%u pending=%u replies=%lu\n",link.sync_params_ready,
     link.sync_params_durable,link.sync_pending_mask,(unsigned long)link.sync_replies);
  }
  factory_print_dsp_observation();
  if(online){
   printf("[DSP_DATA] grid_V=%u output_V=%u output_W=%d\n",dsp.GridVolt,dsp.ACOutputVolt,(int16_t)dsp.OutputWatt);
   printf("[DSP_BAT] battery_dV=%u charge_dA=%u discharge_dA=%u\n",dsp.BatVolt,dsp.BatChgCurr,dsp.BatDischgCurr);
   printf("[DSP_PV] pv_W=%u temp_dC=%d\n",dsp.Ppv,(int16_t)dsp.InvertTemp);
  }
 }else if(result==ADMIN_SN_PIN_VALID){
  factory_result_t r=FACTORY_BUSY;
  if(ota_service_admin_begin()){
   r=strcmp(sn,g_device_sn)?FACTORY_INVALID_ARGUMENT:factory_identity_set_pairing(sn,admin_input.pin,!CONFIG_DIRECT_IDENTITY_PRODUCTION);
   ota_service_admin_end();
  }
  printf("[SET_PIN] %s\n",factory_result_name(r));
 }else if(result!=ADMIN_SN_VALID)printf("[SET_SN] ERROR %s\n",result==ADMIN_SN_AUTH_FAILED?"AUTH_FAILED":"INVALID_ARGUMENT");
 else if(!ota_service_admin_begin())printf("[SET_SN] ERROR OTA_BUSY_OR_STARTING\n");
 else{
  factory_result_t r=factory_identity_set_admin_sn(sn,!CONFIG_DIRECT_IDENTITY_PRODUCTION);
  ota_service_admin_end();
  if(r==FACTORY_OK)printf("[SET_SN] OK saved_sn=%s REBOOT_REQUIRED; current_sn=%s\n",sn,g_device_sn);
  else printf("[SET_SN] ERROR %s\n",factory_result_name(r));
 }
 fflush(stdout);funlockfile(stdout);
}
static void inspect(cJSON *obj){
 uint8_t mac[6]={0};esp_read_mac(mac,ESP_MAC_WIFI_STA);char text[18];snprintf(text,sizeof(text),"%02X:%02X:%02X:%02X:%02X:%02X",mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
 cJSON_AddStringToObject(obj,"mac",text);cJSON_AddStringToObject(obj,"firmware",esp_app_get_description()->version);
 cJSON_AddStringToObject(obj,"model","CS-L10-6K2");
 uint32_t flash_size=0;esp_err_t err=esp_flash_get_size(NULL,&flash_size);cJSON_AddNumberToObject(obj,"flash_bytes",err==ESP_OK?flash_size:0);
 cJSON_AddNumberToObject(obj,"heap",esp_get_free_heap_size());cJSON_AddNumberToObject(obj,"reset_reason",esp_reset_reason());
 cJSON_AddBoolToObject(obj,"factory_write_enabled",CONFIG_DIRECT_FACTORY_WRITES);
 cJSON_AddBoolToObject(obj,"diagnostics_enabled",true);
 cJSON_AddNumberToObject(obj,"factory_api",3);
 cJSON_AddNumberToObject(obj,"identity_schema",FACTORY_RECORD_SCHEMA);
 cJSON_AddBoolToObject(obj,"provisioning_identity_v2",true);
 cJSON_AddStringToObject(obj,"chip","esp32c3");
 cJSON_AddNumberToObject(obj,"runtime_generation",factory_identity_runtime_generation());
 cJSON_AddBoolToObject(obj,"pairing_identity_v1",true);
 char pin[7]={0};cJSON_AddBoolToObject(obj,"pairing_ready",factory_identity_pairing(g_device_sn,pin));memset(pin,0,sizeof(pin));
 cJSON_AddStringToObject(obj,"runtime_sn",g_device_sn);
 cJSON_AddBoolToObject(obj,"production_build",CONFIG_DIRECT_IDENTITY_PRODUCTION);
 cJSON_AddStringToObject(obj,"firmware_profile",direct_ota_profile_name(direct_firmware_metadata()[6]));
 char app_hash[65];const uint8_t *hash=esp_app_get_description()->app_elf_sha256;for(unsigned i=0;i<32;i++)snprintf(app_hash+2*i,3,"%02x",hash[i]);
 cJSON_AddStringToObject(obj,"elf_sha256",app_hash);
 factory_record_t r;factory_result_t res=factory_identity_read(&r);cJSON_AddStringToObject(obj,"identity_status",factory_result_name(res));
 bool pending=res==FACTORY_OK && (strcmp(r.sn,g_device_sn) || r.generation!=factory_identity_runtime_generation());
 char saved_sn[17]={0};
 if(factory_identity_admin_sn(saved_sn,sizeof(saved_sn)) && strcmp(saved_sn,g_device_sn))pending=true;
 cJSON_AddBoolToObject(obj,"identity_pending_reboot",pending);
 if(res==FACTORY_OK){cJSON *id=cJSON_AddObjectToObject(obj,"identity");if(!factory_record_json(id,&r))cJSON_DeleteItemFromObject(obj,"identity");}
}
static void diagnostics(cJSON *obj){
 RunParamDef dsp={0};uint32_t received=0,age=0;direct_bms_measurement_t bms={0};
 bool dsp_ok=direct_dsp_get_measurement(&dsp,&received);
 bool bms_ok=direct_bms_get_snapshot(&bms,&age)&&age<5000;
 cJSON_AddBoolToObject(obj,"dsp_online",dsp_ok);cJSON_AddBoolToObject(obj,"bms_online",bms_ok);
 cJSON_AddNumberToObject(obj,"keys",atomic_load(&keys));
 cJSON_AddBoolToObject(obj,"diagnostic_active",atomic_load(&display_test));
 if(atomic_load(&display_test))atomic_store(&diagnostic_tick,(uint32_t)(esp_timer_get_time()/1000));
 if(dsp_ok){cJSON *m=cJSON_AddObjectToObject(obj,"measurements");cJSON_AddNumberToObject(m,"battery_v",dsp.BatVolt/10.0);cJSON_AddNumberToObject(m,"output_v",dsp.ACOutputVolt);cJSON_AddNumberToObject(m,"grid_v",dsp.GridVolt);cJSON_AddNumberToObject(m,"output_w",dsp.OutputWatt);cJSON_AddNumberToObject(m,"inverter_c",dsp.InvertTemp/10.0);cJSON_AddNumberToObject(m,"received_ms",received);}
 if(bms_ok){cJSON *b=cJSON_AddObjectToObject(obj,"bms");cJSON_AddNumberToObject(b,"voltage_v",bms.voltage_10mv/100.0);cJSON_AddNumberToObject(b,"current_a",bms.current_10ma/100.0);cJSON_AddNumberToObject(b,"soc",bms.soc_percent/(double)(bms.percent_divisor?bms.percent_divisor:1U));cJSON_AddNumberToObject(b,"age_ms",age);}
 cJSON_AddStringToObject(obj,"calibration_write","UNSUPPORTED_NO_CONFIRMED_TARGET_PROTOCOL");
}
static void process(void){
 cJSON *obj=cJSON_CreateObject();if(!obj)return;
 const char *result="OK";
 if(request.command==1){inspect(obj);}
 else if(request.command==2){
  factory_record_t r;factory_result_t res=factory_record_decode(request.payload,request.length,&r);
  if(!CONFIG_DIRECT_FACTORY_WRITES)result="WRITE_DISABLED";
  else if(res!=FACTORY_OK)result=factory_result_name(res);
  else if(r.schema==FACTORY_RECORD_SCHEMA && !provision_sn_valid(r.sn))result="INVALID_SN";
  else if(strcmp(r.model,"CS-L10-6K2"))result="MODEL_MISMATCH";
  else if(!ota_service_admin_begin())result="BUSY";
  else {result=factory_result_name(factory_identity_provision(&r));ota_service_admin_end();}
  inspect(obj);cJSON_AddBoolToObject(obj,"reboot_required",strcmp(result,"OK")==0);
 }else if(request.command==3){
  factory_record_t r;factory_result_t current=factory_identity_read(&r);
  if(!CONFIG_DIRECT_FACTORY_WRITES)result="WRITE_DISABLED";
  else if(current!=FACTORY_OK)result=factory_result_name(current);
  else if(!r.hw_system[0]||!r.hw_controller[0]||!strcmp(r.hw_system,"UNSET")||!strcmp(r.hw_controller,"UNSET"))result="HARDWARE_VERSION_REQUIRED";
  else if(!r.calibration_summary[0])result="TEST_RECORD_REQUIRED";
  else if(strcmp(r.sn,g_device_sn)||r.generation!=factory_identity_runtime_generation())result="REBOOT_REQUIRED";
  else if(!ota_service_admin_begin())result="BUSY";
  else {result=factory_result_name(factory_identity_lock());ota_service_admin_end();}
  inspect(obj);
 }else if(request.command==4){inspect(obj);diagnostics(obj);}
 else if(request.command==6){
  if(ota_service_is_busy())result="BUSY";
  else if(request.length!=1||request.payload[0]>2)result="INVALID_ARGUMENT";
  else{atomic_store(&keys,0);atomic_store(&diagnostic_tick,(uint32_t)(esp_timer_get_time()/1000));atomic_store(&display_test,request.payload[0]!=0);if(request.payload[0]==2)direct_buzzer_pulse();}
 }else if(request.command==7){
  if(request.length!=1||request.payload[0]>1)result="INVALID_ARGUMENT";
  else if(request.payload[0] && ota_service_is_busy())result="BUSY";
  else if(request.payload[0] && !direct_bms_identity_request())result="BUSY_OR_UNAVAILABLE";
  direct_bms_identity_t id={0};bool pending=false;uint8_t queried=0;uint32_t age=UINT32_MAX;
  if(direct_bms_identity_snapshot(&id,&pending,&queried,&age)){
   cJSON_AddBoolToObject(obj,"pending",pending);cJSON_AddNumberToObject(obj,"queried",queried);
   cJSON_AddNumberToObject(obj,"valid_mask",id.valid_mask);cJSON_AddNumberToObject(obj,"age_ms",age);
   const char *names[]={"software","compile_time","cpuid","hardware","module_sn","pcb_sn","product"};
   const char *values[]={id.software,id.compile_time,id.cpuid,id.hardware,id.module_sn,id.pcb_sn,id.product};
   for(unsigned i=0;i<7;i++)if(id.valid_mask&(1U<<i))cJSON_AddStringToObject(obj,names[i],values[i]);
   if(id.valid_mask&0x80)cJSON_AddNumberToObject(obj,"app_crc32",id.app_crc32);
   cJSON_AddBoolToObject(obj,"upgrade_authorized",false);
   cJSON_AddStringToObject(obj,"capability",pending?"QUERYING":queried!=8?"NOT_QUERIED":!id.valid_mask?"UNSUPPORTED_OR_NO_REPLY":id.valid_mask!=255?"PARTIAL":"IDENTITY_READ_ONLY");
  }else result="BUSY";
 }else if(request.command==8){
  /* Factory pairing write; no product-wide secret is sent over USB. */
  if(!CONFIG_DIRECT_FACTORY_WRITES)result="WRITE_DISABLED";
  else if(request.length!=23)result="INVALID_ARGUMENT";
  else if(!ota_service_admin_begin())result="BUSY";
  else{
   char sn[17]={0},pin[7]={0};memcpy(sn,request.payload,16);memcpy(pin,request.payload+16,6);
   result=request.payload[22]?"INVALID_ARGUMENT":factory_result_name(factory_identity_set_pairing(sn,pin,false));
   memset(pin,0,sizeof(pin));ota_service_admin_end();
  }
  cJSON_AddBoolToObject(obj,"pairing_ready",!strcmp(result,"OK"));
 }else result="UNSUPPORTED";
 cJSON_AddStringToObject(obj,"result",result);
 char *json=cJSON_PrintUnformatted(obj);
 if(json){size_t n=strlen(json);if(n>FACTORY_PAYLOAD_MAX){cJSON_free(json);json=NULL;}
    const char *payload=json?json:"{\"result\":\"RESPONSE_TOO_LARGE\"}";n=strlen(payload);
    if(n<=FACTORY_PAYLOAD_MAX){factory_frame_view_t reply={.command=request.command|0x80,.id=request.id,.length=(uint16_t)n,.payload=(const uint8_t*)payload};size_t bytes=factory_frame_view_encode(&reply,parser.bytes,sizeof(parser.bytes));flockfile(stdout);
      size_t sent=0;while(sent<bytes){int n=usb_serial_jtag_write_bytes(parser.bytes+sent,bytes-sent,pdMS_TO_TICKS(500));if(n<=0)break;sent+=(size_t)n;}
      funlockfile(stdout);}cJSON_free(json);}
 cJSON_Delete(obj);
}
static void factory_task(void *arg){
 (void)arg;uint8_t rx[64];int64_t last=0;
 for(;;){uint32_t tick=(uint32_t)(esp_timer_get_time()/1000);atomic_store(&usb_tick,tick);
  if(atomic_load(&display_test)&&tick-atomic_load(&diagnostic_tick)>60000U)atomic_store(&display_test,false);
  int n=usb_serial_jtag_read_bytes(rx,sizeof(rx),pdMS_TO_TICKS(50));
  if(n>0){int64_t now=esp_timer_get_time();if(now-last>1000000)parser.used=0;last=now;
   for(int i=0;i<n;i++){
    if(parser.used||rx[i]==0xc5){
     memset(&admin_input,0,sizeof(admin_input));
     if(factory_parser_feed_view(&parser,rx[i],&request))process();
    }else{
     char sn[17]={0};admin_sn_result_t result=admin_sn_input_byte(&admin_input,rx[i],admin_sn_password_valid,sn);
     serial_result(result,sn);memset(sn,0,sizeof(sn));memset(admin_input.pin,0,sizeof(admin_input.pin));
     memset(admin_input.dsp_bundle,0,sizeof(admin_input.dsp_bundle));
    }
   }
  }
 }
}
bool factory_service_start(void){
 usb_serial_jtag_driver_config_t conf={.tx_buffer_size=2048,.rx_buffer_size=2048};
 if(usb_serial_jtag_driver_install(&conf)!=ESP_OK)return false;
 usb_serial_jtag_vfs_use_driver();setvbuf(stdout,NULL,_IONBF,0);
 /* The schema-2 decoder/store has nested record/slot validation buffers. */
 bool started=xTaskCreate(factory_task,"factory_usb",12288,NULL,2,NULL)==pdPASS;
 atomic_store(&usb_started,started);return started;
}
bool factory_service_software_healthy(void){return atomic_load(&usb_started)&&
 (uint32_t)(esp_timer_get_time()/1000)-atomic_load(&usb_tick)<=2000U;}
void factory_note_key(uint8_t key){if(key>=1&&key<=6)atomic_fetch_or(&keys,1U<<(key-1));}
uint32_t factory_key_mask(void){return atomic_load(&keys);}
bool factory_test_display_requested(void){return atomic_load(&display_test);}
