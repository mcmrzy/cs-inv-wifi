/* USB-only UART diagnostic firmware. No LCD, WiFi, BLE, NVS or eFuse writes. */
#include "diag_core.h"
#include "panel_modbus.h"
#include <ctype.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#ifdef DIAG_HOST_TEST
#include "diag_test_sdk.h"
#else
#include "driver/gpio.h"
#include "driver/uart.h"
#include "driver/usb_serial_jtag.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#endif

#define PORT UART_NUM_0
typedef struct {
    uint32_t baud,timeout;
    uint8_t tx,rx,address,function,stops,invert;
    char parity;
    bool verbose;
} config_t;
static config_t cfg={.baud=9600,.timeout=350,.tx=0,.rx=1,.address=1,
    .function=3,.stops=1,.parity='N',.verbose=true};
static struct {
    uint32_t requests,tx_bytes,rx_bytes,result[7],parity,frame,overflow,breaks,usb_dropped;
} stats;
static QueueHandle_t events;
static uint8_t rx_data[DIAG_RX_MAX];
static char line[DIAG_LINE_MAX];
static size_t line_used;
static bool line_overflow,skip_lf,stopped,uart_ready;
#define COMMAND_QUEUE_DEPTH 8U
typedef struct {diag_command_t command;char text[96];} queued_command_t;
static queued_command_t commands[COMMAND_QUEUE_DEPTH];
static unsigned command_head,command_count;

