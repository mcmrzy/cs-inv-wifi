#include "panel_lvgl.h"
#include "full_ui_bridge.h"
#include "direct_gfx_assets.h"
#include "direct_home.h"
#include "direct_ui_locale.h"
#include "lvgl.h"
#include "panel_home_art.h"
#include "panel_font_codec.h"
#include "panel_wifi.h"
#include "panel_key_labels.h"
#include "direct_boot_logo.h"
#include <stdio.h>
#include <string.h>

enum { WIDTH=320, HEIGHT=240, ROWS=9, HOME_CARD_WIDTH=94, HOME_CARD_HEIGHT=82, SCENE_ALARMS=0x104U };
typedef struct { lv_obj_t *card,*label,*value; } row_t;
typedef struct {
    uint32_t bg,card,selected,text,muted,blue,line,warning,header,success;
} theme_t;
static const theme_t dark={
    .bg=0x101113,.card=0x181e25,.selected=0x193244,.text=0xf4f7fc,.muted=0xb8c5d2,
    .blue=0x29a7ff,.line=0x3c454d,.warning=0xffd447,.header=0x1b222b,.success=0x7edca7,
};
static const theme_t light={
    .bg=0xf4f7fc,.card=0xffffff,.selected=0xe2eeff,.text=0x111d30,.muted=0x475971,
    .blue=0x1458b6,.line=0xd1def1,.warning=0x915900,.header=0xe6eef9,.success=0x146544,
};
static const theme_t *theme=&dark;
static lv_obj_t *screen,*title,*subtitle,*page_number,*footer,*marker,*track,*indicator,*qr_box;
static lv_obj_t *dots[4];
static char key_hints[KEY_SLOTS][24];
static row_t rows[ROWS];
static lv_disp_draw_buf_t draw_buffer;
static lv_disp_drv_t driver;
static lv_disp_t *display;
static panel_lvgl_flush_fn write_pixels;
static uint32_t tick_ms;
static uint32_t scene=UINT32_MAX;
static int selection=-1;
static bool waiting;
static const onboarding_qr_t *qr_data;
static uint32_t qr_generation;
static int home_wifi=-1;
static bool home_bluetooth;
static bool home_alarm;
static uint8_t home_motion,home_reverse,home_phase=UINT8_MAX;
static unsigned product_mode,product_kind;
static void product_tile(lv_draw_ctx_t *ctx,const lv_area_t *area,unsigned index);
static int menu_focus=255;
static bool bms_overview,bms_live,bms_has_soc;
static unsigned bms_soc;
static unsigned bms_page;
static full_battery_source_t bms_source;
static int wifi_key=-1;
static unsigned wifi_mode;
static bool wifi_zh;
static bool city_keyboard;
static uint32_t splash_started;
static unsigned splash_elapsed;
#include "panel_device_art.inc"
#include "panel_wireless_icons.inc"
static void icon_bar(lv_draw_ctx_t *ctx,int x,int y,int w,int h,uint32_t color);
static void card_fill(lv_draw_ctx_t *ctx,const lv_area_t *area,uint32_t color)
{
    int x=area->x1,y=area->y1,w=lv_area_get_width(area),h=lv_area_get_height(area);
    /* The embedded LVGL build uses simple rectangles. Draw corners as spans. */
    icon_bar(ctx,x+3,y,w-6,1,color);
    icon_bar(ctx,x+1,y+1,w-2,2,color);
    icon_bar(ctx,x,y+3,w,h-6,color);
    icon_bar(ctx,x+1,y+h-3,w-2,2,color);
    icon_bar(ctx,x+3,y+h-1,w-6,1,color);
}

static void outline_card(lv_draw_ctx_t *ctx,const lv_area_t *area,uint32_t background,uint32_t border)
{
    card_fill(ctx,area,border);
    if(background!=border){
        const lv_area_t inside={area->x1+1,area->y1+1,area->x2-1,area->y2-1};
        card_fill(ctx,&inside,background);
    }
}
static void wireless_mask(lv_draw_ctx_t *ctx,const uint8_t *mask,int x,int y,int w,int h,int level)
{
    lv_area_t bounds={x,y,x+w-1,y+h-1},clip;
    if(!_lv_area_intersect(&clip,&bounds,ctx->clip_area))return;
    lv_draw_rect_dsc_t ink;
    lv_draw_rect_dsc_init(&ink);
    for(int row=clip.y1;row<=clip.y2;row++)for(int col=clip.x1;col<=clip.x2;col++){
        unsigned index=(unsigned)((row-y)*w+col-x);
        unsigned pixel=(mask[index/2]>>(4*(index&1)))&15;
        if(!pixel)continue;
        ink.bg_opa=(pixel&3)*85;
        ink.bg_color=lv_color_hex(level>=(int)(pixel>>2)?theme->blue:theme->muted);
        lv_area_t dot={col,row,col,row};
        lv_draw_rect(ctx,&ink,&dot);
    }
}
static void wireless_icons(lv_draw_ctx_t *ctx)
{
    wireless_mask(ctx,wifi_mask,270,5,22,18,home_wifi);
    wireless_mask(ctx,bluetooth_mask,297,4,16,20,home_bluetooth?1:0);
}
static const lv_point_t wires[4][3]={
    {{102,68},{127,68},{127,82}},{{217,68},{192,68},{192,82}},
    {{102,178},{127,178},{127,162}},{{217,178},{192,178},{192,162}},
};
static const lv_point_t home_cards[4]={{8,35},{218,35},{8,126},{218,126}};

static uint32_t home_color(unsigned index)
{
    const uint32_t colors[]={theme->blue,theme->warning,theme->success,theme==&dark?0x65d2dd:0x176674};
    return colors[index];
}

static void home_artwork(lv_draw_ctx_t *ctx)
{
    for(unsigned i=0;i<4;i++){
        const lv_point_t *p=wires[i];
        int left=LV_MIN(p[0].x,p[1].x),top=LV_MIN(p[1].y,p[2].y);
        icon_bar(ctx,left,p[0].y,LV_ABS(p[1].x-p[0].x)+1,2,theme->line);
        icon_bar(ctx,p[1].x,top,2,LV_ABS(p[2].y-p[1].y)+2,theme->line);
        const int x=home_cards[i].x,y=home_cards[i].y;
        const lv_area_t card={x,y,x+HOME_CARD_WIDTH-1,y+HOME_CARD_HEIGHT-1};
        outline_card(ctx,&card,theme->card,theme->line);
        icon_bar(ctx,x+7,y+9,2,12,home_color(i));
    }
    panel_home_art_draw(ctx,102,80);
}

/* Reuse the shipped, licensed CJK subset. LVGL packs pixels continuously
 * across rows while the legacy atlas padded each row to a whole byte. */
static bool glyph_info(const lv_font_t *font,lv_font_glyph_dsc_t *out,uint32_t ch,uint32_t next)
{
    (void)next;
    if(ch<32){*out=(lv_font_glyph_dsc_t){0};return true;}
    int size=(int)(uintptr_t)font->dsc;
    const direct_gfx_glyph_t *g=panel_font_glyph_info(ch,size);
    *out=(lv_font_glyph_dsc_t){.adv_w=g->advance,.box_w=g->width,.box_h=g->height,
        .ofs_y=size>=24?(g->height==20?2:4):size==12 && g->height==12?2:0,.bpp=4};
    return true;
}

