#include "onboarding_state.h"
#include <string.h>
static bool blocked(const onboarding_inputs_t *i) { return i->alarm || i->ota || i->factory || i->editing; }
void onboarding_state_init(onboarding_state_t *s) { if(s)memset(s,0,sizeof(*s)); }
onboarding_change_t onboarding_state_tick(onboarding_state_t *s,const onboarding_inputs_t *i,uint32_t now,bool boot_ready) {
    if(!s || !i)return ONBOARDING_NONE;
    if(s->visible && (blocked(i) || (s->automatic && i->has_wifi && i->bound) ||
       (uint32_t)(now-s->last_activity_ms)>=(s->automatic?20000u:60000u))) {
        s->visible=false; return ONBOARDING_CLOSED;
    }
    if(!s->boot_considered && boot_ready && !blocked(i)) {
        s->boot_considered=true;
        if(!i->dismissed && i->identity_ready && (!i->has_wifi || (i->binding_known && !i->bound))) {
            s->visible=true;s->automatic=true;s->last_activity_ms=now;return ONBOARDING_OPENED;
        }
    }
    return ONBOARDING_NONE;
}
onboarding_change_t onboarding_state_open(onboarding_state_t *s,const onboarding_inputs_t *i,uint32_t now) {
    if(!s || !i || blocked(i))return ONBOARDING_NONE;
    s->boot_considered=true;s->visible=true;s->automatic=false;s->last_activity_ms=now;return ONBOARDING_OPENED;
}
onboarding_change_t onboarding_state_back(onboarding_state_t *s) {
    if(!s || !s->visible)return ONBOARDING_NONE;
    s->visible=false;return ONBOARDING_SAVE_DISMISSED;
}
void onboarding_state_activity(onboarding_state_t *s,uint32_t now) { if(s && s->visible)s->last_activity_ms=now; }
