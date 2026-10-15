#include "direct_dsp_ota_transport.h"
#include "direct_protocol.h"
#include <stdlib.h>
#include <string.h>

static const uint8_t upgrade[]={1,5,0x42,0,0,0,0,0x73,0x9a};
static const uint8_t erase[]={1,0x10,4,0x38,0,2,4,0,0,0,0,0xc3,0xdd};
static const uint8_t erase_ack[]={1,0x10,4,0x38,0,2,4,0,0,0,0x11,3,0xd1};
static const uint8_t eot[]={2,0,0xff,0x45,0x4f,0x54};

static bool cancelled(const direct_iap_io_t *io){
    return io->cancelled && io->cancelled(io->context);
}
static bool valid_io(const direct_iap_io_t *io){
    return io && io->send && io->receive && io->delay && io->now_ms;
}
static direct_iap_result_t transmit(const direct_iap_io_t *io,const uint8_t *bytes,size_t size){
    if(cancelled(io))return DIRECT_IAP_CANCELLED;
    if(io->flush)io->flush(io->context);
    return io->send(io->context,bytes,size)?DIRECT_IAP_OK:DIRECT_IAP_TX_FAILED;
}
/* 重试只重发, 不重新冲接收缓冲: HR 可能已经躺在里面, 一冲就丢(现场 rx=0 的一种
 * 可能来源)。只有一段会话的首帧才需要冲掉上一次业务留下的残帧。 */
static direct_iap_result_t transmit_raw(const direct_iap_io_t *io,const uint8_t *bytes,size_t size){
    if(cancelled(io))return DIRECT_IAP_CANCELLED;
    return io->send(io->context,bytes,size)?DIRECT_IAP_OK:DIRECT_IAP_TX_FAILED;
}

/* Read one byte per call so fragmented replies never restart the full timeout.
 * Each receive waits at most 25ms. The owner updates its health heartbeat and
 * cancellation is bounded even during a 12-second erase wait. Extra HRs/noise
 * are scanned rather than mistaken for the next CR/erase acknowledgement. */
static direct_iap_result_t wait_reply(const direct_iap_io_t *io,const uint8_t *expected,
                                      size_t size,unsigned timeout_ms){
    size_t matched=0;
    uint32_t start=io->now_ms(io->context);
    for(;;){
        if(cancelled(io))return DIRECT_IAP_CANCELLED;
        unsigned elapsed=(uint32_t)(io->now_ms(io->context)-start);
        if(elapsed>=timeout_ms)break;
        unsigned remaining=timeout_ms-elapsed;
        unsigned slice=remaining<25?remaining:25;
        uint8_t byte;
        size_t got=io->receive(io->context,&byte,1,slice);
        if(got>1)return DIRECT_IAP_ACK_FAILED;
        if(!got)continue;
        if(byte==expected[matched]){
            if(++matched==size)return DIRECT_IAP_OK;
        }else matched=byte==expected[0]?1:0;
    }
    return DIRECT_IAP_ACK_FAILED;
}

static direct_iap_result_t exchange(const direct_iap_io_t *io,const uint8_t *command,
                                    size_t command_size,const uint8_t *reply,size_t reply_size,
                                    unsigned timeout,unsigned attempts){
    for(unsigned attempt=0;attempt<attempts;attempt++){
        direct_iap_result_t result = attempt
            ? transmit_raw(io,command,command_size)
            : transmit(io,command,command_size);
        if(result!=DIRECT_IAP_OK)return result;
        result=wait_reply(io,reply,reply_size,timeout);
        if(result!=DIRECT_IAP_ACK_FAILED || attempt+1==attempts)return result;
        io->delay(io->context,100);
    }
    return DIRECT_IAP_ACK_FAILED;
}

direct_iap_result_t direct_dsp_ota_enter_boot(const direct_iap_io_t *io){
    if(!valid_io(io))return DIRECT_IAP_INCOMPATIBLE;
    if(io->phase)io->phase(io->context,DIRECT_IAP_PHASE_HR);
    /* 12*250ms receive + 11*100ms retry delay = 4100ms, plus TX time.
     * This budget does not extend the target's boot window. */
    return exchange(io,upgrade,sizeof(upgrade),(const uint8_t *)"HR",2,250,12);
}
direct_iap_result_t direct_dsp_ota_exit_boot(const direct_iap_io_t *io){
    if(!valid_io(io))return DIRECT_IAP_INCOMPATIBLE;
    if(io->phase)io->phase(io->context,DIRECT_IAP_PHASE_EOT);
    direct_iap_result_t result=transmit(io,eot,sizeof(eot));
    if(result==DIRECT_IAP_OK)io->delay(io->context,1000);
    return result;
}