static const uint8_t *glyph_pixels(const lv_font_t *font,uint32_t ch)
{
    if(ch<32)return NULL;
    int size=(int)(uintptr_t)font->dsc;
    const direct_gfx_glyph_t *g=direct_gfx_glyph(ch,size);
    if(g->bpp!=4)return NULL;
    if(!(g->width&1))return g->data;
    uint8_t *packed=(uint8_t *)g->data;
    unsigned count=(unsigned)g->width*g->height;
    unsigned pending=0;
    for(unsigned y=0;y<g->height;y++)for(unsigned x=0;x<g->width;x++){
        unsigned pixel=y*g->width+x;
        const uint8_t *row=g->data+y*g->stride;
        unsigned alpha=(row[x/2]>>(x&1?0:4))&15U;
        if(!(pixel&1))pending=alpha<<4;
        else packed[pixel/2]=(uint8_t)(pending|alpha);
    }
    if(count&1)packed[count/2]=(uint8_t)pending;
    return packed;
}

const lv_font_t panel_lvgl_body_font={.get_glyph_dsc=glyph_info,.get_glyph_bitmap=glyph_pixels,.line_height=20,.dsc=(const void *)16};
static const lv_font_t value_font={.get_glyph_dsc=glyph_info,.get_glyph_bitmap=glyph_pixels,.line_height=28,.dsc=(const void *)24};
static const lv_font_t small_font={.get_glyph_dsc=glyph_info,.get_glyph_bitmap=glyph_pixels,.line_height=20,.dsc=(const void *)12};
static int footer_spacing(const char *text,int width)
{
    lv_point_t extent;
    for(int spacing=0;spacing>-2;spacing--){
        lv_txt_get_size(&extent,text,&panel_lvgl_body_font,spacing,0,LV_COORD_MAX,LV_TEXT_FLAG_NONE);
        if(extent.x<=width)return spacing;
    }
    return -2;
}
static const lv_font_t headline_font={.get_glyph_dsc=glyph_info,.get_glyph_bitmap=glyph_pixels,.line_height=36,.dsc=(const void *)32};
#include "panel_weather_art.inc"

static lv_area_t wifi_key_area(int i)
{
    int x,y,w;
    if(i<PANEL_WIFI_KEYS){x=8+i%PANEL_WIFI_COLUMNS*31;y=110+i/PANEL_WIFI_COLUMNS*21;w=29;}
    else if(i<PANEL_WIFI_ACTION_FIRST){x=8+(i-PANEL_WIFI_MODE_FIRST)*77;y=87;w=75;}
    else{x=9+(i-PANEL_WIFI_ACTION_FIRST)*101;y=198;w=98;}
    return (lv_area_t){x,y,x+w-1,y+19};
}

static void draw_wifi_keys(lv_draw_ctx_t *ctx)
{
    static const char *const zh[]={"退格","连接","取消"},*const en[]={"Delete","Connect","Cancel"};
    static const char *const modes[]={"abc","ABC","123","#+="};
    const char *chars=panel_wifi_charset(wifi_mode);unsigned count=(unsigned)strlen(chars);
    lv_draw_label_dsc_t text;
    lv_draw_label_dsc_init(&text);text.font=&panel_lvgl_body_font;text.align=LV_TEXT_ALIGN_CENTER;
    for(int i=0;i<PANEL_WIFI_ACTION_FIRST+3;i++){
        if(i<PANEL_WIFI_KEYS && i>=(int)count)continue;
        lv_area_t key=wifi_key_area(i),clip;
        if(!_lv_area_intersect(&clip,&key,ctx->clip_area))continue;
        icon_bar(ctx,key.x1,key.y1,lv_area_get_width(&key),20,i==wifi_key?theme->blue:theme->selected);
        text.color=lv_color_hex(i==wifi_key && theme==&dark?theme->bg:theme->text);
        char c[]={i<(int)count?chars[i]:0,0};
        const char *caption=city_keyboard && i==PANEL_WIFI_ACTION_FIRST+1?(wifi_zh?"搜索":"Search"):
            i>=PANEL_WIFI_ACTION_FIRST?(wifi_zh?zh:en)[i-PANEL_WIFI_ACTION_FIRST]:
            i>=PANEL_WIFI_MODE_FIRST?modes[i-PANEL_WIFI_MODE_FIRST]:
            c[0]==' '?(wifi_zh?"空":"_"):c;
        lv_draw_label(ctx,&text,&key,caption,NULL);
        if(i==PANEL_WIFI_MODE_FIRST+(int)wifi_mode)icon_bar(ctx,key.x1+4,key.y2-1,lv_area_get_width(&key)-8,2,theme->warning);
    }
}

/* Paint shared chrome without allocating another pair of retained objects. */
static void splash_draw(lv_draw_ctx_t *ctx)
{
    int left=(WIDTH-direct_boot_logo_width)/2,top=(HEIGHT-direct_boot_logo_height)/2;
    unsigned t=splash_elapsed;
    unsigned opacity=t<200?t*255/200:t>1500?(PANEL_SPLASH_DURATION_MS-t)*255/200:255;
    int sweep=t<200?-48:t>=1100?direct_boot_logo_width:
        (int)((t-200)*(direct_boot_logo_width+48)/900)-48;
    lv_color_t bg=lv_color_hex(theme->bg),gray=lv_color_hex(theme==&dark?0x58616c:0x9aa8bb);
    for(unsigned part=0;part<2;part++){
        const direct_boot_logo_part_t *a=&direct_boot_logo_parts[part];
        uint32_t mark=theme==&dark?0x4a86ff:0x1b3f8f;
        uint32_t wordmark=theme==&dark?0xffffff:0x111d30;
        lv_color_t color[17],target=lv_color_hex(part?wordmark:mark);
        for(unsigned i=0;i<17;i++)color[i]=lv_color_mix(lv_color_mix(target,gray,i*255/16),bg,opacity);
        int x0=left+a->x_offset,y0=top+a->y_offset;
        int first=ctx->clip_area->x1-x0;if(first<0)first=0;
        int last=ctx->clip_area->x2-x0;if(last>=a->width)last=a->width-1;
        for(unsigned y=0;y<a->height;y++){
            int py=y0+(int)y;
            if(py<ctx->clip_area->y1 || py>ctx->clip_area->y2)continue;
            int run=-1;lv_color_t ink=bg;
            for(int x=first;x<=last+1;x++){
                int n=(sweep-(int)a->x_offset-x+48)/3;
                if(n<0)n=0;
                if(n>16)n=16;
                bool set=x<=last && (a->data[y*a->stride+x/8]&(0x80>>(x%8)));
                lv_color_t next=set?color[n]:bg;
                if(run>=0 && next.full!=ink.full){
                    lv_draw_rect_dsc_t d;lv_draw_rect_dsc_init(&d);d.bg_color=ink;
                    lv_area_t span={x0+run,py,x0+x-1,py};lv_draw_rect(ctx,&d,&span);run=-1;
                }
                if(run<0 && next.full!=bg.full){run=x;ink=next;}
            }
        }
    }
}

