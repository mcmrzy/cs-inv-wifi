#include "onboarding_state.h"
#include <assert.h>
#include <stdio.h>
int main(void) {
    onboarding_state_t s;onboarding_state_init(&s);
    onboarding_inputs_t i={.identity_ready=true};
    assert(onboarding_state_tick(&s,&i,100,true)==ONBOARDING_OPENED);
    assert(onboarding_state_tick(&s,&i,20099,true)==ONBOARDING_NONE);
    assert(onboarding_state_tick(&s,&i,20100,true)==ONBOARDING_CLOSED);
    assert(onboarding_state_tick(&s,&i,21000,true)==ONBOARDING_NONE);
    assert(onboarding_state_open(&s,&i,22000)==ONBOARDING_OPENED);
    onboarding_state_activity(&s,40000);
    assert(onboarding_state_tick(&s,&i,99999,true)==ONBOARDING_NONE);
    assert(onboarding_state_tick(&s,&i,100000,true)==ONBOARDING_CLOSED);
    assert(onboarding_state_open(&s,&i,110000)==ONBOARDING_OPENED);
    assert(onboarding_state_back(&s)==ONBOARDING_SAVE_DISMISSED);
    i.dismissed=true;onboarding_state_init(&s);
    assert(onboarding_state_tick(&s,&i,0,true)==ONBOARDING_NONE);
    assert(onboarding_state_open(&s,&i,0)==ONBOARDING_OPENED);
    i.alarm=true;assert(onboarding_state_tick(&s,&i,1,true)==ONBOARDING_CLOSED);
    assert(onboarding_state_open(&s,&i,1)==ONBOARDING_NONE);
    i.alarm=false;i.ota=true;assert(onboarding_state_open(&s,&i,1)==ONBOARDING_NONE);
    i.ota=false;i.factory=true;assert(onboarding_state_open(&s,&i,1)==ONBOARDING_NONE);
    i.factory=false;i.editing=true;assert(onboarding_state_open(&s,&i,1)==ONBOARDING_NONE);
    i.editing=false;i.dismissed=false;i.has_wifi=true;i.binding_known=false;
    onboarding_state_init(&s);assert(onboarding_state_tick(&s,&i,0,true)==ONBOARDING_NONE);
    i.binding_known=true;onboarding_state_init(&s);assert(onboarding_state_tick(&s,&i,0,true)==ONBOARDING_OPENED);
    i.bound=true;assert(onboarding_state_tick(&s,&i,1,true)==ONBOARDING_CLOSED);
    assert(onboarding_state_open(&s,&i,2)==ONBOARDING_OPENED);
    assert(onboarding_state_tick(&s,&i,3,true)==ONBOARDING_NONE);
    assert(onboarding_state_back(&s)==ONBOARDING_SAVE_DISMISSED);
    onboarding_state_init(&s);i.has_wifi=false;i.bound=false;
    assert(onboarding_state_tick(&s,&i,UINT32_MAX-100,true)==ONBOARDING_OPENED);
    assert(onboarding_state_tick(&s,&i,19899,true)==ONBOARDING_CLOSED);
    puts("onboarding state: PASS (timeouts, blockers, unknown binding, dismissal, wrap)");return 0;
}
