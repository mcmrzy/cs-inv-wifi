#include "factory_record.h"
#include <string.h>

#define RECORD_MAGIC 0x31544346u /* bytes F C T 1 */
#define HEADER_SIZE 20u

static void put32(uint8_t *p, uint32_t n) {
    p[0]=(uint8_t)n; p[1]=(uint8_t)(n>>8); p[2]=(uint8_t)(n>>16); p[3]=(uint8_t)(n>>24);
}
static uint32_t get32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1]<<8 | (uint32_t)p[2]<<16 | (uint32_t)p[3]<<24;
}
static uint32_t crc32(const uint8_t *p, size_t n) {
    uint32_t crc=0xffffffffu;
    while(n--) {
        crc ^= *p++;
        for(unsigned i=0;i<8;i++) crc=(crc>>1) ^ (0xedb88320u & (0u-(crc&1u)));
    }
    return ~crc;
}
static bool valid_string(const char *p, size_t capacity, bool required, bool identifier) {
    size_t n=0;
    while(n<capacity && p[n]) {
        unsigned char c=(unsigned char)p[n++];
        if(c<32 || c>126) return false;
        if(identifier && !((c>='A' && c<='Z') || (c>='a' && c<='z') ||
             (c>='0' && c<='9') || c=='-' || c=='_' || c=='.')) return false;
    }
    return n<capacity && (!required || n>0);
}
static unsigned decimal(const char *p, unsigned n) {
    unsigned v=0;
    while(n--) { if(*p<'0' || *p>'9') return 99999; v=v*10+(unsigned)(*p++-'0'); }
    return v;
}
static bool valid_time(const char *p) {
    if(strlen(p)!=20 || p[4]!='-' || p[7]!='-' || p[10]!='T' ||
       p[13]!=':' || p[16]!=':' || p[19]!='Z') return false;
    unsigned y=decimal(p,4), m=decimal(p+5,2), d=decimal(p+8,2);
    static const unsigned days[]={31,28,31,30,31,30,31,31,30,31,30,31};
    if(y<2000 || y>9999 || m<1 || m>12) return false;
    unsigned limit=days[m-1]+(m==2 && (y%4==0 && (y%100!=0 || y%400==0)));
    return d>=1 && d<=limit && decimal(p+11,2)<24 &&
           decimal(p+14,2)<60 && decimal(p+17,2)<60;
}
#define LEGACY_STRING_FIELDS(X) \
 X(sn,true,true) X(product,true,false) X(model,true,false) \
 X(hw_system,true,false) X(hw_controller,true,false) X(hw_dsp,false,false) \
 X(hw_bms,false,false) X(batch,true,false) X(production_time,true,false) \
 X(calibration_version,false,false) X(calibration_summary,false,false)
#define EXTENSION_STRING_FIELDS(X) \
 X(controller_type,false,false) X(controller_code,false,false) X(controller_sn,false,false) \
 X(dsp_type,false,false) X(dsp_code,false,false) X(dsp_sn,false,false) \
 X(bms_type,false,false) X(bms_code,false,false) X(bms_sn,false,false) \
 X(power_type,false,false) X(power_code,false,false) X(power_sn,false,false) \
 X(hw_power,false,false) X(work_order,false,false) X(station_id,false,false)
#define STRING_FIELDS(X) LEGACY_STRING_FIELDS(X) EXTENSION_STRING_FIELDS(X)

