#pragma once
#include "direct_ui.h"
#include "direct_gfx.h"
#include "panel_ui.h"
#include "panel_bms_protocol.h"
#define FULL_UI_OPEN_MODBUS ((direct_ui_action_t)99)
#define FULL_UI_SAVE_BMS_PROTOCOL ((direct_ui_action_t)100)
#define FULL_UI_DETECT_BMS ((direct_ui_action_t)101)
typedef enum {FULL_BATTERY_NONE,FULL_BATTERY_INVERTER,FULL_BATTERY_DIRECT} full_battery_source_t;
static inline bool full_inverter_alarm_active(const direct_ui_data_t *d)
{
    return d->status_valid && (d->status.fault || d->status.warning[0] ||
                               d->status.warning[1] || d->status.warning[2]);
}
static inline bool full_battery_sample_valid(const direct_ui_data_t *d)
{
    if(!d || (!d->bms_received && !d->bms_online))return false;
    const direct_bms_measurement_t *b=&d->bms;
    unsigned scale=b->percent_divisor?b->percent_divisor:1U;
    /* Do not promote decoded unavailable-register sentinels to pack data. */
    return b->voltage_10mv>0 && b->voltage_10mv!=327670U && b->voltage_10mv!=655350U &&
        b->current_10ma!=327670 && b->current_10ma!=-327680 &&
        b->soc_percent<=100U*scale && b->soh_percent<=100U*scale;
}
static inline full_battery_source_t full_battery_source(const direct_ui_data_t *d)
{
    return full_battery_sample_valid(d)?FULL_BATTERY_DIRECT:FULL_BATTERY_NONE;
}
static inline unsigned full_battery_cell_pages(const direct_ui_data_t *d)
{
    if(!full_battery_sample_valid(d))return 0;
    unsigned cells=0;
    for(unsigned i=0;i<32;i++)if(d->bms.cell_mv[i] && d->bms.cell_mv[i]<=8191U)cells=i+1;
    return (cells+7U)/8U;
}
static inline unsigned full_battery_pages(const direct_ui_data_t *d)
{
    return 1+full_battery_cell_pages(d);
}
static inline unsigned full_battery_page(const direct_ui_state_t *u,const direct_ui_data_t *d)
{
    unsigned count=full_battery_pages(d);
    return u->detail_page<count?u->detail_page:0;
}
static inline bool full_battery_cells(const direct_ui_state_t *u,const direct_ui_data_t *d)
{
    unsigned page=full_battery_page(u,d);
    return page && page<=full_battery_cell_pages(d);
}
/* event.index is the native destination; value is a setting category. */
/* Changes on successful local Modbus context saves; BMS is independent. */
uint32_t full_link_context_generation(void);
/* Reload committed LCD/local preferences into the direct compatibility cache. */
void full_ui_reload_local_preferences(void);
void full_ui_scene_draw_frame(direct_gfx_t *,const direct_ui_state_t *,
                              const direct_ui_data_t *,const direct_ui_view_t *,uint8_t focus);
