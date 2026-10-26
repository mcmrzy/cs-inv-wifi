#include <assert.h>
#include <stdio.h>
#include <string.h>
#if !__has_include("direct_dsp_params.h")
int main(void){assert(!"CMD00/01/02 persistent parameter codec is missing");}
#else
#include "direct_dsp_params.h"
static void seal(uint8_t *frame,size_t size){uint16_t c=direct_crc16(frame,size-2);frame[size-2]=(uint8_t)c;frame[size-1]=(uint8_t)(c>>8);}
static void word(uint8_t *p,unsigned at,uint16_t v){p[at]=(uint8_t)(v>>8);p[at+1]=(uint8_t)v;}
static void fixture(uint8_t bundle[64]){
    memset(bundle,0,64);bundle[0]=1;bundle[1]=21;bundle[2]=0;
    uint16_t words[]={2300,5000,540,400,1,60,140,3,1,480};
    for(unsigned i=0;i<10;i++)word(bundle,3+2*i,words[i]);
    seal(bundle,25);
    uint8_t *p=bundle+25;p[0]=1;p[1]=19;p[2]=1;
    uint16_t cfg[]={576,2800,900,0x1dda,1,50,95,5,0x0321};
    for(unsigned i=0;i<9;i++)word(p,3+2*i,cfg[i]);
    seal(p,23);
    p=bundle+48;p[0]=1;p[1]=12;p[2]=2;p[3]=0;
    word(p,4,20);word(p,6,80);word(p,8,480);word(p,10,560);word(p,12,6000);seal(p,16);
}
int main(void){
    direct_dsp_params_t defaults,user;uint16_t setting;
    assert(direct_dsp_params_arm_defaults(&defaults));user=defaults;
    assert(direct_dsp_params_set(&user,7,2400) && direct_dsp_params_get(&user,7,&setting) && setting==2400);
    assert(direct_dsp_params_get(&defaults,7,&setting) && setting==2300);
    assert(direct_dsp_params_set(&user,5,0) && direct_dsp_params_get(&user,5,&setting) && setting==0);
    assert((user.frames[34]==defaults.frames[34]) && (user.frames[35]==(defaults.frames[35]&~0x40U)));
    assert(direct_dsp_params_set(&user,34,490) && direct_dsp_params_get(&user,34,&setting) && setting==490);
    assert(direct_dsp_params_set(&user,70,550) && direct_dsp_params_get(&user,70,&setting) && setting==550);
    direct_dsp_params_t saved=user;
    assert(!direct_dsp_params_set(&user,1,20) && !memcmp(&saved,&user,sizeof(user)));
    assert(!direct_dsp_params_set(&user,4,4) && !direct_dsp_params_set(&user,67,580));
    assert(!direct_dsp_params_get(&user,68,&setting));
    puts("PASS desired settings codec: native units, flags preserve reserved bits, active profile only, atomic invalid rejection, factory defaults remain immutable");
    uint8_t bundle[64];fixture(bundle);direct_dsp_params_t params={0},decoded={0};
    assert(direct_dsp_params_decode(bundle,64,&params));
    for(unsigned cmd=0;cmd<3;cmd++){uint8_t reply[25];size_t n=direct_dsp_params_reply(&params,cmd,reply);
        assert(n==(cmd==0?25:cmd==1?23:16));assert(!memcmp(reply,bundle+(cmd==0?0:cmd==1?25:48),n));}
    uint8_t record[DIRECT_DSP_PARAMS_RECORD_SIZE];
    assert(direct_dsp_params_pack(&params,"H1ZZX00139000079",record));
    assert(direct_dsp_params_unpack(record,sizeof(record),"H1ZZX00139000079",&decoded));
    assert(!memcmp(&params,&decoded,sizeof(params)));
    params.revision=1234;params.buzzer=false;
    assert(direct_dsp_params_pack(&params,"H1ZZX00139000079",record));
    assert(direct_dsp_params_unpack(record,sizeof(record),"H1ZZX00139000079",&decoded) && decoded.revision==1234 && !decoded.buzzer);
    assert(direct_dsp_params_arm_defaults(&defaults) && defaults.revision==1 && defaults.buzzer);
    assert(!direct_dsp_params_unpack(record,sizeof(record),"H1ZZX00139000081",&decoded));
    for(unsigned i=0;i<sizeof(record);i++){record[i]^=1;assert(!direct_dsp_params_unpack(record,sizeof(record),"H1ZZX00139000079",&decoded));record[i]^=1;}
    direct_dsp_params_t before=params;bundle[5]^=1;
    assert(!direct_dsp_params_decode(bundle,64,&params));assert(!memcmp(&before,&params,sizeof(params)));bundle[5]^=1;
    word(bundle,3,0);seal(bundle,25);assert(!direct_dsp_params_decode(bundle,64,&params));fixture(bundle);
    assert(!direct_dsp_params_decode(bundle,63,&params));
    uint8_t readback[43]={1,39,0x25};readback[3]=4;readback[4]=3;readback[12]=4;readback[11]=140;readback[26]=60;
    word(readback,7,2400);word(readback,9,6000);word(readback,13,580);word(readback,15,560);word(readback,17,440);
    word(readback,19,480);word(readback,21,576);readback[23]=5;readback[24]=45;readback[25]=90;readback[40]=9;seal(readback,43);
    uint8_t voltage_write[7]={1,3,7};word(voltage_write,3,2400);seal(voltage_write,7);
    assert(direct_dsp_params_apply_write(&params,7,voltage_write,7,readback,43));
    assert(params.frames[3]==9 && params.frames[4]==96); /* 2400 */
    assert(!memcmp(params.frames+25,before.frames+25,39)); /* unrelated settings unchanged */
    uint8_t type_write[7]={1,3,0x15,4,9};seal(type_write,7);
    assert(direct_dsp_params_apply_write(&params,48,type_write,7,readback,43));
    assert(params.frames[25+9]==0x1f && params.frames[25+10]==0xda); /* only Li bit9 changed */
    assert(params.frames[25+19]==3 && params.frames[25+20]==9); /* preserved word1 high byte */
    assert(!memcmp(params.frames+48,before.frames+48,16)); /* generator untouched */
    assert(!direct_dsp_params_apply_write(&params,48,type_write,7,readback,43));
    assert(params.frames[13]==0 && params.frames[14]==60 && params.frames[15]==0 && params.frames[16]==140);
    uint8_t write[9]={1,5,0x17};word(write,3,150);word(write,5,60);seal(write,9);
    readback[11]=150;readback[26]=50;seal(readback,43);
    assert(!direct_dsp_params_write_matches(write,9,readback,43));
    assert(!direct_dsp_params_apply_write(&params,1,write,9,readback,43));
    readback[26]=60;seal(readback,43);assert(direct_dsp_params_write_matches(write,9,readback,43));
    assert(direct_dsp_params_apply_write(&params,1,write,9,readback,43));
    assert(params.frames[15]==0 && params.frames[16]==150);
    word(write,3,160);word(write,5,5);seal(write,9);readback[11]=160;readback[26]=5;seal(readback,43);
    assert(direct_dsp_params_apply_write(&params,1,write,9,readback,43));
    assert(params.frames[14]==60); /* running companion limit must not become user setting */
    uint8_t reply[25];for(unsigned cmd=0;cmd<3;cmd++)assert(direct_dsp_validate(reply,direct_dsp_params_reply(&params,cmd,reply)));
    const uint8_t collided[]={1,18,0,0x21,0x90,1,65,0,0x21,0x90};
    assert(direct_dsp_params_collision_hint(collided,10));
    assert(!direct_dsp_params_collision_hint(collided,9));
    uint8_t bad[10];memcpy(bad,collided,10);bad[9]^=1;assert(!direct_dsp_params_collision_hint(bad,10));
    puts("PASS DSP parameters: exact 00/01/02 replies; SN-bound versioned record; corrupt/truncated/unsafe import rejected atomically; only completely matched explicit writes persist, preserves unrelated bits and generator, unchanged readback is no-op");
}
#endif
