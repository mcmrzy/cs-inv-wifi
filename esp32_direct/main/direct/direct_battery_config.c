#include "direct_battery_config.h"
#include <string.h>

unsigned direct_battery_profile_slot(const direct_battery_config_t *c){
    if(!c || c->type<1 || c->type>5 || c->material>1)return 0;
    return c->type==5?5U+c->material:c->type;
}
direct_battery_result_t direct_battery_validate(const direct_battery_config_t *c){
    if(!c || !direct_battery_profile_slot(c))return DIRECT_BATTERY_INVALID;
    if((c->type==4 && c->protocol!=DIRECT_BATTERY_BBJ_PROTOCOL) ||
       (c->type!=4 && c->protocol))return DIRECT_BATTERY_PROTOCOL_UNSUPPORTED;
    if(!c->bulk_dv || !c->float_dv || !c->cutoff_dv)return DIRECT_BATTERY_PROFILE_REQUIRED;
    if(c->bulk_dv<400 || c->bulk_dv>600 || c->float_dv<400 || c->float_dv>600 ||
       c->cutoff_dv<400 || c->cutoff_dv>=c->float_dv || c->float_dv>c->bulk_dv ||
       c->max_charge_a>140 || c->ac_charge_a>60 || c->ac_charge_a>c->max_charge_a ||
       c->back_utility_dv<440 || c->back_utility_dv>512 ||
       c->back_battery_dv<480 || c->back_battery_dv>580 || c->back_utility_dv>=c->back_battery_dv ||
       c->soc_low>100 || c->soc_high>100 || c->soc_low>=c->soc_high ||
       c->soc_cutoff<5 || c->soc_cutoff>30 || c->soc_cutoff>c->soc_low)return DIRECT_BATTERY_INVALID;
    if(c->type<=3 && (c->bulk_dv<480 || c->bulk_dv>584 || c->float_dv<480 ||
                      c->float_dv>584 || c->cutoff_dv>480))return DIRECT_BATTERY_INVALID;
    if(c->type==5){
        unsigned n=c->cells;
        if(c->bulk_dv!=c->float_dv)return DIRECT_BATTERY_INVALID;
        if(!c->material){
            if((n!=15 && n!=16) || c->bulk_dv*10U<n*360U || c->bulk_dv*10U>n*365U ||
               c->cutoff_dv*10U<n*270U || c->cutoff_dv*10U>n*300U)return DIRECT_BATTERY_INVALID;
        }else if((n!=13 && n!=14) || c->bulk_dv*10U<n*400U || c->bulk_dv*10U>n*417U ||
                 c->cutoff_dv*10U<n*310U || c->cutoff_dv*10U>n*343U)return DIRECT_BATTERY_INVALID;
    }else if(c->cells || c->material)return DIRECT_BATTERY_INVALID;
    return DIRECT_BATTERY_OK;
}
bool direct_battery_preset(uint16_t type,uint16_t material,direct_battery_config_t *c){
    if(!c || type<1 || type>5 || material>1)return false;
    memset(c,0,sizeof(*c));c->type=type;
    /* App(10) APP/SystemConfig.c ReadSysParamFormEEPROM. Source presets,
     * not readings or automatic identification of the connected battery. */
    c->bulk_dv=560;c->float_dv=540;c->cutoff_dv=450;
    c->max_charge_a=140;c->ac_charge_a=60;
    c->back_utility_dv=480;c->back_battery_dv=576;
    c->soc_low=50;c->soc_high=95;c->soc_cutoff=5;
    if(type==4){c->protocol=DIRECT_BATTERY_BBJ_PROTOCOL;c->bulk_dv=540;}
    if(type==5){c->material=material;c->cells=material?14:16;
        c->bulk_dv=c->float_dv=material?560:576;c->cutoff_dv=480;}
    return true;
}
