#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

#define DSP_PORT 0
#define BMS_PORT 1
#define DSP_UART_EVENT_QUEUE_LENGTH 16
#define UART_DATA_8_BITS 8
#define UART_PARITY_DISABLE 0
#define UART_STOP_BITS_1 1
#define UART_HW_FLOWCTRL_DISABLE 0
#define UART_SCLK_DEFAULT 0
#define UART_PIN_NO_CHANGE -1
#define UART_MODE_RS485_HALF_DUPLEX 1
#define GPIO_PULLUP_ONLY 1
#define ESP_OK 0
#define ESP_LOGE(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
typedef int esp_err_t;
typedef int uart_port_t;
typedef struct { int baud_rate,data_bits,parity,stop_bits,flow_ctrl,source_clk; } uart_config_t;
static void *s_dsp_uart_events;
static unsigned pullups, deletions, stage;
static unsigned fail_stage;
static int current_port;

int uart_driver_install(int port,int rx,int tx,int count,void **queue,int flags) {
    assert(rx==1024 && flags==0);current_port=port;stage++;
    /* DSP control frames must reach hardware FIFO without a TX ring refill. */
    assert(tx==(port==DSP_PORT?0:8192));
    assert(count==(port==DSP_PORT?16:0));
    if(queue){assert(port==DSP_PORT);*queue=(void *)7;}
    return fail_stage==stage?-1:0;
}
int uart_param_config(int port,const uart_config_t *cfg) {
    assert(port==current_port && cfg->data_bits==8 && !cfg->parity);
    assert(cfg->stop_bits==1 && !cfg->flow_ctrl);stage++;
    assert(cfg->baud_rate==(port==DSP_PORT?115200:9600));
    return fail_stage==stage?-1:0;
}
int uart_set_pin(int port,int tx,int rx,int de,int cts) {
    assert(port==current_port && cts==-1 && de==-1);
    assert(tx==(port==DSP_PORT?1:21) && rx==(port==DSP_PORT?0:20));stage++;
    return fail_stage==stage?-1:0;
}
int gpio_set_pull_mode(int pin,int mode) {
    assert(current_port==DSP_PORT && pin==0 && mode==GPIO_PULLUP_ONLY);
    stage++;pullups++;return fail_stage==stage?-1:0;
}
int uart_set_mode(int port,int mode) {(void)port;(void)mode;assert(false);return 0;}
int uart_driver_delete(int port) {assert(port==current_port);deletions++;return 0;}
const char *esp_err_to_name(int code) {(void)code;return "test";}

#include "../tmp/uart_setup_under_test.inc"

static void reset(void) {pullups=deletions=stage=fail_stage=0;s_dsp_uart_events=0;}
int main(void) {
    reset();assert(uart_setup(DSP_PORT,1,0,115200,-1));
    assert(pullups==1 && stage==4 && s_dsp_uart_events==(void *)7 && !deletions);
    reset();assert(uart_setup(1,21,20,9600,-1));assert(!pullups && stage==3);
    for(unsigned i=1;i<=4;i++){
        reset();fail_stage=i;assert(!uart_setup(DSP_PORT,1,0,115200,-1));
        assert(stage==i && deletions==1 && !s_dsp_uart_events);
    }
    reset();assert(!uart_setup(DSP_PORT,-1,0,115200,-1));assert(!stage);
    puts("PASS actual UART setup: DSP RX pullup after routing; original 115200 8N1; BMS unchanged; every SDK failure cleans driver/queue");
    return 0;
}
