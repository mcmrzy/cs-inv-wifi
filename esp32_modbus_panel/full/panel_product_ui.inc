/* Illustrated six-key views. Reuses the existing row pool and strip buffer. */
static void product_begin(unsigned mode,unsigned kind)
{
    if(product_mode!=mode || product_kind!=kind)lv_obj_invalidate(screen);
    product_mode=mode;product_kind=kind;
}

static void product_tile(lv_draw_ctx_t *ctx,const lv_area_t *a,unsigned index)
{
    static const panel_indexed_art_t *const details[]={&grid_category_art,&battery_category_art,
        &solar_category_art,&bms_category_art,&temperature_category_art,&generator_category_art,
        &energy_category_art,&modes_category_art};
    static const uint8_t *const detail_light_palettes[]={NULL,battery_category_light_palette,
        NULL,bms_category_light_palette,NULL,generator_category_light_palette,NULL,NULL};
    bool settings=product_mode==3;
    if(index>=(settings?PANEL_SETTING_GROUPS:PANEL_DETAIL_PAGES))return;
    tile_background(ctx,a,selection==(int)index);
    panel_indexed_art_t art=*(settings?setting_category_art[index]:details[index]);
    if(theme==&light){
        const uint8_t *palette=settings?setting_category_light_palettes[index]:detail_light_palettes[index];
        if(palette)art.palette=palette;
    }
    int x=a->x1+(settings?(lv_area_get_width(a)-art.width)/2:6);
    int y=a->y1+(settings?2:(lv_area_get_height(a)-art.height)/2);
    panel_indexed_art_draw(ctx,&art,x,y);
}

static int product_text_width(const char *text,const lv_font_t *font,int spacing)
{
    lv_point_t size;lv_txt_get_size(&size,text,font,spacing,0,LV_COORD_MAX,LV_TEXT_FLAG_NONE);
    return size.x;
}

static void product_fit(lv_obj_t *obj,const char *text,const lv_font_t *font,int width)
{
    int spacing=0;
    while(spacing>-2 && product_text_width(text,font,spacing)>width)spacing--;
    text_font(obj,font);
    text_spacing(obj,spacing);
}

static void product_gallery(const panel_view_t *v)
{
    bool settings=v->setting_groups;
    unsigned columns=settings?PANEL_SETTING_GROUP_COLUMNS:PANEL_DETAIL_COLUMNS,count=settings?PANEL_SETTING_GROUPS:PANEL_DETAIL_PAGES;
    int top=settings?52:49,pitch=settings?55:42,height=settings?50:38;
    int width=settings?96:148,label_x=settings?2:44,label_y=settings?28:9;
    layout(count,0,20,-1,false,false);
    for(unsigned i=0;i<count;i++){
        int x=8+(int)(i%columns)*(width+8),y=top+(int)(i/columns)*pitch;
        const char *name=settings?panel_setting_group_label(i,v->language_en):
            panel_detail_group_label(i,v->language_en);
        apply_row(i,x,y,width,height,name,"",v->selected==(int)i);
        background_opacity(rows[i].card,LV_OPA_TRANSP);
        text_align(rows[i].label,settings?LV_TEXT_ALIGN_CENTER:LV_TEXT_ALIGN_LEFT);
        int text_width=width-label_x-(settings?2:4);
        place(rows[i].label,label_x,label_y,text_width,-1);
        product_fit(rows[i].label,name,&panel_lvgl_body_font,text_width);
        label(rows[i].label,name,v->selected==(int)i?theme->blue:theme->text);
    }
    selection=v->selected;
}