factory_result_t factory_record_validate(const factory_record_t *r) {
    if(!r) return FACTORY_INVALID_ARGUMENT;
    uint8_t any=0, all=0xff;
    for(unsigned i=0;i<6;i++) { any|=r->mac[i]; all&=r->mac[i]; }
    if(!any || all==0xff || (r->mac[0]&1)) return FACTORY_INVALID_RECORD;
#define CHECK(field,required,identifier) if(!valid_string(r->field,sizeof(r->field),required,identifier)) return FACTORY_INVALID_RECORD;
    STRING_FIELDS(CHECK)
#undef CHECK
    if(!valid_time(r->production_time)) return FACTORY_INVALID_RECORD;
    return FACTORY_OK;
}
factory_result_t factory_record_encode(const factory_record_t *r, uint8_t *wire, size_t n) {
    if(!wire || (n!=FACTORY_RECORD_WIRE_SIZE && n!=FACTORY_RECORD_WIRE_SIZE_LEGACY)) return FACTORY_INVALID_ARGUMENT;
    factory_result_t result=factory_record_validate(r);
    if(result!=FACTORY_OK) return result;
    if(!r->generation) return FACTORY_INVALID_RECORD;
    bool legacy=n==FACTORY_RECORD_WIRE_SIZE_LEGACY;
#define EMPTY(field,required,identifier) if(r->field[0]) return FACTORY_INVALID_RECORD;
    if(legacy) { EXTENSION_STRING_FIELDS(EMPTY) }
#undef EMPTY
    memset(wire,0,n);
    put32(wire,RECORD_MAGIC);
    wire[4]=legacy ? FACTORY_RECORD_SCHEMA_LEGACY : FACTORY_RECORD_SCHEMA;
    wire[6]=(uint8_t)n; wire[7]=(uint8_t)(n>>8);
    put32(wire+8,r->generation); wire[12]=r->locked ? 1 : 0;
    memcpy(wire+14,r->mac,6);
    size_t offset=HEADER_SIZE;
#define ENCODE(field,required,identifier) memcpy(wire+offset,r->field,strlen(r->field)); offset+=sizeof(r->field);
    LEGACY_STRING_FIELDS(ENCODE)
    if(!legacy) { EXTENSION_STRING_FIELDS(ENCODE) }
#undef ENCODE
    if(offset!=n-4) return FACTORY_INVALID_RECORD;
    put32(wire+n-4,crc32(wire,n-4));
    return FACTORY_OK;
}
factory_result_t factory_record_decode(const uint8_t *wire, size_t n, factory_record_t *r) {
    if(!r || !wire) return FACTORY_INVALID_ARGUMENT;
    /* Check size first, before accessing any byte of a partial slot. */
    if(n!=FACTORY_RECORD_WIRE_SIZE && n!=FACTORY_RECORD_WIRE_SIZE_LEGACY) return FACTORY_CORRUPT;
    bool legacy=n==FACTORY_RECORD_WIRE_SIZE_LEGACY;
    if(get32(wire)!=RECORD_MAGIC ||
       wire[4]!=(legacy ? FACTORY_RECORD_SCHEMA_LEGACY : FACTORY_RECORD_SCHEMA) || wire[5]!=0 || wire[6]!=(uint8_t)n ||
       wire[7]!=(uint8_t)(n>>8) || wire[12]>1 || wire[13]!=0 ||
       get32(wire+n-4)!=crc32(wire,n-4)) return FACTORY_CORRUPT;
    factory_record_t candidate={0};
    candidate.schema=wire[4];
    candidate.generation=get32(wire+8); candidate.locked=wire[12]!=0;
    if(!candidate.generation) return FACTORY_CORRUPT;
    memcpy(candidate.mac,wire+14,6);
    size_t offset=HEADER_SIZE;
#define DECODE(field,required,identifier) \
    do { \
        size_t capacity=sizeof(candidate.field); \
        const uint8_t *zero=memchr(wire+offset,0,capacity); \
        if(!zero)return FACTORY_CORRUPT; \
        for(const uint8_t *p=zero;p<wire+offset+capacity;p++)if(*p)return FACTORY_CORRUPT; \
        memcpy(candidate.field,wire+offset,capacity);offset+=capacity; \
    }while(0);
    LEGACY_STRING_FIELDS(DECODE)
    if(!legacy) { EXTENSION_STRING_FIELDS(DECODE) }
#undef DECODE
    if(factory_record_validate(&candidate)!=FACTORY_OK) return FACTORY_CORRUPT;
    /* Header/reserved bytes and each string's zero padding were checked above.
     * Avoid a second full-size canonical buffer on the small task stacks. */
    *r=candidate;
    return FACTORY_OK;
}
bool factory_record_same_identity(const factory_record_t *a, const factory_record_t *b) {
    if(!a || !b || factory_record_validate(a)!=FACTORY_OK || factory_record_validate(b)!=FACTORY_OK) return false;
    if(memcmp(a->mac,b->mac,6)) return false;
#define SAME(field,required,identifier) if(strcmp(a->field,b->field)) return false;
    STRING_FIELDS(SAME)
#undef SAME
    return true;
}
factory_result_t factory_record_select(const uint8_t *s0, size_t n0, const uint8_t *s1,
                                       size_t n1, factory_record_t *out, int *selected) {
    if(!out || !selected) return FACTORY_INVALID_ARGUMENT;
    factory_record_t r1;
    bool valid0=s0 && factory_record_decode(s0,n0,out)==FACTORY_OK;
    bool valid1=s1 && factory_record_decode(s1,n1,&r1)==FACTORY_OK;
    if(!valid0 && !valid1) return (!s0 && !s1) ? FACTORY_UNPROVISIONED : FACTORY_CORRUPT;
    int index=valid1 && (!valid0 || r1.generation>out->generation) ? 1 : 0;
    if(valid0 && valid1 && out->generation==r1.generation &&
       (out->locked!=r1.locked || !factory_record_same_identity(out,&r1))) return FACTORY_CORRUPT;
    if(index)*out=r1;
    *selected=index;
    return FACTORY_OK;
}
const char *factory_result_name(factory_result_t result) {
    switch(result) {
    case FACTORY_OK:return "OK";
    case FACTORY_UNPROVISIONED:return "UNPROVISIONED";
    case FACTORY_INVALID_ARGUMENT:return "INVALID_ARGUMENT";
    case FACTORY_INVALID_RECORD:return "INVALID_RECORD";
    case FACTORY_CORRUPT:return "CORRUPT";
    case FACTORY_MAC_MISMATCH:return "MAC_MISMATCH";
    case FACTORY_LOCKED:return "LOCKED";
    case FACTORY_GENERATION_EXHAUSTED:return "GENERATION_EXHAUSTED";
    case FACTORY_NOT_INITIALIZED:return "NOT_INITIALIZED";
    case FACTORY_STORAGE_ERROR:return "STORAGE_ERROR";
    case FACTORY_READBACK_ERROR:return "READBACK_ERROR";
    case FACTORY_BUSY:return "BUSY";
    default:return "UNKNOWN";
    }
}
