"""Check the production USB request handler against the real panel model."""
from pathlib import Path
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
service = (root / 'full/factory_service.c').read_text()
handler = service[service.index('static bool cdc_current('):service.index('static bool cdc_request(void)')]
parser = service[service.index('static void serial_byte('):service.index('static void factory_task(')]
reply = service[service.index('static void reply_json('):service.index('static void process(void)')]
dispatch = service[service.index('static void process(void)'):service.index('} factory_work_t;')]
for command in (2, 3, 6, 8):
    assert f'request.command=={command})' not in dispatch
assert 'else if(cdc_request())result=panel_cdc(obj);' in dispatch
assert 'if(binary && cdc_request()){process();return;}' in service
assert 'request.command>=16 && request.command<=20' in service

stub = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "panel_catalog.h"
#include "panel_control.h"
#include "factory_wire.h"

typedef struct {const char *key; double value;} field_t;
typedef struct {field_t fields[40]; unsigned count; char options[227];char registers[257];} cJSON;
static void cJSON_AddNumberToObject(cJSON *o,const char *key,double value)
{ o->fields[o->count++]=(field_t){key,value}; }
static void cJSON_AddStringToObject(cJSON *o,const char *key,const char *value)
{
 if(!strcmp(key,"option_bits"))snprintf(o->options,sizeof(o->options),"%s",value);
 if(!strcmp(key,"settings_registers"))snprintf(o->registers,sizeof(o->registers),"%s",value);
}
static void cJSON_AddBoolToObject(cJSON *o,const char *key,bool value)
{ cJSON_AddNumberToObject(o,key,value); }
static double value(const cJSON *o,const char *key)
{ for(unsigned i=0;i<o->count;i++)if(!strcmp(o->fields[i].key,key))return o->fields[i].value;
  assert(!"missing response field");return 0; }