static void product_power_action(const panel_view_t *v)
{
    layout(3,0,20,-1,false,v->activity_frame!=0);
    const char *action=v->rows[0];
    const lv_font_t *font=!v->language_en && product_text_width(action,&value_font,0)<=280?
        &value_font:&panel_lvgl_body_font;
    apply_row(0,8,56,304,72,action,"",true);
    product_fit(rows[0].label,action,font,280);
    place(rows[0].label,12,(72-(int)lv_font_get_line_height(font))/2,280,-1);
    label(rows[0].label,action,theme->warning);
    apply_row(1,8,139,304,28,v->rows[1],"",false);
    apply_row(2,8,170,304,22,v->rows[2],"",false);
}

static void product_value(unsigned row,int x,int y,int width,int height,
                          const char *name,const char *value,const lv_font_t *font,uint32_t color,bool english)
{
    row_t *r=place_row(row,x,y,width,height);
    bool editing=scene==SCENE_SETTING_EDITOR;
    bool draft=editing && row==2;
    fill(r->card,theme->card);
    background_opacity(r->card,draft?LV_OPA_TRANSP:LV_OPA_COVER);
    text_align(r->label,LV_TEXT_ALIGN_CENTER);
    text_align(r->value,LV_TEXT_ALIGN_CENTER);
    int text_width=width-8;
    const lv_font_t *name_font=english?&panel_lvgl_body_font:&small_font;
    bool wrap=english && editing;
    text_long_mode(r->value,wrap?LV_LABEL_LONG_WRAP:LV_LABEL_LONG_CLIP);
    if(!english && product_text_width(value,font,0)>text_width){
        if(font==&headline_font && product_text_width(value,&value_font,0)<=text_width)font=&value_font;
        else if(font!=&panel_lvgl_body_font && product_text_width(value,&panel_lvgl_body_font,0)<=text_width)
            font=&panel_lvgl_body_font;
        else font=&small_font;
    }
    product_fit(r->label,name,name_font,text_width);
    int name_h=(int)lv_font_get_line_height(name_font);
    int value_h=(int)lv_font_get_line_height(font);
    if(wrap){
        lv_point_t size;
        lv_txt_get_size(&size,value,font,0,0,text_width,LV_TEXT_FLAG_NONE);
        if(size.y>value_h)value_h=size.y;
        text_font(r->value,font);
        text_spacing(r->value,0);
    }else product_fit(r->value,value,font,text_width);
    int gap=height<40?-4:3;
    int name_y=editing?12:LV_MAX(english?0:2,(height-name_h-gap-value_h)/2);
    int value_y=editing?65-value_h/2:name_y+name_h+gap;
    place(r->label,4,name_y,text_width,-1);
    place(r->value,4,value_y,text_width,-1);
    label(r->label,name,theme->muted);
    label(r->value,value,color);
}

static void product_parameter(const panel_view_t *v,bool has_notice)
{
    layout(5,52,28,v->selected,false,v->activity_frame!=0);
    for(unsigned i=0;i<5;i++){
        apply_row(i,8,52+(int)i*28,304,25,v->rows[i],v->values[i],v->selected==(int)i);
        if((v->stale_rows|v->unread_rows)&(1U<<i))
            label(rows[i].value,v->values[i],theme->muted);
        if(v->read_error_rows&(1U<<i))label(rows[i].value,v->values[i],theme->warning);
    }
    if(has_notice)place(footer,4,195,312,-1);
}

static void product_editor(const panel_view_t *v,bool has_notice)
{
    layout(6,0,20,-1,false,v->activity_frame!=0);
    const char *current=v->stale_rows&2?(v->language_en?"Last data":"上次数据"):v->rows[1];
    uint32_t current_color=v->read_error_rows&2?theme->warning:
        (v->stale_rows|v->unread_rows)&2?theme->muted:theme->text;
    const lv_font_t *value=v->language_en?&panel_lvgl_body_font:&headline_font;
    product_value(1,8,58,148,104,current,v->values[1],value,current_color,v->language_en);
    product_value(2,164,58,148,104,v->rows[2],v->values[2],value,theme->blue,v->language_en);
    const lv_font_t *hint_font=v->language_en || product_text_width(v->subtitle,&panel_lvgl_body_font,0)<=292?
        &panel_lvgl_body_font:&small_font;
    product_fit(subtitle,v->subtitle,hint_font,292);
    apply_row(4,8,171,304,22,v->activity_frame?"":v->rows[4],"",false);
    if(!has_notice && v->rows[3][0]){
        apply_row(5,8,195,304,21,v->rows[3],"",false);
        if(v->language_en)product_fit(rows[5].label,v->rows[3],&panel_lvgl_body_font,280);
        else text_font(rows[5].label,&small_font);
    }else visible(rows[5].card,false);
    visible(rows[0].card,false);visible(rows[3].card,false);
}

