#ifndef ONBOARDING_STATE_H
#define ONBOARDING_STATE_H
#include <stdbool.h>
#include <stdint.h>
typedef struct {
    bool has_wifi, bound, binding_known, identity_ready, dismissed;
    bool alarm, ota, factory, editing;
} onboarding_inputs_t;
typedef struct {
    bool visible, automatic, boot_considered;
    uint32_t last_activity_ms;
} onboarding_state_t;
typedef enum { ONBOARDING_NONE, ONBOARDING_OPENED, ONBOARDING_CLOSED,
               ONBOARDING_SAVE_DISMISSED } onboarding_change_t;
void onboarding_state_init(onboarding_state_t *state);
onboarding_change_t onboarding_state_tick(onboarding_state_t *, const onboarding_inputs_t *, uint32_t now_ms, bool boot_ready);
onboarding_change_t onboarding_state_open(onboarding_state_t *, const onboarding_inputs_t *, uint32_t now_ms);
onboarding_change_t onboarding_state_back(onboarding_state_t *);
void onboarding_state_activity(onboarding_state_t *, uint32_t now_ms);
#endif
