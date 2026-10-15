#include "direct_energy_codec.h"
#include <string.h>
#define DAY_MS 86400000LL
void direct_energy_power(uint32_t pv,int32_t ac,int32_t battery,int32_t output,
                          uint32_t power[DIRECT_ENERGY_CHANNELS])
{
    memset(power,0,sizeof(uint32_t)*DIRECT_ENERGY_CHANNELS);
    power[DIRECT_ENERGY_PV]=pv;
    if (ac>0) power[DIRECT_ENERGY_AC_CHARGE]=(uint32_t)ac;
    else if (ac<0) power[DIRECT_ENERGY_AC_DISCHARGE]=(uint32_t)(-(int64_t)ac);
    if (battery>0) power[DIRECT_ENERGY_BAT_CHARGE]=(uint32_t)battery;
    else if (battery<0) power[DIRECT_ENERGY_BAT_DISCHARGE]=(uint32_t)(-(int64_t)battery);
    if (output>0) power[DIRECT_ENERGY_OUTPUT]=(uint32_t)output;
}
void direct_energy_accumulator_init(direct_energy_accumulator_t *s,int timezone)
{
    memset(s,0,sizeof(*s));
    s->record.magic=DIRECT_ENERGY_RECORD_MAGIC; s->record.version=1;
    s->record.timezone_minutes=timezone>=-840 && timezone<=840 ? timezone : 480;
}
bool direct_energy_record_load(direct_energy_accumulator_t *s,const direct_energy_record_t *r)
{
    if (!s || !r || r->magic!=DIRECT_ENERGY_RECORD_MAGIC || r->version!=1 ||
        r->day_valid>1 || r->timezone_minutes<-840 || r->timezone_minutes>840 ||
        (r->day_valid && (r->day<18000 || r->day>100000))) return false;
    for (unsigned i=0;i<DIRECT_ENERGY_CHANNELS;++i)
        if (r->today[i]>r->total[i]) return false;
    s->record=*r; s->baseline_valid=false; s->clock_valid=false; return true;
}
void direct_energy_accumulator_timezone(direct_energy_accumulator_t *s,int timezone)
{
    if (!s || timezone<-840 || timezone>840 || s->record.timezone_minutes==timezone) return;
    s->record.timezone_minutes=timezone;
    s->record.day_valid=0; memset(s->record.today,0,sizeof(s->record.today));
    s->baseline_valid=false;
}
static uint64_t saturating_add(uint64_t previous,uint64_t delta)
{
    return UINT64_MAX-previous<delta ? UINT64_MAX : previous+delta;
}
bool direct_energy_accumulate(direct_energy_accumulator_t *s,
    uint32_t received_ms,int64_t utc_ms,bool clock_valid,bool fresh,
    const uint32_t power[DIRECT_ENERGY_CHANNELS])
{
    if (!s || !power) return false;
    clock_valid=clock_valid && utc_ms>=1577836800000LL;
    const int64_t wall=utc_ms+(int64_t)s->record.timezone_minutes*60000;
    const int64_t day=clock_valid ? wall/DAY_MS : 0;
    bool new_day=false;
    if (clock_valid && (!s->record.day_valid || day!=s->record.day)) {
        memset(s->record.today,0,sizeof(s->record.today));
        s->record.day=day; s->record.day_valid=1; new_day=true;
    }
    const uint32_t elapsed=received_ms-s->received_ms;
    bool contiguous=fresh && s->baseline_valid && elapsed>0 && elapsed<=2000U &&
                    s->clock_valid==clock_valid;
    if (contiguous && clock_valid) {
        const int64_t wall_elapsed=wall-s->wall_ms;
        if (wall_elapsed<0 || wall_elapsed>(int64_t)elapsed+1000 ||
            wall_elapsed<(int64_t)elapsed-1000) contiguous=false;
    }
    if (contiguous) {
        uint32_t today_elapsed=clock_valid ? elapsed : 0;
        if (clock_valid) {
            const int64_t since_midnight=wall-day*DAY_MS;
            if (since_midnight<(int64_t)today_elapsed) today_elapsed=(uint32_t)since_midnight;
        }
        for (unsigned i=0;i<DIRECT_ENERGY_CHANNELS;++i) {
            if (i==DIRECT_ENERGY_GEN) continue;
            s->record.total[i]=saturating_add(s->record.total[i],(uint64_t)s->power[i]*elapsed);
            s->record.today[i]=saturating_add(s->record.today[i],(uint64_t)s->power[i]*today_elapsed);
        }
    }
    if (!fresh) s->baseline_valid=false;
    else if (!s->baseline_valid || elapsed || s->clock_valid!=clock_valid) {
        s->baseline_valid=true; s->received_ms=received_ms; s->wall_ms=wall;
        s->clock_valid=clock_valid; memcpy(s->power,power,sizeof(s->power));
    }
    return new_day;
}