direct_iap_result_t direct_dsp_ota_validate(size_t size,const direct_iap_io_t *io){
    if(!io || !io->read_image)return DIRECT_IAP_INCOMPATIBLE;
    if(size<64 || size%2 || size>DIRECT_DSP_OTA_MAX_SIZE)
        return DIRECT_IAP_IMAGE_SIZE_INVALID;
    uint8_t header[6];
    if(!io->read_image(io->context,0,header,sizeof(header)))return DIRECT_IAP_READ_FAILED;
    uint16_t word=(uint16_t)((uint16_t)header[0]<<8|header[1]);
    if((word&0xfffcU)!=0x0048U)return DIRECT_IAP_IMAGE_OPCODE_INVALID;
    if(header[4]!=0x5a || header[5]!=0x5a)return DIRECT_IAP_IMAGE_MAGIC_INVALID;
    return DIRECT_IAP_OK;
}
direct_iap_result_t direct_dsp_ota_flash(size_t size,const direct_iap_io_t *io){
    if(!valid_io(io))return DIRECT_IAP_INCOMPATIBLE;
    direct_iap_result_t result=direct_dsp_ota_validate(size,io);
    if(result!=DIRECT_IAP_OK)return result;
    /* Dedicated DSP packet uses 1029 bytes; avoid the legacy shared 8KB BMS
     * workspace and avoid adding a 1KB array to the UART owner's call stack. */
    uint8_t *packet=malloc(DIRECT_DSP_OTA_PACKET_SIZE+5U);
    if(!packet)return DIRECT_IAP_NO_MEMORY;
    result=direct_dsp_ota_enter_boot(io);
    if(result!=DIRECT_IAP_OK)goto done;
    for(unsigned attempt=0;attempt<5;attempt++){
        if(io->phase)io->phase(io->context,DIRECT_IAP_PHASE_ERASE_ACK);
        result = attempt?transmit_raw(io,erase,sizeof(erase)):transmit(io,erase,sizeof(erase));
        if(result!=DIRECT_IAP_OK)goto done;
        result=wait_reply(io,erase_ack,sizeof(erase_ack),3000);
        if(result==DIRECT_IAP_CANCELLED)goto done;
        if(result!=DIRECT_IAP_OK)continue;
        if(io->phase)io->phase(io->context,DIRECT_IAP_PHASE_ERASE_CR);
        result=wait_reply(io,(const uint8_t *)"CR",2,12000);
        if(result!=DIRECT_IAP_ACK_FAILED)break;
    }
    if(result!=DIRECT_IAP_OK)goto done;
    for(size_t offset=0,index=0;offset<size;index++){
        if(cancelled(io)){result=DIRECT_IAP_CANCELLED;goto done;}
        size_t bytes=size-offset;
        if(bytes>DIRECT_DSP_OTA_PACKET_SIZE)bytes=DIRECT_DSP_OTA_PACKET_SIZE;
        uint16_t address=(uint16_t)(0x01feU+0x00ffU*index);
        if(address==0x00ffU){result=DIRECT_IAP_INCOMPATIBLE;goto done;}
        packet[0]=2;packet[1]=(uint8_t)(address>>8);packet[2]=(uint8_t)address;
        if(!io->read_image(io->context,offset,packet+3,bytes)){
            result=DIRECT_IAP_READ_FAILED;goto done;
        }
        uint16_t crc=direct_crc16(packet,bytes+3U);
        packet[bytes+3U]=(uint8_t)(crc>>8);
        packet[bytes+4U]=(uint8_t)crc;
        if(io->phase)io->phase(io->context,DIRECT_IAP_PHASE_DATA_ACK);
        result=exchange(io,packet,bytes+5U,(const uint8_t *)"CR",2,3000,10);
        if(result!=DIRECT_IAP_OK)goto done;
        offset+=bytes;
        if(io->progress)io->progress(io->context,offset,size);
    }
    result=direct_dsp_ota_exit_boot(io);
done:
    free(packet);return result;
}
