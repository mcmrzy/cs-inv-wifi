#define DIAG_HOST_TEST 1
#include "../main/main.c"
#include <assert.h>

uint32_t mock_ms;
static config_t wire;
static unsigned reset_mask,writes;
static uint8_t pending[1024],last_tx[256];
static size_t pending_used,pending_at,last_tx_size;
static char captured[131072];
static size_t captured_used;
static uint32_t cancel_at;
static bool cancel_sent,echo_mode,silent_mode;
static const char *usb_script;
static size_t usb_at;
static bool flood_rx;
static unsigned flood_calls;

int64_t esp_timer_get_time(void){return (int64_t)mock_ms*1000;}
int usb_serial_jtag_write_bytes(const void *p,size_t n,unsigned t)
{
    (void)t;if(captured_used+n<sizeof(captured)){
        memcpy(captured+captured_used,p,n);captured_used+=n;captured[captured_used]=0;
    }
    return (int)n;
}
int usb_serial_jtag_read_bytes(void *p,uint32_t n,unsigned wait)
{
    mock_ms+=wait;
    if(cancel_at && mock_ms>=cancel_at && !cancel_sent){*(uint8_t *)p=3;cancel_sent=true;return 1;}
    if(usb_script && usb_script[usb_at]){
        size_t size=strlen(usb_script+usb_at);if(size>n)size=n;
        memcpy(p,usb_script+usb_at,size);usb_at+=size;return (int)size;
    }
    return 0;
}
esp_err_t usb_serial_jtag_driver_install(usb_serial_jtag_driver_config_t *c){(void)c;return ESP_OK;}
esp_err_t usb_serial_jtag_wait_tx_done(unsigned n){(void)n;return ESP_OK;}
int xQueueReceive(QueueHandle_t q,void *p,unsigned t){(void)q;(void)p;(void)t;return pdFALSE;}
void vTaskDelay(unsigned n){mock_ms+=n;}
int gpio_get_level(gpio_num_t n){(void)n;return 1;}
esp_err_t gpio_reset_pin(gpio_num_t n){reset_mask|=1U<<n;return ESP_OK;}
esp_err_t uart_wait_tx_done(int p,unsigned t){(void)p;(void)t;return ESP_OK;}
static uart_config_t actual_uart;
static unsigned actual_tx,actual_rx;
static uint32_t actual_inversion;
esp_err_t uart_param_config(int p,const uart_config_t *c){(void)p;actual_uart=*c;return ESP_OK;}
esp_err_t uart_set_pin(int p,int tx,int rx,int rts,int cts)
{
    (void)p;(void)rts;(void)cts;assert(reset_mask==3);reset_mask=0;
    assert(tx!=rx && tx<=1 && rx<=1);actual_tx=(unsigned)tx;actual_rx=(unsigned)rx;return ESP_OK;
}
esp_err_t uart_set_line_inverse(int p,uint32_t mask){(void)p;actual_inversion=mask;return ESP_OK;}
esp_err_t uart_flush_input(int p){(void)p;pending_used=pending_at=0;return ESP_OK;}
esp_err_t uart_driver_install(int p,int r,int t,int n,QueueHandle_t *q,int f)
{(void)p;(void)r;(void)t;(void)n;(void)q;(void)f;return ESP_OK;}
int uart_write_bytes(int p,const void *bytes,size_t n)
{
    (void)p;const uint8_t *data=bytes;writes++;memcpy(last_tx,data,n);last_tx_size=n;
    if(echo_mode){memcpy(pending,data,n);pending_used=n;return (int)n;}
    if(silent_mode || actual_tx!=wire.tx || actual_rx!=wire.rx || actual_uart.baud_rate!=(int)wire.baud ||
       actual_uart.parity!=(wire.parity=='N'?0:wire.parity=='E'?1:2) || actual_inversion)return (int)n;
    if(n==8 && data[0]==wire.address && (data[1]==3 || data[1]==4)){
        pending[0]=data[0];pending[1]=data[1];pending[2]=2;pending[3]=0;pending[4]=7;
        uint16_t crc=panel_mb_crc(pending,5);pending[5]=(uint8_t)crc;pending[6]=(uint8_t)(crc>>8);pending_used=7;
    }
    return (int)n;
}
int uart_read_bytes(int p,void *bytes,uint32_t max,unsigned wait)
{
    if(flood_rx){assert(++flood_calls<10000);memset(bytes,0x55,max);return (int)max;}
    (void)p;mock_ms+=wait;size_t n=pending_used-pending_at;if(n>max)n=max;
    memcpy(bytes,pending+pending_at,n);pending_at+=n;return (int)n;
}
esp_err_t uart_get_buffered_data_len(int p,size_t *n){(void)p;*n=pending_used-pending_at;return ESP_OK;}
esp_err_t uart_get_baudrate(int p,uint32_t *n){(void)p;*n=(uint32_t)actual_uart.baud_rate;return ESP_OK;}
const char *esp_err_to_name(esp_err_t e){(void)e;return "OK";}
const esp_app_desc_t *esp_app_get_description(void){static esp_app_desc_t d={"host"};return &d;}
uint32_t esp_get_free_heap_size(void){return 100000;}
unsigned uxTaskGetStackHighWaterMark(void *p){(void)p;return 4096;}
void esp_restart(void){abort();}
void esp_log_level_set(const char *t,int l){(void)t;(void)l;}
const esp_partition_t *esp_ota_get_running_partition(void){static esp_partition_t p;return &p;}
esp_err_t esp_ota_get_state_partition(const esp_partition_t *p,esp_ota_img_states_t *s){(void)p;*s=0;return ESP_OK;}
esp_err_t esp_ota_mark_app_valid_cancel_rollback(void){return ESP_OK;}