static uint32_t ms(void){return (uint32_t)(esp_timer_get_time()/1000);}
static void output(const char *format,...) __attribute__((format(printf,1,2)));
static void output(const char *format,...)
{
    char buffer[512];va_list ap;va_start(ap,format);
    int n=vsnprintf(buffer,sizeof(buffer),format,ap);va_end(ap);
    if(n<=0)return;
    size_t size=(size_t)n<sizeof(buffer)?(size_t)n:sizeof(buffer)-1;
    int sent=usb_serial_jtag_write_bytes(buffer,size,pdMS_TO_TICKS(100));
    if(sent<(int)size)stats.usb_dropped+=(uint32_t)(sent<0?size:size-(size_t)sent);
}
static void hex_dump(const char *prefix,const uint8_t *data,size_t size)
{
    if(!size){output("%s -\r\n",prefix);return;}
    for(size_t i=0;i<size;i+=32){
        char text[32*3+1];size_t end=size-i>32?32:size-i,used=0;
        for(size_t j=0;j<end;j++)used+=(size_t)snprintf(text+used,sizeof(text)-used,"%02X ",data[i+j]);
        output("%s +%03u %s\r\n",prefix,(unsigned)i,text);
    }
}
static void poll_events(void)
{
    uart_event_t e;
    while(xQueueReceive(events,&e,0)==pdTRUE)switch(e.type){
        case UART_PARITY_ERR:stats.parity++;break;
        case UART_FRAME_ERR:stats.frame++;break;
        case UART_FIFO_OVF:case UART_BUFFER_FULL:stats.overflow++;break;
        case UART_BREAK:stats.breaks++;break;
        default:break;
    }
}
/* Same line discipline in idle and busy modes. Never execute a truncated line. */
static bool accept_byte(uint8_t byte)
{
    if(byte==3){stopped=true;line_used=0;line_overflow=false;return false;}
    if(skip_lf && byte=='\n'){skip_lf=false;return false;}
    skip_lf=byte=='\r';
    if(byte=='\r' || byte=='\n'){
        if(line_overflow){output("\r\nERROR line too long; discarded\r\n");line_used=0;line_overflow=false;return false;}
        line[line_used]=0;bool complete=line_used>0;line_used=0;return complete;
    }
    if(byte==8 || byte==127){if(line_used && !line_overflow){line_used--;output("\b \b");}return false;}
    if(byte<32 || byte>126)return false;
    if(line_used+1>=sizeof(line)){line_overflow=true;return false;}
    if(!line_overflow)line[line_used++]=(char)byte;
    return false;
}
static void input_byte(uint8_t byte,bool busy)
{
    bool ignored_lf=skip_lf && byte=='\n';
    bool complete=accept_byte(byte);
    if(byte==3){
        command_count=0;command_head=0;
        output("^C STOP requested; command queue cleared\r\n");
        return;
    }
    if(!busy && byte>=32 && byte<=126)output("%c",byte);
    if(!complete){
        if(!busy && (byte=='\r' || byte=='\n') && !ignored_lf)output("\r\ndiag> ");
        return;
    }
    if(!busy)output("\r\n");
    diag_command_t parsed;
    if(!diag_command(line,&parsed)){
        output("ERROR invalid command/range: %.95s\r\n",line);return;
    }
    if(parsed.kind==CMD_STOP){
        stopped=true;command_count=0;command_head=0;
        output("STOP requested; command queue cleared\r\n");return;
    }
    if(command_count==COMMAND_QUEUE_DEPTH){
        output("ERROR command queue full (%u); rejected: %.95s\r\n",COMMAND_QUEUE_DEPTH,line);return;
    }
    queued_command_t *slot=&commands[(command_head+command_count)%COMMAND_QUEUE_DEPTH];
    slot->command=parsed;snprintf(slot->text,sizeof(slot->text),"%.95s",line);command_count++;
    if(busy)output("QUEUED depth=%u command=%s\r\n",command_count,slot->text);
}
static void busy_input(void)
{
    uint8_t bytes[64];int n=usb_serial_jtag_read_bytes(bytes,sizeof(bytes),0);
    for(int i=0;i<n;i++){
        input_byte(bytes[i],true);
        if(stopped)break; /* Never execute a pasted command after stop/Ctrl+C. */
    }
}
static bool pause_ms(uint32_t duration)
{
    uint32_t start=ms();
    while(ms()-start<duration && !stopped){busy_input();vTaskDelay(pdMS_TO_TICKS(10));}
    return !stopped;
}
static void show_config(void)
{
    output("CFG UART0 TX=GPIO%u RX=GPIO%u addr=%u baud=%"PRIu32" 8%c%u invert=%s fc=%02X timeout=%"PRIu32"ms verbose=%u ready=%u\r\n",
        cfg.tx,cfg.rx,cfg.address,cfg.baud,cfg.parity,cfg.stops,
        (const char *const[]){"none","rx","tx","both"}[cfg.invert],cfg.function,cfg.timeout,cfg.verbose,uart_ready);
}
static bool apply_config(void)
{
    /* Release BOTH old pin routes before swapping: the old TX must become input. */
    uart_ready=false;
    esp_err_t err=uart_wait_tx_done(PORT,pdMS_TO_TICKS(1000));
    if(err!=ESP_OK){output("ERROR TX drain: %s\r\n",esp_err_to_name(err));return false;}
    gpio_reset_pin(GPIO_NUM_0);gpio_reset_pin(GPIO_NUM_1);
    uart_config_t settings={.baud_rate=(int)cfg.baud,.data_bits=UART_DATA_8_BITS,
        .parity=cfg.parity=='N'?UART_PARITY_DISABLE:cfg.parity=='E'?UART_PARITY_EVEN:UART_PARITY_ODD,
        .stop_bits=cfg.stops==2?UART_STOP_BITS_2:UART_STOP_BITS_1,
        .flow_ctrl=UART_HW_FLOWCTRL_DISABLE,.source_clk=UART_SCLK_DEFAULT};
    err=uart_param_config(PORT,&settings);
    if(err==ESP_OK)err=uart_set_pin(PORT,cfg.tx,cfg.rx,UART_PIN_NO_CHANGE,UART_PIN_NO_CHANGE);
    uint32_t inverse=(cfg.invert&1?UART_SIGNAL_RXD_INV:0)|(cfg.invert&2?UART_SIGNAL_TXD_INV:0);
    if(err==ESP_OK)err=uart_set_line_inverse(PORT,inverse);
    uart_flush_input(PORT);poll_events();
    if(err!=ESP_OK){output("ERROR UART config: %s\r\n",esp_err_to_name(err));return false;}
    uart_ready=true;return true;
}
static bool matched(diag_result_t r)
{return r==DIAG_READ_OK || r==DIAG_EXCEPTION || r==DIAG_LEGACY_OK;}

