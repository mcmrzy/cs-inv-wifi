#include "panel_alarm.h"
#include "panel_modbus.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static panel_alarm_store_t store;
static panel_alarm_sample_t sample;
static void reset(void){panel_alarm_init(&store);sample=(panel_alarm_sample_t){.fresh=15,.reachable=true,.now_ms=100};}
int main(void){
    reset();assert(!panel_alarm_front(&store));
    sample.words[0]=0x8001;sample.words[1]=0x8000;sample.words[3]=0x8000;
    assert(panel_alarm_observe(&store,&sample,false));assert(store.count==4);
    const panel_alarm_event_t *e=panel_alarm_front(&store);
    assert(e && e->code==0x4600 && e->source==0 && (e->flags&3)==3);
    assert(store.events[1].code==0x460f && store.events[2].code==0x470f && store.events[3].code==0x490f);
    assert(!panel_alarm_observe(&store,&sample,false) && store.count==4);
    assert(!panel_alarm_assign_time(&store,0,200,1));
    assert(panel_alarm_assign_time(&store,1800000000,1100,1));
    char json[160];assert(panel_alarm_json(panel_alarm_front(&store),json,sizeof(json))>0);
    assert(strstr(json,"\"v\":1") && strstr(json,"\"code\":17920") && strstr(json,"\"level\":2") && strstr(json,"\"state\":1"));
    FILE *f=fopen("tmp/alarm_contract.jsonl","w");assert(f);
    for(unsigned index=0;index<PANEL_ALARM_SLOTS;index++)for(unsigned active=0;active<2;active++){
        bool system=index>=64 && index<PANEL_ALARM_LEGACY_SLOTS;
        bool is_3ku=index>=PANEL_ALARM_LEGACY_SLOTS;
        unsigned offset=is_3ku?index-PANEL_ALARM_LEGACY_SLOTS:index;
        unsigned code=system?PANEL_ALARM_COMM+index-64:0x4600+(offset/16)*256+offset%16;
        if(is_3ku)code+=PANEL_ALARM_3KU_OFFSET;
        panel_alarm_event_t wire={.serial=index+1,.epoch=1800000000,.code=(uint16_t)code,
            .source=(uint8_t)(system?2:0),.flags=(uint8_t)(active|((offset<32 || index==64)?2:0))};
        assert(panel_alarm_json(&wire,json,sizeof(json))>0);fprintf(f,"%s\n",json);
    }
    fclose(f);
    panel_alarm_seal(&store);assert(panel_alarm_validate(&store));
    uint32_t serial=e->serial;assert(!panel_alarm_pop(&store,serial+1));assert(panel_alarm_pop(&store,serial));
    panel_alarm_store_t saved=store;panel_alarm_seal(&saved);assert(panel_alarm_validate(&saved));
    saved.events[saved.head].code^=1;assert(!panel_alarm_validate(&saved));
    /* No telemetry samples, voltage/current or model are needed to collect. */
    sample.fresh=0;sample.reachable=false;
    assert(panel_alarm_observe(&store,&sample,true));unsigned pending=store.count;
    assert(store.words[0]==0x8001 && store.words[1]==0x8000 && store.words[3]==0x8000);
    sample.paused=true;assert(!panel_alarm_observe(&store,&sample,true) && store.count==pending);
    sample.paused=false;sample.fresh=15;sample.reachable=true;memset(sample.words,0,sizeof(sample.words));
    assert(panel_alarm_observe(&store,&sample,true));assert(store.words[0]==0);
    bool recovered=false;
    for(unsigned i=0;i<store.count;i++){const panel_alarm_event_t *x=&store.events[(store.head+i)%128];if(x->code==0x460f && !(x->flags&1))recovered=true;}
    assert(recovered);
    reset();sample.words[0]=1;
    assert(panel_alarm_observe(&store,&sample,true));
    sample.is_3ku=true;
    assert(panel_alarm_observe(&store,&sample,true));
    assert(store.count==2 && store.events[0].code==0x4600 && store.events[1].code==0x5600);
    sample.fresh=0;sample.words[0]=0;
    panel_alarm_observe(&store,&sample,true);
    assert(store.words_3ku[0]==1);
    sample.fresh=15;
    assert(panel_alarm_observe(&store,&sample,true));
    assert(store.words[0]==1 && store.words_3ku[0]==0);
    /* Old queue bytes have the same prefix and original CRC. */
    reset();sample.words[1]=1;panel_alarm_observe(&store,&sample,false);
    const size_t prefix=offsetof(panel_alarm_store_t,legacy_end);
    store.magic=0x4d424101U;
    uint16_t crc=panel_mb_crc((const uint8_t *)&store,prefix);
    memcpy((uint8_t *)&store+prefix,&crc,sizeof(crc));
    assert(!panel_alarm_restore(&store,prefix+1));
    assert(panel_alarm_restore(&store,prefix+sizeof(crc)) && panel_alarm_validate(&store));
    assert(store.count==1 && store.words[1]==1 && !store.words_3ku[1]);
    /* Known timestamps and pending events survive a restart unchanged. */
    reset();sample.words[2]=1;panel_alarm_observe(&store,&sample,false);
    uint32_t boot=store.next_serial;assert(panel_alarm_assign_time(&store,1800000000,1000,boot));
    assert(panel_alarm_front(&store)->flags&0x80);
    uint32_t timestamp=panel_alarm_front(&store)->epoch;serial=panel_alarm_front(&store)->serial;
    assert(!panel_alarm_assign_time(&store,1800000001,2000,boot));
    panel_alarm_pop(&store,serial);assert(panel_alarm_resync(&store,2100));
    assert(panel_alarm_front(&store)->epoch==timestamp); /* Broker reconnect replay is idempotent. */
    reset();for(unsigned i=0;i<150;i++){sample.words[0]^=1;panel_alarm_observe(&store,&sample,true);}
    assert(store.count<=128 && store.loss_count>0 && (store.sys_active&2));
    panel_alarm_seal(&store);assert(panel_alarm_validate(&store));
    puts("PASS alarms: first active/high bits, independent fresh words, missing never clears, active/recovered per bit, persistent CRC/queue, timestamp gate, reconnect replay and explicit overflow");return 0;
}
