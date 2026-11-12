"""Execute the actual UART configuration function against driver spies."""
from pathlib import Path
import subprocess
import sys

root=Path(__file__).resolve().parents[1]
source=(root/'main/panel_bus.c').read_text(encoding='utf-8-sig')
start=source.index('static esp_err_t configure_uart(')
end=source.index('static panel_io_result_t exchange(',start)
function=source[start:end]
harness=r'''
#include "panel_types.h"
#include "diag_test_sdk.h"
#include <assert.h>
#include <stdio.h>
static uart_config_t actual;
static unsigned mask=UART_SIGNAL_TXD_INV|UART_SIGNAL_RXD_INV,calls;
static int param_error,invert_error;
esp_err_t uart_param_config(int port,const uart_config_t *c)
{assert(port==UART_NUM_0);actual=*c;return param_error;}
esp_err_t uart_set_line_inverse(int port,uint32_t value)
{assert(port==UART_NUM_0);mask=value;calls++;return invert_error;}
''' + function + r'''
int main(void){
    panel_config_t c={.address=1,.baud=9600};
    assert(configure_uart(&c)==ESP_OK);
    assert(calls==1 && mask==0);
    assert(actual.baud_rate==9600 && actual.data_bits==UART_DATA_8_BITS &&
           actual.parity==UART_PARITY_DISABLE && actual.stop_bits==UART_STOP_BITS_1 &&
           actual.flow_ctrl==UART_HW_FLOWCTRL_DISABLE);
    for(unsigned p=0;p<3;p++){
        c.parity=(uint8_t)p;c.baud=19200;
        mask=UART_SIGNAL_TXD_INV|UART_SIGNAL_RXD_INV;
        assert(configure_uart(&c)==ESP_OK && mask==0);
    }
    assert(calls==4 && actual.parity==UART_PARITY_ODD);
    param_error=71;assert(configure_uart(&c)==71 && calls==4);
    param_error=0;invert_error=72;assert(configure_uart(&c)==72 && calls==5);
    puts("PASS UART board: clears both inversion bits at startup and reconfigure, UART0 only, driver errors propagated");
    return 0;
}
'''
tmp=root/'tmp';tmp.mkdir(exist_ok=True)
test=tmp/'uart_board_test.c';test.write_text(harness,encoding='utf-8')
subprocess.run([sys.argv[1],'-save-temps=obj','-std=c11','-Wall','-Wextra','-Werror',
               '-I'+str(root/'main'),'-I'+str(root.parent/'esp32_uart_diag/tests'),
               str(test),'-o',str(tmp/'uart_board_test.exe')],check=True)
result=subprocess.run([str(tmp/'uart_board_test.exe')],check=True,capture_output=True,text=True)
print(result.stdout,end='')
