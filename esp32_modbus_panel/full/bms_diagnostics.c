#include "bms_diagnostics.h"
#include "direct_protocol.h"
#include "bms_bbj_frame.h"
#include <string.h>

static uint16_t le16(const uint8_t *p){return (uint16_t)(p[0]|((uint16_t)p[1]<<8));}
static bms_diag_result_t inspect(const uint8_t *f,size_t n)
{
    bms_diag_result_t r={.code=BMS_DIAG_NO_RX};
    if(!f || !n)return r;
    if(n<7){r.code=BMS_DIAG_SHORT_HEADER;return r;}
    r.expected_length=7U+(uint32_t)le16(f+5);
    r.wire_crc=le16(f+1);r.calculated_crc=direct_crc16(f+3,n-3);
    if(f[0]!=0x7c)r.code=BMS_DIAG_BAD_HEADER;
    else if(f[3]!=0x80)r.code=BMS_DIAG_BAD_ADDRESS;
    else if(f[4]!=1)r.code=BMS_DIAG_BAD_FUNCTION;
    else if(n!=r.expected_length)r.code=BMS_DIAG_LENGTH_MISMATCH;
    else if(r.wire_crc!=r.calculated_crc)r.code=BMS_DIAG_BAD_CRC;
    else if(n<8 || !f[7] || f[7]>16)r.code=BMS_DIAG_BAD_CELLS;
    else{
        size_t temps=8U+2U*f[7];
        if(temps>=n)r.code=BMS_DIAG_SHORT_PACK;
        else if(f[temps]>16)r.code=BMS_DIAG_BAD_TEMPS;
        else if(temps+1U+2U*f[temps]+61U>n)r.code=BMS_DIAG_SHORT_PACK;
        else{
            uint8_t summary[DIRECT_BMS_SUMMARY_SIZE];
            r.code=direct_bms_encode_summary_7c(f,n,summary)?BMS_DIAG_VALID:BMS_DIAG_BAD_VALUES;
        }
    }
    return r;
}
bms_diag_result_t bms_diag_inspect(const uint8_t *request,size_t request_length,
                                 const uint8_t *received,size_t received_length)
{
    bms_diag_result_t r=inspect(received,received_length);
    if(!received || !received_length)return r;
    if(request && request_length && request_length==received_length && !memcmp(request,received,request_length)){
        r.code=BMS_DIAG_ECHO_ONLY;return r;
    }
    if(r.code==BMS_DIAG_VALID){r.candidate_length=(uint16_t)received_length;return r;}
    size_t offset,length;
    if(bms_bbj_find_response(received,received_length,&offset,&length)){
        r.code=BMS_DIAG_FRAME_IN_BUFFER;r.candidate_offset=(uint16_t)offset;
        r.candidate_length=(uint16_t)length;
    }
    return r;
}
const char *bms_diag_reason(bms_diag_code_t code)
{
    static const char *const names[]={"NO_RX","ECHO_ONLY","SHORT_HEADER","BAD_HEADER",
        "BAD_ADDRESS","BAD_FUNCTION","LENGTH_MISMATCH","BAD_CRC","BAD_CELLS",
        "BAD_TEMPS","SHORT_PACK","BAD_VALUES","VALID","FRAME_IN_BUFFER"};
    return (unsigned)code<sizeof(names)/sizeof(*names)?names[code]:"UNKNOWN";
}
