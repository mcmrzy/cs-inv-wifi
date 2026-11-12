#include "panel_modbus.h"
#include "panel_catalog.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

#ifndef PANEL_USE_REAL_MODEL
bool panel_value(const panel_snapshot_t *s,uint16_t a,uint16_t *v){
    if(!s || a>=PANEL_REG_COUNT || !s->valid[panel_slot(a)])return false;
    if(v)*v=s->values[panel_slot(a)];
    return true;
}
bool panel_fresh(const panel_snapshot_t *s,uint16_t a){return panel_value(s,a,NULL) && s->now_ms-s->updated_ms[panel_slot(a)]<=PANEL_FRESH_MS;}
bool panel_online(const panel_snapshot_t *s){return panel_fresh(s,0x42);}
#endif

static size_t finish(uint8_t *p,size_t n){uint16_t c=panel_mb_crc(p,n);p[n]=(uint8_t)c;p[n+1]=(uint8_t)(c>>8);return n+2;}
static bool feed(panel_mb_stream_t *s,const uint8_t *p,size_t n,uint8_t f,uint16_t a,uint16_t c,panel_mb_reply_t *r){
    bool done=false;for(size_t i=0;i<n;i++)done=panel_mb_feed(s,p[i],1,f,a,c,r)||done;return done;
}
static panel_snapshot_t snap(void){panel_snapshot_t s={0};s.now_ms=100;s.config.address=1;s.config.profile_override=PANEL_PROFILE_6KU;s.valid[panel_slot(0x42)]=true;s.values[panel_slot(0x42)]=3;s.updated_ms[panel_slot(0x42)]=100;s.valid[panel_slot(0x150)]=true;s.values[panel_slot(0x150)]=4;s.updated_ms[panel_slot(0x150)]=100;return s;}
static void protocol(void){
    /* Exact CRC-valid status reply captured on the user's inverted TTL link. */
    panel_mb_stream_t observed={0};panel_mb_reply_t status={0};
    const uint8_t status_reply[]={1,3,2,0,5,0x78,0x47};
    assert(feed(&observed,status_reply,sizeof(status_reply),3,0x42,1,&status));
    assert(!status.exception && status.values[0]==5 && status.start==0x42);
    uint8_t out[11];assert(panel_mb_crc((const uint8_t*)"123456789",9)==0x4B37);
    assert(panel_mb_read(1,0x40,2,out)==8);uint8_t expect[]={1,3,0,0x40,0,2,0xC5,0xDF};assert(memcmp(out,expect,8)==0);
    const uint8_t csergy_request[]={0x10,4,0x10,0,0,1,0x36,0x4b};
    const uint8_t csergy_reply[]={0x10,4,2,2,0x8c,0x45,0xf6};
    assert(panel_mb_read_input(0x10,0x1000,1,out)==8 && !memcmp(out,csergy_request,8));
    for(size_t i=0;i<sizeof(csergy_request);i++)
        assert(!panel_mb_feed(&observed,csergy_request[i],0x10,4,0x1000,1,&status));
    for(size_t i=0;i<sizeof(csergy_reply);i++)
        assert(panel_mb_feed(&observed,csergy_reply[i],0x10,4,0x1000,1,&status)==(i+1==sizeof(csergy_reply)));
    assert(status.function==4 && !status.exception && status.values[0]==652);
    assert(!panel_mb_read(0,0,1,out));assert(!panel_mb_read(248,0,1,out));assert(!panel_mb_read(1,0xffff,2,out));assert(!panel_mb_read(1,0,126,out));
    assert(panel_mb_read(247,0xffff,1,out)==8);assert(!panel_mb_read(1,0,0,out));assert(!panel_mb_read(1,0,1,NULL));
    assert(panel_mb_write(1,0x4123,2,out)==11);assert(out[1]==0x10 && out[5]==1 && out[6]==2 && out[8]==2);
    assert(!panel_mb_write(0,0x4123,2,out));
    panel_mb_stream_t s={0};panel_mb_reply_t r={0};uint8_t p[]={1,3,4,0xFF,0x9C,0,42,0,0};finish(p,7);
    assert(!feed(&s,p,3,3,0x50,2,&r));assert(feed(&s,p+3,6,3,0x50,2,&r));assert(r.values[panel_slot(0)]==0xFF9C && r.values[panel_slot(1)]==42 && r.count==2);
    uint8_t corrupt[9];memcpy(corrupt,p,9);corrupt[4]^=1;assert(!feed(&s,corrupt,9,3,0x50,2,&r));assert(feed(&s,p,9,3,0x50,2,&r));
    uint8_t wrong[9];memcpy(wrong,p,9);wrong[0]=2;finish(wrong,7);assert(!feed(&s,wrong,9,3,0x50,2,&r));assert(feed(&s,p,9,3,0x50,2,&r));
    uint8_t badlen[]={1,3,250,0xaa,0xbb,1,3,0xff};assert(!feed(&s,badlen,sizeof badlen,3,0x50,2,&r));assert(feed(&s,p,9,3,0x50,2,&r));
    uint8_t ack[]={1,0x10,0x41,0x23,0,1,0,0};finish(ack,6);assert(feed(&s,ack,8,0x10,0x4123,1,&r));assert(!r.exception && r.start==0x4123);
    ack[3]=0x22;finish(ack,6);assert(!feed(&s,ack,8,0x10,0x4123,1,&r));
    uint8_t exc[]={1,0x90,3,0,0};finish(exc,3);assert(feed(&s,exc,5,0x10,0x4123,1,&r));assert(r.exception==3);
    for(unsigned i=0;i<1000;i++)panel_mb_feed(&s,(uint8_t)(i*37),1,3,0x50,2,&r);
    assert(feed(&s,p,9,3,0x50,2,&r));
    uint8_t maximum[255]={1,3,250};
    for(unsigned i=0;i<125;i++){maximum[3+i*2]=(uint8_t)(i>>8);maximum[4+i*2]=(uint8_t)i;}
    finish(maximum,253);assert(feed(&s,maximum,sizeof maximum,3,0,125,&r));assert(r.count==125 && r.values[panel_slot(124)]==124);
    uint8_t three[]={1,3,6,0,1,0,2,0,3,0,0};finish(three,9);
    assert(!feed(&s,three,sizeof three,3,0x50,2,&r));assert(feed(&s,p,9,3,0x50,2,&r));
}
static void catalog(void){
    panel_snapshot_t s=snap();uint16_t n;char b[64];assert(panel_setting_count()>=20);assert(panel_setting_find(0x4105));assert(!panel_setting_find(0x4000));
    assert(panel_setting_allowed(&s,0x4105,120));assert(!panel_setting_allowed(&s,0x4105,12));assert(!panel_setting_allowed(&s,0x4105,130));
    assert(panel_setting_allowed(&s,0x4106,12));assert(!panel_setting_allowed(&s,0x4106,120));
    assert(panel_setting_allowed(&s,0x4108,420));assert(!panel_setting_allowed(&s,0x4108,419));assert(panel_setting_allowed(&s,0x4109,480));assert(!panel_setting_allowed(&s,0x4109,481));
    s.config.profile_override=PANEL_PROFILE_2KU;assert(panel_setting_allowed(&s,0x4108,105));assert(!panel_setting_allowed(&s,0x4108,210));assert(panel_setting_allowed(&s,0x4105,100));
    s.config.profile_override=PANEL_PROFILE_12KU;assert(!panel_setting_allowed(&s,0x4100,0));assert(!panel_setting_allowed(&s,0x4106,0));assert(!panel_setting_allowed(&s,0x411F,0xFFF2));
    s.config.profile_override=PANEL_PROFILE_4KL;assert(!panel_setting_allowed(&s,0x4107,8));assert(!panel_setting_allowed(&s,0x4101,0));
    s.config.profile_override=PANEL_PROFILE_UNKNOWN;assert(!panel_setting_allowed(&s,0x4108,240));assert(!panel_setting_allowed(&s,0x4107,0));assert(panel_setting_allowed(&s,0x4103,1));assert(panel_setting_allowed(&s,0x4123,2));
    s=snap();s.valid[panel_slot(0x150)]=false;assert(!panel_setting_allowed(&s,0x410A,550));assert(!panel_setting_allowed(&s,0x4107,4));s=snap();s.now_ms=5101;assert(!panel_setting_allowed(&s,0x4123,2));s=snap();s.values[panel_slot(0x42)]=7;assert(!panel_setting_allowed(&s,0x4103,1));
    s=snap();assert(panel_setting_step(&s,0x4105,2,1,&n)&&n==10);assert(panel_setting_step(&s,0x4119,900,1,&n)&&n==0xFFF1);assert(panel_setting_step(&s,0x4119,0xFFF1,1,&n)&&n==0xFFF2);assert(!panel_setting_step(&s,0x4119,0xFFF2,1,&n));
    assert(!panel_setting_allowed(&s,0x4120,0xFFFF));assert(panel_setting_allowed(&s,0x4120,0xFFF1));
    panel_setting_format(&s,0x4106,12,b,sizeof b);assert(strstr(b,"120")!=NULL);panel_setting_format(&s,0x4108,476,b,sizeof b);assert(strstr(b,"47.6")!=NULL);
    panel_setting_format(&s,0x4106,17,b,sizeof b);assert(strstr(b,"code")!=NULL);panel_setting_format(&s,0x4105,0xfff1,b,sizeof b);assert(strstr(b,"OFF")==NULL);
    assert(panel_detect_profile(0,0x10)==PANEL_PROFILE_6KU);assert(panel_detect_profile(0x10,0)==PANEL_PROFILE_6KU);assert(panel_detect_profile(0x10,0x20)==PANEL_PROFILE_UNKNOWN);assert(panel_detect_profile(0,0)==PANEL_PROFILE_UNKNOWN);
    assert(panel_detect_profile(0x0110,0)==PANEL_PROFILE_UNKNOWN);assert(panel_detect_profile(0,0x30)==PANEL_PROFILE_UNKNOWN);
    s=snap();s.now_ms=5100;assert(panel_setting_allowed(&s,0x4103,0));s.now_ms=5101;assert(!panel_setting_allowed(&s,0x4103,0));
    s=snap();s.updated_ms[panel_slot(0x42)]=0xfffffff0U;s.now_ms=20;assert(panel_setting_allowed(&s,0x4103,0));
    s=snap();s.values[panel_slot(0x150)]=0;assert(!panel_setting_allowed(&s,0x410A,564));assert(panel_setting_allowed(&s,0x4112,1));s.values[panel_slot(0x150)]=2;assert(!panel_setting_allowed(&s,0x4112,1));assert(panel_setting_allowed(&s,0x4112,0));
    s=snap();s.values[panel_slot(0x42)]=8;assert(panel_setting_allowed(&s,0x4123,1));s.values[panel_slot(0x42)]=10;assert(!panel_setting_allowed(&s,0x4123,1));
}
static void model_settings(void){
    panel_snapshot_t s=snap();uint16_t next;
    s.detected_profile=PANEL_PROFILE_3KU;s.config.profile_override=PANEL_PROFILE_UNKNOWN;
    assert(panel_setting_access(&s,0x4105)==PANEL_ACCESS_MODEL);
    assert(!panel_setting_allowed(&s,0x4101,0));
    s.config.profile_override=PANEL_PROFILE_3KU;
    panel_store(&s,0x42,3,s.now_ms);
    panel_store(&s,0x150,2,s.now_ms);
    assert(panel_setting_allowed(&s,0x4101,1));
    assert(panel_setting_step(&s,0x4105,29,1,&next) && next==30);
    assert(panel_setting_allowed(&s,0x4105,40) && !panel_setting_allowed(&s,0x4105,41));
    assert(panel_setting_allowed(&s,0x4106,10) && !panel_setting_allowed(&s,0x4106,11));
    assert(panel_setting_allowed(&s,0x4104,3) && !panel_setting_allowed(&s,0x4102,3));
    assert(panel_setting_allowed(&s,0x4108,206) && !panel_setting_allowed(&s,0x4108,205));
    assert(!panel_setting_allowed(&s,0x4110,0) && !panel_setting_allowed(&s,0x411f,1));
    assert(!panel_setting_allowed(&s,0x411b,36) && !panel_setting_allowed(&s,0x411d,1));
    assert(panel_setting_allowed(&s,0x4124,1) && !panel_setting_allowed(&s,0x414b,20));
    assert(panel_setting_allowed(&s,0x4119,894) && !panel_setting_allowed(&s,0x4119,900));
    assert(panel_setting_current_address(&s,panel_setting_find(0x4106))==0x179);
    assert(panel_setting_decode_current(&s,0x4106,6000,&next) && next==6);
    assert(!panel_setting_decode_current(&s,0x4106,6100,&next));
    assert(panel_setting_decode_current(&s,0x411a,1U<<8,&next) && next==0);
    assert(panel_setting_decode_current(&s,0x4116,30,&next) && next==30);
    assert(panel_setting_decode_current(&s,0x4119,0xffff,&next) && next==0xfff2);
    assert(!panel_setting_decode_current(&s,0x4119,60,&next));
    assert(panel_setting_allowed(&s,0x4120,20));
    panel_store(&s,0x150,3,s.now_ms);
    assert(panel_setting_access(&s,0x4108)==PANEL_ACCESS_DEFINITION);
    assert(!panel_setting_allowed(&s,0x4120,20) && panel_setting_allowed(&s,0x4120,0xfff1));
    panel_store(&s,0x42,5,s.now_ms);
    assert(panel_setting_access(&s,0x4101)==PANEL_ACCESS_STATE);
    assert(panel_setting_allowed(&s,0x4100,2));
    s.config.profile_override=PANEL_PROFILE_12KU;
    assert(!panel_setting_allowed(&s,0x4102,0));

    s=snap();panel_store(&s,0x140,0,s.now_ms);
    assert(panel_setting_allowed(&s,0x410e,154) && !panel_setting_allowed(&s,0x410e,155));
    assert(panel_setting_allowed(&s,0x4142,24) && !panel_setting_allowed(&s,0x4142,25));
    s.config.profile_override=PANEL_PROFILE_12KU;
    assert(panel_setting_allowed(&s,0x4142,42) && !panel_setting_allowed(&s,0x4142,43));
    assert(!panel_setting_allowed(&s,0x4153,1));
    panel_store(&s,0x42,2,s.now_ms);
    assert(panel_setting_allowed(&s,0x4153,1));
    s.config.profile_override=PANEL_PROFILE_2KU;
    assert(panel_setting_allowed(&s,0x4142,28) && !panel_setting_allowed(&s,0x4142,29));
    assert(panel_setting_step(&s,0x418d,55,1,&next) && next==0x8001);
    assert(panel_setting_step(&s,0x418d,0x8001,-1,&next) && next==55);
    assert(!panel_setting_find(0x419e) && !panel_setting_find(0x41a0));
    const uint16_t retired[]={0x11,0x20,0x70};
    for(unsigned i=0;i<sizeof(retired)/sizeof(retired[0]);i++){
        s.detected_profile=panel_detect_profile(0,retired[i]);
        assert(s.detected_profile!=PANEL_PROFILE_UNKNOWN && !panel_profile_supported(s.detected_profile));
        assert(!panel_setting_allowed(&s,0x4101,0));
        assert(panel_setting_access(&s,0x4101)==PANEL_ACCESS_UNSUPPORTED);
    }
}
int main(void){protocol();catalog();model_settings();
    uint16_t value;
    assert(panel_setting_decode_readback(0x4101,5000,&value) && value==0);
    assert(panel_setting_decode_readback(0x4101,6000,&value) && value==1);
    assert(!panel_setting_decode_readback(0x4101,5001,&value));
    assert(panel_setting_decode_readback(0x4128,0xa004,&value) && value==1);
    assert(panel_setting_decode_readback(0x4128,0xa000,&value) && value==0);
    assert(panel_setting_decode_readback(0x4132,0x4000,&value) && value==1);
    assert(!panel_setting_decode_readback(0x4105,0xffff,&value));
    char hex[73],tiny[2]={'x','x'};uint8_t raw[30]={1,0x83,2,0xc0,0xf1};
    panel_mb_hex_head(raw,5,hex,sizeof(hex));assert(!strcmp(hex,"01 83 02 C0 F1"));
    panel_mb_hex_head(raw,30,hex,sizeof(hex));assert(strlen(hex)==71);
    panel_mb_hex_head(NULL,0,hex,sizeof(hex));assert(!strcmp(hex,"-"));
    panel_mb_hex_head(raw,5,tiny,sizeof(tiny));assert(tiny[0]==0);
    bool seen[64]={0};unsigned total=0;
    for(unsigned group=1;group<PANEL_SETTING_GROUPS;group++)
        for(unsigned p=0;p<panel_setting_group_count(group);p++){
            unsigned index=panel_setting_group_index(group,p);
            assert(index<panel_setting_count() && !seen[index]);seen[index]=true;total++;
            assert(panel_setting_group_position(group,index)==p);
        }
    assert(total==panel_setting_count() && panel_setting_group_count(0)==total);
    assert(panel_setting_group_index(PANEL_SETTING_GROUPS,0)==total);
    panel_snapshot_t s=snap();
    s.config.profile_override=PANEL_PROFILE_UNKNOWN;
    assert(strstr(panel_setting_restriction(&s,0x4100),"机型"));
    s.now_ms=9000;assert(strstr(panel_setting_restriction(&s,0x4100),"应答"));
    puts("PASS Modbus stream, transaction matching, model ranges, frequency/bitfield readbacks and edit steps");return 0;}
