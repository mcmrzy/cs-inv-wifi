#include "panel_types.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    static panel_snapshot_t s;
    assert(sizeof(s)<=1920U);
    uint16_t v = 999;
    assert(!panel_value(&s, 0x50, &v) && v == 999);
    assert(panel_store(&s,0x50,0,0));
    assert(panel_value(&s, 0x50, &v) && v == 0); /* real zero, not unknown */
    assert(!panel_value(&s, PANEL_REG_COUNT, &v));
    s.now_ms = 5000;
    assert(panel_fresh(&s, 0x50));
    s.now_ms++;
    assert(!panel_fresh(&s, 0x50));
    assert(panel_value(&s, 0x50, &v) && v == 0); /* stale remains displayable */
    assert(panel_store(&s,0x50,0,UINT32_MAX-10U));
    s.now_ms = 20;
    assert(panel_fresh(&s, 0x50));
    assert(!panel_online(&s));
    assert(panel_store(&s,0x42,0,20));
    assert(panel_online(&s));
    assert(panel_store(&s,0x42,0xffff,20) && !panel_online(&s));
    assert(panel_store(&s,0x42,3,20) && panel_online(&s));
    panel_result_record(&s,UINT32_MAX,PANEL_WRITE_CONFIRMED,0);
    panel_result_record(&s,0,PANEL_WRITE_REJECTED,3);
    assert(panel_result_find(&s,UINT32_MAX)->state==PANEL_WRITE_CONFIRMED);
    assert(panel_result_find(&s,0)->exception==3);
    panel_result_record(&s,1,PANEL_WRITE_PROCESSED,0);
    panel_result_record(&s,2,PANEL_WRITE_UNCERTAIN,0);
    panel_result_record(&s,3,PANEL_WRITE_CONFIRMED,0);
    assert(!panel_result_find(&s,UINT32_MAX));
    assert(panel_result_find(&s,0)->state==PANEL_WRITE_REJECTED);
    s.context_generation=9;s.ota_valid=7;s.detected_profile=PANEL_PROFILE_6KU;
    panel_config_t next={2,19200,1,PANEL_PROFILE_5KU};
    panel_device_context_reset(&s,&next);
    assert(s.context_generation==10 && s.config.address==2 && s.config.baud==19200);
    assert(!panel_online(&s) && !panel_value(&s,0x50,&v) && !s.ota_valid);
    assert(!s.detected_profile && panel_result_find(&s,3)->state==PANEL_WRITE_CONFIRMED);
    const uint16_t edges[]={0,31,32,63,64,0x1a0};
    for(unsigned i=0;i<sizeof(edges)/sizeof(edges[0]);i++)
        assert(panel_store(&s,edges[i],(uint16_t)i,20));
    assert(!panel_store(&s,0x100,0xffff,20));
    panel_invalidate(&s,31);panel_invalidate(&s,64);
    assert(!panel_value(&s,31,&v) && !panel_value(&s,64,&v));
    assert(panel_value(&s,0,&v) && v==0 && panel_value(&s,32,&v) && v==2);
    assert(panel_value(&s,63,&v) && v==3 && panel_value(&s,0x1a0,&v) && v==5);
    printf("PASS snapshot: %zu bytes, bitmap boundaries, unknown, zero, stale, wrap, device freshness\n",sizeof(s));
}
