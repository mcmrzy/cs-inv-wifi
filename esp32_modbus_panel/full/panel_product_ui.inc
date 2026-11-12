/* Illustrated six-key views. Reuses the existing row pool and strip buffer. */
static void product_begin(unsigned mode,unsigned kind)
{
    if(product_mode!=mode || product_kind!=kind)lv_obj_invalidate(screen);
    product_mode=mode;product_kind=kind;
}

static void product_tile(lv_draw_ctx_t *ctx,const lv_area_t *a,unsigned index)
{
    tile_background(ctx,a,selection==(int)index);
    unsigned kind=product_kind+index;
    static const uint8_t details[]={9,2,10,5,11,4,12,13};
    unsigned icon=product_mode==3?kind:details[kind%8];
    int x=a->x1+(lv_area_get_width(a)-32)/2;
    product_icon(ctx,icon,x,a->y1+9,selection==(int)index?theme->blue:theme->muted);
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
    if(lv_obj_get_style_text_letter_space(obj,0)!=spacing)
        lv_obj_set_style_text_letter_space(obj,spacing,0);
}

static void product_gallery(const panel_view_t *v)
{
    static const char *const setting_en[]={"All settings","Output","Battery","Equalize","Generator",
        "BMS link","System","Parallel","Output 2"};
    static const char *const detail_en[]={"Grid & load","Battery","Solar","Battery status",
        "Temperatures","Generator","Energy","Modes"};
    unsigned columns=v->detail_groups?2:3,count=v->detail_groups?4:6;
    unsigned shown=0;for(unsigned i=0;i<count;i++)shown+=v->rows[i][0]!=0;
    int top=shown && shown<=columns?89:49;
    layout(count,0,20,-1,false,false);
    for(unsigned i=0;i<count;i++){
        if(!v->rows[i][0]){visible(rows[i].card,false);continue;}
        int width=columns==2?148:96;
        int x=8+(int)(i%columns)*(width+8),y=top+(int)(i/columns)*82;
        char text[128];snprintf(text,sizeof(text),"%s",v->rows[i]);
        char *count_text=strrchr(text,'(');
        if(count_text){while(count_text>text && count_text[-1]==' ')count_text--;*count_text=0;}
        unsigned index=v->illustration+i;
        const char *name=v->language_en?(v->detail_groups?detail_en[index]:setting_en[index]):text;
        apply_row(i,x,y,width,76,name,"",v->selected==(int)i);
        background_opacity(rows[i].card,LV_OPA_TRANSP);
        text_align(rows[i].label,LV_TEXT_ALIGN_CENTER);
        place(rows[i].label,2,50,width-4,-1);
        product_fit(rows[i].label,name,&panel_lvgl_body_font,width-4);
        label(rows[i].label,name,v->selected==(int)i?theme->blue:theme->text);
    }
    selection=v->selected;
}

static void product_value(unsigned row,int x,int y,int width,int height,
                          const char *name,const char *value,bool large,bool stale)
{
    apply_row(row,x,y,width,height,name,"",false);
    label(rows[row].label,name,theme->muted);
    label(rows[row].value,value,stale?theme->muted:theme->text);
    text_align(rows[row].label,LV_TEXT_ALIGN_CENTER);
    text_align(rows[row].value,LV_TEXT_ALIGN_CENTER);
    product_fit(rows[row].label,name,&small_font,width-8);
    const lv_font_t *font=large?&value_font:&panel_lvgl_body_font;
    lv_point_t size;lv_txt_get_size(&size,value,font,0,0,LV_COORD_MAX,LV_TEXT_FLAG_NONE);
    if(size.x>width-8)font=&panel_lvgl_body_font;
    lv_txt_get_size(&size,value,font,0,0,LV_COORD_MAX,LV_TEXT_FLAG_NONE);
    if(size.x>width-8)font=&small_font;
    product_fit(rows[row].value,value,font,width-8);
    int name_h=(int)lv_font_get_line_height(&small_font);
    int value_h=(int)lv_font_get_line_height(font);
    int gap=large?8:4,top=(height-name_h-gap-value_h)/2;
    if(top<2)top=2;
    place(rows[row].label,4,top,width-8,-1);
    place(rows[row].value,4,top+name_h+gap,width-8,-1);
}