static void dashboard_value(const panel_view_t *v,unsigned source,unsigned row,const char *name,
                            int x,int y,int width,int height,const lv_font_t *font)
{
    const char *value=v->values[source];
    bool stale=(v->stale_rows&(1U<<source))!=0;
    char marked[sizeof(v->values[0])+1];
    if(stale){
        snprintf(marked,sizeof(marked),"%s*",value);
        value=marked;
    }
    product_value(row,x,y,width,height,name,value,font,
                  stale || !strcmp(value,"--")?theme->muted:theme->text,v->language_en);
}

static void product_dashboard(const panel_view_t *v,bool has_notice)
{
    static const uint8_t primary[]={3,3,6,0,1,3,5,0};
    static const char *const names[][7]={
        {"Grid V","Grid A","Grid Hz",NULL,"Load V","Load W","Load %"},
        {"Pack V","Pack A","SOC",NULL,"Bus V","Bulk V","Float V"},
        {"Solar 1 V","Solar 1 A","Solar 1 W","Solar 2 V","Solar 2 A","Solar 2 W","Solar power"},
        {"Pack voltage","Pack current","Charge level","Battery link"},
        {"Solar 1 temp","Inverter temp","Charger 1","Charger 2","Ambient","Discharge","Solar 2 temp"},
        {"Gen V","Gen A","Gen Hz","Generator power","Enable","Input","Path"},
        {"Solar today","Solar total","Output V","Output A","Output Hz","Output power","Output VA"},
        {"Input range","Output priority","Charge priority","Battery","Parallel","Temp restart","Bypass"}
    };
    unsigned page=v->illustration%8,hero=primary[page],order[7],n=0;
    for(unsigned i=0;i<7;i++)if(i!=hero && v->rows[i][0])order[n++]=i;
    layout(7,0,20,-1,false,false);
    char summary[sizeof(v->subtitle)+16];
    const char *status=has_notice?v->footer:v->subtitle;
    if(v->stale_rows){
        const char *legend=v->language_en?"* Last data":"* 上次数据";
        snprintf(summary,sizeof(summary),"%s  %s",v->subtitle,legend);
        status=summary;
    }
    const lv_font_t *status_font=v->language_en || product_text_width(status,&panel_lvgl_body_font,0)<=292?
        &panel_lvgl_body_font:&small_font;
    product_fit(subtitle,status,status_font,292);
    label(subtitle,status,v->stale_rows?theme->warning:theme->muted);
    const char *const *en=names[page];
    /* Temperature and mode labels use glyphs outside the large numeric face. */
    const lv_font_t *hero_font=v->language_en && (page==4 || page==7)?
        &panel_lvgl_body_font:&value_font;
    dashboard_value(v,hero,0,v->language_en && en[hero]?en[hero]:v->rows[hero],
                    8,50,304,50,hero_font);
    for(unsigned i=0;i<6;i++){
        if(i>=n){visible(rows[i+1].card,false);continue;}
        unsigned source=order[i];
        dashboard_value(v,source,i+1,v->language_en && !(page==4 && source==0)?en[source]:v->rows[source],
                        8+(int)(i%2)*156,104+(int)(i/2)*38,148,36,&panel_lvgl_body_font);
    }
}
