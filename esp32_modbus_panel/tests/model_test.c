#include "panel_types.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    static panel_snapshot_t s;
    uint16_t v = 999;
    assert(!panel_value(&s, 0x50, &v) && v == 999);
    s.valid[panel_slot(0x50)] = true;
    assert(panel_value(&s, 0x50, &v) && v == 0); /* real zero, not unknown */
    assert(!panel_value(&s, PANEL_REG_COUNT, &v));
    s.now_ms = 5000;
    assert(panel_fresh(&s, 0x50));
    s.now_ms++;
    assert(!panel_fresh(&s, 0x50));
    assert(panel_value(&s, 0x50, &v) && v == 0); /* stale remains displayable */
    s.updated_ms[panel_slot(0x50)] = UINT32_MAX - 10;
    s.now_ms = 20;
    assert(panel_fresh(&s, 0x50));
    assert(!panel_online(&s));
    s.valid[panel_slot(0x42)] = true;
    s.updated_ms[panel_slot(0x42)] = 20;
    assert(panel_online(&s));
    s.values[panel_slot(0x42)]=0xffff;assert(!panel_online(&s));
    s.values[panel_slot(0x42)]=3;assert(panel_online(&s));
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
    puts("PASS snapshot: unknown, zero, stale, wrap, device freshness");
}
