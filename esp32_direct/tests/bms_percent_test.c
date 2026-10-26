#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "direct_ui.h"

static void put16(uint8_t *p, uint16_t n) { p[0]=(uint8_t)n; p[1]=(uint8_t)(n>>8); }

static void check_display(const direct_bms_measurement_t *m, unsigned soc, unsigned soh)
{
    direct_ui_state_t u; direct_ui_init(&u); u.page=UI_BMS;
    direct_ui_data_t d={0}; d.bms_online=true; d.bms=*m;
    direct_ui_view_t v; direct_ui_build(&u,&d,&v);
    char expected[32];
    snprintf(expected,sizeof(expected),"SOC: %u%%",soc);
    if(strcmp(v.rows[2],expected)) { fprintf(stderr,"expected %s, got %s\n",expected,v.rows[2]); assert(0); }
    snprintf(expected,sizeof(expected),"SOH: %u%%",soh);
    assert(!strcmp(v.rows[3],expected));
}

int main(void)
{
    const uint16_t raw[]={430,1000,0,10,439,80};
    direct_bms_measurement_t m;
    for(unsigned i=0;i<sizeof(raw)/sizeof(raw[0]);i++) {
        uint8_t f[23]={0x7c,0,0,0x80,1,16,0,1,0xe4,0x0c,0};
        put16(f+15,raw[i]); put16(f+17,1000);
        put16(f+1,direct_crc16(f+3,sizeof(f)-3));
        assert(direct_bms_parse_7c(f,sizeof(f),&m));
        check_display(&m,raw[i]/10,100);
        /* Presentation must not change the raw SOC forwarded to the DSP. */
        assert(m.soc_percent==raw[i] && m.soh_percent==1000);
        uint8_t request[9];
        assert(direct_dsp_make_request(6,0,0,m.soc_percent,request)==9);
        assert(request[5]==(uint8_t)(raw[i]>>8) && request[6]==(uint8_t)raw[i]);
    }
    uint8_t f[79]={1,3,0x4a};
    f[8]=43; f[10]=100;
    put16(f+77,direct_crc16(f,77));
    assert(direct_bms_parse_modbus(f,sizeof(f),1,&m));
    check_display(&m,43,100); /* Modbus already uses whole percent. */
    puts("PASS BMS percentages: BBJ 0.1% and Modbus 1%; DSP wire SOC preserved");
    return 0;
}
