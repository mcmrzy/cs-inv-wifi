"""Compile the real UART-owner/job bridge with real DSP OTA core and boundary stubs.

The BMS codec is deliberately stubbed: this checks its legacy dispatch, while
the separate IAP tests validate its wire protocol. No firmware source is copied
or rewritten; the bridge is extracted verbatim at test execution time.
"""
from pathlib import Path
import os
import re
import shutil
import subprocess

root = Path(__file__).resolve().parents[1]
poll = (root / "main/direct/direct_poll.c").read_text(encoding="utf-8")
link = (root / "main/direct/direct_link.h").read_text(encoding="utf-8")
job = re.search(r"typedef struct \{\s*direct_dsp_control_t operation;.*?\} iap_job_t;", poll, re.S)
control = re.search(r"typedef enum \{DIRECT_DSP_ENTER_BOOT=.*?direct_dsp_control_t;", link)
assert job and control
bridge = poll[poll.index("static uint32_t now_ms(void)"):poll.index("/* Shared enqueue")]
code = r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
/* 生产代码在作业失败时会打印 RX 原始字节; 这里只丢弃, 不参与断言。 */
#define ESP_LOGW(...) ((void)0)
#include "direct_dsp_ota_transport.h"
#include "direct_protocol.h"
typedef struct {bool mutex, held, ready;} semaphore_t;
typedef semaphore_t *SemaphoreHandle_t;
typedef int TaskHandle_t;
typedef int uart_port_t;
typedef int esp_err_t;
#define DSP_PORT 0
#define BMS_PORT 1
#define DSP_BAUD 115200
#define BMS_BAUD 9600
#define pdMS_TO_TICKS(n) (n)
#define portMAX_DELAY UINT32_MAX
#define pdTRUE 1
#define ESP_OK 0
#define ESP_ERR_TIMEOUT 1
CONTROL
JOB
static semaphore_t mutex={.mutex=true};
static SemaphoreHandle_t s_lock=&mutex;
static atomic_bool s_quiet,s_dsp_started,s_bms_started,s_iap_busy,s_dsp_boot;
/* 生产代码里 owner 只记录失败证据(不格式化), 由 main_loop 打印。 */
static struct {bool pending;bool dsp;int result;
 unsigned rx_bytes;uint8_t head[DIRECT_IAP_RX_HEAD_SIZE];uint8_t head_len;direct_iap_trace_t trace;} s_iap_evidence;
