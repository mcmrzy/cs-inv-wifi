#include "direct_display_sample.h"
#include <assert.h>
#include <stdio.h>
int main(void) {
    direct_display_sample_t cache={0}; RunParamDef input={0},out;
    uint32_t age;
    input.BatVolt=524;
    assert(direct_display_sample_update(&cache,100,false,true,&input,100,&out,&age));
    /* A reception gap must keep the last real reading, not replace it by --. */
    assert(direct_display_sample_update(&cache,30100,false,false,NULL,0,&out,&age));
    assert(out.BatVolt==524 && age==30000);
    assert(direct_display_sample_update(&cache,3600100,false,false,NULL,0,&out,&age));
    assert(out.BatVolt==524 && age==3600000);
    input.BatVolt=0;
    assert(direct_display_sample_update(&cache,3600200,false,true,&input,3600200,&out,&age));
    assert(out.BatVolt==0 && age==0);
    assert(!direct_display_sample_update(&cache,3600300,true,false,NULL,0,&out,&age));
    assert(!direct_display_sample_update(&cache,3600400,false,true,&input,3600200,&out,&age));
    puts("PASS stale display: retains real readings through 30s/1h gaps, accepts new zero, clears for OTA/Boot only");
}
