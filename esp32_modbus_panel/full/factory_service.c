#include <stdlib.h>
#include "panel_usb_service.h"
#include "factory_wire.h"
#include "factory_identity.h"
#include "factory_json.h"
#include "panel_pairing.h"
#include "panel_bus.h"
#include "panel_catalog.h"
#include "panel_control.h"
#include "direct_link.h"
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
static atomic_bool usb_started;
static atomic_uint usb_tick;
static factory_parser_t *parser;
/* Requests borrow the parser bytes. Process them synchronously before encoding
 * the reply into the same buffer, avoiding a second resident 4KB payload. */
static factory_frame_view_t request;
static admin_sn_input_t admin_input;
#define PC_RX_BYTES 24U
#define PC_TX_BYTES 1400U
static uint8_t pc_rx[PC_RX_BYTES],pc_tx[PC_TX_BYTES];
static size_t pc_used;
static bool pc_reply;
extern bool admin_sn_password_valid(const char *);
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

/* Original serial-tool commands are retained only to reject the private
 * protocol explicitly. They must never reserve OTA ownership or touch a store. */
static bool factory_private_command(admin_sn_result_t result){
 if(result==ADMIN_SN_DSP_VALID){
  memset(admin_input.dsp_bundle,0,sizeof(admin_input.dsp_bundle));
  printf("[SET_DSP] PROTOCOL_UNSUPPORTED; private DSP bundles are unavailable on CS-MB-PANEL\n");
 }else if(result==ADMIN_SN_DSP_STATUS){
  printf("[DSP_PARAMS] ready=0 durable=0 result=PROTOCOL_UNSUPPORTED sn=%s\n",g_device_sn);
 }else if(result==ADMIN_SN_DSP_BOOT || result==ADMIN_SN_DSP_RUN || result==ADMIN_SN_DSP_REBOOT){
  printf("[DSP_CONTROL] PROTOCOL_UNSUPPORTED; no Boot/control request sent\n");
 }else return false;
 return true;
}
static void serial_result(admin_sn_result_t result,const char *sn){
 if(result==ADMIN_SN_NONE)return;
 flockfile(stdout);
 if(factory_private_command(result)){fflush(stdout);funlockfile(stdout);return;}
 if(result==ADMIN_SN_STATUS){
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
static void inspect_inverter(cJSON *obj){
 panel_snapshot_t snapshot;
 if(!panel_bus_snapshot(&snapshot))return;
 cJSON *data=cJSON_AddObjectToObject(obj,"inverter");
 if(!data)return;
 cJSON_AddBoolToObject(data,"online",panel_online(&snapshot));
 cJSON_AddBoolToObject(data,"automatic",snapshot.config.profile_override==PANEL_PROFILE_UNKNOWN);
 cJSON_AddStringToObject(data,"active_model",panel_profile_name(panel_effective_profile(&snapshot)));
 cJSON_AddStringToObject(data,"candidate",panel_profile_name(snapshot.detected_profile));
 cJSON_AddNumberToObject(data,"address",snapshot.config.address);
 cJSON_AddNumberToObject(data,"baud",snapshot.config.baud);
 cJSON_AddNumberToObject(data,"parity",snapshot.config.parity);
 cJSON_AddNumberToObject(data,"read_errors",snapshot.errors);
 uint16_t raw;
 if(panel_fresh(&snapshot,0x38) && panel_value(&snapshot,0x38,&raw))cJSON_AddNumberToObject(data,"id38",raw);
 if(panel_fresh(&snapshot,0x39) && panel_value(&snapshot,0x39,&raw))cJSON_AddNumberToObject(data,"id39",raw);
}
static void inspect(cJSON *obj){
 uint8_t mac[6]={0};esp_read_mac(mac,ESP_MAC_WIFI_STA);char text[18];snprintf(text,sizeof(text),"%02X:%02X:%02X:%02X:%02X:%02X",mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
 cJSON_AddStringToObject(obj,"mac",text);cJSON_AddStringToObject(obj,"firmware",esp_app_get_description()->version);
 cJSON_AddStringToObject(obj,"model","CS-MB-PANEL");inspect_inverter(obj);
 uint32_t flash_size=0;esp_err_t err=esp_flash_get_size(NULL,&flash_size);cJSON_AddNumberToObject(obj,"flash_bytes",err==ESP_OK?flash_size:0);
 cJSON_AddNumberToObject(obj,"heap",esp_get_free_heap_size());cJSON_AddNumberToObject(obj,"reset_reason",esp_reset_reason());
 cJSON_AddBoolToObject(obj,"factory_write_enabled",false);
 cJSON_AddBoolToObject(obj,"diagnostics_enabled",false);
 cJSON_AddNumberToObject(obj,"factory_api",3);
 cJSON_AddNumberToObject(obj,"identity_schema",FACTORY_RECORD_SCHEMA);
 cJSON_AddBoolToObject(obj,"provisioning_identity_v2",true);
 cJSON_AddStringToObject(obj,"chip","esp32c3");
 cJSON_AddNumberToObject(obj,"runtime_generation",factory_identity_runtime_generation());
 cJSON_AddBoolToObject(obj,"pairing_identity_v1",true);
 char pin[7]={0};cJSON_AddBoolToObject(obj,"pairing_ready",panel_pairing_pin(g_device_sn,pin));memset(pin,0,sizeof(pin));
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
 if(dsp_ok){cJSON *m=cJSON_AddObjectToObject(obj,"measurements");cJSON_AddNumberToObject(m,"battery_v",dsp.BatVolt/10.0);cJSON_AddNumberToObject(m,"output_v",dsp.ACOutputVolt);cJSON_AddNumberToObject(m,"grid_v",dsp.GridVolt);cJSON_AddNumberToObject(m,"output_w",dsp.OutputWatt);cJSON_AddNumberToObject(m,"inverter_c",dsp.InvertTemp/10.0);cJSON_AddNumberToObject(m,"received_ms",received);}
 if(bms_ok){cJSON *b=cJSON_AddObjectToObject(obj,"bms");cJSON_AddNumberToObject(b,"voltage_v",bms.voltage_10mv/100.0);cJSON_AddNumberToObject(b,"current_a",bms.current_10ma/100.0);cJSON_AddNumberToObject(b,"soc",bms.soc_percent/(double)(bms.percent_divisor?bms.percent_divisor:1U));cJSON_AddNumberToObject(b,"age_ms",age);}
 cJSON_AddStringToObject(obj,"calibration_write","UNSUPPORTED_NO_CONFIRMED_TARGET_PROTOCOL");
}
/* User USB requests share the panel's Modbus owner, settings guards and receipts. */
static bool cdc_current(const panel_snapshot_t *s,uint16_t address,uint16_t *value){
 const panel_setting_t *setting=panel_setting_find(address);
 uint16_t reg=panel_setting_current_address(s,setting),raw;
 if(reg!=0xffff && panel_fresh(s,reg) && panel_value(s,reg,&raw) &&
    panel_setting_decode_current(s,address,raw,value))return true;
 const panel_setting_read_t *read=panel_setting_read_find(s,address);
 if(!read || !read->independent || read->state!=PANEL_SETTING_VALUE ||
    s->now_ms-read->stamp>PANEL_FRESH_MS)return false;
 *value=read->value;return true;
}
static void cdc_metric(cJSON *obj,const panel_snapshot_t *s,const char *name,
                       uint16_t reg,double scale,bool signed_value){
 uint16_t raw;
 if(panel_fresh(s,reg) && panel_value(s,reg,&raw))
  cJSON_AddNumberToObject(obj,name,(signed_value?(int16_t)raw:(int32_t)raw)/scale);
}

/* USB settings keep the same permissions and independent readback as the panel. */
static bool cdc_profile_confirmed(const panel_snapshot_t *s){
 panel_profile_t profile=panel_effective_profile(s);
 if(!panel_profile_supported(profile))return false;
 return s->detected_profile==PANEL_PROFILE_UNKNOWN ||
  !panel_fresh(s,0x38) || !panel_fresh(s,0x39) ||
  panel_profile_same_family(profile,s->detected_profile);
}

static bool cdc_setting_supported(const panel_snapshot_t *s,uint16_t address){
 if(!cdc_profile_confirmed(s))return false;
 panel_profile_t profile=panel_effective_profile(s);
 if(address==0x4115 && profile!=PANEL_PROFILE_3KU)return false;
 if(address==0x4110 && profile!=PANEL_PROFILE_6KU_PARALLEL && profile!=PANEL_PROFILE_12KU)
  return false;
 return address!=0x4123 && panel_setting_current_address(s,panel_setting_find(address))!=0xffff;
}

static bool cdc_setting_allowed(const panel_snapshot_t *s,uint16_t address,uint16_t value){
 if(address==0x4104 && panel_is_3ku(s) && value==3)return false;
 /* The corresponding readback is only defined in whole hours. */
 if((address==0x4114 || address==0x4115 ||
     (address==0x4119 && !panel_is_3ku(s))) && value%60U)return false;
 if(address==0x4119 && panel_is_3ku(s) && value!=0xfff1 && value!=0xfff2)return false;
 return panel_setting_allowed(s,address,value);
}

static const char hex_digits[]="0123456789abcdef";

static void cdc_available_settings(cJSON *obj,const panel_snapshot_t *s){
 char registers[4U*64U+1U];
 unsigned count=0;
 for(size_t i=0;i<panel_setting_count() && count<64U;i++){
  uint16_t reg=panel_setting(i)->address;
  if(!cdc_setting_supported(s,reg))continue;
  for(unsigned shift=0;shift<4;shift++)
   registers[count*4U+shift]=hex_digits[(reg>>(12U-4U*shift))&0xfU];
  count++;
 }
 registers[count*4U]=0;
 cJSON_AddStringToObject(obj,"settings_registers",registers);
}

static void cdc_setting_options(cJSON *obj,const panel_snapshot_t *s,uint16_t address){
 /* Emit a compact bitmap. Enumerate using the panel's model-aware value rules. */
 char options[226+1]={0};
 unsigned length=0;
 uint16_t value=UINT16_MAX;
 while(panel_setting_step(s,address,value,1,&value)){
  if(!cdc_setting_allowed(s,address,value))continue;
  if(value==0xfff1){cJSON_AddBoolToObject(obj,"option_off",true);continue;}
  if(value==0xfff2){cJSON_AddBoolToObject(obj,"option_unlimited",true);continue;}
  if(value>900)continue;
  options[value/4U]|=1U<<(value%4U);
  length=value/4U+1U;
 }
 for(unsigned i=0;i<length;i++)options[i]=hex_digits[(unsigned)options[i]];
 cJSON_AddStringToObject(obj,"option_bits",options);
}

static uint8_t cdc_power_options(const panel_snapshot_t *s,int *output){
 *output=panel_power_output_state(s);
 if(*output<0 || s->command_busy || s->scanning ||
    !cdc_profile_confirmed(s) ||
    panel_setting_access(s,0x4123)!=PANEL_ACCESS_EDITABLE)return 0;
 uint16_t action=*output?2U:1U;
 return panel_setting_allowed(s,0x4123,action)?action:0;
}

static uint32_t cdc_u32(const uint8_t *p){
 return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;
}

static const char *cdc_submit(cJSON *obj,const panel_snapshot_t *s,uint16_t address,uint16_t value){
 if(ota_service_is_busy())return "BUSY";
 uint32_t sequence;
 panel_command_t command={.type=PANEL_CMD_WRITE,.address=address,.value=value,
   .require_opposite_output=address==0x4123};
 if(!panel_bus_submit_in_context(&command,s->context_generation,&sequence))return "BUSY";
 cJSON_AddNumberToObject(obj,"sequence",sequence);
 return "OK";
}

static const char *panel_cdc(cJSON *obj){
 const uint8_t *p=request.payload;
 panel_snapshot_t s;
 if(!panel_bus_snapshot(&s))return "NOT_READY";
 cJSON_AddNumberToObject(obj,"context",s.context_generation);
 switch(request.command){
 case 16:{
  if(request.length)return "INVALID_ARGUMENT";
  cJSON_AddStringToObject(obj,"product","CS-MB-PANEL");
  cJSON_AddStringToObject(obj,"firmware",esp_app_get_description()->version);
  cJSON_AddStringToObject(obj,"model",panel_profile_name(panel_effective_profile(&s)));
  cJSON_AddStringToObject(obj,"candidate",panel_profile_name(s.detected_profile));
  cJSON_AddBoolToObject(obj,"online",panel_online(&s));
  cJSON_AddNumberToObject(obj,"station",s.config.address);
  cJSON_AddNumberToObject(obj,"baud",s.config.baud);
  cJSON_AddNumberToObject(obj,"parity",s.config.parity);
  cJSON_AddNumberToObject(obj,"errors",s.errors);
  cJSON_AddNumberToObject(obj,"settings_api",3);
  cdc_available_settings(obj,&s);
  int power_output;
  uint8_t power_options=cdc_power_options(&s,&power_output);
  cJSON_AddNumberToObject(obj,"power_options",power_options);
  if(power_output>=0)cJSON_AddNumberToObject(obj,"power_output",power_output);
  cJSON_AddNumberToObject(obj,"uptime_s",s.now_ms/1000U);
  cJSON_AddNumberToObject(obj,"reset_reason",esp_reset_reason());
  static const struct {const char *name;uint16_t reg,scale;bool signed_value;} fields[]={
   {"id38",0x38,1,0},{"id39",0x39,1,0},{"state",0x42,1,0},
   {"grid_v",0x50,10,0},{"grid_a",0x51,100,1},{"grid_hz",0x52,100,0},
   {"output_v",0x58,10,0},{"output_a",0x59,100,1},{"output_hz",0x5a,100,0},
   {"output_w",0x5b,1,1},{"output_va",0x5c,1,0},{"output_pct",0x5d,10,0},
   {"battery_v",0x80,10,0},{"battery_a",0x81,10,1},{"battery_pct",0x84,10,0},{"battery_w",0x85,1,1},
   {"pv1_v",0x96,10,0},{"pv1_a",0x97,100,0},{"pv1_w",0x98,1,0},
   {"pv2_v",0x99,10,0},{"pv2_a",0x9a,100,0},{"pv2_w",0x9b,1,0},
   {"pv_day",0x9c,10,0},{"pv_total",0x9d,10,0},
   {"fault_1",0x46,1,0},{"fault_2",0x47,1,0},{"warn_1",0x48,1,0},{"warn_2",0x49,1,0},
  };
  for(unsigned i=0;i<sizeof(fields)/sizeof(fields[0]);i++){
   if(panel_is_3ku(&s) && fields[i].reg>=0x99 && fields[i].reg<=0x9b)continue;
   cdc_metric(obj,&s,fields[i].name,fields[i].reg,fields[i].scale,fields[i].signed_value);
  }
  cdc_metric(obj,&s,"inverter_c",0x331,panel_is_3ku(&s)?1:10,true);
  bool fresh;int32_t grid;uint32_t pv;
  if(panel_grid_watts(&s,&grid,&fresh) && fresh)cJSON_AddNumberToObject(obj,"grid_w",grid);
  if(panel_pv_watts(&s,&pv,&fresh) && fresh)cJSON_AddNumberToObject(obj,"pv_w",pv);
  return "OK";
 }
 case 19:{
  if(request.length!=4)return "INVALID_ARGUMENT";
  uint32_t sequence=cdc_u32(p);
  const panel_result_t *r=panel_result_find(&s,sequence);
  if(!r)return "PENDING";
  cJSON_AddNumberToObject(obj,"state",r->state);
  cJSON_AddNumberToObject(obj,"exception",r->exception);
  if(r->readback_valid)cJSON_AddNumberToObject(obj,"value",r->readback_value);
  return "OK";
 }
 case 20:{
  if(request.length!=5 || (p[0]!=1 && p[0]!=2))return "INVALID_ARGUMENT";
  if(cdc_u32(p+1)!=s.context_generation)return "CONTEXT_CHANGED";
  if(!panel_online(&s))return "INVERTER_OFFLINE";
  if(s.command_busy || s.scanning)return "BUSY";
  int output;
  if(cdc_power_options(&s,&output)!=p[0])return "POWER_STATE_CHANGED";
  return cdc_submit(obj,&s,0x4123,p[0]);
 }
 default:break;
 }
 bool write=request.command==18;
 if(request.length!=(write?10:2))return "INVALID_ARGUMENT";
 uint16_t address=(uint16_t)(p[0]|p[1]<<8);
 if(!cdc_setting_supported(&s,address))return "SETTING_UNAVAILABLE";
 if(!panel_online(&s))return "INVERTER_OFFLINE";
 uint16_t current;
 if(!cdc_current(&s,address,&current)){
  if(write)return "VALUE_CHANGED";
  const panel_setting_read_t *read=panel_setting_read_find(&s,address);
  if(read && s.now_ms-read->stamp<=PANEL_FRESH_MS && read->state!=PANEL_SETTING_UNREAD){
   if(read->state==PANEL_SETTING_TIMEOUT)return "INVERTER_READ_TIMEOUT";
   return "NO_READBACK";
  }
  panel_bus_request_ble_setting(address);
  return "READING";
 }
 if(!write){
  cJSON_AddNumberToObject(obj,"value",current);
  bool editable=panel_profile_supported(panel_effective_profile(&s)) &&
     panel_setting_access(&s,address)==PANEL_ACCESS_EDITABLE;
  cJSON_AddBoolToObject(obj,"editable",editable);
  char display[64];
  panel_setting_format(&s,address,current,display,sizeof(display));
  cJSON_AddStringToObject(obj,"display",display);
  if(editable)cdc_setting_options(obj,&s,address);
  else cJSON_AddStringToObject(obj,"restriction",panel_setting_restriction(&s,address));
  return "OK";
 }
 uint16_t value=(uint16_t)(p[2]|p[3]<<8),expected=(uint16_t)(p[4]|p[5]<<8);
 uint32_t context=cdc_u32(p+6);
 if(context!=s.context_generation)return "CONTEXT_CHANGED";
 if(current!=expected)return "VALUE_CHANGED";
 if(!panel_profile_supported(panel_effective_profile(&s)) ||
    !cdc_setting_allowed(&s,address,value))return "NOT_EDITABLE";
 if(current==value)return "UNCHANGED";
 return cdc_submit(obj,&s,address,value);
}

static bool cdc_request(void){
 return request.command>=16 && request.command<=20;
}

static void reply_json(const char *payload){
 size_t length=strlen(payload);
 if(length>FACTORY_PAYLOAD_MAX){
  payload="{\"result\":\"RESPONSE_TOO_LARGE\"}";
  length=strlen(payload);
 }
 factory_frame_view_t reply={.command=request.command|0x80,.id=request.id,
    .length=(uint16_t)length,.payload=(const uint8_t*)payload};
 uint8_t *buffer=pc_reply?pc_tx:parser->bytes;
 size_t capacity=pc_reply?sizeof(pc_tx):sizeof(parser->bytes);
 size_t bytes=factory_frame_view_encode(&reply,buffer,capacity);
 if(!bytes){
  static const uint8_t fallback[]="{\"result\":\"RESPONSE_TOO_LARGE\"}";
  reply.payload=fallback;reply.length=sizeof(fallback)-1U;
  bytes=factory_frame_view_encode(&reply,buffer,capacity);
 }
 flockfile(stdout);
 size_t sent=0;
 while(sent<bytes){
  size_t chunk=bytes-sent;
  if(chunk>512U)chunk=512U;
  int n=usb_serial_jtag_write_bytes(buffer+sent,chunk,pdMS_TO_TICKS(500));
  if(n<=0)break;
  sent+=(size_t)n;
 }
 funlockfile(stdout);
}

static void process(void){
 cJSON *obj=cJSON_CreateObject();
 if(!obj){reply_json("{\"result\":\"BUSY\"}");return;}
 const char *result="OK";
 if(request.command==1){inspect(obj);}
 else if(request.command==4){inspect(obj);diagnostics(obj);}
 else if(request.command==7){
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
 }else if(cdc_request())result=panel_cdc(obj);
 else result="UNSUPPORTED";
 char *json=cJSON_AddStringToObject(obj,"result",result)?cJSON_PrintUnformatted(obj):NULL;
 reply_json(json?json:"{\"result\":\"BUSY\"}");
 cJSON_free(json);
 cJSON_Delete(obj);
}
typedef struct {
 TaskHandle_t owner;
 bool binary;
 admin_sn_result_t result;
 char sn[17];
} factory_work_t;
static void factory_worker(void *arg){
 factory_work_t *work=arg;
 if(work->binary)process();
 else serial_result(work->result,work->sn);
 xTaskNotifyGive(work->owner);
 vTaskSuspend(NULL);
}
/* Parsing owns the borrowed request until the worker finishes. Reclaim its
 * full validation stack synchronously before accepting another command. */
static void factory_execute(bool binary,admin_sn_result_t result,const char *sn){
 if(!binary && result==ADMIN_SN_NONE)return;
 /* The ordinary PC path needs no per-request 12 KiB task allocation. */
 if(binary && cdc_request()){process();return;}
 factory_work_t work={.owner=xTaskGetCurrentTaskHandle(),.binary=binary,.result=result};
 if(sn)snprintf(work.sn,sizeof(work.sn),"%s",sn);
 TaskHandle_t worker=NULL;
 if(xTaskCreate(factory_worker,"panel_usb_cmd",12288,&work,2,&worker)!=pdPASS){
  if(binary)reply_json("{\"result\":\"BUSY\"}");
  else printf("[PANEL_USB] BUSY_NO_MEMORY\n");
  return;
 }
 ulTaskNotifyTake(pdTRUE,portMAX_DELAY);
 vTaskDelete(worker);
}
/* Delay the large legacy allocation until the header identifies its command. */
static void serial_byte(uint8_t byte,bool *discard){
 if(parser){
  if(factory_parser_feed_view(parser,byte,&request))factory_execute(true,ADMIN_SN_NONE,NULL);
  if(!parser->used){free(parser);parser=NULL;}
  return;
 }
 if(pc_used || byte==0xc5){
  if(!pc_used)memset(&admin_input,0,sizeof(admin_input));
  if(pc_used==1 && byte!=0x46){pc_used=byte==0xc5?1U:0U;return;}
  pc_rx[pc_used++]=byte;
  if(pc_used==3 && byte!=1){pc_used=0;return;}
  if(pc_used<10)return;
  size_t length=pc_rx[8]|(size_t)pc_rx[9]<<8;
  if(length>FACTORY_PAYLOAD_MAX){pc_used=0;return;}
  if(pc_rx[3]<16 || pc_rx[3]>20 || length>PC_RX_BYTES-14U){
   parser=calloc(1,sizeof(*parser));
   if(!parser){pc_used=0;*discard=true;return;}
   memcpy(parser->bytes,pc_rx,pc_used);parser->used=pc_used;pc_used=0;
   return;
  }
  if(pc_used<length+14U)return;
  if(cdc_u32(pc_rx+10+length)==factory_wire_crc32(pc_rx+2,8+length)){
   request=(factory_frame_view_t){.command=pc_rx[3],.id=cdc_u32(pc_rx+4),
     .length=(uint16_t)length,.payload=pc_rx+10};
   pc_reply=true;process();pc_reply=false;
  }
  pc_used=0;
  return;
 }
 char sn[17]={0};
 admin_sn_result_t result=admin_sn_input_byte(&admin_input,byte,admin_sn_password_valid,sn);
 factory_execute(false,result,sn);
 memset(admin_input.pin,0,sizeof(admin_input.pin));
 memset(admin_input.dsp_bundle,0,sizeof(admin_input.dsp_bundle));
}

static void factory_task(void *arg){
 (void)arg;uint8_t rx[64];int64_t last=0;bool discard=false;
 for(;;){uint32_t tick=(uint32_t)(esp_timer_get_time()/1000);atomic_store(&usb_tick,tick);
  int n=usb_serial_jtag_read_bytes(rx,sizeof(rx),pdMS_TO_TICKS(50));
  int64_t now=esp_timer_get_time();
  if(now-last>1000000){free(parser);parser=NULL;pc_used=0;discard=false;}
  if(n>0){
   last=now;
   for(int i=0;i<n && !discard;i++)serial_byte(rx[i],&discard);
  }
 }
}
bool panel_usb_service_start(void){
 usb_serial_jtag_driver_config_t conf={.tx_buffer_size=1024,.rx_buffer_size=512};
 if(usb_serial_jtag_driver_install(&conf)!=ESP_OK)return false;
 usb_serial_jtag_vfs_use_driver();setvbuf(stdout,NULL,_IONBF,0);
 /* Deep record validation uses an on-demand worker stack. */
 bool started=xTaskCreate(factory_task,"panel_usb",6144,NULL,2,NULL)==pdPASS;
 atomic_store(&usb_started,started);return started;
}
bool panel_usb_service_software_healthy(void){return atomic_load(&usb_started)&&
 (uint32_t)(esp_timer_get_time()/1000)-atomic_load(&usb_tick)<=2000U;}