static atomic_uint s_dsp_tick,s_bms_tick;
static iap_job_t *s_iap_pending;
static TaskHandle_t s_dsp_owner=2,s_bms_owner=3,current_task=1;
static struct {bool pending;} s_write;
static bool s_dsp_params_importing,s_status_valid,s_config_valid[84];
static struct {bool connected;} g_arm_stat;
static uint32_t s_status_last_ms,s_measurement_last_ms,s_bms_last_ms;
static uint8_t s_dsp_sync_pending;
static uint32_t tick;
static uint8_t image[1100],rx[64];
static size_t rx_size,rx_offset;
static unsigned hr_tx,erase_tx,data_tx,eot_tx,legacy_runs,progress_calls;
static bool fail_hr,fail_erase,fail_data,noisy_data,fail_eot,fail_drain,fail_alloc,fail_read;
static bool iap_owner_step(direct_iap_target_t target);
static void peer_reply(const uint8_t *bytes,size_t size){
    assert(rx_offset==rx_size);memcpy(rx,bytes,size);rx_size=size;rx_offset=0;
}
static int64_t esp_timer_get_time(void){return (int64_t)tick*1000;}
static TaskHandle_t xTaskGetCurrentTaskHandle(void){return current_task;}
static SemaphoreHandle_t xSemaphoreCreateBinary(void){
    if(fail_alloc)return NULL;
    return calloc(1,sizeof(semaphore_t));
}
static void vSemaphoreDelete(SemaphoreHandle_t semaphore){assert(!semaphore->mutex);free(semaphore);}
static void xSemaphoreGive(SemaphoreHandle_t semaphore){
    if(semaphore->mutex){assert(semaphore->held);semaphore->held=false;}
    else {assert(current_task==2||current_task==3);semaphore->ready=true;}
}
static int xSemaphoreTake(SemaphoreHandle_t semaphore,unsigned wait){
    (void)wait;
    if(semaphore->mutex){assert(!semaphore->held);semaphore->held=true;return pdTRUE;}
    assert(current_task==1);
    if(!semaphore->ready){
        assert(s_iap_pending && !mutex.held && atomic_load(&s_quiet));
        direct_iap_target_t target=s_iap_pending->image.target;
        current_task=target==DIRECT_IAP_DSP?2:3;
        assert(iap_owner_step(target));current_task=1;
    }
    assert(semaphore->ready);semaphore->ready=false;return pdTRUE;
}
static void vTaskDelay(unsigned ms){assert(!mutex.held);tick+=ms;}
static esp_err_t uart_wait_tx_done(uart_port_t port,unsigned wait){
    (void)port;(void)wait;assert(current_task==2||current_task==3);
    return fail_drain?2:ESP_OK;
}
static int uart_flush_input(uart_port_t port){
    (void)port;assert(current_task==2||current_task==3);rx_offset=rx_size=0;return 0;
}
static int uart_write_bytes(uart_port_t port,const uint8_t *bytes,size_t size){
    assert(!mutex.held && atomic_load(&s_quiet));
    assert(current_task==(port==DSP_PORT?2:3));
    static const uint8_t ack[]={1,0x10,4,0x38,0,2,4,0,0,0,0x11,3,0xd1,'C','R'};
    if(port==BMS_PORT){assert(size==1 && bytes[0]==0xfc);return (int)size;}
    assert(atomic_load(&s_dsp_boot));
    if(size==9 && bytes[2]==0x42){hr_tx++;if(!fail_hr)peer_reply((const uint8_t *)"HR",2);}
    else if(size==13 && bytes[2]==4){erase_tx++;
        if(!fail_erase)peer_reply(ack,sizeof(ack));
        else if(erase_tx==1)peer_reply((const uint8_t *)"t",1);}
    else if(size==6 && bytes[2]==0xff){eot_tx++;if(fail_eot)return -1;}
    else {assert(bytes[0]==2);data_tx++;
        if(!fail_data)peer_reply((const uint8_t *)"CR",2);
        else if(noisy_data){uint8_t noise[32];memset(noise,0xa6,sizeof(noise));peer_reply(noise,sizeof(noise));}}
    return (int)size;
}
static int uart_read_bytes(uart_port_t port,uint8_t *bytes,size_t size,unsigned wait){
    (void)port;assert(!mutex.held && (current_task==2||current_task==3));
    if(rx_offset==rx_size){tick+=wait;return 0;}
    size_t n=rx_size-rx_offset;if(n>size)n=size;
    memcpy(bytes,rx+rx_offset,n);rx_offset+=n;tick++;return (int)n;
}
/* Legacy BMS is a dispatch spy, not a replacement implementation test. */
direct_iap_result_t direct_iap_run_legacy(const direct_iap_image_t *m,const direct_iap_io_t *io){
    assert(m->target==DIRECT_IAP_BMS && current_task==3 && !mutex.held);
    legacy_runs++;uint8_t command=0xfc;
    return io->send(io->context,&command,1)?DIRECT_IAP_OK:DIRECT_IAP_TX_FAILED;
}
BRIDGE
static bool source_read(void *ctx,size_t offset,uint8_t *out,size_t size){
    assert(ctx==image && current_task==2 && !mutex.held);
    assert(offset+size<=sizeof(image));if(fail_read)return false;
    memcpy(out,image+offset,size);return true;
}
static void source_progress(void *ctx,size_t done,size_t total){
    assert(ctx==image && current_task==1 && !mutex.held && done==total);progress_calls++;
}
static void reset(void){
    assert(!mutex.held && !s_iap_pending && current_task==1);
    atomic_store(&s_quiet,false);atomic_store(&s_dsp_started,true);atomic_store(&s_bms_started,true);
    atomic_store(&s_iap_busy,false);atomic_store(&s_dsp_boot,false);
    s_write.pending=false;s_dsp_params_importing=false;
    g_arm_stat.connected=true;s_status_valid=true;s_status_last_ms=123;s_measurement_last_ms=124;
    s_bms_last_ms=125;memset(s_config_valid,1,sizeof(s_config_valid));s_dsp_sync_pending=0;
    hr_tx=erase_tx=data_tx=eot_tx=legacy_runs=progress_calls=0;rx_size=rx_offset=0;tick=100;
    fail_hr=fail_erase=fail_data=noisy_data=fail_eot=fail_drain=fail_alloc=fail_read=false;
    memset(&s_iap_evidence,0,sizeof(s_iap_evidence));
    memset(image,0xa6,sizeof(image));image[0]=0;image[1]=0x48;image[4]=image[5]=0x5a;
}
static void assert_idle(bool held){
    assert(!atomic_load(&s_iap_busy) && !atomic_load(&s_quiet) && !s_iap_pending && !mutex.held);
    assert(direct_dsp_boot_active()==held);
}
static void assert_freshness_reset(void){
    assert(!g_arm_stat.connected && !s_status_valid && !s_status_last_ms && !s_measurement_last_ms);
    for(unsigned i=0;i<84;i++)assert(!s_config_valid[i]);
    assert(s_dsp_sync_pending==7);
}
int main(void){
    direct_iap_image_t m={.target=DIRECT_IAP_DSP,.size=64};
    direct_iap_io_t io={.read_image=source_read,.progress=source_progress,.context=image};
    reset();image[4]=0;
    assert(direct_link_run_iap(&m,&io)==DIRECT_IAP_IMAGE_MAGIC_INVALID);
    assert(!hr_tx && !erase_tx && !data_tx && !eot_tx);assert_idle(false);
    reset();m.size=65;assert(direct_link_run_iap(&m,&io)==DIRECT_IAP_IMAGE_SIZE_INVALID);
    assert(!hr_tx && !erase_tx);assert_idle(false);m.size=64;
    reset();fail_read=true;assert(direct_link_run_iap(&m,&io)==DIRECT_IAP_READ_FAILED);
    assert(!hr_tx && !erase_tx);assert_idle(false);
    reset();assert(direct_link_dsp_control(DIRECT_DSP_ENTER_BOOT)==DIRECT_IAP_OK);
    assert(hr_tx==1 && !erase_tx && !eot_tx);assert_idle(true);assert_freshness_reset();
    assert(direct_link_dsp_control(DIRECT_DSP_EXIT_BOOT)==DIRECT_IAP_OK);
    assert(hr_tx==1 && eot_tx==1 && !erase_tx);assert_idle(false);assert_freshness_reset();
    reset();assert(direct_link_dsp_control(DIRECT_DSP_REBOOT)==DIRECT_IAP_OK);
    assert(hr_tx==1 && eot_tx==1 && !erase_tx && !data_tx);assert_idle(false);assert_freshness_reset();
    reset();fail_hr=true;assert(direct_link_dsp_control(DIRECT_DSP_ENTER_BOOT)==DIRECT_IAP_ACK_FAILED);
    /* 等 HR 预算 = 250ms × 12(覆盖 bootloader 上电 3 秒窗口) */
    assert(hr_tx==12 && !erase_tx && !eot_tx);assert_idle(true);
    fail_hr=false;assert(direct_link_dsp_control(DIRECT_DSP_EXIT_BOOT)==DIRECT_IAP_OK);assert_idle(false);
    reset();fail_eot=true;assert(direct_link_dsp_control(DIRECT_DSP_REBOOT)==DIRECT_IAP_TX_FAILED);
    assert(hr_tx==1 && eot_tx==1);assert_idle(true);
    reset();assert(direct_link_run_iap(&m,&io)==DIRECT_IAP_OK);
    assert(hr_tx==1 && erase_tx==1 && data_tx==1 && eot_tx==1 && progress_calls==1);
    assert_idle(false);assert_freshness_reset();
    reset();fail_erase=true;assert(direct_link_run_iap(&m,&io)==DIRECT_IAP_ACK_FAILED);
    /* HR succeeded; t arrived while waiting for erase ACK, before recovery EOT. */
    assert(s_iap_evidence.pending && s_iap_evidence.rx_bytes==3);
    assert(s_iap_evidence.trace.phase==DIRECT_IAP_PHASE_ERASE_ACK);
    assert(s_iap_evidence.trace.tx_frames==5 && s_iap_evidence.trace.tx_len==13);
    assert(s_iap_evidence.trace.rx_bytes==1 && s_iap_evidence.trace.rx_head[0]=='t');
    assert(!data_tx && eot_tx==1);assert_idle(false);
    reset();fail_data=true;assert(direct_link_run_iap(&m,&io)==DIRECT_IAP_ACK_FAILED);
    /* HR + full erase ACK + CR = 17 bytes can mean DATA ACK timeout. */
    static const uint8_t erase_completed[]={
        'H','R',1,0x10,4,0x38,0,2,4,0,0,0,0x11,3,0xd1,'C','R'};
    assert(s_iap_evidence.rx_bytes==sizeof(erase_completed));
    assert(s_iap_evidence.head_len==sizeof(erase_completed));
    assert(!memcmp(s_iap_evidence.head,erase_completed,sizeof(erase_completed)));
    assert(s_iap_evidence.trace.phase==DIRECT_IAP_PHASE_DATA_ACK);
    assert(s_iap_evidence.trace.tx_frames==10 && !s_iap_evidence.trace.rx_bytes);
    /* 传输失败后必须回到正常业务: 发 EOT 结束 Boot 会话并清掉挂起。 */
    assert(data_tx==10 && eot_tx==1);assert_idle(false);
    reset();fail_data=noisy_data=true;
    assert(direct_link_run_iap(&m,&io)==DIRECT_IAP_ACK_FAILED);
    assert(s_iap_evidence.rx_bytes==17+10*32 && s_iap_evidence.head_len==24);
    assert(!memcmp(s_iap_evidence.head,erase_completed,sizeof(erase_completed)));
    for(unsigned i=17;i<24;i++)assert(s_iap_evidence.head[i]==0xa6);
    assert(s_iap_evidence.trace.rx_bytes==10*32 && s_iap_evidence.trace.rx_head_len==24);
    for(unsigned i=0;i<24;i++)assert(s_iap_evidence.trace.rx_head[i]==0xa6);
    puts("PASS IAP RX diagnostics: exact 17-byte erase completion survives DATA_ACK timeout; total/stage prefixes cap at 24 without truncating byte counts");
    reset();s_write.pending=true;assert(direct_link_run_iap(&m,&io)==DIRECT_IAP_BUSY);
    assert(!hr_tx && !erase_tx);assert_idle(false);
    reset();s_dsp_params_importing=true;assert(direct_link_dsp_control(DIRECT_DSP_REBOOT)==DIRECT_IAP_BUSY);assert_idle(false);
    reset();atomic_store(&s_iap_busy,true);assert(direct_link_run_iap(&m,&io)==DIRECT_IAP_BUSY);
    assert(atomic_load(&s_iap_busy) && !hr_tx);atomic_store(&s_iap_busy,false);
    reset();current_task=s_dsp_owner;assert(direct_link_dsp_control(DIRECT_DSP_REBOOT)==DIRECT_IAP_INCOMPATIBLE);current_task=1;assert_idle(false);
    reset();current_task=s_bms_owner;assert(direct_link_run_iap(&m,&io)==DIRECT_IAP_INCOMPATIBLE);current_task=1;assert_idle(false);
    reset();atomic_store(&s_dsp_started,false);assert(direct_link_dsp_control(DIRECT_DSP_ENTER_BOOT)==DIRECT_IAP_NOT_READY);assert_idle(false);
    reset();fail_alloc=true;assert(direct_link_dsp_control(DIRECT_DSP_ENTER_BOOT)==DIRECT_IAP_NO_MEMORY);assert_idle(false);
    reset();fail_drain=true;assert(direct_link_run_iap(&m,&io)==DIRECT_IAP_TX_FAILED);
    assert(!hr_tx && !erase_tx);assert_idle(false);
    reset();atomic_store(&s_quiet,true);assert(direct_link_dsp_control(DIRECT_DSP_REBOOT)==DIRECT_IAP_OK);
    assert(atomic_load(&s_quiet) && !atomic_load(&s_iap_busy));atomic_store(&s_quiet,false);
    reset();m.target=DIRECT_IAP_BMS;assert(direct_link_run_iap(&m,&io)==DIRECT_IAP_OK);
    assert(legacy_runs==1 && !hr_tx && !erase_tx && !data_tx && !eot_tx && s_bms_last_ms==0);
    assert(g_arm_stat.connected && s_measurement_last_ms==124 && !s_dsp_sync_pending);assert_idle(false);
    reset();assert(direct_link_dsp_control(0)==DIRECT_IAP_INCOMPATIBLE);
    assert(direct_link_dsp_control(4)==DIRECT_IAP_INCOMPATIBLE);assert_idle(false);
    puts("PASS DSP OTA actual owner + submission + real transport: preflight before commands, HR/EOT controls, failed transfer exits Boot and resumes normal business, UART exclusivity, freshness reset + sync7, pending/concurrent rejection, BMS legacy dispatch");
}
'''.replace("CONTROL", control.group(0)).replace("JOB", job.group(0)).replace("BRIDGE", bridge)
tmp = root / "tmp"
tmp.mkdir(exist_ok=True)
source = tmp / "dsp_ota_owner_test.c"
source.write_text(code, encoding="utf-8")
gcc = os.environ.get("HOST_GCC") or shutil.which("gcc") or str(Path(
    r"C:\Users\29538\AppData\Local\Microsoft\WinGet\Packages\BrechtSanders.WinLibs.POSIX.UCRT_Microsoft.Winget.Source_8wekyb3d8bbwe\mingw64\bin\gcc.exe"))
exe = tmp / "dsp_ota_owner_test.exe"
subprocess.run([gcc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-Imain/direct",
                str(source), "main/direct/direct_dsp_ota_transport.c",
                "main/direct/direct_protocol.c", "-o", str(exe)], cwd=root, check=True)
subprocess.run([str(exe)], cwd=root, check=True)
