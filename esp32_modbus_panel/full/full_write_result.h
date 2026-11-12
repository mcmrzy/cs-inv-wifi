#pragma once
#include "panel_types.h"
/* Separate unknown adoption from rejected/submission failures on old APIs. */
#define FULL_WRITE_UNCONFIRMED (-22)
#define FULL_WRITE_PROTOCOL_ACCEPTED (-23)
static inline int full_write_result(panel_write_state_t state,bool ours)
{
    if(!ours || state==PANEL_WRITE_PROCESSED || state==PANEL_WRITE_UNCERTAIN)
        return FULL_WRITE_UNCONFIRMED;
    if(state==PANEL_WRITE_PROTOCOL_ACCEPTED)return FULL_WRITE_PROTOCOL_ACCEPTED;
    return state==PANEL_WRITE_CONFIRMED?0:-21;
}
