#include "panel_ui.h"
#include "panel_catalog.h"
#include "direct_key_codec.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static panel_snapshot_t sample(void)
{
    panel_snapshot_t s = {0};
    s.now_ms = 1000;
    s.config = (panel_config_t){1,9600,0,PANEL_PROFILE_6KU};
    const uint16_t regs[] = {0x42,0x50,0x53,0x80,0x81,0x84,0x5b,0xa0,0x150,0x140};
    for (size_t i=0;i<sizeof(regs)/sizeof(regs[0]);i++) {
        s.valid[panel_slot(regs[i])]=true;s.updated_ms[panel_slot(regs[i])]=1000;
    }
    s.values[panel_slot(0x42)]=3;s.values[panel_slot(0x150)]=4;
    return s;
}
static bool press(panel_ui_t *u,panel_snapshot_t *s,uint8_t key,panel_command_t *c)
{
    panel_ui_sync(u,s);
    return panel_ui_key(u,key,s,c);
}
int main(void)
{
    panel_ui_t u;panel_ui_init(&u);
    panel_snapshot_t s=sample();panel_command_t c={0};panel_view_t a,b;
    assert(!press(&u,&s,DIRECT_KEY_ENTER,&c));
    assert(u.page==PANEL_PAGE_MENU);
    /* Menu -> power. First ENTER is only a review, second submits once. */
    for(unsigned i=0;i<3;i++)assert(!press(&u,&s,DIRECT_KEY_DOWN,&c));
    assert(!press(&u,&s,DIRECT_KEY_ENTER,&c));
    assert(u.page==PANEL_PAGE_POWER);
    assert(!press(&u,&s,DIRECT_KEY_ENTER,&c));
    assert(u.review);
    assert(press(&u,&s,DIRECT_KEY_ENTER,&c));
    assert(c.type==PANEL_CMD_WRITE && c.address==0x4123 && c.value==1);
    assert(!press(&u,&s,DIRECT_KEY_ENTER,&c));
    assert(!press(&u,&s,DIRECT_KEY_RIGHT,&c));
    /* Terminal result for an earlier command must not unlock this one. */
    s.write_state=PANEL_WRITE_CONFIRMED;
    assert(!press(&u,&s,DIRECT_KEY_ENTER,&c));
    s.command_sequence++;s.command_busy=false;
    panel_result_record(&s,s.command_sequence,PANEL_WRITE_CONFIRMED,0);
    panel_ui_sync(&u,&s);assert(!u.pending);
    panel_ui_view(&u,&s,&a);assert(!strcmp(a.title,"操作结果"));
    assert(!press(&u,&s,DIRECT_KEY_ENTER,&c) && !u.have_result);
    /* Back cancels the review and must never send a command. */
    assert(!press(&u,&s,DIRECT_KEY_ENTER,&c));
    assert(u.review);
    assert(!press(&u,&s,DIRECT_KEY_BACK,&c));
    assert(!u.review);
    s.now_ms=10000;
    assert(!press(&u,&s,DIRECT_KEY_ENTER,&c));
    assert(!press(&u,&s,DIRECT_KEY_ENTER,&c));
    assert(!u.pending);
    /* Real zero is preserved; unknown and stale are separately visible. */
    panel_ui_init(&u);s=sample();
    assert(panel_ui_view(&u,&s,&a));
    assert(strstr(a.rows[1],"0.0V")!=NULL);
    s.now_ms=1100;assert(panel_ui_view(&u,&s,&b));
    assert(memcmp(&a,&b,sizeof(a))==0);
    s.now_ms=10000;assert(panel_ui_view(&u,&s,&b));
    assert(memcmp(&a,&b,sizeof(a))!=0);
    s.valid[panel_slot(0x50)]=false;panel_ui_view(&u,&s,&a);
    assert(strstr(a.rows[1],"--")!=NULL);
    /* Communication address cannot wrap to the Modbus broadcast address. */
    panel_ui_init(&u);s=sample();u.page=PANEL_PAGE_COMM;
    assert(!press(&u,&s,DIRECT_KEY_ENTER,&c));
    assert(u.editing);
    assert(!press(&u,&s,DIRECT_KEY_LEFT,&c));
    assert(u.comm_draft.address==1);
    for(unsigned i=0;i<300;i++)assert(!press(&u,&s,DIRECT_KEY_RIGHT,&c));
    assert(u.comm_draft.address==247);
    assert(!press(&u,&s,DIRECT_KEY_BACK,&c));
    assert(!u.editing && !u.pending);
    /* A failed local configuration save is explicit and unlocks only on its completion. */
    assert(!press(&u,&s,DIRECT_KEY_ENTER,&c));
    assert(!press(&u,&s,DIRECT_KEY_ENTER,&c));
    assert(press(&u,&s,DIRECT_KEY_ENTER,&c));
    assert(c.type==PANEL_CMD_CONFIG && c.config.address==1);
    s.command_sequence++;s.write_state=PANEL_WRITE_REJECTED;s.exception=0xff;
    panel_result_record(&s,s.command_sequence,PANEL_WRITE_REJECTED,0xff);
    panel_ui_sync(&u,&s);assert(!u.pending);
    panel_ui_view(&u,&s,&a);assert(strstr(a.footer,"失败")!=NULL);
    /* Read-only search may be cancelled with BACK even while commands are locked. */
    s=sample();panel_ui_init(&u);u.page=PANEL_PAGE_COMM;u.selected=4;
    assert(!press(&u,&s,DIRECT_KEY_ENTER,&c) && u.review);
    panel_ui_view(&u,&s,&a);assert(strstr(a.rows[6],"暂停")!=NULL);
    assert(!press(&u,&s,DIRECT_KEY_BACK,&c) && !u.review);
    assert(!press(&u,&s,DIRECT_KEY_ENTER,&c) && u.review);
    assert(press(&u,&s,DIRECT_KEY_ENTER,&c) && c.type==PANEL_CMD_SCAN);
    s.command_sequence++;s.command_busy=true;s.scanning=true;s.scan_address=24;
    assert(!press(&u,&s,DIRECT_KEY_RIGHT,&c));
    assert(press(&u,&s,DIRECT_KEY_BACK,&c) && c.type==PANEL_CMD_SCAN_CANCEL);
    uint32_t scan_sequence=u.pending_sequence;
    panel_ui_submit_failed(&u,c.type);
    assert(u.pending && u.pending_sequence==scan_sequence && u.pending_type==PANEL_CMD_SCAN);
    panel_ui_submitted(&u,++s.command_sequence,c.type);
    assert(!press(&u,&s,DIRECT_KEY_BACK,&c));
    s.command_busy=false;s.scanning=false;s.scan_found=0;
    panel_result_record(&s,s.command_sequence,PANEL_WRITE_IDLE,0);
    panel_ui_sync(&u,&s);assert(!u.pending);
    panel_ui_view(&u,&s,&a);assert(strstr(a.footer,"已停止")!=NULL);
    panel_ui_submit_failed(&u,PANEL_CMD_SCAN);assert(!u.pending);
    panel_ui_view(&u,&s,&a);assert(strstr(a.footer,"未提交")!=NULL);
    /* Distinguish unavailable current-value queries from missing samples. */
    panel_ui_init(&u);u.page=PANEL_PAGE_SETTINGS;s=sample();
    size_t absent=0;
    while(absent<panel_setting_count() && panel_setting(absent)->read_address!=0xffff)absent++;
    assert(absent<panel_setting_count());u.selected=(uint16_t)absent;
    panel_ui_view(&u,&s,&a);
    assert(strstr(a.subtitle,"正在读取")!=NULL);
    assert(!press(&u,&s,DIRECT_KEY_ENTER,&c) && u.editing);
    panel_ui_view(&u,&s,&a);
    assert(!strcmp(a.values[1],"正在读取") && !strcmp(a.values[2],"--"));
    press(&u,&s,DIRECT_KEY_BACK,&c);
    u.selected=(uint16_t)(panel_setting_find(0x4101)-panel_setting(0));
    panel_ui_view(&u,&s,&a);
    assert(strstr(a.subtitle,"正在读取")!=NULL);
    panel_invalidate(&s,0x42);
    panel_ui_view(&u,&s,&a);
    assert(strstr(a.subtitle,"通信已断开")!=NULL);
    panel_store(&s,0x42,3,s.now_ms);
    panel_store(&s,0x02a1,5000,s.now_ms);
    panel_ui_view(&u,&s,&a);
    assert(strstr(a.subtitle,"50 Hz")!=NULL);
    u.selected=(uint16_t)(panel_setting_find(0x4107)-panel_setting(0));
    s.config.profile_override=PANEL_PROFILE_4KL;
    panel_ui_view(&u,&s,&a);
    assert(strstr(a.subtitle,"正在读取")!=NULL);
    u.have_result=true;u.result_address=0x4100;u.result_value=2;
    u.last_result=PANEL_WRITE_PROTOCOL_ACCEPTED;
    panel_ui_view(&u,&s,&a);
    assert(!a.alert && !strcmp(a.rows[2],"设备已受理") && strstr(a.rows[4],"完成应答"));
    u.last_result=PANEL_WRITE_PROCESSED;
    panel_ui_view(&u,&s,&a);
    assert(a.alert && strstr(a.rows[2],"设置未生效"));
    /* A real 03 value from the R/W address is displayed and seeds the draft,
     * but is labelled as returned data rather than independent confirmation. */
    panel_ui_init(&u);u.page=PANEL_PAGE_SETTINGS;u.setting_group=2;s=sample();
    u.selected=(uint16_t)(panel_setting_find(0x4105)-panel_setting(0));
    s.setting_reads[0]=(panel_setting_read_t){0x4105,20,s.now_ms,PANEL_SETTING_VALUE,false};
    panel_ui_view(&u,&s,&a);assert(a.setting_count==5 && a.setting_addresses[0]==0x4105);
    assert(!strcmp(a.values[0],"20 A"));
    assert(!press(&u,&s,DIRECT_KEY_ENTER,&c) && u.draft==20 && !u.edit_original_valid);
    panel_ui_view(&u,&s,&a);assert(!strcmp(a.rows[1],"设备返回值") && !strcmp(a.values[1],"20 A"));
    assert(!a.rows[5][0] && a.setting_count==1 && a.setting_addresses[0]==0x4105);
    s.setting_reads[0].state=PANEL_SETTING_EMPTY;
    panel_ui_view(&u,&s,&a);assert(!strcmp(a.values[1],"设备未返回当前值"));
    u.editing=false;u.have_result=true;u.last_result=PANEL_WRITE_UNCERTAIN;
    u.result_address=0x4105;u.result_value=20;u.result_profile=PANEL_PROFILE_6KU;
    s.setting_reads[0].state=PANEL_SETTING_VALUE;s.setting_reads[0].stamp=s.now_ms-PANEL_FRESH_MS-1U;
    panel_ui_view(&u,&s,&a);
    assert(a.result && (a.stale_rows&2) && strstr(a.footer,"关闭") && !strstr(a.footer,"刷新"));
    /* A diagnostic model candidate is not permission to use model voltage limits. */
    panel_ui_init(&u);u.page=PANEL_PAGE_SETTINGS;s=sample();
    s.config.profile_override=PANEL_PROFILE_UNKNOWN;s.detected_profile=PANEL_PROFILE_6KU;
    assert(!press(&u,&s,DIRECT_KEY_ENTER,&c));assert(!u.editing && !u.pending);
    puts("PASS UI: confirmation, old completion, pending, cancel, stale/zero, address bounds, unknown settings");
    /* Completion belongs to the submitted sequence even if another owner is busy. */
    panel_ui_init(&u);s=sample();u.page=PANEL_PAGE_POWER;
    press(&u,&s,DIRECT_KEY_ENTER,&c);assert(press(&u,&s,DIRECT_KEY_ENTER,&c));
    panel_ui_submitted(&u,41,c.type);
    s.command_sequence=42;s.command_busy=true;s.write_state=PANEL_WRITE_REJECTED;
    panel_result_record(&s,41,PANEL_WRITE_CONFIRMED,0);
    panel_ui_sync(&u,&s);assert(!u.pending && strstr(u.message,"设置已生效"));
    s.command_busy=false;press(&u,&s,DIRECT_KEY_DOWN,&c);panel_ui_view(&u,&s,&a);
    assert(!u.have_result && !strstr(a.footer,"回读确认") && !strstr(a.footer,"失败"));
    panel_ui_init(&u);s=sample();u.page=PANEL_PAGE_POWER;u.language_en=true;
    assert(!press(&u,&s,DIRECT_KEY_UP,&c) && u.selected==2);
    panel_ui_view(&u,&s,&a);
    assert(a.selected==2 && !strcmp(a.rows[2],"Disable remote power control"));
    assert(!press(&u,&s,DIRECT_KEY_ENTER,&c) && u.review);
    assert(!press(&u,&s,DIRECT_KEY_BACK,&c) && !u.review && !u.pending);
    assert(!press(&u,&s,DIRECT_KEY_ENTER,&c) && u.review);
    assert(press(&u,&s,DIRECT_KEY_ENTER,&c) && c.address==0x4123 && c.value==0);
    /* A later unrelated completion must never finish an unobserved request. */
    u.pending=true;u.pending_sequence=43;s.command_sequence=44;s.command_busy=false;
    panel_result_record(&s,44,PANEL_WRITE_CONFIRMED,0);
    panel_ui_sync(&u,&s);assert(u.pending);
    panel_ui_init(&u);s=sample();u.page=PANEL_PAGE_MENU;u.selected=2;
    assert(!press(&u,&s,DIRECT_KEY_ENTER,&c) && u.page==PANEL_PAGE_SETTING_GROUPS);
    u.selected=3;press(&u,&s,DIRECT_KEY_ENTER,&c);
    assert(u.page==PANEL_PAGE_SETTINGS && u.setting_group==3);
    unsigned start=u.selected;
    for(unsigned i=0;i<panel_setting_group_count(3);i++)press(&u,&s,DIRECT_KEY_DOWN,&c);
    assert(u.selected==start);
    press(&u,&s,DIRECT_KEY_BACK,&c);
    assert(u.page==PANEL_PAGE_SETTING_GROUPS && u.selected==3);
    u.selected=0;press(&u,&s,DIRECT_KEY_ENTER,&c);
    assert(!press(&u,&s,DIRECT_KEY_RIGHT,&c));
    assert(panel_setting_group_position(0,u.selected)==5);
    assert(!press(&u,&s,DIRECT_KEY_DOWN,&c));
    assert(panel_setting_group_position(0,u.selected)==6);
    assert(!press(&u,&s,DIRECT_KEY_LEFT,&c));
    assert(panel_setting_group_position(0,u.selected)==1);
    /* Battery limits can supply a draft in standby, but never write on entry.
     * A change in the battery limit before confirmation invalidates the draft. */
    panel_ui_init(&u);s=sample();u.page=PANEL_PAGE_SETTINGS;u.setting_group=2;
    u.selected=(uint16_t)(panel_setting_find(0x4106)-panel_setting(0));
    u.battery_limits_page=true;
    u.battery_charge_deciamp=499;u.battery_charge_decivolt=568;u.battery_discharge_decivolt=448;
    panel_store(&s,0x42,2,s.now_ms);
    assert(!press(&u,&s,DIRECT_KEY_ENTER,&c) && u.battery_draft && u.draft==4);
    u.battery_charge_deciamp=301;
    assert(!press(&u,&s,DIRECT_KEY_ENTER,&c) && !u.editing);
    assert(!press(&u,&s,DIRECT_KEY_ENTER,&c) && u.battery_draft && u.draft==3);
    assert(press(&u,&s,DIRECT_KEY_ENTER,&c) && c.address==0x4106 && c.value==3);
    return 0;
}
