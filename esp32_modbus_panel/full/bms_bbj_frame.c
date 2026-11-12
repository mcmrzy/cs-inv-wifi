#include "bms_bbj_frame.h"
#include "direct_protocol.h"

bool bms_bbj_find_response(const unsigned char *bytes,size_t count,
                           size_t *offset,size_t *length)
{
    if(!bytes || !offset || !length)return false;
    for(size_t i=0;i<count;i++){
        if(count-i<7)break;
        const uint8_t *frame=bytes+i;
        if(frame[0]!=0x7c || frame[3]!=0x80 || frame[4]!=1)continue;
        size_t n=7U+(size_t)(frame[5]|((uint16_t)frame[6]<<8));
        /* Strict summary validation also checks the CRC, cell/temperature
         * counts, full pack_info and scaled value ranges. The seven-byte
         * query echo has no pack_info and cannot pass this check. */
        if(n>256 || n>count-i)continue;
        uint8_t summary[DIRECT_BMS_SUMMARY_SIZE];
        if(!direct_bms_encode_summary_7c(frame,n,summary))continue;
        *offset=i;*length=n;return true;
    }
    return false;
}
