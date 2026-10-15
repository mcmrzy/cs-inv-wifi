#include "direct_iap.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
typedef int uart_port_t;
typedef int esp_err_t;
typedef struct {direct_iap_image_t image;direct_iap_trace_t trace;} iap_job_t;
#define DSP_PORT 0
#define BMS_PORT 1
#define DSP_BAUD 115200
#define BMS_BAUD 9600
#define ESP_OK 0
#define ESP_ERR_TIMEOUT 1
#define pdMS_TO_TICKS(ms) (ms)
static unsigned tick,writes,beats,waits,complete_at;
static int write_result=-1,wait_error;
static size_t expected_n;
static uint32_t now_ms(void){return tick;}
static uart_port_t iap_port(const iap_job_t *job){return job->image.target==DIRECT_IAP_DSP?DSP_PORT:BMS_PORT;}
static void iap_heartbeat(const iap_job_t *job){(void)job;beats++;}
static int uart_write_bytes(uart_port_t port,const void *bytes,size_t n){
 assert(port==BMS_PORT||port==DSP_PORT);assert(bytes&&n==expected_n);writes++;
 return write_result<0?(int)n:write_result;
}
static esp_err_t uart_wait_tx_done(uart_port_t port,unsigned ms){
 (void)port;assert(ms<=50);waits++;tick+=ms;
 if(wait_error)return wait_error;
 return tick>=complete_at?ESP_OK:ESP_ERR_TIMEOUT;
}
#include "../tmp/iap_uart_under_test.inc"
int main(void){
 uint8_t bytes[4106]={0};iap_job_t job={.image={.target=DIRECT_IAP_BMS}};
 expected_n=sizeof(bytes);complete_at=4300;
 assert(iap_send(&job,bytes,sizeof(bytes))&&writes==1&&waits>100&&beats>=waits);
 tick=0;writes=waits=beats=0;complete_at=100000;
 assert(!iap_send(&job,bytes,sizeof(bytes))&&writes==1&&tick<5000);
 tick=0;writes=waits=beats=0;wait_error=2;
 assert(!iap_send(&job,bytes,sizeof(bytes))&&writes==1&&waits==1);
 wait_error=0;write_result=0;writes=waits=0;
 assert(!iap_send(&job,bytes,sizeof(bytes))&&writes==1&&!waits);
 write_result=-1;tick=0;writes=waits=0;job.image.target=DIRECT_IAP_DSP;
 expected_n=1029;complete_at=100;
 assert(iap_send(&job,bytes,1029)&&writes==1&&waits);
 assert(job.trace.tx_len==1029 && job.trace.tx_max_ms>=100);
 /* Control frames are written once and waited to completion too. */
 const unsigned control_sizes[]={9,13};
 for(unsigned i=0;i<2;i++){
  tick=0;writes=waits=0;expected_n=control_sizes[i];complete_at=2;
  assert(iap_send(&job,bytes,expected_n)&&writes==1&&waits);
 }
 puts("PASS real UART sender: one complete frame, BMS wire-time, heartbeat, bounded timeout, TX failures");
}
