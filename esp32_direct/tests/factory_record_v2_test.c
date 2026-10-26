#include "factory_store.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static uint8_t slots[2][736];
static size_t sizes[2];
static bool cut_write;
static void refresh_crc(uint8_t *wire,size_t size) {
    uint32_t crc=0xffffffffu;
    for(size_t j=0;j<size-4;j++) {
        crc^=wire[j];
        for(unsigned bit=0;bit<8;bit++) crc=(crc>>1)^(0xedb88320u&(0u-(crc&1u)));
    }
    crc=~crc;
    for(unsigned i=0;i<4;i++) wire[size-4+i]=(uint8_t)(crc>>(8*i));
}
static bool read_slot(void *ctx,int slot,uint8_t *out,size_t *size) {
    (void)ctx;
    if(*size<sizes[slot]) return false;
    memcpy(out,slots[slot],sizes[slot]); *size=sizes[slot]; return true;
}
static bool write_slot(void *ctx,int slot,const uint8_t *in,size_t size) {
    (void)ctx;
    sizes[slot]=cut_write ? 97 : size;
    memcpy(slots[slot],in,sizes[slot]); return !cut_write;
}
static factory_record_t sample(void) {
    factory_record_t r={0};
    uint8_t mac[]={2,3,4,5,6,7}; memcpy(r.mac,mac,6);
    r.generation=7;
    strcpy(r.sn,"H1ZZX0013900001P"); strcpy(r.product,"CSERGY");
    strcpy(r.model,"CS-L10-6K2"); strcpy(r.hw_system,"1.0");
    strcpy(r.hw_controller,"1.1"); strcpy(r.batch,"test");
    strcpy(r.production_time,"2026-10-12T00:00:00Z"); return r;
}
int main(int argc,char **argv) {
    if(argc==3) {
        uint8_t wire[736],encoded[736]; factory_record_t r;
        FILE *in=fopen(argv[1],"rb"); assert(in);
        size_t n=fread(wire,1,sizeof(wire),in); assert(feof(in)||n==sizeof(wire)); fclose(in);
        assert(factory_record_decode(wire,n,&r)==FACTORY_OK);
        assert(factory_record_encode(&r,encoded,n)==FACTORY_OK);
        FILE *out=fopen(argv[2],"wb"); assert(out);
        assert(fwrite(encoded,1,n,out)==n); fclose(out); return 0;
    }
    assert(FACTORY_RECORD_WIRE_SIZE==736);
    factory_record_t r=sample(),got; uint8_t legacy[336],modern[736]; int selected;
    assert(factory_record_encode(&r,legacy,sizeof(legacy))==FACTORY_OK);
    assert(factory_record_encode(&r,modern,sizeof(modern))==FACTORY_OK);
    assert(factory_record_decode(legacy,sizeof(legacy),&got)==FACTORY_OK);
    assert(got.schema==1 && !got.controller_type[0]);
    assert(factory_record_select(legacy,sizeof(legacy),modern,sizeof(modern),&got,&selected)==FACTORY_OK);
    assert(got.generation==7);
    r.locked=true; assert(factory_record_encode(&r,modern,sizeof(modern))==FACTORY_OK);
    assert(factory_record_select(legacy,sizeof(legacy),modern,sizeof(modern),&got,&selected)==FACTORY_CORRUPT);
    r.locked=false; strcpy(r.controller_type,"controller"); strcpy(r.controller_code,"CTRL-001");
    strcpy(r.controller_sn,"CTRL000001"); strcpy(r.dsp_type,"DSP"); strcpy(r.dsp_code,"DSP-002");
    strcpy(r.dsp_sn,"DSP000001"); strcpy(r.bms_type,"BMS"); strcpy(r.bms_code,"BMS-003");
    strcpy(r.bms_sn,"BMS000001"); strcpy(r.power_type,"power"); strcpy(r.power_code,"PWR-004");
    strcpy(r.power_sn,"PWR000001"); strcpy(r.hw_power,"1.2");
    strcpy(r.work_order,"WO-005"); strcpy(r.station_id,"ST-006");
    assert(factory_record_encode(&r,legacy,sizeof(legacy))==FACTORY_INVALID_RECORD);
    assert(factory_record_encode(&r,modern,sizeof(modern))==FACTORY_OK);
    assert(factory_record_decode(modern,sizeof(modern),&got)==FACTORY_OK && got.schema==2);
    assert(factory_record_same_identity(&r,&got));
    assert(factory_record_select(legacy,sizeof(legacy),modern,sizeof(modern),&got,&selected)==FACTORY_CORRUPT);
    modern[700]^=1;
    assert(factory_record_select(legacy,sizeof(legacy),modern,sizeof(modern),&got,&selected)==FACTORY_OK && selected==0);
    assert(factory_record_select(legacy,sizeof(legacy),modern,19,&got,&selected)==FACTORY_OK && selected==0);
    assert(factory_record_encode(&r,modern,sizeof(modern))==FACTORY_OK);
    modern[20+strlen(r.sn)+1]=1; refresh_crc(modern,sizeof(modern));
    assert(factory_record_decode(modern,sizeof(modern),&got)==FACTORY_CORRUPT);
    assert(factory_record_encode(&r,modern,sizeof(modern))==FACTORY_OK);
    modern[4]=1; refresh_crc(modern,sizeof(modern));
    assert(factory_record_decode(modern,sizeof(modern),&got)==FACTORY_CORRUPT);
    factory_store_io_t io={read_slot,write_slot,NULL};
    memcpy(slots[0],legacy,sizeof(legacy)); sizes[0]=sizeof(legacy);
    r=sample();
    assert(factory_store_read(&io,r.mac,&got,NULL)==FACTORY_OK && got.schema==1);
    cut_write=true;
    assert(factory_store_provision(&io,r.mac,&r)==FACTORY_STORAGE_ERROR);
    assert(factory_store_read(&io,r.mac,&got,NULL)==FACTORY_OK && got.schema==1);
    cut_write=false;
    assert(factory_store_provision(&io,r.mac,&r)==FACTORY_OK);
    assert(factory_store_read(&io,r.mac,&got,NULL)==FACTORY_OK && got.schema==2 && got.generation==8);
    assert(factory_store_provision(&io,r.mac,&r)==FACTORY_OK);
    assert(factory_store_read(&io,r.mac,&got,NULL)==FACTORY_OK && got.generation==8);
    assert(factory_store_lock(&io,r.mac)==FACTORY_OK);
    strcpy(r.work_order,"NEW"); assert(factory_store_provision(&io,r.mac,&r)==FACTORY_LOCKED);
    sizes[1]=0; r=sample(); r.locked=true;
    assert(factory_record_encode(&r,slots[0],336)==FACTORY_OK); sizes[0]=336;
    r.locked=false;
    assert(factory_store_provision(&io,r.mac,&r)==FACTORY_OK);
    assert(factory_store_read(&io,r.mac,&got,NULL)==FACTORY_OK && got.schema==1 && got.locked);
    strcpy(r.work_order,"NEW"); assert(factory_store_provision(&io,r.mac,&r)==FACTORY_LOCKED);
    /* Legacy SN remains readable even though new host writes are stricter. */
    r=sample(); strcpy(r.sn,"legacy-SN_1");
    assert(factory_record_encode(&r,legacy,sizeof(legacy))==FACTORY_OK);
    assert(factory_record_decode(legacy,sizeof(legacy),&got)==FACTORY_OK);
    puts("PASS schema1/schema2 canonical compatibility, conflicts, interrupted migration and sticky locks");
    return 0;
}
