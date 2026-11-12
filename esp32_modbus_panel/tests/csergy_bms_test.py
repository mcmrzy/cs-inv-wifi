"""Drive the production CSERGY UART exchange and preference with host stubs."""
from pathlib import Path
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
source = (root / 'full/direct_poll.c').read_text(encoding='utf-8')
preference = source[source.index('panel_bms_protocol_t panel_bms_protocol_get'):source.index('static direct_dsp_status_t s_status')]
poll = source[source.index('#define CSERGY_BMS_ADDRESS'):source.index('\n#endif\n\nstatic uint32_t bms_query_interval')]
fixture = r'''
#include "panel_bms_protocol.h"
#include "direct_protocol.h"
#include "panel_modbus.h"
#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#define ESP_OK 0
#define NVS_READONLY 0
#define NVS_READWRITE 1
#define BMS_PORT 1
#define pdMS_TO_TICKS(ms) (ms)
typedef int nvs_handle_t;
static uint8_t persisted, staged;
static bool has_saved, fail_commit, locked;
static unsigned commits;
static atomic_bool s_quiet, s_iap_busy, s_bms_started;
static atomic_uint s_bms_mode, s_bms_tick;
static atomic_uint s_bms_detection;
static atomic_uint s_bms_failed_mode=UINT32_MAX;
static uint32_t s_bms_last_ms, s_bms_identity_last_ms, s_bms_identity_generation;
static bool s_bms_identity_pending;
static uint8_t s_bms_identity_next, s_bms_identity[4];
static direct_bms_summary_t s_bms_summary;
static bool arm_poll_take_param(void){assert(!locked);locked=true;return true;}
static void arm_poll_give_param(void){assert(locked);locked=false;}
static int nvs_open(const char *name,int mode,nvs_handle_t *h){
    assert(!strcmp(name,"direct_ui"));*h=mode;return ESP_OK;
}
static int nvs_get_u8(nvs_handle_t h,const char *key,uint8_t *value){
    assert(h==NVS_READONLY && !strcmp(key,"bms_protocol"));
    if(!has_saved)return -1;
    *value=persisted;return ESP_OK;
}
static int nvs_set_u8(nvs_handle_t h,const char *key,uint8_t value){
    assert(h==NVS_READWRITE && locked && !strcmp(key,"bms_protocol"));
    staged=value;return ESP_OK;
}
static int nvs_commit(nvs_handle_t h){
    assert(h==NVS_READWRITE && locked);++commits;
    if(fail_commit)return -1;
    persisted=staged;has_saved=true;return ESP_OK;
}
static void nvs_close(nvs_handle_t h){(void)h;}

static uint32_t clock_ms;
static unsigned writes, rx_len, rx_at;
static bool corrupt_status, exception_cells, disabled_limits;
static uint16_t status_words[]={652,975,7000,(uint16_t)-125};
static uint8_t rx[80];
static uint32_t now_ms(void){return clock_ms;}
static void vTaskDelay(unsigned ticks){clock_ms+=ticks;}
static void uart_flush_input(int port){assert(port==BMS_PORT);rx_at=rx_len=0;}
static int uart_write_bytes(int port,const uint8_t *bytes,size_t length){
    assert(port==BMS_PORT && length==8 && bytes[0]==0x10 && (bytes[1]==4 || bytes[1]==3) && panel_mb_crc(bytes,length)==0);
    assert(bytes[4]==0);
    uint16_t words[32]={0};
    unsigned count=bytes[5];
    bool status=bytes[2]==0x10 && bytes[3]==0;
    bool alarms=bytes[2]==0x10 && bytes[3]==0x20;
    bool limits=bytes[2]==0x12 && bytes[3]==0 && bytes[1]==3;
    if(limits){
        assert(count==5);words[0]=!disabled_limits;words[1]=401;words[2]=600;words[3]=576;words[4]=440;
    }else if(status){
        assert(bytes[2]==0x10 && bytes[3]==0 && count==12);
        memcpy(words,status_words,sizeof(status_words));
    }else if(alarms){
        assert(bytes[2]==0x10 && bytes[3]==0x20 && count==4);
        words[1]=1; /* Balancing alone must not raise an alarm. */
    }else{
        assert(bytes[2]==0x11 && bytes[3]==0 && count==32);
        words[0]=3300;words[1]=UINT16_MAX;words[2]=0x7fff;words[3]=0x8000;
        words[4]=8192;words[5]=8191;words[31]=3400;
    }
    memcpy(rx,bytes,8); /* Local echo precedes the split response. */
    uint8_t *response=rx+8;
    response[0]=0x10;response[1]=bytes[1];response[2]=(uint8_t)(count*2);
    unsigned payload=3+count*2;
    for(unsigned i=0;i<count;i++){
        response[3+i*2]=(uint8_t)(words[i]>>8);response[4+i*2]=(uint8_t)words[i];
    }
    if(!status && !alarms && !limits && exception_cells){response[1]=0x84;response[2]=2;payload=3;}
    uint16_t crc=panel_mb_crc(response,payload);
    response[payload]=(uint8_t)crc;response[payload+1]=(uint8_t)(crc>>8);
    if(status && corrupt_status)response[payload]^=1;
    rx_len=8+payload+2;++writes;return (int)length;
}
static int uart_read_bytes(int port,uint8_t *out,size_t capacity,unsigned wait){
    assert(port==BMS_PORT && capacity>=7);
    unsigned count=rx_len-rx_at;
    if(count>7)count=7;
    memcpy(out,rx+rx_at,count);rx_at+=count;clock_ms+=count?1:wait;
    return (int)count;
}
'''
checks = r'''
int main(void){
    bms_protocol_load();assert(panel_bms_protocol_get()==PANEL_BMS_BBJ);
    assert(!panel_bms_detect_start());
    atomic_store(&s_bms_started,true);
    assert(panel_bms_detect_start() && !panel_bms_detect_start());
    assert(!panel_bms_protocol_set(PANEL_BMS_CSERGY) && !commits);
    atomic_store(&s_bms_detection,PANEL_BMS_DETECT_MISSING);
    s_bms_last_ms=123;s_bms_summary.acquired=true;s_bms_identity_pending=true;
    atomic_store(&s_iap_busy,true);
    assert(!panel_bms_protocol_set(PANEL_BMS_CSERGY) && !commits);
    atomic_store(&s_iap_busy,false);
    fail_commit=true;
    assert(!panel_bms_protocol_set(PANEL_BMS_CSERGY) && s_bms_last_ms==123 && s_bms_summary.acquired);
    fail_commit=false;
    assert(panel_bms_protocol_set(PANEL_BMS_CSERGY) && persisted==PANEL_BMS_CSERGY);
    assert(!s_bms_last_ms && !s_bms_summary.acquired && !s_bms_identity_pending && s_bms_identity_generation==1);
    assert(panel_bms_protocol_set(PANEL_BMS_CSERGY) && commits==2);
    atomic_store(&s_bms_mode,0);bms_protocol_load();assert(panel_bms_protocol_get()==PANEL_BMS_CSERGY);
    unsigned mode=atomic_load(&s_bms_mode);
    atomic_store(&s_bms_failed_mode,mode);assert(panel_bms_probe_failed());
    direct_bms_measurement_t sample;
    assert(bms_csergy_poll(mode,&sample) && writes==4);
    assert(sample.soc_percent==652 && sample.soh_percent==975 && sample.percent_divisor==10);
    assert(sample.voltage_10mv==70000 && sample.current_10ma==-1250);
    assert(sample.cell_mv[0]==3300 && sample.cell_mv[1]==0 && sample.cell_mv[31]==3400);
    assert(!sample.cell_mv[2] && !sample.cell_mv[3] && !sample.cell_mv[4] && sample.cell_mv[5]==8191);
    assert(!sample.protection && !sample.warning);
    assert(sample.charge_max_deciamp==401 && sample.charge_max_decivolt==576 && sample.discharge_min_decivolt==440);
    exception_cells=true;disabled_limits=true;
    assert(bms_csergy_poll(mode,&sample) && !sample.cell_mv[0] && !sample.charge_max_deciamp && writes==8);
    corrupt_status=true;
    assert(!bms_csergy_poll(mode,&sample) && writes==9);
    corrupt_status=false;
    const uint16_t invalid[][2]={{0,1001},{0,0x7fff},{1,1001},{1,0xffff},
        {2,0},{2,0x7fff},{2,0x8000},{2,0xffff},{3,0x7fff},{3,0x8000}};
    for(unsigned i=0;i<sizeof(invalid)/sizeof(invalid[0]);i++){
        unsigned field=invalid[i][0];uint16_t old=status_words[field];
        direct_bms_measurement_t saved=sample;unsigned before=writes;
        status_words[field]=invalid[i][1];
        assert(!bms_csergy_poll(mode,&sample) && writes==before+1 && !memcmp(&sample,&saved,sizeof(sample)));
        status_words[field]=old;
    }
    exception_cells=false;status_words[3]=0xffff;
    assert(bms_csergy_poll(mode,&sample) && sample.current_10ma==-10);
    status_words[3]=0;assert(bms_csergy_poll(mode,&sample) && sample.current_10ma==0);
    unsigned before_switch=writes;
    assert(panel_bms_protocol_set(PANEL_BMS_BBJ) && persisted==PANEL_BMS_BBJ);
    assert(!panel_bms_probe_failed());
    assert(!bms_csergy_poll(mode,&sample) && writes==before_switch);
    puts("PASS CSERGY BMS: FC04 RTU, signed scaling, 32 cells, exceptions, invalid CRC, persistent switching and OTA guard");
}
'''
generated = root / 'tmp/csergy_bms_test.c'
exe = generated.with_suffix('.exe')
generated.parent.mkdir(exist_ok=True)
generated.write_text(fixture + preference + poll + checks, encoding='utf-8')
subprocess.run([sys.argv[1], '-std=c11', '-Wall', '-Wextra', '-Werror', '-Ifull', '-Imain',
                '-I../esp32_direct/main/direct', str(generated), 'main/panel_modbus.c', '-o', str(exe)],
               cwd=root, check=True)
subprocess.run([str(exe)], cwd=root, check=True)
