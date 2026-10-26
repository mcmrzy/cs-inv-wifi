#ifndef DIRECT_ONBOARDING_SCENE_H
#define DIRECT_ONBOARDING_SCENE_H
#include "direct_gfx.h"
#include "onboarding_qr.h"
/* Precompute qr once outside the per-strip draw loop. No encoding/allocation here. */
void direct_onboarding_scene_draw(direct_gfx_t *,const onboarding_qr_t *,const char *sn,const char *pin,bool has_wifi,bool bound);
#endif
