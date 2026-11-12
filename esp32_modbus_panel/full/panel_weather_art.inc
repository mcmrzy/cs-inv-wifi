/* Compact native weather marks. No image framebuffer or extra LVGL objects. */
static bool home_weather_valid,home_weather_stale;
static int16_t home_weather_temperature;
static uint16_t home_weather_code;
void panel_lvgl_weather(const panel_weather_snapshot_t *w)
{
    bool valid=w && w->valid,stale=valid && w->stale;
    int16_t temperature=valid?w->temperature_10:0;
    uint16_t code=valid?w->code:0;
    if(valid==home_weather_valid && stale==home_weather_stale &&
       temperature==home_weather_temperature && code==home_weather_code)return;
    home_weather_valid=valid;home_weather_stale=stale;
    home_weather_temperature=temperature;home_weather_code=code;
    if(screen && scene==UI_HOME){const lv_area_t area={84,3,151,25};lv_obj_invalidate_area(screen,&area);}
}
static void weather_home_draw(lv_draw_ctx_t *ctx)
{
    static const uint16_t sun[]={0x0100,0x0100,0x2448,0x13b0,0x07c0,0x0fe0,0x1ff0,0xdff6,
        0x1ff0,0x0fe0,0x07c0,0x13b0,0x2448,0x0100,0x0100,0};
    static const uint16_t cloud[]={0,0,0,0x0380,0x07c0,0x0fe0,0x3ff0,0x7ffc,
        0x7ffe,0x7ffe,0x3ffc,0,0,0,0,0};
    bool live=home_weather_valid && !home_weather_stale;
    unsigned code=home_weather_code;
    uint32_t ink=live?(code<3?theme->warning:theme->text):theme->muted;
    const uint16_t *shape=home_weather_valid && code<3?sun:cloud;
    for(unsigned y=0;y<16;y++)for(unsigned x=0;x<16;x++)
        if(shape[y]&(0x8000U>>x))icon_bar(ctx,86+(int)x,5+(int)y,1,1,ink);
    if(home_weather_valid && code>=51){
        bool snow=(code>=71 && code<=77)||code==85||code==86;
        for(int x=0;x<3;x++){
            uint32_t color=live?theme->blue:theme->muted;
            icon_bar(ctx,89+x*4,17,1,3,color);
            if(snow)icon_bar(ctx,88+x*4,18,3,1,color);
        }
    }else if(home_weather_valid && (code==45 || code==48)){
        icon_bar(ctx,87,18,13,1,ink);icon_bar(ctx,89,21,11,1,ink);
    }
    char value[12];
    if(home_weather_valid){
        int n=home_weather_temperature;
        snprintf(value,sizeof(value),"%d°",(n+(n<0?-5:5))/10);
    }else strcpy(value,"--");
    lv_draw_label_dsc_t text;lv_draw_label_dsc_init(&text);
    text.font=&panel_lvgl_body_font;text.color=lv_color_hex(live?theme->text:theme->muted);
    const lv_area_t area={106,3,151,24};lv_draw_label(ctx,&text,&area,value,NULL);
}
