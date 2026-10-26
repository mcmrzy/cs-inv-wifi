#include "direct_ui.h"
#include "direct_ui_scene.h"
#include "direct_ui_locale.h"
#include "direct_gfx_assets.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static direct_ui_data_t d;
static direct_ui_event_t key(direct_ui_state_t *u,unsigned k){return direct_ui_key(u,(uint8_t)k,100,&d);}
static void fixture(void){
    memset(&d,0,sizeof(d));d.battery_valid=true;
    d.battery=(direct_battery_config_t){.type=1,.bulk_dv=564,.float_dv=540,.cutoff_dv=420,
        .max_charge_a=75,.ac_charge_a=25,.back_utility_dv=460,.back_battery_dv=540,
        .soc_low=25,.soc_high=90,.soc_cutoff=10,.revision=42};
    d.battery_profiles[1]=d.battery;d.battery_profiles[1].bulk_dv=570;
    d.battery_profiles[4]=d.battery;d.battery_profiles[4].type=4;d.battery_profiles[4].protocol=5;
    d.battery_profiles[4].bulk_dv=550;d.battery_profiles[4].float_dv=540;d.battery_profiles[4].cutoff_dv=450;
    d.battery_profiles[4].max_charge_a=5;d.battery_profiles[4].soc_low=1;d.battery_profiles[4].revision=2;
    for(unsigned i=0;i<CONFIG_PARAM_MAX_INDEX;i++)d.valid[i]=d.writable[i]=true;
    d.config[4]=1;
}
static void enter(direct_ui_state_t *u,unsigned group,unsigned selected){
    direct_ui_init(u);u->page=UI_CONFIG;u->group=(uint8_t)group;u->selected=(uint8_t)selected;
    assert(key(u,2).action==UI_ACT_NONE);assert(u->page==UI_BATTERY);
}
static void field_step(direct_ui_state_t *u,unsigned field,int steps){
    u->selected=(uint8_t)field;assert(key(u,2).action==UI_ACT_NONE);assert(u->editing);
    for(int i=0;i<(steps<0?-steps:steps);i++)key(u,steps<0?4:6);
    key(u,2);assert(!u->editing);assert(u->page==UI_BATTERY);
}
static void common_preserved(const direct_ui_state_t *u){
    assert(u->battery_draft.max_charge_a==75 && u->battery_draft.ac_charge_a==25);
    assert(u->battery_draft.soc_low==25 && u->battery_draft.soc_high==90 && u->battery_draft.soc_cutoff==10);
    assert(u->battery_draft.back_utility_dv==460 && u->battery_draft.back_battery_dv==540);
    assert(u->battery_draft.revision==42);
}
static void flow(void){
    direct_ui_state_t u;
    for(unsigned i=0;i<3;i++){
        unsigned groups[]={8,7,3},selected[]={4,4,0};
        enter(&u,groups[i],selected[i]);assert(u.battery_draft.revision==42);
        key(&u,1);assert(u.page==UI_CONFIG && u.group==groups[i] && u.selected==selected[i]);
    }
    enter(&u,8,4);
    assert(!memcmp(&u.battery_draft,&d.battery,sizeof(d.battery)));
    field_step(&u,0,3);assert(u.battery_draft.type==4 && u.battery_draft.protocol==5);
    assert(u.battery_draft.bulk_dv==550);common_preserved(&u);
    field_step(&u,1,1);assert(u.battery_draft.protocol==5); /* Only BBJ can be selected. */
    field_step(&u,0,-3);assert(u.battery_draft.type==1 && u.battery_draft.bulk_dv==564);common_preserved(&u);
    field_step(&u,0,4);assert(u.battery_draft.type==5 && u.battery_draft.material==0 && u.battery_draft.cells==16);
    assert(u.battery_draft.bulk_dv==576);common_preserved(&u);
    field_step(&u,2,1);assert(u.battery_draft.material==1 && u.battery_draft.cells==14 && u.battery_draft.bulk_dv==560);
    common_preserved(&u);
    field_step(&u,3,-1);assert(u.battery_draft.cells==13);
    /* No implicit recovery of voltages on a cell-count edit: review/validation owns the relationship. */
    u.selected=4;key(&u,2);key(&u,6);key(&u,1);assert(!u.editing && u.battery_draft.bulk_dv==560);
    u.selected=15;key(&u,2);assert(u.page==UI_CONFIG && u.selected==4);
    assert(d.battery.type==1 && d.battery.revision==42);
}
static void review_and_results(void){
    direct_ui_state_t u;enter(&u,3,0);
    field_step(&u,4,1);u.selected=14;key(&u,2);assert(u.page==UI_BATTERY_REVIEW);
    direct_ui_view_t v;direct_ui_build(&u,&d,&v);assert(strstr(v.title,"Review"));
    assert(strstr(v.rows[4],"56.4V") && strstr(v.rows[4],"56.5V"));
    key(&u,6);direct_ui_build(&u,&d,&v);assert(strstr(v.rows[0],"Cutoff"));
    key(&u,6);direct_ui_build(&u,&d,&v);assert(strstr(v.rows[0],"SOC"));
    key(&u,1);assert(u.page==UI_BATTERY && u.selected==14);
    key(&u,2);direct_ui_event_t e=key(&u,2);assert(e.action==UI_ACT_BATTERY_WRITE && u.pending);
    assert(u.battery_draft.revision==42 && u.battery_draft.bulk_dv==565);
    assert(key(&u,1).action==UI_ACT_NONE && u.pending);
    direct_ui_write_result(&u,1);assert(u.pending);
    direct_ui_write_result(&u,2);assert(!u.pending && u.page==UI_CONFIG && u.selected==0);
    assert(!strcmp(u.message,"Saved / submitted unconfirmed"));
    const int failures[]={-10,-12,-13,-14,-15,-16,-20,-1};
    const char *messages[]={"Battery values invalid","Upgrade busy","Target not ready","Settings changed / reopen",
        "Only BBJ protocol 5 supported","Battery profile incomplete","Save failed / retry","Settings busy / retry"};
    for(unsigned i=0;i<sizeof(failures)/sizeof(*failures);i++){
        enter(&u,8,4);u.selected=14;key(&u,2);assert(key(&u,2).action==UI_ACT_BATTERY_WRITE);
        direct_ui_write_result(&u,failures[i]);assert(!u.pending && u.page==UI_BATTERY_REVIEW);
        assert(!strcmp(u.message,messages[i]));assert(u.battery_draft.revision==42);
    }
    enter(&u,8,4);u.selected=14;key(&u,2);key(&u,2);direct_ui_write_result(&u,-21);
    assert(u.page==UI_CONFIG && !strcmp(u.message,"Saved / sync incomplete"));
    enter(&u,8,4);u.selected=14;key(&u,2);key(&u,2);direct_ui_write_result(&u,0);
    assert(!strcmp(u.message,"Saved / unconfirmed"));
}
static void guards_and_legacy(void){
    direct_ui_state_t u;enter(&u,8,4);d.ota_busy=true;
    assert(key(&u,2).action==UI_ACT_NONE && !u.editing);assert(!strcmp(u.message,"Upgrade busy"));
    key(&u,1);assert(u.page==UI_CONFIG);d.ota_busy=false;
    enter(&u,8,4);u.battery_draft.ac_charge_a=76;u.selected=14;key(&u,2);
    assert(u.page==UI_BATTERY && !strcmp(u.message,"AC current exceeds total"));
    u.battery_draft.ac_charge_a=25;u.battery_draft.soc_low=u.battery_draft.soc_high;key(&u,2);
    assert(u.page==UI_BATTERY && !strcmp(u.message,"SOC thresholds out of order"));
    u.battery_draft=d.battery;u.battery_draft.type=4;u.battery_draft.protocol=1;key(&u,2);
    assert(u.page==UI_BATTERY && !strcmp(u.message,"Only BBJ protocol 5 supported"));
    field_step(&u,1,1);assert(u.battery_draft.protocol==5);
    fixture();d.battery_valid=false;memset(&d.battery,0,sizeof(d.battery));
    memset(d.valid,0,sizeof(d.valid));d.config[4]=5;d.valid[4]=true;
    d.config[23]=1;d.valid[23]=true;d.config[25]=14;d.valid[25]=true;
    d.config[75]=540;d.valid[75]=true;d.config[81]=420;d.valid[81]=true;
    enter(&u,8,4);assert(u.battery_draft.type==5 && u.battery_draft.material==1 && u.battery_draft.cells==14);
    assert(u.battery_draft.bulk_dv==0 && u.battery_draft.float_dv==540);
    direct_ui_view_t v;direct_ui_build(&u,&d,&v);assert(strstr(v.rows[4],"--"));
    u.selected=14;key(&u,2);
    assert(u.battery_draft.bulk_dv==560 && u.battery_draft.float_dv==540 && u.battery_draft.cutoff_dv==420);
    assert(u.page==UI_BATTERY); /* Existing inconsistent thresholds still reject. */
    /* Explicit review supplies source bulk, preserving known thresholds/revision. */
    fixture();d.battery.bulk_dv=0;enter(&u,8,4);assert(u.battery_draft.bulk_dv==0 && u.battery_draft.revision==42);
    u.selected=14;key(&u,2);assert(u.page==UI_BATTERY_REVIEW);
    assert(u.battery_draft.bulk_dv==560 && u.battery_draft.float_dv==540 && u.battery_draft.cutoff_dv==420);
    assert(u.battery_draft.revision==42 && u.battery_original.bulk_dv==0);
    direct_ui_build(&u,&d,&v);assert(!strcmp(v.rows[7],"Source preset / review"));
    assert(strstr(v.rows[4],"-- > 56.0V"));
}
static void remembered_and_revision(void){
    fixture();direct_ui_state_t u;
    d.battery_profiles[4].bulk_dv=0;enter(&u,8,4);field_step(&u,0,3);
    assert(u.battery_draft.bulk_dv==540 && u.battery_draft.float_dv==540 && u.battery_draft.cutoff_dv==450);
    direct_ui_view_t v;direct_ui_build(&u,&d,&v);assert(!strcmp(v.rows[7],"Source preset / review"));
    u.selected=14;key(&u,2);assert(u.page==UI_BATTERY_REVIEW);
    assert(u.battery_draft.bulk_dv==540 && u.battery_original.bulk_dv==564);
    fixture();
    d.battery_profiles[5]=d.battery;d.battery_profiles[5].type=5;d.battery_profiles[5].cells=15;
    d.battery_profiles[5].bulk_dv=d.battery_profiles[5].float_dv=545;d.battery_profiles[5].cutoff_dv=440;
    d.battery_profiles[6]=d.battery_profiles[5];d.battery_profiles[6].material=1;d.battery_profiles[6].cells=13;
    d.battery_profiles[6].bulk_dv=d.battery_profiles[6].float_dv=535;d.battery_profiles[6].cutoff_dv=430;
    enter(&u,8,4);field_step(&u,0,4);
    assert(u.battery_draft.cells==15 && u.battery_draft.bulk_dv==545);
    field_step(&u,2,1);assert(u.battery_draft.cells==13 && u.battery_draft.bulk_dv==535);common_preserved(&u);
    field_step(&u,2,-1);assert(u.battery_draft.cells==15 && u.battery_draft.bulk_dv==545);
    d.battery.revision=99;d.battery.type=3;
    u.selected=14;key(&u,2);assert(key(&u,2).action==UI_ACT_BATTERY_WRITE);
    assert(u.battery_draft.revision==42); /* New snapshots never rebase an open draft. */
    direct_ui_write_result(&u,-14);assert(u.battery_draft.revision==42);
}
static uint16_t frame[320*240],strips[320*240],guarded[320*16+2];
static void glyphs(const char *source){
    char b[512];const char *s=direct_ui_translate(source,d.language_zh,b,sizeof(b));
    while(*s){uint32_t cp=direct_gfx_utf8_next(&s);assert(direct_gfx_has_glyph(cp));}
}
static void render(direct_ui_state_t *u){
    direct_ui_view_t v;direct_ui_build(u,&d,&v);assert(v.title[0]);
    glyphs(v.title);glyphs(v.footer);for(unsigned i=0;i<DIRECT_UI_ROWS;i++)glyphs(v.rows[i]);
    const char *hints[6];direct_ui_key_hints(u,&d,hints);for(unsigned i=0;i<6;i++)glyphs(hints[i]);
    direct_gfx_t g;direct_gfx_begin(&g,frame,320,240,0,0);direct_ui_scene_draw(&g,u,&d,&v);
    for(unsigned y=0;y<240;y+=16){
        guarded[0]=guarded[320*16+1]=0xabcd;
        direct_gfx_begin(&g,guarded+1,320,16,(int)y,0);direct_ui_scene_draw(&g,u,&d,&v);
        assert(guarded[0]==0xabcd && guarded[320*16+1]==0xabcd);
        memcpy(strips+y*320,guarded+1,320*16*sizeof(uint16_t));
    }
    assert(!memcmp(frame,strips,sizeof(frame)));
    unsigned count=0;for(unsigned i=0;i<320*220;i++)count+=frame[i]!=frame[0];assert(count>500);
}
static void renders(void){
    fixture();direct_ui_state_t u;
    for(unsigned theme=0;theme<2;theme++)for(unsigned zh=0;zh<2;zh++){
        d.theme_light=theme;d.language_zh=zh;enter(&u,8,4);
        for(unsigned i=0;i<16;i++){u.selected=(uint8_t)i;render(&u);}
        u.selected=4;key(&u,2);render(&u);key(&u,1);
        u.selected=14;key(&u,2);assert(u.page==UI_BATTERY_REVIEW);
        for(unsigned i=0;i<14;i++){u.selected=(uint8_t)i;render(&u);}
        key(&u,2);render(&u);direct_ui_write_result(&u,-14);render(&u);
        key(&u,1);u.selected=0;field_step(&u,0,4);
        for(unsigned material=0;material<2;material++){
            if(material)field_step(&u,2,1);
            for(unsigned i=0;i<16;i++){u.selected=(uint8_t)i;render(&u);}
        }
    }
}
static void localized_review(void){
    char out[128];
    assert(!strcmp(direct_ui_translate("Battery type: AGM > User Li",true,out,sizeof(out)),"电池类型: AGM > 自定锂电"));
    assert(!strcmp(direct_ui_translate("Lithium material: Not used > LFP",true,out,sizeof(out)),"锂电材料: 未使用 > 铁锂"));
}
static void all_locale_glyphs(void){
    bool missing=false;
    for(size_t i=0;i<direct_ui_locale_count();i++){
        const char *s=direct_ui_locale_zh(i);
        while(*s){uint32_t cp=direct_gfx_utf8_next(&s);if(!direct_gfx_has_glyph(cp)){
            printf("Missing glyph U+%04lX: %s\n",(unsigned long)cp,direct_ui_locale_key(i));missing=true;
        }}
    }
    fflush(stdout);assert(!missing);
}
static void panel_contract(void){
    char source[40000];FILE *f=fopen("main/direct/direct_panel_runtime.inc","rb");assert(f);
    size_t n=fread(source,1,sizeof(source)-1,f);assert(!ferror(f) && feof(f));fclose(f);source[n]=0;
    assert(strstr(source,"direct_battery_config_get(&s_ui_data.battery)"));
    assert(strstr(source,"direct_battery_config_profiles(s_ui_data.battery_profiles)"));
    assert(strstr(source,"direct_battery_config_write(&s_ui.battery_draft,false)"));
}
int main(void) {
    all_locale_glyphs();fixture();flow();fixture();review_and_results();fixture();guards_and_legacy();
    remembered_and_revision();renders();panel_contract();localized_review();
    puts("PASS battery UI: grouped draft/profile/preset/revision, chemistry, review/cancel, validation/results, legacy unknowns, EN/ZH TN strip rendering");
    return 0;
}