static void chrome_draw(lv_event_t *event)
{
    lv_draw_ctx_t *ctx=lv_event_get_draw_ctx(event);
    if(scene==0x500U){splash_draw(ctx);return;}
    lv_draw_rect_dsc_t ink;lv_draw_rect_dsc_init(&ink);
    ink.bg_color=lv_color_hex(theme->header);
    lv_area_t header={0,0,319,27};lv_draw_rect(ctx,&ink,&header);
    ink.bg_color=lv_color_hex(theme->blue);
    lv_area_t accent={0,5,2,22};lv_draw_rect(ctx,&ink,&accent);
    ink.bg_color=lv_color_hex(theme->line);
    lv_area_t rule={0,218,319,218};lv_draw_rect(ctx,&ink,&rule);
    {
        lv_draw_label_dsc_t text;
        lv_draw_label_dsc_init(&text);
        text.font=&panel_lvgl_body_font;
        text.align=LV_TEXT_ALIGN_CENTER;
        for(int i=0;i<KEY_SLOTS;i++){
            int left=i*WIDTH/KEY_SLOTS,right=(i+1)*WIDTH/KEY_SLOTS;
            if(i){lv_area_t divider={left,223,left,235};lv_draw_rect(ctx,&ink,&divider);}
            if(!key_hints[i][0])continue;
            if(i==KEY_SLOTS-1){
                const lv_area_t action={left+3,221,right-4,237};
                outline_card(ctx,&action,theme->selected,theme->selected);
            }
            text.color=lv_color_hex(scene==UI_HOME && i==KEY_LEFT && home_alarm?theme->warning:
                i==KEY_ENTER?theme->blue:theme->muted);
            lv_area_t key={left,220,right-1,239};
            text.letter_space=footer_spacing(key_hints[i],right-left);
            lv_draw_label(ctx,&text,&key,key_hints[i],NULL);
        }
    }
    if(scene==UI_HOME){
        weather_home_draw(ctx);
        home_artwork(ctx);
        wireless_icons(ctx);
    }
    if(scene==UI_BMS && bms_overview){
        panel_indexed_art_draw(ctx,&storage_art,20,62);
        if(bms_has_soc){
            icon_bar(ctx,116,87,184,6,theme->line);
            if(bms_soc)icon_bar(ctx,116,87,(int)(184*bms_soc/100),6,bms_live?theme->blue:theme->muted);
        }
    }
    if(scene==0x101U){
        const lv_area_t draft={164,58,311,161};
        outline_card(ctx,&draft,theme->selected,theme->blue);
    }
    if(scene==UI_WIFI && wifi_key>=0)draw_wifi_keys(ctx);
}