static void product_parameter(const panel_view_t *v,bool has_notice)
{
    label(subtitle,v->language_en?"Select, then ENTER to edit":"上下选择，确认修改",theme->muted);
    layout(5,0,20,-1,false,v->activity_frame!=0);
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
    product_value(1,8,58,148,104,v->rows[1],v->values[1],true,
        ((v->stale_rows|v->unread_rows)&2)!=0);
    product_value(2,164,58,148,104,v->rows[2],v->values[2],true,false);
    for(unsigned i=1;i<=2;i++){
        place(rows[i].label,4,12,140,-1);
        int line=(int)lv_font_get_line_height(lv_obj_get_style_text_font(rows[i].value,0));
        place(rows[i].value,4,65-line/2,140,-1);
    }
    background_opacity(rows[2].card,LV_OPA_TRANSP);
    label(rows[2].value,v->values[2],theme->blue);
    apply_row(4,8,171,304,22,v->rows[4],"",false);
    apply_row(5,8,195,304,21,has_notice?"":v->rows[3],"",false);
    text_font(rows[5].label,&small_font);
    visible(rows[0].card,false);visible(rows[3].card,false);
}

static void product_pair(unsigned row,int x,int y,int w,int h,const char *name,const char *value,bool stale)
{
    apply_row(row,x,y,w,h,name,value,false);
    int vw=product_text_width(value,&small_font,0)+6;
    int min_label=product_text_width(name,&small_font,-2)+2;
    if(min_label>w/2)min_label=w/2;
    int max_value=w-16-min_label;
    if(vw>max_value)vw=max_value;
    int lw=w-16-vw,top=(h-(int)lv_font_get_line_height(&small_font))/2;
    place(rows[row].label,6,top,lw,-1);
    place(rows[row].value,w-6-vw,top,vw,-1);
    product_fit(rows[row].label,name,&small_font,lw);
    product_fit(rows[row].value,value,&small_font,vw);
    if(stale)label(rows[row].value,value,theme->muted);
}

static void product_dashboard(const panel_view_t *v)
{
    static const uint8_t primary[]={3,3,6,0,1,3,5,0};
    static const char *const names[][7]={
        {"Grid V","Grid A","Grid Hz",NULL,"Load V","Load W","Load %"},
        {"Pack V","Pack A","SOC",NULL,"Bus V","Bulk V","Float V"},
        {"Solar 1 V","Solar 1 A","Solar 1 W","Solar 2 V","Solar 2 A","Solar 2 W","Solar power"},
        {"Pack V","Pack A","SOC","Pack temp","Link","Faults","Warnings"},
        {"Solar 1 temp","Inverter temp","Charger 1","Charger 2","Ambient","Discharge","Solar 2 temp"},
        {"Gen V","Gen A","Gen Hz","Generator power","Enable","Input","Path"},
        {"Solar today","Solar total","Output V","Output A","Output Hz","Output power","Output VA"},
        {"Mode","Output","Charging","Battery","Parallel","Temp restart","Bypass"}
    };
    unsigned page=v->illustration%8,hero=primary[page],order[7],n=0;
    for(unsigned i=0;i<7;i++)if(i!=hero && v->rows[i][0])order[n++]=i;
    layout(7,0,20,-1,false,false);
    const char *const *en=names[page];
    product_value(0,8,49,304,65,v->language_en && en[hero]?en[hero]:v->rows[hero],v->values[hero],true,
        (v->stale_rows&(1U<<hero))!=0);
    for(unsigned i=0;i<6;i++){
        if(i>=n){visible(rows[i+1].card,false);continue;}
        unsigned source=order[i];
        product_pair(i+1,8+(int)(i%2)*156,123+(int)(i/2)*30,148,26,
            v->language_en?en[source]:v->rows[source],v->values[source],(v->stale_rows&(1U<<source))!=0);
    }
}
