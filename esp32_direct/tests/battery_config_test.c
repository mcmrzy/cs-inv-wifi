#include "direct_battery_config.h"
#include <assert.h>
#include <stdio.h>
int main(void){
    direct_battery_config_t c={0};
    assert(direct_battery_preset(4,0,&c));
    assert(c.type==4 && c.protocol==5 && c.bulk_dv==540 && c.float_dv==540 && c.cutoff_dv==450);
    assert(direct_battery_validate(&c)==DIRECT_BATTERY_OK);
    c.protocol=1;assert(direct_battery_validate(&c)==DIRECT_BATTERY_PROTOCOL_UNSUPPORTED);
    assert(direct_battery_preset(5,0,&c));
    assert(c.cells==16 && c.bulk_dv==576 && c.cutoff_dv==480 && !c.protocol);
    assert(direct_battery_validate(&c)==DIRECT_BATTERY_OK);
    c.cells=15;assert(direct_battery_validate(&c)==DIRECT_BATTERY_INVALID);
    c.bulk_dv=c.float_dv=540;c.cutoff_dv=450;
    assert(direct_battery_validate(&c)==DIRECT_BATTERY_OK);
    c.ac_charge_a=c.max_charge_a+1;assert(direct_battery_validate(&c)==DIRECT_BATTERY_INVALID);
    assert(direct_battery_preset(5,1,&c));assert(c.cells==14 && c.bulk_dv==560);
    assert(direct_battery_validate(&c)==DIRECT_BATTERY_OK);
    c.soc_low=c.soc_high;assert(direct_battery_validate(&c)==DIRECT_BATTERY_INVALID);
    assert(!direct_battery_preset(0,0,&c));
    puts("PASS battery configuration: original ARM presets, chemistry/cells/voltage relationships, actual BMS protocol, current/SOC cross checks");
}