static void visible(lv_obj_t *obj,bool show)
{
    if(show==lv_obj_has_flag(obj,LV_OBJ_FLAG_HIDDEN)){
        if(show)lv_obj_clear_flag(obj,LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(obj,LV_OBJ_FLAG_HIDDEN);
    }
}

static void fill(lv_obj_t *obj,uint32_t rgb)
{
    lv_color_t color=lv_color_hex(rgb);
    if(lv_obj_get_style_bg_color(obj,0).full!=color.full)lv_obj_set_style_bg_color(obj,color,0);
}

static void background_opacity(lv_obj_t *obj,lv_opa_t opacity)
{
    if(lv_obj_get_style_bg_opa(obj,0)!=opacity)lv_obj_set_style_bg_opa(obj,opacity,0);
}

static void text_font(lv_obj_t *obj,const lv_font_t *font)
{
    if(lv_obj_get_style_text_font(obj,0)!=font)lv_obj_set_style_text_font(obj,font,0);
}

static void text_align(lv_obj_t *obj,lv_text_align_t align)
{
    if(lv_obj_get_style_text_align(obj,0)!=align)lv_obj_set_style_text_align(obj,align,0);
}

static void label(lv_obj_t *obj,const char *text,uint32_t rgb)
{
    lv_color_t color=lv_color_hex(rgb);
    if(lv_obj_get_style_text_color(obj,0).full!=color.full)lv_obj_set_style_text_color(obj,color,0);
    if(strcmp(lv_label_get_text(obj),text))lv_label_set_text(obj,text);
}

static void footer_label(const char *text)
{
    int spacing=footer_spacing(text,312);
    lv_point_t extent;
    lv_txt_get_size(&extent,text,&panel_lvgl_body_font,spacing,0,LV_COORD_MAX,LV_TEXT_FLAG_NONE);
    if(lv_obj_get_style_text_letter_space(footer,0)!=spacing)
        lv_obj_set_style_text_letter_space(footer,spacing,0);
    lv_label_long_mode_t mode=extent.x>312?LV_LABEL_LONG_SCROLL_CIRCULAR:LV_LABEL_LONG_CLIP;
    if(lv_label_get_long_mode(footer)!=mode)lv_label_set_long_mode(footer,mode);
    label(footer,text,theme->muted);
}

static void place(lv_obj_t *obj,int x,int y,int width,int height)
{
    if(lv_obj_get_style_x(obj,0)!=x)lv_obj_set_x(obj,x);
    if(lv_obj_get_style_y(obj,0)!=y)lv_obj_set_y(obj,y);
    if(width>=0 && lv_obj_get_style_width(obj,0)!=width)lv_obj_set_width(obj,width);
    if(height>=0 && lv_obj_get_style_height(obj,0)!=height)lv_obj_set_height(obj,height);
}

static void notice(const char *text)
{
    visible(footer,*text!=0);
    if(*text){
        place(footer,4,198,312,-1);
        text_align(footer,LV_TEXT_ALIGN_CENTER);
        footer_label(text);
    }
}

static void key_caption(unsigned slot,const char *text)
{
    if(strcmp(key_hints[slot],text)){
        snprintf(key_hints[slot],sizeof(key_hints[slot]),"%s",text);
        const lv_area_t cell={slot*WIDTH/KEY_SLOTS,219,(slot+1)*WIDTH/KEY_SLOTS-1,HEIGHT-1};
        lv_obj_invalidate_area(screen,&cell);
    }
}

static void key_codes(const uint8_t keys[KEY_SLOTS],bool zh)
{
    static const char *const captions[][2]={
        {"",""},{"Back","返回"},{"Prev","上页"},{"Next","下页"},{"Up","上移"},{"Down","下移"},
        {"Open","进入"},{"Edit","修改"},{"Save","保存"},{"OK","确认"},{"Cancel","取消"},
        {"Send","发送"},{"Scan","扫描"},{"Refresh","刷新"},{"City","城市"},{"Auto","自动"},
        {"Search","搜索"},{"Type","输入"},{"Close","关闭"},{"-","减小"},{"+","增大"},
        {"Left","左移"},{"Right","右移"},{"Join","连接"},{"Update","升级"},{"Mode","切换"},{"Del","删除"},
        {"Fast+","快加"},{"Fast-","快减"},{"Info","说明"},
    };
    for(unsigned i=0;i<KEY_SLOTS;i++)key_caption(i,captions[keys[i]][zh]);
}

static lv_obj_t *box(lv_obj_t *parent,int radius)
{
    lv_obj_t *obj=lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_clear_flag(obj,LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(obj,LV_OPA_COVER,0);
    lv_obj_set_style_radius(obj,radius,0);
    return obj;
}

static lv_obj_t *text_obj(lv_obj_t *parent,int x,int y,int width)
{
    lv_obj_t *obj=lv_label_create(parent);
    lv_obj_set_style_text_font(obj,&panel_lvgl_body_font,0);
    lv_label_set_long_mode(obj,LV_LABEL_LONG_CLIP);
    lv_obj_set_width(obj,width);
    lv_obj_set_pos(obj,x,y);
    lv_label_set_text(obj,"");
    return obj;
}

static void flush_cb(lv_disp_drv_t *disp,const lv_area_t *area,lv_color_t *pixels)
{
    write_pixels(area->x1,area->y1,lv_area_get_width(area),lv_area_get_height(area),(uint16_t *)pixels);
    lv_disp_flush_ready(disp);
}

static void align_flush(lv_disp_drv_t *disp,lv_area_t *area)
{
    (void)disp;
    /* Two RGB565 pixels form one DMA word. The physical width is even. */
    area->x1&=~1;
    area->x2|=1;
}

static void qr_draw(lv_event_t *event)
{
    int size=qr_data?onboarding_qr_size(qr_data):0;
    if(!size)return;
    lv_draw_ctx_t *ctx=lv_event_get_draw_ctx(event);
    lv_area_t box_area;lv_obj_get_coords(qr_box,&box_area);
    int scale=148/(size+8);
    if(!scale)return;
    int left=box_area.x1+(148-size*scale)/2,top=box_area.y1+(148-size*scale)/2;
    lv_draw_rect_dsc_t ink;lv_draw_rect_dsc_init(&ink);ink.bg_color=lv_color_black();
    for(int y=0;y<size;y++){
        int py=top+y*scale;
        if(py+scale<=ctx->clip_area->y1 || py>ctx->clip_area->y2)continue;
        for(int x=0;x<size;x++)if(onboarding_qr_module(qr_data,x,y)){
            lv_area_t a={left+x*scale,py,left+(x+1)*scale-1,py+scale-1};
            lv_draw_rect(ctx,&ink,&a);
        }
    }
}

static void icon_bar(lv_draw_ctx_t *ctx,int x,int y,int w,int h,uint32_t color)
{
    lv_draw_rect_dsc_t ink;lv_draw_rect_dsc_init(&ink);ink.bg_color=lv_color_hex(color);
    lv_area_t area={x,y,x+w-1,y+h-1};lv_draw_rect(ctx,&ink,&area);
}

static uint32_t shade(uint32_t from,uint32_t to,unsigned n,unsigned total)
{
    uint32_t out=0;
    for(unsigned shift=0;shift<24;shift+=8){
        unsigned a=(from>>shift)&255U,b=(to>>shift)&255U;
        out|=((a*(total-n)+b*n)/total)<<shift;
    }
    return out;
}

static void tile_background(lv_draw_ctx_t *ctx,const lv_area_t *a,bool active)
{
    uint32_t border=active?shade(theme->line,theme->blue,(unsigned)menu_focus,255):theme->line;
    outline_card(ctx,a,active?theme->selected:theme->card,border);
}

#include "panel_semantic_icons.inc"
/* One device portrait plus semantic menu icons. */
static void menu_icon(lv_event_t *event)
{
    unsigned index=(unsigned)(uintptr_t)lv_event_get_user_data(event);
    if(index>=6)return;
    lv_area_t a;lv_obj_get_coords(lv_event_get_target(event),&a);
    lv_draw_ctx_t *ctx=lv_event_get_draw_ctx(event);
    if(product_mode>=3){product_tile(ctx,&a,index);return;}
    if(scene!=UI_MENU || index>=5)return;
    tile_background(ctx,&a,selection==(int)index);
    if(index==4){
        product_icon(ctx,6,a.x1+(lv_area_get_width(&a)-32)/2,a.y1+14,theme->blue);
        return;
    }
    static const panel_indexed_art_t *const art[]={&solar_art,&inverter_art,&power_art,&alarm_art};
    int left=a.x1+(lv_area_get_width(&a)-art[index]->width)/2;
    panel_indexed_art_draw(ctx,art[index],left,a.y1+(index==1?48:8));
}

bool panel_lvgl_init(uint16_t *buffer,panel_lvgl_flush_fn flush,uint32_t now)
{
    if(!buffer || !flush)return false;
    write_pixels=flush;tick_ms=now;
    lv_init();
    lv_disp_draw_buf_init(&draw_buffer,buffer,NULL,WIDTH*16);
    lv_disp_drv_init(&driver);
    driver.hor_res=WIDTH;driver.ver_res=HEIGHT;driver.draw_buf=&draw_buffer;driver.flush_cb=flush_cb;
    driver.rounder_cb=align_flush;
    display=lv_disp_drv_register(&driver);
    if(!display)return false;
    screen=lv_scr_act();
    lv_obj_remove_style_all(screen);
    lv_obj_clear_flag(screen,LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(screen,LV_OPA_COVER,0);
    lv_obj_add_event_cb(screen,chrome_draw,LV_EVENT_DRAW_MAIN,NULL);
    for(unsigned i=0;i<4;i++){
        dots[i]=box(screen,3);lv_obj_set_size(dots[i],5,5);
    }
    title=text_obj(screen,10,3,300);
    subtitle=text_obj(screen,14,29,292);
    page_number=text_obj(screen,242,3,68);
    lv_obj_set_style_text_align(page_number,LV_TEXT_ALIGN_RIGHT,0);
    footer=text_obj(screen,4,220,312);
    for(unsigned i=0;i<ROWS;i++){
        rows[i].card=box(screen,2);
        if(i<6)lv_obj_add_event_cb(rows[i].card,menu_icon,LV_EVENT_DRAW_MAIN,(void *)(uintptr_t)i);
        rows[i].label=text_obj(rows[i].card,10,0,280);
        rows[i].value=text_obj(rows[i].card,204,0,84);
        lv_obj_set_style_text_align(rows[i].value,LV_TEXT_ALIGN_RIGHT,0);
    }
    marker=box(screen,2);track=box(screen,2);indicator=box(track,2);
    qr_box=box(screen,6);lv_obj_set_size(qr_box,148,148);lv_obj_set_pos(qr_box,10,49);
    fill(qr_box,0xffffff);
    lv_obj_add_event_cb(qr_box,qr_draw,LV_EVENT_DRAW_MAIN,NULL);
    return true;
}

static void apply_row(unsigned i,int x,int y,int w,int h,const char *left,const char *right,bool active)
{
    row_t *r=&rows[i];
    bool battery=scene==UI_BMS;
    bool soc=battery && bms_overview && i==1;
    visible(r->card,*left || *right);
    if(!*left && !*right)return;
    place(r->card,x,y,w,h);
    fill(r->card,active?theme->selected:theme->card);
    background_opacity(r->card,LV_OPA_COVER);
    if(lv_obj_get_style_border_width(r->card,0))lv_obj_set_style_border_width(r->card,0,0);
    text_font(r->label,&panel_lvgl_body_font);
    if(lv_obj_get_style_text_letter_space(r->label,0))
        lv_obj_set_style_text_letter_space(r->label,0,0);
    text_align(r->label,(*right || (battery && i!=0) || scene==UI_HOME)?
               LV_TEXT_ALIGN_LEFT:LV_TEXT_ALIGN_CENTER);
    text_font(r->value,soc?&headline_font:&panel_lvgl_body_font);
    if(lv_obj_get_style_text_letter_space(r->value,0))
        lv_obj_set_style_text_letter_space(r->value,0,0);
    text_align(r->value,LV_TEXT_ALIGN_RIGHT);
    if(lv_label_get_long_mode(r->label)!=LV_LABEL_LONG_DOT)lv_label_set_long_mode(r->label,LV_LABEL_LONG_DOT);
    lv_point_t size;lv_txt_get_size(&size,right,&panel_lvgl_body_font,0,0,LV_COORD_MAX,LV_TEXT_FLAG_NONE);
    int value_width=*right?size.x+4:0;
    if(value_width>w/2)value_width=w/2;
    if(soc)value_width=114;
    int label_y=(h-(int)lv_font_get_line_height(&panel_lvgl_body_font))/2;
    int value_y=(h-(int)lv_font_get_line_height(soc?&headline_font:&panel_lvgl_body_font))/2;
    place(r->label,12,label_y,w-24-value_width,-1);
    place(r->value,w-12-value_width,value_y,value_width,-1);
    label(r->label,left,battery && i==0?(bms_live?theme->blue:theme->warning):theme->text);
    label(r->value,right,active?theme->blue:theme->text);
}

static void marker_width(void *obj,int32_t width){lv_obj_set_width(obj,width);}
static void indicator_x(void *obj,int32_t x){lv_obj_set_x(obj,x);}
static void card_opa(void *obj,int32_t opacity){menu_focus=opacity;lv_obj_invalidate(obj);}

static void animate(lv_obj_t *obj,lv_anim_exec_xcb_t exec,int from,int to,uint32_t time,bool repeat)
{
    lv_anim_del(obj,exec);
    lv_anim_t animation;lv_anim_init(&animation);
    lv_anim_set_var(&animation,obj);lv_anim_set_exec_cb(&animation,exec);
    lv_anim_set_values(&animation,from,to);lv_anim_set_time(&animation,time);
    if(repeat)lv_anim_set_repeat_count(&animation,LV_ANIM_REPEAT_INFINITE);
    else lv_anim_set_path_cb(&animation,lv_anim_path_ease_out);
    lv_anim_start(&animation);
}

static bool counter_text(const char *s)
{
    bool digit=false,slash=false;
    for(;*s;s++){
        if(*s>='0' && *s<='9')digit=true;
        else if(*s=='/')slash=true;
        else if(*s!=' ')return false;
    }
    return digit && slash;
}

static void base(uint32_t next,bool light_mode,const char *heading,const char *hint)
{
    theme=light_mode?&light:&dark;
    if(scene!=next){
        /* Screen-level artwork can change outside any retained label/card. */
        lv_obj_invalidate(screen);
        scene=next;selection=-1;qr_generation=UINT32_MAX;
        home_phase=UINT8_MAX;
        menu_focus=255;
        lv_anim_del(marker,NULL);lv_anim_del(indicator,NULL);waiting=false;
        for(unsigned i=0;i<ROWS;i++)lv_anim_del(rows[i].card,card_opa);
        for(unsigned i=0;i<4;i++)visible(dots[i],false);
        visible(qr_box,false);
    }
    fill(screen,theme->bg);
    place(subtitle,14,29,292,-1);
    text_align(subtitle,LV_TEXT_ALIGN_CENTER);
    text_font(subtitle,&panel_lvgl_body_font);
    char header[96];snprintf(header,sizeof(header),"%s",heading);
    char *counter=strrchr(header,' ');
    const char *number=NULL;
    if(counter && counter_text(counter+1)){*counter++=0;number=counter;}
    if(counter_text(hint)){number=hint;hint="";}
    visible(page_number,number!=NULL);
    if(number)label(page_number,number,theme->blue);
    place(title,number?50:10,3,number?220:300,-1);
    text_align(title,LV_TEXT_ALIGN_CENTER);
    label(title,header,theme->text);label(subtitle,hint,theme->muted);
}

static void layout(unsigned count,int top,int pitch,int selected,bool result,bool busy)
{
    for(unsigned i=count;i<ROWS;i++)visible(rows[i].card,false);
    int marker_row=result?2:selected;
    visible(marker,marker_row>=0);
    if(marker_row>=0){
        place(marker,10,top+pitch*marker_row+4,-1,pitch-10);
        fill(marker,theme->blue);
        if(selection!=marker_row)animate(marker,marker_width,7,3,120,false);
    }
    selection=marker_row;
    visible(track,busy);
    if(busy){
        place(track,10,218,300,1);fill(track,theme->line);
        if(!waiting)place(indicator,0,0,60,1);
        fill(indicator,theme->blue);
        if(!waiting)animate(indicator,indicator_x,0,240,850,true);
    }else if(waiting)lv_anim_del(indicator,indicator_x);
    waiting=busy;
}

#include "panel_product_ui.inc"

void panel_lvgl_show_modbus(const panel_view_t *v)
{
    product_begin(v->setting_groups?3:v->detail_groups?4:v->detail_dashboard?2:
        (v->setting_list || v->setting_editor) && !v->result?1:0,v->illustration);
    base(v->alarm_list?SCENE_ALARMS:0x100U+v->setting_editor+2U*v->result,
         v->theme_light,v->title,v->subtitle);
    bool has_notice=v->footer_notice && !v->result && v->footer[0];
    notice(has_notice?v->footer:"");
    key_codes(v->key_labels,!v->language_en);
    if(v->setting_groups || v->detail_groups){product_gallery(v);return;}
    if(v->detail_dashboard){product_dashboard(v);return;}
    if(v->alarm_list){
        layout(7,0,20,-1,false,false);
        apply_row(0,8,52,304,27,v->rows[0],"",v->alert);
        visible(rows[1].card,false);
        if(v->alert)label(rows[0].label,v->rows[0],theme->warning);
        for(unsigned i=2;i<7;i++){
            if(!v->rows[i][0]){visible(rows[i].card,false);continue;}
            bool historical=(v->stale_rows&(1U<<i))!=0;
            bool fault=(v->fault_rows&(1U<<i))!=0;
            apply_row(i,8,83+(int)(i-2)*26,304,23,v->rows[i],"",false);
            text_align(rows[i].label,LV_TEXT_ALIGN_LEFT);
            label(rows[i].label,v->rows[i],historical?theme->muted:theme->text);
            if(v->alert || historical){
                uint32_t color=historical?theme->line:
                    fault?(theme==&dark?0xff847d:0xb32424):theme->warning;
                lv_obj_set_style_border_side(rows[i].card,LV_BORDER_SIDE_LEFT,0);
                lv_obj_set_style_border_color(rows[i].card,lv_color_hex(color),0);
                lv_obj_set_style_border_width(rows[i].card,3,0);
            }
        }
        return;
    }
    if(v->setting_editor && !v->result){product_editor(v,has_notice);return;}
    if(v->result){
        layout(6,54,23,-1,false,false);
        apply_row(0,10,53,300,23,v->rows[0],"",false);
        apply_row(1,10,78,300,23,v->rows[1],v->values[1],false);
        apply_row(2,10,106,300,38,v->rows[2],"",true);
        label(rows[2].label,v->rows[2],v->alert?theme->warning:theme->success);
        for(unsigned i=3;i<6;i++)apply_row(i,10,148+(int)(i-3)*23,300,22,v->rows[i],"",false);
        visible(marker,true);
        place(marker,10,113,3,24);fill(marker,v->alert?theme->warning:theme->success);
        return;
    }
    if(v->setting_list){product_parameter(v,has_notice);return;}
    unsigned count=7;
    while(count && !v->rows[count-1][0] && !v->values[count-1][0])count--;
    int pitch=count<=5?28:has_notice?20:23;
    for(unsigned i=0;i<count;i++){
        bool result=v->result && i==2;
        apply_row(i,10,54+(int)i*pitch,300,pitch-3,v->rows[i],v->values[i],v->selected==(int)i || result);
        if(v->stale_rows&(1U<<i)){
            label(rows[i].label,v->rows[i],theme->muted);label(rows[i].value,v->values[i],theme->muted);
        }
        if(result && v->alert)label(rows[i].label,v->rows[i],theme->warning);
    }
    layout(count,54,pitch,v->selected,v->result,v->activity_frame!=0);
    if(v->result && v->alert)fill(marker,theme->warning);
}

static void home_flow(const direct_ui_data_t *d)
{
    direct_home_values_t value;direct_home_values(d,&value);
    bool direct=full_battery_source(d)==FULL_BATTERY_DIRECT;
    bool direct_only=direct && !d->dsp_online;
    if(direct_only){
        unsigned soc=direct_bms_percent(&d->bms,d->bms.soc_percent);
        snprintf(value.battery_power,sizeof(value.battery_power),"%u%%",soc>100?100:soc);
        snprintf(value.battery_voltage,sizeof(value.battery_voltage),"%u.%uV",
            (unsigned)(d->bms.voltage_10mv/100),(unsigned)(d->bms.voltage_10mv/10%10));
    }else if(direct && d->bms_online){
        snprintf(value.battery_soc,sizeof(value.battery_soc),"%u%%",direct_bms_percent(&d->bms,d->bms.soc_percent));
    }
    const char *const cn[]={"电网","光伏","电池","负载"},*const en[]={"Grid","Solar","Battery","Load"};
    const char *const power[]={value.grid_power,value.pv_power,value.battery_power,value.load_power};
    const char *const voltage[]={value.grid_voltage,value.pv_voltage,value.battery_voltage,value.load_voltage};
    const bool active[]={d->run.ACInWatt!=0,d->run.Ppv!=0,value.battery_watts!=0,d->run.OutputWatt!=0};
    const bool reverse[]={d->run.ACInWatt<0,false,value.battery_state==DIRECT_HOME_CHARGING,d->run.OutputWatt>=0};
    char status[48];
    const char *state=d->dsp_online?(d->language_zh?"实时":"Live"):
        d->dsp_display_held?(d->language_zh?"上次数据":"Last data"):(d->language_zh?"等待数据":"Waiting");
    if(d->clock_valid)snprintf(status,sizeof(status),"%s %02d:%02d",state,d->hour,d->minute);
    else snprintf(status,sizeof(status),"%s",state);
    base(UI_HOME,d->theme_light,d->language_zh?"能源概览":"Energy",status);
    layout(ROWS,0,20,-1,false,false);
    int wifi=direct_home_wifi_level(d);
    if(wifi!=home_wifi || d->bluetooth_connected!=home_bluetooth){
        const lv_area_t icons={270,3,314,25};lv_obj_invalidate_area(screen,&icons);
        home_wifi=wifi;home_bluetooth=d->bluetooth_connected;
    }
    place(title,10,3,72,-1);place(subtitle,151,3,115,-1);
    text_align(title,LV_TEXT_ALIGN_LEFT);
    lv_obj_set_style_text_align(subtitle,LV_TEXT_ALIGN_RIGHT,0);
    lv_obj_set_style_text_font(subtitle,&panel_lvgl_body_font,0);
    label(subtitle,status,d->dsp_online?theme->success:d->dsp_display_held?theme->warning:theme->muted);
    uint8_t motion=0,directions=0;
    for(unsigned i=0;i<4;i++){
        const int x=home_cards[i].x,y=home_cards[i].y;
        const char *name=d->language_zh?cn[i]:en[i];
        if(d->dsp_online && i==0 && active[i])
            name=reverse[i]?(d->language_zh?"电网馈电":"Grid out"):(d->language_zh?"电网取电":"Grid in");
        if(d->dsp_online && i==2 && active[i])
            name=reverse[i]?(d->language_zh?"电池充电":"Charge"):(d->language_zh?"电池放电":"Discharge");
        apply_row(i,x,y,HOME_CARD_WIDTH,HOME_CARD_HEIGHT,name,"",false);
        background_opacity(rows[i].card,LV_OPA_TRANSP);
        place(rows[i].label,13,5,HOME_CARD_WIDTH-20,-1);
        label(rows[i].label,name,theme->muted);
        bool live=i==2 && direct_only?d->bms_online:d->dsp_online;
        label(rows[i].value,power[i],live?theme->text:theme->muted);
        lv_point_t extent;lv_txt_get_size(&extent,power[i],&value_font,0,0,LV_COORD_MAX,LV_TEXT_FLAG_NONE);
        text_font(rows[i].value,extent.x<=HOME_CARD_WIDTH-16?&value_font:&panel_lvgl_body_font);
        text_align(rows[i].value,LV_TEXT_ALIGN_CENTER);
        place(rows[i].value,8,28,HOME_CARD_WIDTH-16,-1);

        char detail[40];
        if(i==2 && !direct_only && strcmp(value.battery_soc,"--%"))
            snprintf(detail,sizeof(detail),"%s  %s",voltage[i],value.battery_soc);
        else snprintf(detail,sizeof(detail),"%s",voltage[i]);
        unsigned auxiliary=i+4;
        apply_row(auxiliary,x+5,y+57,HOME_CARD_WIDTH-10,20,detail,"",false);
        background_opacity(rows[auxiliary].card,LV_OPA_TRANSP);
        text_font(rows[auxiliary].label,&small_font);
        text_align(rows[auxiliary].label,LV_TEXT_ALIGN_CENTER);
        place(rows[auxiliary].label,0,0,HOME_CARD_WIDTH-10,-1);
        label(rows[auxiliary].label,detail,theme->muted);
        if(d->dsp_online && !d->ota_busy && active[i])motion|=(uint8_t)(1U<<i);
        if(reverse[i])directions|=(uint8_t)(1U<<i);
    }
    if(home_motion!=motion || home_reverse!=directions)home_phase=UINT8_MAX;
    home_motion=motion;home_reverse=directions;
    home_alarm=full_inverter_alarm_active(d);
    const char *device=home_alarm?(d->language_zh?"查看告警":"View alarms"):direct_only?(d->language_zh?(d->bms_online?"电池已连接":"电池已断联"):
        (d->bms_online?"Battery online":"Battery offline")):(d->language_zh?"逆变器":"Inverter");
    apply_row(8,105,191,110,20,device,"",false);
    background_opacity(rows[8].card,LV_OPA_TRANSP);
    text_align(rows[8].label,LV_TEXT_ALIGN_CENTER);
    place(rows[8].label,0,0,110,-1);
    label(rows[8].label,device,home_alarm?theme->warning:theme->muted);
}

static const char *localized(const char *text,bool zh,char out[128])
{return direct_ui_translate(text,zh,out,128);}

static void battery_row(unsigned row,int x,int y,int width,int height,const char *text,bool live)
{
    char name[96];snprintf(name,sizeof(name),"%s",text);
    char *value=strchr(name,':');
    if(value){*value++=0;while(*value==' ')value++;}
    apply_row(row,x,y,width,height,name,value?value:"",false);
    if(value && !live)label(rows[row].value,value,theme->muted);
}

static void show_local(const direct_ui_state_t *u,const direct_ui_data_t *d,
                       const direct_ui_view_t *v,const onboarding_qr_t *q,
                       const char *pin,bool has_wifi,bool bound)
{
    char a[128],b[128],keys[128]="";
    if(u->message[0])snprintf(keys,sizeof(keys),"%s",localized(u->message,d->language_zh,a));
    if(u->page==UI_HOME){
        home_flow(d);
        return;
    }
    if(u->page==UI_BMS){
        unsigned page=full_battery_page(u,d);
        full_battery_source_t source=full_battery_source(d);
        bool overview=!full_battery_cells(u,d) && source!=FULL_BATTERY_NONE;
        bool live=source!=FULL_BATTERY_NONE && d->bms_online;
        bool has_soc=source!=FULL_BATTERY_NONE;
        unsigned soc=direct_bms_percent(&d->bms,d->bms.soc_percent);
        if(soc>100)soc=100;
        if(overview!=bms_overview || source!=bms_source || page!=bms_page){
            const lv_area_t content={0,28,WIDTH-1,217};
            lv_obj_invalidate_area(screen,&content);
        }else if(overview && (live!=bms_live || soc!=bms_soc || has_soc!=bms_has_soc)){
            const lv_area_t gauge={116,87,299,92};
            lv_obj_invalidate_area(screen,&gauge);
        }
        bms_source=source;bms_page=page;
        bms_overview=overview;bms_live=live;bms_soc=soc;bms_has_soc=has_soc;
        if(overview){
            base(UI_BMS,d->theme_light,v->title,"");
            layout(8,0,20,-1,false,false);
            apply_row(0,10,30,300,21,v->rows[0],"",false);
            battery_row(1,104,52,206,34,v->rows[3],bms_live);
            for(unsigned i=0;i<3;i++)
                battery_row(i+2,104,96+(int)i*24,206,22,v->rows[i==2?4:i+1],bms_live);
            for(unsigned i=0;i<3;i++)
                battery_row(i+5,10,163+(int)i*18,300,19,v->rows[i+5],bms_live);
            return;
        }
    }
    if(u->page==UI_PHONE_SETUP){
        base(UI_PHONE_SETUP,d->theme_light,d->language_zh?"连接辰烁 App":"Connect CSERGY App",
             d->language_zh?"扫描设备二维码":"Scan the device QR code");
        layout(7,0,20,-1,false,false);
        bool ready=q && q->ready;
        bool pin_ready=pin && strlen(pin)==6 && strspn(pin,"0123456789")==6;
        if(qr_data!=q || (q && qr_generation!=q->generation)){
            qr_data=q;qr_generation=q?q->generation:0;lv_obj_invalidate(qr_box);
        }
        visible(qr_box,ready);
        apply_row(0,166,50,144,25,d->language_zh?"1 打开手机 App":"1 Open the App","",false);
        apply_row(1,166,82,144,25,has_wifi?(d->language_zh?"2 添加设备":"2 Add device"):
                  (d->language_zh?"2 配网并添加":"2 Set up Wi-Fi"),"",false);
        apply_row(2,166,114,144,25,d->language_zh?"配对 PIN":"Pairing PIN","",false);
        apply_row(3,166,145,144,26,pin_ready?pin:"--","",true);
        apply_row(4,166,176,144,21,bound?(d->language_zh?"已绑定":"Device linked"):
                  (d->language_zh?"可稍后设置":"Set up later"),"",false);
        snprintf(b,sizeof(b),"SN %s",d->sn&&d->sn[0]?d->sn:"--");
        apply_row(5,10,199,300,19,b,"",false);
        const char *notice=pin_ready?(d->language_zh?"请手动输入 PIN":"Enter PIN"):
            d->sn && d->sn[0]?(d->language_zh?"未配置配对码":"PIN not ready"):
            (d->language_zh?"缺少设备身份":"Missing ID");
        apply_row(6,10,89,148,36,ready?"":notice,"",false);
        apply_row(7,10,128,148,31,ready||pin_ready?"":d->language_zh?"可用面板配网":"Use panel Wi-Fi","",false);
        return;
    }
    if(u->page==UI_WIFI || (u->page==UI_WEATHER && u->editing)){
        bool previous_city_keyboard=city_keyboard;
        city_keyboard=u->page==UI_WEATHER;
        base(UI_WIFI,d->theme_light,v->title,"");
        int next=u->editing?(v->selected&63):-1;
        unsigned mode=u->editing?(unsigned)v->selected>>PANEL_WIFI_MODE_SHIFT:0;
        bool changed=mode!=wifi_mode || d->language_zh!=wifi_zh || previous_city_keyboard!=city_keyboard;
        if(next<0 || wifi_key<0 || changed){
            if(next!=wifi_key || changed){const lv_area_t body={0,28,WIDTH-1,217};lv_obj_invalidate_area(screen,&body);}
        }else if(next!=wifi_key){
            lv_area_t old=wifi_key_area(wifi_key),current=wifi_key_area(next);
            lv_obj_invalidate_area(screen,&old);lv_obj_invalidate_area(screen,&current);
        }
        wifi_key=next;wifi_mode=mode;wifi_zh=d->language_zh;
        if(u->editing){
            layout(3,0,20,-1,false,false);
            apply_row(0,10,29,300,19,keys[0]?keys:v->rows[0],"",false);
            apply_row(1,10,48,300,19,v->rows[1],"",false);
            apply_row(2,10,67,300,19,v->rows[2],"",false);
        }else{
            for(unsigned i=0;i<5;i++)apply_row(i,10,39+(int)i*34,300,31,v->rows[i],"",v->selected==(int)i);
            layout(5,39,34,v->selected,false,false);
        }
        return;
    }
    if(u->page==UI_HELP && u->detail_page){
        base(0x200U+u->detail_page,d->theme_light,d->language_zh?"屏幕对比测试":"LCD contrast test",
             u->detail_page==2?"B · INV ON":"A · INV OFF");
        layout(ROWS,0,20,-1,false,false);
        for(unsigned i=0;i<8;i++){
            apply_row(i,10+(int)i*38,76,34,35," ","",false);
            unsigned c=255*i/7;fill(rows[i].card,(c<<16)|(c<<8)|c);
        }
        static const uint32_t rgb[]={0xff0000,0x00ff00,0x0000ff};
        for(unsigned i=0;i<3;i++){
            visible(dots[i],true);place(dots[i],10+(int)i*101,116,93,12);
            fill(dots[i],rgb[i]);
        }
        apply_row(8,10,135,300,36,"0123456789  52.4 V","",true);
        return;
    }
    if(u->page==UI_MENU || u->page==UI_SUBMENU){
        bool main=u->page==UI_MENU;
        unsigned count=main?direct_ui_menu_count():direct_ui_submenu_count(u->menu_selected);
        unsigned per=main?5:6,start=u->selected/per*per;
        unsigned shown=count>start?count-start:0;
        if(shown>per)shown=per;
        const char *heading=main?"System menu":direct_ui_menu_name(u->menu_selected);
        snprintf(b,sizeof(b),"%u / %u",count?u->selected+1:0,count);
        base(main?UI_MENU:0x300U+u->menu_selected,d->theme_light,localized(heading,d->language_zh,a),b);
        if(main){
            int previous=selection;
            layout(5,0,20,-1,false,false);
            static const char *const en_names[]={"Live data","Inverter","Power","Alarms","Extras"};
            static const int left[]={128,10,221,128,221},top[]={35,35,35,125,125};
            for(unsigned i=0;i<5;i++){
                bool active=u->selected==i,large=i==1;
                int width=large?112:89,height=large?174:84;
                const char *name=d->language_zh?localized(direct_ui_menu_name(i),true,a):en_names[i];
                apply_row(i,left[i],top[i],width,height,name,"",active);
                lv_obj_set_style_bg_opa(rows[i].card,LV_OPA_TRANSP,0);
                lv_obj_set_style_text_align(rows[i].label,LV_TEXT_ALIGN_CENTER,0);
                place(rows[i].label,2,large?11:57,width-4,-1);
                label(rows[i].label,name,active?theme->blue:theme->text);
                if(previous!=(int)u->selected)lv_anim_del(rows[i].card,card_opa);
            }
            if(previous!=(int)u->selected)animate(rows[u->selected].card,card_opa,165,255,150,false);
            selection=u->selected;
            return;
        }
        int selected=(int)(u->selected-start),pitch=main?32:27;
        for(unsigned i=0;i<shown;i++){
            const char *entry=main?direct_ui_menu_name(start+i):direct_ui_submenu_name(u->menu_selected,start+i);
            apply_row(i,10,51+(int)i*pitch,300,pitch-3,localized(entry,d->language_zh,a),">",selected==(int)i);
        }
        layout(shown,51,pitch,selected,false,false);
        return;
    }
    base(u->page,d->theme_light,localized(v->title,d->language_zh,a),"");
    int selected=u->page==UI_FACTORY?3:v->selected;
    unsigned count=ROWS;
    while(count && !v->rows[count-1][0])count--;
    int pitch=u->page==UI_WEATHER || count>6?23:28;
    if(count>7 || (keys[0] && count>6))pitch=20;
    for(unsigned i=0;i<ROWS;i++){
        snprintf(a,sizeof(a),"%s",localized(v->rows[i],d->language_zh,b));
        char *value=NULL;
        if(u->page==UI_BMS || u->page==UI_APPEARANCE || u->page==UI_TIME || u->page==UI_HELP || u->page==UI_WEATHER || u->page==UI_BMS_PROTOCOL){
            value=strchr(a,':');
            if(value){*value++=0;while(*value==' ')value++;}
        }
        apply_row(i,10,31+(int)i*pitch,300,pitch-1,a,value?value:"",selected==(int)i);
    }
    layout(count,31,pitch,selected,false,false);
    if(u->page==UI_UPGRADE && d->ota_busy){
        visible(track,true);
        lv_obj_set_pos(track,10,218);lv_obj_set_size(track,300,1);fill(track,theme->line);
        lv_obj_set_pos(indicator,0,0);lv_obj_set_size(indicator,3*(d->ota.progress>100?100:d->ota.progress),1);
        fill(indicator,theme->blue);
    }
}

void panel_lvgl_show_local(const direct_ui_state_t *u,const direct_ui_data_t *d,
                           const direct_ui_view_t *v,const onboarding_qr_t *q,
                           const char *pin,bool has_wifi,bool bound)
{
    product_begin(0,0);
    show_local(u,d,v,q,pin,has_wifi,bound);
    bool connected=u->page==UI_WIFI || u->page==UI_WEATHER || (u->page==UI_UPGRADE && !d->ota_busy);
    if(connected)key_codes(v->key_labels,d->language_zh);
    else{
        const char *hints[KEY_SLOTS];direct_ui_key_hints(u,d,hints);
        for(unsigned i=0;i<KEY_SLOTS;i++){
            const char *hint=hints[i]?hints[i]:"";
            if(u->page==UI_HOME && i==KEY_LEFT && home_alarm){
                key_caption(i,d->language_zh?"有告警":"Alarm!");
                continue;
            }
            if(!strcmp(hint,"--"))hint="";
            if(!d->language_zh){
                if(!strcmp(hint,"Confirm"))hint="OK";
                else if(!strcmp(hint,"Overview"))hint="Home";
                else if(!strcmp(hint,"Settings"))hint="Setup";
            }
            char text[128];
            key_caption(i,localized(hint,d->language_zh,text));
        }
    }
    if(u->message[0] && (u->page==UI_APPEARANCE || u->page==UI_TIME || u->page==UI_FACTORY ||
       (!u->editing && (u->page==UI_WIFI || u->page==UI_WEATHER)))){
        char text[128];
        notice(localized(u->message,d->language_zh,text));
    }else notice("");
}

void panel_lvgl_splash(bool light_mode)
{
    base(0x500U,light_mode,"","");
    notice("");
    layout(0,0,20,-1,false,false);
    splash_started=tick_ms;splash_elapsed=0;
    lv_obj_invalidate(screen);
}

void panel_lvgl_refresh(uint32_t now,bool redraw)
{
    if(!display)return;
    lv_tick_inc(now-tick_ms);tick_ms=now;
    if(scene==0x500U){
        unsigned elapsed=now-splash_started;
        if(elapsed>PANEL_SPLASH_DURATION_MS)elapsed=PANEL_SPLASH_DURATION_MS;
        if(elapsed!=splash_elapsed){
            splash_elapsed=elapsed;
            int x=(WIDTH-direct_boot_logo_width)/2,y=(HEIGHT-direct_boot_logo_height)/2;
            const lv_area_t logo={x,y,x+direct_boot_logo_width-1,y+direct_boot_logo_height-1};
            lv_obj_invalidate_area(screen,&logo);
        }
    }
    if(scene==UI_HOME){
        uint8_t phase=home_motion?(uint8_t)((now/90U)%16U):0;
        if(phase!=home_phase){
            home_phase=phase;
            for(unsigned i=0;i<4;i++){
                bool moving=(home_motion&(1U<<i))!=0;
                visible(dots[i],moving);
                if(!moving)continue;
                int step=(phase+(int)i*3)%16;
                if(home_reverse&(1U<<i))step=15-step;
                const lv_point_t *p=wires[i]+step/8;
                int t=step%8;
                bool horizontal=p[0].y==p[1].y;
                place(dots[i],p[0].x+(p[1].x-p[0].x)*t/8-1,p[0].y+(p[1].y-p[0].y)*t/8-1,horizontal?7:2,horizontal?2:7);
                fill(dots[i],home_color(i));
            }
        }
    }
    if(redraw)lv_obj_invalidate(screen);
    lv_timer_handler();
    /* Key feedback is rendered on the current task pass. */
    lv_refr_now(display);
}