static diag_result_t transaction(const uint8_t *tx,size_t count,uint8_t address,
                                 uint8_t function,uint16_t reg,uint16_t words,bool legacy,bool verbose)
{
    if(!uart_ready || !count){output("ERROR UART not ready or empty request\r\n");return DIAG_UNMATCHED;}
    /* At least 4 character times of silence, also when testing slow baud rates. */
    uint32_t gap=(48000U+cfg.baud-1)/cfg.baud+10U;
    if(!pause_ms(gap))return DIAG_UNMATCHED;
    if(verbose){output("PROBE protocol=%s\r\n",legacy?"LEGACY_CMD04":function?"MODBUS":"RAW");show_config();}
    int rx_before=gpio_get_level((gpio_num_t)cfg.rx);
    uart_flush_input(PORT);poll_events();
    if(verbose)hex_dump("TX",tx,count);
    uint32_t start=ms();stats.requests++;
    int written=uart_write_bytes(PORT,tx,count);
    if(written>0)stats.tx_bytes+=(uint32_t)written;
    if(written!=(int)count){output("ERROR UART TX %d/%u\r\n",written,(unsigned)count);return DIAG_UNMATCHED;}
    uint32_t tx_budget=(uint32_t)((count*12000U+cfg.baud-1)/cfg.baud)+200;
    esp_err_t drained;
    do{
        busy_input();drained=uart_wait_tx_done(PORT,pdMS_TO_TICKS(10));
    }while(drained==ESP_ERR_TIMEOUT && ms()-start<tx_budget);
    if(drained!=ESP_OK){output("ERROR UART TX drain %s; run status/config before retry\r\n",esp_err_to_name(drained));uart_ready=false;return DIAG_UNMATCHED;}
    uint32_t receive_start=ms(),first=UINT32_MAX,last=receive_start;
    size_t have=0,total=0;
    /* timeout is first-byte allowance plus expected response wire time. */
    uint32_t expected=legacy?69U:(function==3 || function==4)?words*2U+5U:256U;
    uint32_t budget=cfg.timeout+(expected*12000U+cfg.baud-1)/cfg.baud;
    uint8_t chunk[128];
    while(!stopped && ms()-receive_start<budget){
        busy_input();poll_events();
        int n=uart_read_bytes(PORT,chunk,sizeof(chunk),pdMS_TO_TICKS(10));
        if(n>0){
            if(first==UINT32_MAX)first=ms()-start;
            last=ms();total+=(size_t)n;stats.rx_bytes+=(uint32_t)n;
            size_t keep=(size_t)n;if(keep>sizeof(rx_data)-have)keep=sizeof(rx_data)-have;
            memcpy(rx_data+have,chunk,keep);have+=keep;
        }
        /* Keep listening after a pure echo: a real slave may reply later. */
        if(have && ms()-last>=gap+20U){
            diag_reply_t current=diag_analyze(tx,count,rx_data,have,address,function,words,legacy);
            if(matched(current.result))break;
        }
        vTaskDelay(1); /* Continuous RX must still let the idle task run. */
    }
    poll_events();
    diag_reply_t reply=diag_analyze(tx,count,rx_data,have,address,function,words,legacy);
    stats.result[reply.result]++;
    if(verbose || have || matched(reply.result) || stopped){
        hex_dump("RX",rx_data,have);
        output("RX_LEVEL before=%d after=%d\r\n",rx_before,gpio_get_level((gpio_num_t)cfg.rx));
        output("RESULT %s%s addr=%u fc=%02X reg=%04X rx=%u kept=%u elapsed=%"PRIu32"ms first_rx=%"PRIu32"ms echo=%u%s\r\n",
            diag_result_name(reply.result),stopped?" STOPPED":"",address,function,reg,(unsigned)total,(unsigned)have,
            ms()-start,first,reply.echo,total>have?" TRUNCATED":"");
        if(reply.result==DIAG_EXCEPTION)output("EXCEPTION code=%02X (valid Modbus reply; register may be unsupported)\r\n",reply.exception);
        if(reply.result==DIAG_READ_OK)for(unsigned i=0;i<reply.count;i++)
            output("REG %04X = %04X (%u)\r\n",reg+i,reply.values[i],reply.values[i]);
        if(!function && !legacy && have>=3)output("RAW entire-buffer CRC16=%04X (zero alone is not a matching reply)\r\n",panel_mb_crc(rx_data,have));
    }
    return reply.result;
}
static diag_result_t read_one(uint8_t address,uint8_t function,uint16_t reg,uint16_t words,bool verbose)
{
    uint8_t tx[8];size_t n=diag_read_request(address,function,reg,words,tx);
    return transaction(tx,n,address,function,reg,words,false,verbose);
}
static void statistics(void)
{
    poll_events();
    output("STATS req=%"PRIu32" tx_bytes=%"PRIu32" rx_bytes=%"PRIu32" parity_err=%"PRIu32" frame_err=%"PRIu32" overflow=%"PRIu32" breaks=%"PRIu32" usb_dropped=%"PRIu32"\r\n",
        stats.requests,stats.tx_bytes,stats.rx_bytes,stats.parity,stats.frame,stats.overflow,stats.breaks,stats.usb_dropped);
    for(unsigned i=0;i<7;i++)output("  %s=%"PRIu32"\r\n",diag_result_name((diag_result_t)i),stats.result[i]);
}
static bool scan_range(unsigned first,unsigned last,uint16_t reg,uint8_t function)
{
    for(unsigned address=first;address<=last && !stopped;address++){
        output("SCAN addr=%u/%u\r\n",address,last);
        diag_result_t result=read_one((uint8_t)address,function,reg,1,cfg.verbose);
        if(!stopped && matched(result)){
            cfg.address=(uint8_t)address;cfg.function=function;
            output("FOUND %s (CRC/address/function/length matched); selected in RAM\r\n",diag_result_name(result));
            show_config();return true;
        }
    }
    return false;
}
static void hunt(unsigned first,unsigned last)
{
    /* No inversion/FC04/private protocol guessed automatically. */
    const uint32_t bauds[]={9600,19200,4800,2400,38400,57600,115200,1200};
    config_t saved=cfg;bool found=false;cfg.invert=0;cfg.stops=1;
    uint32_t attempts=(last-first+1)*2U*3U*8U;
    output("HUNT %"PRIu32" probes max, addresses %u..%u, FC03 reg0042, 2 directions x 8 baud x 3 parity. stop/Ctrl+C cancels.\r\n",
        attempts,first,last);
    for(unsigned direction=0;direction<2 && !stopped && !found;direction++){
        cfg.tx=direction==0?saved.tx:saved.rx;cfg.rx=direction==0?saved.rx:saved.tx;
        for(unsigned b=0;b<8 && !stopped && !found;b++)for(unsigned p=0;p<3 && !stopped && !found;p++){
            cfg.baud=bauds[b];cfg.parity="NEO"[p];
            if(!apply_config())goto done;
            show_config();found=scan_range(first,last,0x42,3);
        }
    }
done:
    if(!found){cfg=saved;apply_config();output("HUNT %s; original config restored\r\n",stopped?"STOPPED":"NO_MATCH");show_config();}
}
static void listen_uart(uint32_t duration)
{
    if(!uart_ready){output("ERROR UART not ready\r\n");return;}
    output("LISTEN passive, %"PRIu32"ms; stop/Ctrl+C cancels\r\n",duration);
    uint32_t start=ms();uint8_t data[128];size_t total=0;
    while(!stopped && ms()-start<duration){
        busy_input();poll_events();
        int n=uart_read_bytes(PORT,data,sizeof(data),pdMS_TO_TICKS(10));
        if(n>0){stats.rx_bytes+=(uint32_t)n;total+=(size_t)n;output("RX at +%"PRIu32"ms\r\n",ms()-start);hex_dump("RX",data,(size_t)n);}
        vTaskDelay(1);
    }
    output("LISTEN %s rx_bytes=%u\r\n",stopped?"STOPPED":"DONE",(unsigned)total);
}
static void sample_levels(uint32_t duration)
{
    uint32_t high=0,low=0,start=ms();
    while(!stopped && ms()-start<duration){
        if(gpio_get_level((gpio_num_t)cfg.rx))high++;else low++;
        busy_input();vTaskDelay(pdMS_TO_TICKS(10));
    }
    output("RX GPIO%u samples high=%"PRIu32" low=%"PRIu32" (10ms samples, not a logic analyzer; does not prove wiring)\r\n",cfg.rx,high,low);
}
static void help(void)
{
    output("\r\nCS-MB USB UART DIAG 0.1.1 | ESP32-C3 | ONLY UART0 GPIO0/1\r\n");
    output("USB commands: CR/LF, no local echo required. Registers HEX; other numbers DECIMAL.\r\n");
    output("help | status | stats | clear | reboot | stop (or Ctrl+C during operations)\r\n");
    output("baud 9600 | pins 0 1 (TX RX) | swap | addr 1 | format 8N1 (8E1/8O1/8N2/8E2/8O2)\r\n");
    output("fc 3 (3/4) | timeout 350 (50..5000ms) | verbose 1 (0/1) | invert none (rx/tx/both)\r\n");
    output("read 0042 [count=1] [addr=current] | poll 0042 [count=1] [interval_ms=1000]\r\n");
    output("scan [first=1] [last=247] [reg=0042] [fc=current] -- stop at first valid reply\r\n");
    output("hunt [first=1] [last=16] -- 2 pin directions x 8 baud x 8N1/8E1/8O1, FC03 reg0042\r\n");
    output("listen [ms=10000] | levels [ms=1000] | loopback (disconnect inverter; short GPIO0/1)\r\n");
    output("tx 01 03 ... | txcrc 01 03 00 42 00 01 | crc 01 03 00 42 00 01\r\n");
    output("legacy -- ONE original direct command04 read, current baud/format/pins, fixed ID1 (original direct uses 115200 8N1)\r\n");
    output("Search is read-only. Raw TX sends exactly your bytes. No settings persist across reboot.\r\n");
    output("Up to 8 pasted commands queue in order; EXEC shows execution, QUEUED shows pending. stop/Ctrl+C clears queue.\r\n");
}
static void execute(const diag_command_t *command)
{
    config_t saved=cfg;bool reconfigure=false;
    switch(command->kind){
        case CMD_HELP:help();break;
        case CMD_STATUS:
            {size_t buffered=0;uint32_t actual_baud=0;
            uart_get_buffered_data_len(PORT,&buffered);uart_get_baudrate(PORT,&actual_baud);
            show_config();output("FW %s uptime=%"PRIu32"ms heap=%"PRIu32" main_stack_free=%u RX_level=%d buffered_rx=%u actual_baud=%"PRIu32"\r\n",
                esp_app_get_description()->version,ms(),esp_get_free_heap_size(),(unsigned)uxTaskGetStackHighWaterMark(NULL),
                gpio_get_level((gpio_num_t)cfg.rx),(unsigned)buffered,actual_baud);break;
            }
        case CMD_STATS:statistics();break;
        case CMD_CLEAR:poll_events();memset(&stats,0,sizeof(stats));uart_flush_input(PORT);output("Counters and RX cleared; config kept\r\n");break;
        case CMD_REBOOT:output("Reboot; all config resets to defaults\r\n");usb_serial_jtag_wait_tx_done(pdMS_TO_TICKS(200));esp_restart();break;
        case CMD_STOP:output("Idle; nothing to stop\r\n");break;
        case CMD_BAUD:cfg.baud=command->arg[0];reconfigure=true;break;
        case CMD_PINS:cfg.tx=(uint8_t)command->arg[0];cfg.rx=(uint8_t)command->arg[1];reconfigure=true;break;
        case CMD_SWAP:{uint8_t t=cfg.tx;cfg.tx=cfg.rx;cfg.rx=t;reconfigure=true;break;}
        case CMD_ADDR:cfg.address=(uint8_t)command->arg[0];show_config();break;
        case CMD_FORMAT:cfg.parity=command->parity;cfg.stops=(uint8_t)command->arg[0];reconfigure=true;break;
        case CMD_FC:cfg.function=(uint8_t)command->arg[0];show_config();break;
        case CMD_TIMEOUT:cfg.timeout=command->arg[0];show_config();break;
        case CMD_VERBOSE:cfg.verbose=command->arg[0]!=0;show_config();break;
        case CMD_INVERT:cfg.invert=(uint8_t)command->arg[0];reconfigure=true;break;
        case CMD_READ:read_one(command->arg[2]?(uint8_t)command->arg[2]:cfg.address,cfg.function,(uint16_t)command->arg[0],(uint16_t)command->arg[1],true);break;
        case CMD_POLL:
            output("POLL starts; stop/Ctrl+C cancels, interval measured start-to-start\r\n");
            while(!stopped){
                uint32_t started=ms();read_one(cfg.address,cfg.function,(uint16_t)command->arg[0],(uint16_t)command->arg[1],true);
                uint32_t elapsed=ms()-started;if(elapsed<command->arg[2])pause_ms(command->arg[2]-elapsed);
            }
            output("POLL STOPPED\r\n");break;
        case CMD_SCAN:
            if(!scan_range(command->arg[0],command->arg[1],(uint16_t)command->arg[2],command->arg[3]?(uint8_t)command->arg[3]:cfg.function))
                output("SCAN %s; config unchanged\r\n",stopped?"STOPPED":"NO_MATCH");
            break;
        case CMD_HUNT:hunt(command->arg[0],command->arg[1]);break;
        case CMD_LISTEN:listen_uart(command->arg[0]);break;
        case CMD_LEVELS:sample_levels(command->arg[0]);break;
        case CMD_TX:case CMD_TXCRC:
            transaction(command->data,command->length,cfg.address,0,0,0,false,true);break;
        case CMD_CRC:{uint16_t crc=panel_mb_crc(command->data,command->length);
            output("CRC16=%04X append bytes %02X %02X (Modbus/legacy polynomial A001)\r\n",crc,crc&255,crc>>8);break;}
        case CMD_LOOPBACK:{
            static const uint8_t pattern[]={0x55,0xaa,0x00,0xff,0x12,0x34,0x56,0x78,0x87,0x65,0x43,0x21,0x5a,0xa5,0xf0,0x0f};
            output("LOOPBACK: inverter must be disconnected and GPIO0/1 shorted; sends 16 test bytes\r\n");
            diag_result_t result=transaction(pattern,sizeof(pattern),0,0,0,0,false,true);
            output("LOOPBACK %s (checks ESP local path only)\r\n",result==DIAG_ECHO && !stopped?"EXACT_ECHO":"NO_EXACT_ECHO");break;
        }
        case CMD_LEGACY:{uint8_t tx[9];diag_legacy_request(tx);
            transaction(tx,sizeof(tx),1,0,0,0,true,true);break;}
    }
    if(reconfigure){
        if(!apply_config()){cfg=saved;output("Restoring previous config\r\n");apply_config();}
        show_config();
    }
}
static bool run_next_command(void)
{
    if(!command_count)return false;
    queued_command_t next=commands[command_head];
    command_head=(command_head+1)%COMMAND_QUEUE_DEPTH;command_count--;
    stopped=false;output("EXEC %s\r\n",next.text);execute(&next.command);
    /* Preserve partial input collected during the operation. */
    stopped=false;output("diag> ");return true;
}
void app_main(void)
{
    esp_log_level_set("*",ESP_LOG_WARN); /* Keep watchdog/driver errors visible. */
    usb_serial_jtag_driver_config_t usb={.tx_buffer_size=8192,.rx_buffer_size=2048};
    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&usb));
    /* If loaded into a pending OTA slot, keep this deliberately selected test app. */
    esp_ota_img_states_t state;const esp_partition_t *running=esp_ota_get_running_partition();
    if(esp_ota_get_state_partition(running,&state)==ESP_OK && state==ESP_OTA_IMG_PENDING_VERIFY)
        esp_ota_mark_app_valid_cancel_rollback();
    ESP_ERROR_CHECK(uart_driver_install(PORT,4096,1024,64,&events,0));
    apply_config();help();show_config();
    output("No automatic transmissions. Type help then Enter if boot text was missed.\r\ndiag> ");
    for(;;){
        if(run_next_command()){vTaskDelay(1);continue;}
        uint8_t byte;int n=usb_serial_jtag_read_bytes(&byte,1,pdMS_TO_TICKS(25));
        poll_events();
        if(n!=1)continue;
        stopped=false;input_byte(byte,false);
        vTaskDelay(1); /* USB input floods must not starve the idle task. */
    }
}
