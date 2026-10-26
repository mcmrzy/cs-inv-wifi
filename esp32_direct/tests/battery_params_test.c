#include "direct_dsp_params.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
int main(void){
    direct_dsp_params_t p;assert(direct_dsp_params_arm_defaults(&p));
    direct_battery_config_t c;assert(direct_dsp_params_battery_get(&p,&c));assert(c.type==1 && !c.bulk_dv);
    assert(direct_battery_preset(4,0,&c));
    assert(direct_dsp_params_battery_set(&p,&c)==DIRECT_BATTERY_OK);
    assert((p.battery_profiles_valid&(1U<<1)) && p.battery_profiles[1].cutoff_dv==400 && !p.battery_profiles[1].bulk_dv);
    assert(p.frames[12]==4 && (p.frames[34]&2) && !(p.frames[34]&0x80) && p.frames[45]==5);
    uint8_t charge[11];assert(direct_dsp_params_charge_reply(&p,charge)==11 && charge[2]==0x2f);
    assert(charge[3]==2 && charge[4]==0x1c && charge[8]==0xc2);
    direct_dsp_params_t before=p;c.protocol=2;
    assert(direct_dsp_params_battery_set(&p,&c)==DIRECT_BATTERY_PROTOCOL_UNSUPPORTED && !memcmp(&p,&before,sizeof(p)));
    assert(direct_battery_preset(1,0,&c));assert(direct_dsp_params_battery_set(&p,&c)==DIRECT_BATTERY_OK);
    assert(p.battery_profiles_valid&(1U<<4));
    assert(direct_battery_preset(5,0,&c));assert(direct_dsp_params_battery_set(&p,&c)==DIRECT_BATTERY_OK);
    uint16_t value;assert(direct_dsp_params_get(&p,24,&value) && value==16);
    assert(direct_dsp_params_get(&p,74,&value) && value==576);
    assert(direct_dsp_params_set(&p,1,100));
    assert(direct_dsp_params_battery_get(&p,&c) && c.max_charge_a==100);
    uint8_t record[DIRECT_DSP_PROFILES_RECORD_SIZE];p.revision=12;
    assert(direct_dsp_params_profiles_pack(&p,"H1ZZX00139000079",record));
    direct_dsp_params_t reboot;
    assert(direct_dsp_params_profiles_unpack(record,sizeof(record),"H1ZZX00139000079",&reboot));
    assert(!memcmp(&p,&reboot,sizeof(p)));
    record[120]^=1;assert(!direct_dsp_params_profiles_unpack(record,sizeof(record),"H1ZZX00139000079",&reboot));
    puts("PASS whole battery transaction: mode bits, charge frame, rejection leaves all unchanged, per-type profiles, User Li, scalar coherence, reboot/CRC");
}
