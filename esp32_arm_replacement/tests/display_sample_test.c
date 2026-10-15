#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "direct_display_sample.h"

int main(void)
{
    direct_display_sample_t cache = {0};
    RunParamDef incoming = {0}, displayed;
    uint32_t age;
    memset(&displayed, 0xa5, sizeof(displayed));
    assert(!direct_display_sample_update(&cache, 100, false, false, &incoming, 0, &displayed, &age));
    assert(age == UINT32_MAX && displayed.BatVolt == 0);
    incoming.BatVolt = 497;
    assert(direct_display_sample_update(&cache, 1000, false, true, &incoming, 900, &displayed, &age));
    assert(displayed.BatVolt == 497 && displayed.OutputWatt == 0 && age == 100);
    /* A missed lock or rejected short frame must not blink known values away. */
    incoming.BatVolt = 65535;
    assert(direct_display_sample_update(&cache, 2000, false, false, &incoming, 2000, &displayed, &age));
    assert(displayed.BatVolt == 497 && age == 1100);
    /* The communication getter stops returning true after 2s, but LCD retains
       the actual old reception time and keeps the last reading until a new sample or an explicit pause. */
    assert(direct_display_sample_update(&cache, 5899, false, false, NULL, 0, &displayed, &age));
    assert(displayed.BatVolt == 497 && age == 4999);
    assert(direct_display_sample_update(&cache, 5900, false, false, NULL, 0, &displayed, &age));
    assert(displayed.BatVolt == 497 && age == 5000);
    incoming.BatVolt = 510;
    assert(direct_display_sample_update(&cache, 6100, false, true, &incoming, 6100, &displayed, &age));
    assert(displayed.BatVolt == 510 && age == 0);
    /* Sampling the same fresh getter repeatedly does not renew its age. */
    assert(direct_display_sample_update(&cache, 6500, false, true, &incoming, 6100, &displayed, &age));
    assert(age == 400);
    /* During OTA/Boot, even a fresh getter cannot expose pre-upgrade values. */
    assert(!direct_display_sample_update(&cache, 6600, true, true, &incoming, 6600, &displayed, &age));
    assert(age == UINT32_MAX);
    assert(!direct_display_sample_update(&cache, 6700, false, true, &incoming, 6100, &displayed, &age));
    incoming.BatVolt = 0;
    assert(direct_display_sample_update(&cache, 6800, false, true, &incoming, 6800, &displayed, &age));
    assert(displayed.BatVolt == 0 && age == 0); /* known zero is valid */
    cache = (direct_display_sample_t){0};
    assert(direct_display_sample_update(&cache, UINT32_MAX - 100, false, true, &incoming, UINT32_MAX - 100, &displayed, &age));
    assert(direct_display_sample_update(&cache, 99, false, false, NULL, 0, &displayed, &age));
    assert(age == 200);
    assert(direct_display_sample_update(&cache, 4899, false, false, NULL, 0, &displayed, &age));
    assert(age == 5000);
    assert(!direct_display_sample_update(&cache, UINT32_MAX - 2, true, false, NULL, 0, &displayed, &age));
    assert(direct_display_sample_update(&cache, 10, false, true, &incoming, 9, &displayed, &age));
    assert(age == 1);
    assert(direct_display_should_render(1, 0, false));
    assert(!direct_display_should_render(1099, 100, false));
    assert(!direct_display_should_render(1100, 100, false));
    assert(direct_display_should_render(101, 100, true)); /* keys immediate */
    assert(!direct_display_should_render(899, UINT32_MAX - 100, false));
    puts("PASS LCD samples: actual 04 data only, known zero, lock/missing-frame hold, stale reception retention, OTA/Boot reset, rollover, event drawing and immediate keys");
}
