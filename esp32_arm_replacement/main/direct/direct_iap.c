/* Direct target wire protocol, extracted from original Boot Collector/Update.c
 * and BmsUpdate.c. No Collector PULL wrapper/ARM or UART driver dependencies. */
#include "direct_iap.h"
#include "direct_protocol.h"
#include <stdlib.h>
#include <string.h>
typedef struct {uint8_t data[4099],tx[4106],rx[15];} workspace_t;
static const uint8_t dsp_start[]={1,5,0x42,0,0,0,0,0x73,0x9a};
static const uint8_t dsp_erase[]={1,0x10,4,0x38,0,2,4,0,0,0,0,0xc3,0xdd};
static const uint8_t dsp_erase_ack[]={1,0x10,4,0x38,0,2,4,0,0,0,0x11,3,0xd1};
static const uint8_t dsp_stop[]={2,0,0xff,0x45,0x4f,0x54};
static bool cancelled(const direct_iap_io_t*io){return io->cancelled&&io->cancelled(io->context);}
static bool exact(const direct_iap_io_t*io,uint8_t*p,size_t n,unsigned ms){
 /* Receiver callback must respect a bounded timeout and deliver at most n bytes. */
 size_t have=0;unsigned reads=0;
 while(have<n&&reads++<16){size_t got=io->receive(io->context,p+have,n-have,ms);if(!got||got>n-have)return false;have+=got;}
 return have==n;
}
static bool send(const direct_iap_io_t*io,const uint8_t*p,size_t n){if(io->flush)io->flush(io->context);return io->send(io->context,p,n);}
static bool read_raw(const direct_iap_io_t*io,size_t raw_size,size_t offset,uint8_t*out,size_t n){
 size_t available=offset<raw_size?raw_size-offset:0;if(available>n)available=n;
 memset(out,0,n);return !available||io->read_image(io->context,offset,out,available);
}
static void progress(const direct_iap_io_t*io,size_t offset,size_t raw_size){
 if(io->progress)io->progress(io->context,offset>raw_size?raw_size:offset,raw_size);
}
static direct_iap_result_t dsp(const direct_iap_image_t*m,const direct_iap_io_t*io,workspace_t*w,size_t raw_size,bool legacy){
 direct_iap_result_t result=DIRECT_IAP_ACK_FAILED;bool started=false;
 bool acknowledged=false;
 for(unsigned attempt=0;attempt<(legacy?5U:1U);++attempt){
  if(cancelled(io)){result=DIRECT_IAP_CANCELLED;goto done;}
  if(!send(io,dsp_start,sizeof(dsp_start))){result=DIRECT_IAP_TX_FAILED;goto done;}
  started=true;
  if(exact(io,w->rx,2,2000)&&!memcmp(w->rx,"HR",2)){acknowledged=true;break;}
  if(legacy)io->delay(io->context,100);
 }
 if(!acknowledged)goto done;
 if(cancelled(io)){result=DIRECT_IAP_CANCELLED;goto done;}
 acknowledged=false;
 for(unsigned attempt=0;attempt<(legacy?5U:1U);++attempt){
  if(cancelled(io)){result=DIRECT_IAP_CANCELLED;goto done;}
  if(!send(io,dsp_erase,sizeof(dsp_erase))){result=DIRECT_IAP_TX_FAILED;goto done;}
  if(exact(io,w->rx,13,2000)&&!memcmp(w->rx,dsp_erase_ack,13)){acknowledged=true;break;}
  if(legacy)io->delay(io->context,100);
 }
 if(!acknowledged)goto done;
 if(!exact(io,w->rx,2,12000)||memcmp(w->rx,"CR",2))goto done;
 for(size_t offset=0,index=0;offset<m->size;index++){
  if(cancelled(io)){result=DIRECT_IAP_CANCELLED;goto done;}
  size_t n=m->size-offset;if(n>1024)n=1024;
  if(!read_raw(io,raw_size,offset,w->tx+3,n)){result=DIRECT_IAP_READ_FAILED;goto done;}
  /* Original DSP word address stride is 0x00FF (not byte offset/1024). */
  uint32_t address=0x01fe + 0x00ff*index;if(!legacy&&address>0xffff){result=DIRECT_IAP_INCOMPATIBLE;goto done;}
  /* ARM Update.c stores the expression in u16 wDSPAddr. */
  address=(uint16_t)address;
  w->tx[0]=2;w->tx[1]=(uint8_t)(address>>8);w->tx[2]=(uint8_t)address;
  uint16_t crc=direct_crc16(w->tx,n+3);w->tx[n+3]=(uint8_t)(crc>>8);w->tx[n+4]=(uint8_t)crc;
  if(!send(io,w->tx,n+5)){result=DIRECT_IAP_TX_FAILED;goto done;}
  if(!exact(io,w->rx,2,1200)||memcmp(w->rx,"CR",2))goto done;
  offset+=n;progress(io,offset,raw_size);
 }
 result=DIRECT_IAP_OK;
done:
 /* Source requires this command even after failures. No blind block retry for
  * DSP: a missing ACK cannot prove whether the address was programmed. */
 if(started&&!io->send(io->context,dsp_stop,sizeof(dsp_stop)))result=DIRECT_IAP_TX_FAILED;
 return result;
}
static direct_iap_result_t bms_exchange(const direct_iap_io_t*io,workspace_t*w,const uint8_t*p,size_t n,unsigned timeout,direct_bms_boot_reply_t*reply){
 size_t bytes=direct_bms_boot_make_frame(p,n,w->tx,sizeof(w->tx));if(!bytes)return DIRECT_IAP_INCOMPATIBLE;
 io->delay(io->context,5);
 if(!send(io,w->tx,bytes))return DIRECT_IAP_TX_FAILED;
 if(!exact(io,w->rx,15,timeout)||!direct_bms_boot_parse_reply(w->rx,15,p[0],reply))return DIRECT_IAP_ACK_FAILED;
 return reply->error?DIRECT_IAP_TARGET_ERROR:DIRECT_IAP_OK;
}
static direct_iap_result_t bms(const direct_iap_image_t*m,const direct_iap_io_t*io,workspace_t*w,size_t raw_size,bool legacy){
 uint8_t cmd[8]={0xff,m->hw_type,(uint8_t)(m->hw_model>>8),(uint8_t)m->hw_model,0,0,(uint8_t)(m->customer>>8),(uint8_t)m->customer};
 if(legacy){memset(cmd,0,sizeof(cmd));cmd[0]=0xff;}
 direct_bms_boot_reply_t reply={0};direct_iap_result_t result=DIRECT_IAP_ACK_FAILED;
 for(unsigned poll=0;poll<30;poll++){
  if(cancelled(io))return DIRECT_IAP_CANCELLED;
  result=bms_exchange(io,w,cmd,8,250,&reply);
  if(result==DIRECT_IAP_OK)break;
  if(result==DIRECT_IAP_TARGET_ERROR&&reply.error>=202)return DIRECT_IAP_INCOMPATIBLE;
  io->delay(io->context,50);
 }
 if(result!=DIRECT_IAP_OK)return result;
 /* Legacy HW-model reply bytes echo the request, not an identity read. */
 if(!legacy&&(reply.device_type!=m->hw_type||((uint16_t)reply.hardware_type_high<<8|reply.hardware_type_low)!=m->hw_model))return DIRECT_IAP_INCOMPATIBLE;
 for(size_t offset=0;offset<m->size;){
  if(cancelled(io))return DIRECT_IAP_CANCELLED;
  size_t n=m->size-offset;if(n>4096)n=4096;
  if(!read_raw(io,raw_size,offset,w->data,n))return DIRECT_IAP_READ_FAILED;
  uint32_t block=(uint32_t)(offset/4096)+1;uint8_t sum=0;for(size_t i=0;i<n;i++)sum+=w->data[i];
  for(unsigned attempt=0;attempt<3;attempt++){
   memset(cmd,0,8);cmd[0]=0xfe;for(unsigned i=0;i<4;i++)cmd[7-i]=(uint8_t)(block>>(8*i));
   result=bms_exchange(io,w,cmd,8,5000,&reply);
   if(result==DIRECT_IAP_OK){
    /* Payload in tx overlaps encoder destination; use workspace data for the
     * F9 payload so frame construction has disjoint input/output. */
    memmove(w->data+3,w->data,n);w->data[0]=0xf9;w->data[1]=w->data[2]=0;
    result=bms_exchange(io,w,w->data,n+3,5000,&reply);
    memmove(w->data,w->data+3,n);
   }
   if(result==DIRECT_IAP_OK){memset(cmd,0,8);cmd[0]=0xfd;cmd[4]=(uint8_t)(n>>8);cmd[5]=(uint8_t)n;cmd[7]=0xff-sum;result=bms_exchange(io,w,cmd,8,5000,&reply);}
   if(result==DIRECT_IAP_OK)break;
   if(result==DIRECT_IAP_TARGET_ERROR&&reply.error>=202)return DIRECT_IAP_INCOMPATIBLE;
  }
  if(result!=DIRECT_IAP_OK)return result;
  offset+=n;progress(io,offset,raw_size);
 }
 memset(cmd,0,8);cmd[0]=0xfc;
 /* FC acknowledgement follows whole-image CRC validation in target Boot. */
 return bms_exchange(io,w,cmd,8,30000,&reply);
}
direct_iap_result_t direct_iap_run(const direct_iap_image_t*m,const direct_iap_io_t*io){
 if(!m||!io||!io->read_image||!io->send||!io->receive||!io->delay||!io->verify_identity||!io->verify_version||!m->size||m->size>256*1024||!m->hw_type||!m->hw_model||!memchr(m->version,0,sizeof(m->version))||!m->version[0]||(m->target!=DIRECT_IAP_DSP&&m->target!=DIRECT_IAP_BMS))return DIRECT_IAP_INCOMPATIBLE;
 if(!io->verify_identity(io->context,m))return DIRECT_IAP_INCOMPATIBLE;
 if(cancelled(io))return DIRECT_IAP_CANCELLED;
 workspace_t*w=calloc(1,sizeof(*w));if(!w)return DIRECT_IAP_NO_MEMORY;
 direct_iap_result_t result=m->target==DIRECT_IAP_DSP?dsp(m,io,w,m->size,false):bms(m,io,w,m->size,false);
 free(w);
 if(result==DIRECT_IAP_OK){io->delay(io->context,1000);if(!io->verify_version(io->context,m))result=DIRECT_IAP_VERSION_UNCONFIRMED;}
 return result;
}