static void reset(void)
{
    cfg=(config_t){.baud=9600,.timeout=50,.tx=0,.rx=1,.address=1,.function=3,.stops=1,.parity='N'};
    wire=cfg;mock_ms=0;reset_mask=writes=0;cancel_at=0;cancel_sent=false;
    echo_mode=silent_mode=false;stopped=false;line_used=0;line_overflow=skip_lf=false;
    captured_used=0;captured[0]=0;memset(&stats,0,sizeof(stats));assert(apply_config());
    usb_script=NULL;usb_at=0;
    command_count=command_head=0;
    flood_rx=false;flood_calls=0;
}
static void run(const char *text){diag_command_t c;assert(diag_command(text,&c));execute(&c);}
int main(void)
{
    reset();run("swap");assert(cfg.tx==1 && cfg.rx==0 && actual_tx==1 && actual_rx==0 && writes==0);
    run("baud 19200");run("format 8E2");run("invert both");
    assert(actual_uart.baud_rate==19200 && actual_uart.parity==UART_PARITY_EVEN && actual_uart.stop_bits==2);
    assert(actual_inversion==(UART_SIGNAL_RXD_INV|UART_SIGNAL_TXD_INV));
    reset();wire.address=2;run("scan 1 3");
    assert(cfg.address==2 && writes==2 && strstr(captured,"FOUND READ_OK"));
    assert(last_tx[1]==3 && last_tx_size==8); /* Search never emits a write function. */
    reset();echo_mode=true;run("scan 1 2");
    assert(cfg.address==1 && writes==2 && strstr(captured,"SCAN NO_MATCH") && !strstr(captured,"FOUND"));
    reset();wire.baud=19200;wire.tx=1;wire.rx=0;wire.parity='E';wire.address=2;
    run("hunt 1 2");
    assert(cfg.baud==19200 && cfg.tx==1 && cfg.rx==0 && cfg.parity=='E' && cfg.address==2);
    assert(strstr(captured,"FOUND READ_OK"));
    reset();silent_mode=true;run("format 8O2");run("invert rx");config_t saved=cfg;
    run("hunt 1 1");assert(!memcmp(&saved,&cfg,sizeof(cfg)) && strstr(captured,"NO_MATCH; original config restored"));
    reset();cancel_at=20;saved=cfg;run("hunt 1 247");
    assert(cancel_sent && !memcmp(&saved,&cfg,sizeof(cfg)) && strstr(captured,"STOPPED; original config restored"));
    reset();cancel_at=100;run("poll 0042 1 1000");assert(cancel_sent && writes==1 && strstr(captured,"POLL STOPPED"));
    reset();run("listen 30");run("levels 30");assert(writes==0);
    reset();echo_mode=true;run("loopback");assert(strstr(captured,"LOOPBACK EXACT_ECHO"));
    reset();run("txcrc 01 03 00 42 00 01");assert(writes==1 && last_tx_size==8 && panel_mb_crc(last_tx,8)==0);
    reset();for(unsigned i=0;i<DIAG_LINE_MAX+10;i++)assert(!accept_byte('X'));
    assert(!accept_byte('\n') && !line_used && !line_overflow && writes==0);
    assert(!accept_byte('s'));assert(!accept_byte('t'));assert(!accept_byte('o'));assert(!accept_byte('p'));
    assert(accept_byte('\r'));assert(!strcmp(line,"stop"));assert(!accept_byte('\n'));
    /* A user pastes the next config + probe while the current probe receives.
     * The old BUSY path discards baud/swap; preserve and execute in order. */
    reset();usb_script="baud 115200\r\nswap\r\nlegacy\r\n";
    run("legacy");
    assert(!strstr(captured,"command discarded"));
    assert(command_count==3 && cfg.baud==9600 && cfg.tx==0);
    while(run_next_command()){}
    assert(cfg.baud==115200 && cfg.tx==1 && cfg.rx==0 && writes==2);
    assert(strstr(captured,"EXEC baud 115200")<strstr(captured,"EXEC swap"));
    assert(strstr(captured,"EXEC swap")<strstr(captured,"EXEC legacy"));
    assert(strstr(captured,"PROBE protocol=LEGACY_CMD04") && strstr(captured,"RX_LEVEL"));
    /* Stop bypasses pending requests and prevents their later transmission. */
    reset();usb_script="swap\r\nlegacy\r\n stop \r\n";run("legacy");
    assert(stopped && command_count==0 && cfg.tx==0 && writes==0);
    /* Input split across operation completion must not lose its first half. */
    reset();usb_script="baud 115";run("legacy");assert(line_used==8);
    input_byte('2',false);input_byte('0',false);input_byte('0',false);input_byte('\r',false);
    assert(command_count==1 && run_next_command() && cfg.baud==115200);
    /* Bounded queue rejects overflow explicitly; CRLF adds no phantom command. */
    reset();for(unsigned i=0;i<9;i++)for(const char *p="status\r\n";*p;p++)input_byte((uint8_t)*p,true);
    assert(command_count==8 && strstr(captured,"ERROR command queue full"));
    input_byte(3,true);assert(command_count==0 && stopped);
    /* Driver reads can return immediately under sustained UART traffic.
     * Time must progress through explicit scheduler yields, not RX blocking. */
    reset();flood_rx=true;run("listen 40");assert(mock_ms>=40 && flood_calls<100);
    reset();flood_rx=true;run("read 0042 1");assert(mock_ms>=cfg.timeout && flood_calls<200);
    puts("PASS runner: actual command paths, safe pin swap, scan success/echo rejection, hunt match/restore/cancel, poll stop, passive RX, loopback, raw CRC, USB overflow/CRLF");
    puts("PASS USB queue: burst paste ordering, actual baud/pins at probe, partial lines, stop bypass/clear, bounded overflow, CRLF");
    puts("PASS UART flood: continuous immediate RX yields in listen and transaction loops");
    return 0;
}
