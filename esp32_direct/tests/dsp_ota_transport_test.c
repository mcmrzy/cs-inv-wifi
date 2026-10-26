#include "direct_dsp_ota_transport.h"
#include "direct_protocol.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static struct {
 uint32_t now;size_t size;unsigned hr,erase,packets,eot,progress,last_index,packet_attempts;
 unsigned hr_drop,erase_drop,erase_cr_drop,packet_drop;bool bad_header,read_fail,tx_fail,abort,noise;
 bool erase_prefix,erase_echo_only;unsigned flushes;
 bool custom_packet_reply;uint8_t packet_reply[80];size_t packet_reply_size;
 uint8_t queue[128],last_packet[1029];size_t used,pos,last_size;
} peer;
static const uint8_t ack[]={1,0x10,4,0x38,0,2,4,0,0,0,0x11,3,0xd1};
static uint8_t image_byte(size_t off){
 const uint8_t header[]={0,0x48,0x12,0x34,0x5a,0x5a};
 return off<sizeof(header)?header[off]:(uint8_t)off;
}
static bool read_image(void *ctx,size_t off,uint8_t *out,size_t n){
 (void)ctx;assert(off+n<=peer.size);if(peer.read_fail)return false;
 for(size_t i=0;i<n;i++)out[i]=image_byte(off+i);
 if(peer.bad_header && off==0)out[0]=0x7f;
 return true;
}
static bool send(void *ctx,const uint8_t *p,size_t n){
 (void)ctx;if(peer.tx_fail)return false;
 if(n==13 && peer.erase_prefix){
  /* One flush for CMD42, then one before the first erase. No retry flush. */
  assert(peer.flushes==2);
  assert(peer.pos==peer.used);
 }
 peer.used=peer.pos=0;
 if(n==9){assert(p[2]==0x42&&p[7]==0x73&&p[8]==0x9a);peer.hr++;
  if(peer.hr_drop){peer.hr_drop--;return true;}
  const uint8_t bytes[]={1,0x12,0,0x21,0x90,'H','H','R'};
  memcpy(peer.queue,bytes,sizeof(bytes));peer.used=sizeof(bytes);
  if(peer.erase_prefix){peer.queue[peer.used++]='H';peer.queue[peer.used++]='R';}
 }else if(n==13){assert(!memcmp(p,(uint8_t[]){1,0x10,4,0x38,0,2,4,0,0,0,0,0xc3,0xdd},13));peer.erase++;
  if(peer.erase_drop){peer.erase_drop--;return true;}
  if(peer.erase_prefix){
   /* Bytes arriving AFTER the flush: duplicate HRs + the full request echo. */
   memcpy(peer.queue,"HRHR",4);peer.used=4;
   memcpy(peer.queue+peer.used,p,n);peer.used+=n;
  }
  if(!peer.erase_echo_only){memcpy(peer.queue+peer.used,ack,sizeof(ack));peer.used+=sizeof(ack);}
  if(peer.erase_cr_drop)peer.erase_cr_drop--;
  else{peer.queue[peer.used++]='C';peer.queue[peer.used++]='R';}
 }else if(n==6){assert(!memcmp(p,(uint8_t[]){2,0,0xff,'E','O','T'},6));peer.eot++;
 }else{
  assert(p[0]==2&&n>5&&n<=1029);
  unsigned index=peer.progress;
  uint16_t address=(uint16_t)(0x01fe + 0x00ff*index);
  assert(p[1]==(uint8_t)(address>>8)&&p[2]==(uint8_t)address);
  size_t bytes=peer.size-(size_t)index*1024;if(bytes>1024)bytes=1024;
  assert(n==bytes+5);
  for(size_t i=0;i<bytes;i++)assert(p[i+3]==image_byte((size_t)index*1024+i));
  uint16_t crc=direct_crc16(p,n-2);assert(p[n-2]==(uint8_t)(crc>>8)&&p[n-1]==(uint8_t)crc);
  if(peer.last_size&&peer.last_index==index)assert(peer.last_size==n&&!memcmp(peer.last_packet,p,n));
  memcpy(peer.last_packet,p,n);peer.last_size=n;peer.last_index=index;peer.packet_attempts++;peer.packets++;
  if(peer.packet_drop){peer.packet_drop--;return true;}
  if(peer.custom_packet_reply){
   memcpy(peer.queue,peer.packet_reply,peer.packet_reply_size);peer.used=peer.packet_reply_size;
   return true;
  }
  peer.queue[0]='H';peer.queue[1]='R';peer.queue[2]='C';peer.queue[3]='C';peer.queue[4]='R';peer.used=5;
 }
 return true;
}
static size_t receive(void *ctx,uint8_t *out,size_t n,unsigned timeout){
 (void)ctx;assert(n==1&&timeout<=25&&timeout>0);
 if(peer.noise){peer.now+=timeout;*out=0x55;return 1;}
 if(peer.pos==peer.used){peer.now+=timeout;return 0;}
 *out=peer.queue[peer.pos++];peer.now++;return 1;
}
static void delay(void *ctx,unsigned ms){(void)ctx;peer.now+=ms;}
static uint32_t now(void *ctx){(void)ctx;return peer.now;}
static void flush(void *ctx){(void)ctx;peer.flushes++;peer.pos=peer.used=0;}
static bool cancelled(void *ctx){(void)ctx;return peer.abort;}
static void progress(void *ctx,size_t done,size_t total){
 (void)ctx;assert(total==peer.size&&done<=total);peer.progress++;peer.packet_attempts=0;
 assert(done==((size_t)peer.progress*1024<total?(size_t)peer.progress*1024:total));
}
static void reset(size_t size){memset(&peer,0,sizeof(peer));peer.size=size;}
int main(void){
 direct_iap_io_t io={.read_image=read_image,.send=send,.receive=receive,.delay=delay,
  .now_ms=now,.flush=flush,.cancelled=cancelled,.progress=progress};
 reset(1030);assert(direct_dsp_ota_flash(peer.size,&io)==DIRECT_IAP_OK);
 assert(peer.hr==1&&peer.erase==1&&peer.progress==2&&peer.last_size==11&&peer.eot==1);
 reset(64);peer.erase_prefix=true;
 assert(direct_dsp_ota_flash(peer.size,&io)==DIRECT_IAP_OK);
 assert(peer.erase==1&&peer.packets==1&&peer.eot==1);
 reset(64);peer.erase_prefix=true;peer.erase_echo_only=true;
 /* Even echo + CR is insufficient: all 13 ACK bytes, including 11 03 D1, are required. */
 assert(direct_dsp_ota_flash(peer.size,&io)==DIRECT_IAP_ACK_FAILED);
 assert(peer.erase==5&&!peer.packets&&!peer.eot);
 puts("PASS erase response alignment: flush before first erase, scan past duplicate HR/request echo, require full 13-byte ACK before CR/data");
 reset(64);peer.bad_header=true;assert(direct_dsp_ota_flash(peer.size,&io)==DIRECT_IAP_IMAGE_OPCODE_INVALID&&peer.hr==0);
 reset(65);assert(direct_dsp_ota_flash(peer.size,&io)==DIRECT_IAP_IMAGE_SIZE_INVALID&&peer.hr==0);
 reset(DIRECT_DSP_OTA_MAX_SIZE+2);assert(direct_dsp_ota_flash(peer.size,&io)==DIRECT_IAP_IMAGE_SIZE_INVALID&&peer.hr==0);
 reset(64);peer.read_fail=true;assert(direct_dsp_ota_flash(peer.size,&io)==DIRECT_IAP_READ_FAILED&&peer.hr==0);
 reset(64);peer.hr_drop=2;peer.erase_drop=1;peer.erase_cr_drop=1;peer.packet_drop=2;
 assert(direct_dsp_ota_flash(peer.size,&io)==DIRECT_IAP_OK);
 assert(peer.hr==3&&peer.erase==3&&peer.packets==3&&peer.eot==1);
 reset(64);peer.packet_drop=20;assert(direct_dsp_ota_flash(peer.size,&io)==DIRECT_IAP_ACK_FAILED);
 assert(peer.packets==10&&peer.eot==0&&peer.progress==0);
 /* 等 HR 预算 = 250ms × 12 ≈ 3.3s(覆盖 bootloader 上电 3 秒窗口) */
 reset(64);peer.hr_drop=20;assert(direct_dsp_ota_flash(peer.size,&io)==DIRECT_IAP_ACK_FAILED&&peer.hr==12&&peer.eot==0);
 reset(64);peer.erase_drop=10;assert(direct_dsp_ota_flash(peer.size,&io)==DIRECT_IAP_ACK_FAILED&&peer.erase==5&&peer.eot==0);
 reset(64);peer.tx_fail=true;assert(direct_dsp_ota_flash(peer.size,&io)==DIRECT_IAP_TX_FAILED&&peer.eot==0);
 reset(64);peer.abort=true;assert(direct_dsp_ota_flash(peer.size,&io)==DIRECT_IAP_CANCELLED&&peer.hr==0);
 reset(DIRECT_DSP_OTA_MAX_SIZE);assert(direct_dsp_ota_flash(peer.size,&io)==DIRECT_IAP_OK&&peer.progress==367);
 reset(64);peer.noise=true;assert(direct_dsp_ota_enter_boot(&io)==DIRECT_IAP_ACK_FAILED);
 assert(peer.now==4100); /* 12×250ms + 11×100ms 重试间隔: 噪声里也按真实截止时间 */
 reset(64);peer.now=UINT32_MAX-100;assert(direct_dsp_ota_enter_boot(&io)==DIRECT_IAP_OK);
 assert(direct_dsp_ota_exit_boot(&io)==DIRECT_IAP_OK&&peer.eot==1);
 /* Diagnostic is a distinct non-success result, ONE pkt0, no EOT/progress.
  * Collect the full two seconds even after CR; don't hide trailing diagnostics. */
 io.probe_packet0=true;
 reset(1030);peer.custom_packet_reply=true;memcpy(peer.packet_reply,"CRP",3);peer.packet_reply_size=3;
 assert(direct_dsp_ota_flash(peer.size,&io)==DIRECT_IAP_DIAGNOSTIC_COMPLETE);
 assert(peer.hr==1&&peer.erase==1&&peer.packets==1&&!peer.progress&&!peer.eot);
 assert(peer.last_size==1029&&peer.last_packet[1]==1&&peer.last_packet[2]==0xfe);
 assert(peer.now>=2000&&peer.now<2100&&peer.pos==peer.used);
 const uint8_t replies[][5]={{0x69,1,0xfe},{0x72,1,0xfe,4,5},{0x50},{0}};
 const size_t lengths[]={3,5,1,0};
 for(unsigned i=0;i<4;i++){
  reset(1030);peer.custom_packet_reply=true;
  memcpy(peer.packet_reply,replies[i],lengths[i]);peer.packet_reply_size=lengths[i];
  assert(direct_dsp_ota_flash(peer.size,&io)==DIRECT_IAP_DIAGNOSTIC_COMPLETE);
  assert(peer.packets==1&&!peer.eot&&!peer.progress&&peer.now>=2000&&peer.pos==peer.used);
 }
 reset(64);assert(direct_dsp_ota_flash(peer.size,&io)==DIRECT_IAP_IMAGE_SIZE_INVALID&&!peer.hr);
 puts("PASS pkt0-only diagnostic: exact 02 01 FE frame+CRC, one send, complete 2s CR/diagnostic/silent capture, no EOT or success progress");
 puts("PASS F280039C actual module protocol: preflight before CMD42, short tail, HR/erase/data retries, exact frame+CRC, 367 packets/address wrap, monotonic noise deadline, EOT only on success");
}
