#include "direct_energy_codec.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
int main(void)
{
    direct_energy_accumulator_t s;
    uint32_t p[DIRECT_ENERGY_CHANNELS];
    const int64_t noon=20000LL*86400000+43200000;
    direct_energy_power(100,200,-300,400,p);
    assert(p[DIRECT_ENERGY_AC_CHARGE]==200 && p[DIRECT_ENERGY_AC_DISCHARGE]==0);
    assert(p[DIRECT_ENERGY_BAT_CHARGE]==0 && p[DIRECT_ENERGY_BAT_DISCHARGE]==300);
    direct_energy_accumulator_init(&s,0);
    assert(direct_energy_accumulate(&s,1000,noon,true,true,p));
    assert(s.record.total[0]==0);
    assert(!direct_energy_accumulate(&s,2000,noon+1000,true,true,p));
    assert(s.record.today[0]==100000 && s.record.total[0]==100000);
    assert(s.record.total[DIRECT_ENERGY_AC_CHARGE]==200000);
    assert(s.record.total[DIRECT_ENERGY_GEN]==0);
    direct_energy_accumulate(&s,2000,noon+1000,true,true,p);
    assert(s.record.total[0]==100000); /* duplicated sample not counted */
    direct_energy_accumulate(&s,0,noon+2000,true,false,p);
    direct_energy_accumulate(&s,6000,noon+5000,true,true,p);
    assert(s.record.total[0]==100000); /* reconnect never spans offline gap */
    direct_energy_accumulate(&s,7000,noon+6000,true,true,p);
    assert(s.record.total[0]==200000);
    direct_energy_accumulate(&s,10000,noon+9000,true,true,p);
    assert(s.record.total[0]==200000); /* elapsed >2s not invented */
    direct_energy_accumulate(&s,11000,noon+3609000,true,true,p);
    assert(s.record.total[0]==200000); /* clock jump not invented */
    direct_energy_accumulate(&s,12000,noon+3608000,true,true,p);
    assert(s.record.total[0]==200000); /* backwards clock */
    direct_energy_accumulator_init(&s,0);
    const int64_t before_midnight=20001LL*86400000-500;
    direct_energy_accumulate(&s,1000,before_midnight,true,true,p);
    assert(direct_energy_accumulate(&s,2000,before_midnight+1000,true,true,p));
    assert(s.record.today[0]==50000 && s.record.total[0]==100000);
    direct_energy_record_t record=s.record;
    assert(direct_energy_record_load(&s,&record));
    direct_energy_accumulate(&s,50000,before_midnight+100000,true,true,p);
    assert(s.record.total[0]==100000); /* restoring counters never restores baseline */
    record.magic=0;
    assert(!direct_energy_record_load(&s,&record));
    record=s.record; record.today[0]=record.total[0]+1;
    assert(!direct_energy_record_load(&s,&record));
    direct_energy_accumulator_timezone(&s,480);
    assert(s.record.today[0]==0 && s.record.total[0]==100000 && !s.baseline_valid);
    assert(strcmp(DIRECT_ENERGY_NVS_NAMESPACE,"dir_energy")==0);
    assert(strcmp(DIRECT_ENERGY_NVS_NAMESPACE,"ota_sec")!=0);
    direct_energy_accumulator_init(&s,0);
    direct_energy_accumulate(&s,1000,0,false,true,p);
    direct_energy_accumulate(&s,2000,0,false,true,p);
    assert(s.record.today[0]==0 && s.record.total[0]==100000);
    direct_energy_accumulate(&s,3000,noon,true,true,p);
    assert(s.record.today[0]==0 && s.record.total[0]==100000);
    direct_energy_power(1,-200,300,-1,p);
    assert(p[DIRECT_ENERGY_AC_CHARGE]==0 && p[DIRECT_ENERGY_AC_DISCHARGE]==200);
    assert(p[DIRECT_ENERGY_BAT_CHARGE]==300 && p[DIRECT_ENERGY_BAT_DISCHARGE]==0);
    assert(p[DIRECT_ENERGY_OUTPUT]==0);
    puts("direct energy tests passed");
    return 0;
}
