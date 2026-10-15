#include "direct_display_sample.h"
#include <assert.h>
#include <stdio.h>
int main(void){
    assert(!direct_display_should_render(60000,100,false));
    assert(direct_display_should_render(60000,100,true));
    assert(direct_display_should_render(100,0,false));
    puts("PASS no periodic display trigger: elapsed time alone never redraws");
}