direct_iap_result_t direct_iap_run_legacy(const direct_iap_image_t*m,const direct_iap_io_t*io){
 if(!m||!io||!io->read_image||!io->send||!io->receive||!io->delay||!m->size||m->size>SIZE_MAX-7||
    (m->target!=DIRECT_IAP_DSP&&m->target!=DIRECT_IAP_BMS))return DIRECT_IAP_INCOMPATIBLE;
 direct_iap_image_t wire=*m;wire.size=(m->size+7)&~(size_t)7;
 /* Original ARM packet ID is u16; block number on BMS wire is u32. */
 if((m->target==DIRECT_IAP_DSP&&(wire.size/1024+(wire.size%1024!=0))>UINT16_MAX)||
    (m->target==DIRECT_IAP_BMS&&(wire.size/4096+(wire.size%4096!=0))>UINT32_MAX))return DIRECT_IAP_INCOMPATIBLE;
 if(cancelled(io))return DIRECT_IAP_CANCELLED;
 workspace_t*w=calloc(1,sizeof(*w));if(!w)return DIRECT_IAP_NO_MEMORY;
 direct_iap_result_t result=m->target==DIRECT_IAP_DSP?dsp(&wire,io,w,m->size,true):bms(&wire,io,w,m->size,true);
 free(w);return result;
}