static factory_frame_view_t request;
static panel_snapshot_t live;
typedef struct {const char *version;} app_info_t;
static const app_info_t *esp_app_get_description(void)
{ static const app_info_t app={"0.4.78-modbus-full"};return &app; }
static int esp_reset_reason(void){return 1;}
static unsigned submitted,requested;
static bool busy;
static bool panel_bus_snapshot(panel_snapshot_t *out){*out=live;return true;}
static bool ota_service_is_busy(void){return busy;}
static void panel_bus_request_ble_setting(uint16_t reg){assert(reg==0x4101);requested++;}
static bool panel_bus_submit_in_context(const panel_command_t *c,uint32_t context,uint32_t *seq)
{
 assert(context==7 && c->type==PANEL_CMD_WRITE);
 if(c->address==0x4123)assert(c->value==2 && c->require_opposite_output);
 else assert(c->address==0x4101 && c->value==1 && !c->require_opposite_output);
 *seq=48+submitted;submitted++;return true;
}
'''
parser_stub = r'''
#define PC_RX_BYTES 24U
#define PC_TX_BYTES 1400U
static uint8_t pc_rx[PC_RX_BYTES];
static uint8_t pc_tx[PC_TX_BYTES], wire[FACTORY_FRAME_MAX];
static size_t wire_bytes;
static bool stdout_locked;
#define pdMS_TO_TICKS(ms) (ms)
#define flockfile(stream) (stdout_locked = true)
#define funlockfile(stream) (stdout_locked = false)
static int usb_serial_jtag_write_bytes(const void *buffer,size_t size,unsigned wait)
{
 assert(stdout_locked && size<=512 && wait==500);
 if(size>300)size=300;
 assert(wire_bytes+size<=sizeof(wire));
 memcpy(wire+wire_bytes,buffer,size);wire_bytes+=size;
 return (int)size;
}
static size_t pc_used;
static bool pc_reply;
static factory_parser_t *parser;
static struct {char pin[8],dsp_bundle[32];} admin_input;
typedef int admin_sn_result_t;
#define ADMIN_SN_NONE 0
#define admin_sn_password_valid 0
#define admin_sn_input_byte(state,byte,valid,sn) (0)
static unsigned pc_count, legacy_count, allocation_calls;
static void *test_calloc(size_t count,size_t size)
{ (void)count;(void)size;allocation_calls++;return NULL; }
#define calloc test_calloc
static void process(void)
{
 assert(pc_reply && request.command==17 && request.id==5);
 assert(request.length==2 && request.payload[0]==2 && request.payload[1]==65);
 pc_count++;
}
static void factory_execute(bool binary,admin_sn_result_t result,const char *sn)
{ (void)result;(void)sn;if(binary)legacy_count++; }
'''
checks = r'''
static void check(unsigned command,const uint8_t *bytes,unsigned length,const char *expected,cJSON *out)
{
 *out=(cJSON){0};request.command=command;request.length=length;request.payload=bytes;
 assert(!strcmp(panel_cdc(out),expected));
}
int main(void)
{
 char large_reply[1250];
 memset(large_reply,'a',sizeof(large_reply)-1);large_reply[sizeof(large_reply)-1]='\0';
 pc_reply=true;request.command=16;request.id=123;
 reply_json(large_reply);
 factory_parser_t receive_parser={0};factory_frame_view_t decoded={0};
 for(size_t i=0;i<wire_bytes;i++)assert(factory_parser_feed_view(&receive_parser,wire[i],&decoded)==(i+1==wire_bytes));
 assert(!stdout_locked && decoded.length==sizeof(large_reply)-1 && decoded.id==123 && decoded.command==(16|0x80));
 assert(!memcmp(decoded.payload,large_reply,decoded.length));
 pc_reply=false;

 uint8_t short_frame[PC_RX_BYTES],read_bytes[]={2,65};
 factory_frame_view_t packet={.command=17,.id=5,.length=2,.payload=read_bytes};
 size_t bytes=factory_frame_view_encode(&packet,short_frame,sizeof(short_frame));
 bool discard=false;
 for(size_t i=0;i<bytes;i++)serial_byte(short_frame[i],&discard);
 assert(pc_count==1 && !discard && !allocation_calls && !pc_used);
 short_frame[bytes-1]^=1;
 for(size_t i=0;i<bytes;i++)serial_byte(short_frame[i],&discard);
 assert(pc_count==1 && !discard && !allocation_calls && !pc_used);
 packet.command=1;packet.length=0;
 bytes=factory_frame_view_encode(&packet,short_frame,sizeof(short_frame));
 for(size_t i=0;i<bytes && !discard;i++)serial_byte(short_frame[i],&discard);
 assert(allocation_calls==1 && discard && !legacy_count);
 live=(panel_snapshot_t){.now_ms=110,.context_generation=7,.config={1,9600,0,PANEL_PROFILE_3KU}};
 panel_store(&live,0x42,3,110);panel_store(&live,0x53,110,110);panel_store(&live,0x5e,0,110);
 panel_store(&live,0x5b,0,110);panel_store(&live,0x81,(uint16_t)-42,110);
 panel_store(&live,0x46,16,110);
 cJSON out;
 check(16,NULL,0,"OK",&out);
 assert(value(&out,"online")==1 && value(&out,"grid_w")==-110 && value(&out,"output_w")==0);
 assert(value(&out,"battery_a")==-4.2 && value(&out,"fault_1")==16 && value(&out,"settings_api")==3);
 assert(strstr(out.registers,"4115") && !strstr(out.registers,"4110"));
 assert(!strstr(out.registers,"4123") && !strstr(out.registers,"4124"));
 const uint8_t read[]={1,65};
 check(17,read,2,"READING",&out);assert(requested==1);
 panel_store(&live,0x2a1,5000,110);
 check(17,read,2,"OK",&out);assert(value(&out,"value")==0 && value(&out,"editable")==1);
 assert(!strcmp(out.options,"3"));
 const uint8_t voltage[]={0,65};
 panel_store(&live,0x2a0,2300,110);
 check(17,voltage,2,"OK",&out);
 assert(value(&out,"value")==2 && !strcmp(out.options,"f"));
 const uint8_t unsafe[]={0x23,65};
 check(17,unsafe,2,"SETTING_UNAVAILABLE",&out);
 uint8_t write[]={1,65,1,0,0,0,7,0,0,0};
 write[6]=6;check(18,write,10,"CONTEXT_CHANGED",&out);
 write[6]=7;write[4]=1;check(18,write,10,"VALUE_CHANGED",&out);
 write[4]=0;busy=true;check(18,write,10,"BUSY",&out);busy=false;
 assert(submitted==0);check(18,write,10,"OK",&out);
 assert(submitted==1 && value(&out,"sequence")==48);
 const uint8_t seq[]={48,0,0,0};
 check(19,seq,4,"PENDING",&out);
 panel_result_t *r=panel_result_record(&live,48,PANEL_WRITE_CONFIRMED,0);
 r->readback_valid=true;r->readback_value=1;
 check(19,seq,4,"OK",&out);assert(value(&out,"state")==5 && value(&out,"value")==1);
 const uint8_t stop[]={2,7,0,0,0},start[]={1,7,0,0,0};
 check(20,stop,5,"POWER_STATE_CHANGED",&out);
 panel_store(&live,0x58,2300,110);
 check(16,NULL,0,"OK",&out);
 assert(value(&out,"power_output")==1 && value(&out,"power_options")==2);
 check(20,start,5,"POWER_STATE_CHANGED",&out);
 panel_store(&live,0x38,0x60,110);panel_store(&live,0x39,0x60,110);
 live.detected_profile=PANEL_PROFILE_2KU;
 check(20,stop,5,"POWER_STATE_CHANGED",&out);
 check(16,NULL,0,"OK",&out);assert(!*out.registers);
 live.detected_profile=PANEL_PROFILE_3KU;
 busy=true;check(20,stop,5,"BUSY",&out);busy=false;
 assert(submitted==1);
 check(20,stop,5,"OK",&out);assert(submitted==2 && value(&out,"sequence")==49);
 live.config.profile_override=PANEL_PROFILE_6KU_PARALLEL;
 live.detected_profile=PANEL_PROFILE_UNKNOWN;
 check(16,NULL,0,"OK",&out);
 assert(strstr(out.registers,"4110") && !strstr(out.registers,"4115"));
 live.now_ms+=PANEL_FRESH_MS+1;
 check(18,write,10,"INVERTER_OFFLINE",&out);assert(submitted==2);
 puts("PASS USB CDC: snapshot, one-shot parameter query, context/value guards and one queued write with matching receipt");
}
'''
path=root/'tmp/usb_pc_test.c'
path.parent.mkdir(exist_ok=True)
path.write_text(stub+handler+parser_stub+parser+reply+checks)
exe=path.with_suffix('.exe')
factory=root.parent/'esp32_direct/main/factory'
subprocess.run([sys.argv[1],'-std=c11','-Wall','-Wextra','-Werror','-I'+str(root/'main'),'-I'+str(factory),str(path),
 str(root/'main/panel_catalog.c'),str(root/'main/panel_model.c'),str(root/'main/panel_control.c'),str(factory/'factory_wire.c'),'-o',str(exe)],check=True)
subprocess.run([str(exe)],check=True)
