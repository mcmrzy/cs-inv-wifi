#include "direct_dsp_params.h"
#include <string.h>
static uint16_t get(const uint8_t *p,unsigned at){return (uint16_t)((p[at]<<8)|p[at+1]);}
static void put(uint8_t *p,unsigned at,uint16_t v){p[at]=(uint8_t)(v>>8);p[at+1]=(uint8_t)v;}
static void seal(uint8_t *p,size_t n){uint16_t c=direct_crc16(p,n-2);p[n-2]=(uint8_t)c;p[n-1]=(uint8_t)(c>>8);}
bool direct_dsp_params_arm_defaults(direct_dsp_params_t *params){
    if(!params)return false;
    uint8_t bundle[64]={1,21,0};
    /* USERFLASH.c init_setting_param_L00F960_flash + L00B8DC_flash. */
    const uint16_t cmd00[]={2300,5000,540,400,1,60,140,3,1,480};
    for(unsigned i=0;i<10;i++)put(bundle,3+2*i,cmd00[i]);
    seal(bundle,25);
    uint8_t *p=bundle+25;p[0]=1;p[1]=19;p[2]=1;
    const uint16_t cmd01[]={576,2800,900,0x1dda,1,50,95,5,0x0321};
    for(unsigned i=0;i<9;i++)put(p,3+2*i,cmd01[i]);
    seal(p,23);
    /* SysParamFlash_Init zeroes SysParam before ReadSysParamFormEEPROM
     * supplies these range defaults. GeBalanceEn remains disabled (0). */
    p=bundle+48;p[0]=1;p[1]=12;p[2]=2;p[3]=0;
    put(p,4,20);put(p,6,80);put(p,8,480);put(p,10,560);put(p,12,3000);
    seal(p,16);
    return direct_dsp_params_decode(bundle,sizeof(bundle),params);
}
static bool sane(const uint8_t *p){
    const uint8_t *q=p+25,*g=p+48;
    uint16_t freq=get(p,5),type=get(p,11),priority=get(p,17);
    return get(p,3)>=2000 && get(p,3)<=2500 && (freq==5000 || freq==6000) &&
        get(p,7)>=400 && get(p,7)<=650 && get(p,9)>=400 && get(p,9)<get(p,7) &&
        type>=1 && type<=5 && get(p,13)<=200 && get(p,15)<=200 && get(p,13)<=get(p,15) &&
        (priority==1 || priority==3 || priority==4) && get(p,19)>=1 && get(p,19)<=4 &&
        get(p,21)>=400 && get(p,21)<=650 && get(q,3)>=400 && get(q,3)<=650 &&
        get(p,21)<get(q,3) && get(q,5)>=1800 && get(q,5)<=3000 &&
        get(q,7)>=800 && get(q,7)<get(q,5) && get(q,11)>=1 && get(q,11)<=5 &&
        get(q,13)<=100 && get(q,15)<=100 && get(q,17)<=100 && get(q,13)<get(q,15) &&
        !(g[3]&~1U) && get(g,4)<=100 && get(g,6)<=100 &&
        (!g[3] || (get(g,4)<get(g,6) && get(g,8)>=400 && get(g,10)<=650 &&
                    get(g,8)<get(g,10) && get(g,12)>0));
}
bool direct_dsp_params_decode(const uint8_t *b,size_t n,direct_dsp_params_t *out){
    if(!b || !out || n!=64 || !direct_dsp_validate(b,25) || b[2]!=0 ||
       !direct_dsp_validate(b+25,23) || b[27]!=1 ||
       !direct_dsp_validate(b+48,16) || b[50]!=2 || !sane(b))return false;
    memset(out,0,sizeof(*out));memcpy(out->frames,b,64);out->buzzer=true;out->revision=1;return true;
}
static int parameter_offset(uint16_t index){
    switch(index){
    case 0:return 19;case 1:return 15;case 4:return 11;case 7:return 3;case 8:return 5;
    case 10:return 13;case 11:return 21;case 12:return 28;case 13:return 17;
    case 34:return 56;case 35:return 58;case 36:return 38;case 37:return 40;
    case 38:return 52;case 39:return 54;case 40:return 42;case 49:return 60;
    default:return -1;
    }
}
static int parameter_bit(uint16_t index){
    switch(index){case 5:return 6;case 6:return 7;case 16:return 11;case 17:return 0;case 21:return 8;case 26:return 15;default:return -1;}
}
bool direct_dsp_params_get(const direct_dsp_params_t *p,uint16_t index,uint16_t *value){
    if(!p || !value)return false;
    int offset=parameter_offset(index),bit=parameter_bit(index);
    if(offset>=0)*value=get(p->frames,(unsigned)offset);
    else if(bit>=0)*value=(get(p->frames,34)>>bit)&1U;
    else if(index==9)*value=(get(p->frames,44)>>9)&1U;
    else if(index==14)*value=(get(p->frames,44)>>8)&1U;
    else if(index==15)*value=1U-((get(p->frames,34)>>10)&1U); /* LCD blank enable */
    else if(index==41)*value=p->buzzer;
    else if(index==48)*value=get(p->frames,44)&255U;
    else if(index==50)*value=p->frames[51];
    else{
        uint16_t type=get(p->frames,11);
        if(index==(type==1?70:type==2?71:type==3?73:type==4?72:UINT16_MAX))*value=get(p->frames,7);
        else if(index==(type==1?76:type==2?77:type==3?79:type==4?78:UINT16_MAX))*value=get(p->frames,9);
        else return false;
    }
    return true;
}
bool direct_dsp_params_set(direct_dsp_params_t *params,uint16_t index,uint16_t value){
    uint16_t old;
    if(!params || !direct_dsp_params_get(params,index,&old))return false;
    direct_dsp_params_t next=*params;uint8_t *p=next.frames;
    int offset=parameter_offset(index),bit=parameter_bit(index);
    uint16_t type=get(p,11);
    if(index==4 && value!=type)return false; /* full battery profile import required */
    if(index==48 && (type!=4 || value>255))return false;
    if(index==26 && value && type!=2 && type!=3)return false;
    if((bit>=0 || index==9 || index==14 || index==15 || index==41 || index==50) && value>1)return false;
    if(offset>=0)put(p,(unsigned)offset,value);
    else if(bit>=0)put(p,34,(uint16_t)((get(p,34)&~(1U<<bit))|(value<<bit)));
    else if(index==9)put(p,44,(uint16_t)((get(p,44)&~0x0200U)|(value<<9)));
    else if(index==14)put(p,44,(uint16_t)((get(p,44)&~0x0100U)|(value<<8)));
    else if(index==15)put(p,34,(uint16_t)((get(p,34)&~0x0400U)|((1U-value)<<10)));
    else if(index==41)next.buzzer=value!=0;
    else if(index==48)put(p,44,(uint16_t)((get(p,44)&0xff00U)|value));
    else if(index==50)p[51]=(uint8_t)value;
    else put(p,index>=76?9:7,value);
    seal(p,25);seal(p+25,23);seal(p+48,16);
    direct_dsp_params_t checked;
    if(!direct_dsp_params_decode(p,64,&checked))return false;
    *params=next;return true;
}
size_t direct_dsp_params_reply(const direct_dsp_params_t *p,uint8_t cmd,uint8_t out[25]){
    if(!p || !out || cmd>2)return 0;
    size_t n=cmd==0?25:cmd==1?23:16;memcpy(out,p->frames+(cmd==0?0:cmd==1?25:48),n);return n;
}
static bool valid_sn(const char *sn){
    if(!sn || strlen(sn)!=16)return false;
    for(unsigned i=0;i<16;i++)if(!((sn[i]>='0'&&sn[i]<='9')||(sn[i]>='A'&&sn[i]<='Z')||(sn[i]>='a'&&sn[i]<='z')))return false;
    return true;
}
bool direct_dsp_params_pack(const direct_dsp_params_t *p,const char *sn,uint8_t out[DIRECT_DSP_PARAMS_RECORD_SIZE]){
    direct_dsp_params_t checked;
    if(!out || !p || !p->revision || !valid_sn(sn) || !direct_dsp_params_decode(p->frames,64,&checked))return false;
    memset(out,0,DIRECT_DSP_PARAMS_RECORD_SIZE);memcpy(out,"DSPP\3",5);memcpy(out+5,sn,16);out[21]=p->buzzer;memcpy(out+22,p->frames,64);
    put(out,86,(uint16_t)(p->revision>>16));put(out,88,(uint16_t)p->revision);seal(out,DIRECT_DSP_PARAMS_RECORD_SIZE);return true;
}
bool direct_dsp_params_unpack(const uint8_t *r,size_t n,const char *sn,direct_dsp_params_t *out){
    if(!r || (n!=88 && n!=DIRECT_DSP_PARAMS_RECORD_SIZE) || !out || !valid_sn(sn) || memcmp(r,"DSPP",4) ||
       (n==88?(r[4]!=1 && r[4]!=2):r[4]!=3) || (r[4]==1?r[21]!=0:r[21]>1) ||
       memcmp(r+5,sn,16) || direct_crc16(r,n-2)!=(uint16_t)(r[n-2]|(r[n-1]<<8)))return false;
    direct_dsp_params_t next;if(!direct_dsp_params_decode(r+22,64,&next))return false;
    next.buzzer=r[4]==1 || r[21]!=0;
    if(r[4]==3){next.revision=((uint32_t)get(r,86)<<16)|get(r,88);if(!next.revision)return false;}
    *out=next;return true;
}
bool direct_dsp_params_write_matches(const uint8_t *w,size_t wn,const uint8_t *r,size_t rn){
    if(!direct_dsp_validate(w,wn) || !direct_dsp_validate(r,rn) || rn<43 || r[2]!=0x25)return false;
    switch(w[2]){
    case 7:return wn==7 && get(w,3)==get(r,7);
    case 8:return wn==7 && get(w,3)==get(r,9);
    case 0x15:return wn==7 && w[3]==r[12] && (w[3]!=4 || w[4]==r[40]);
    case 0x17:return wn==9 && get(w,3)==r[11] && get(w,5)==r[26];
    case 0x16:return wn==9 && get(w,3)==get(r,19) && get(w,5)==get(r,21);
    case 0x18:return wn==7 && w[3]==r[3] && w[4]==r[4];
    case 0x24:return wn==11 && get(w,3)==r[24] && get(w,5)==r[25] && get(w,7)==r[23];
    case 0x2f:return wn==11 && get(w,3)==get(r,13) && get(w,5)==get(r,15) && get(w,7)==get(r,17);
    default:return false;
    }
}

bool direct_dsp_params_apply_write(direct_dsp_params_t *params,uint16_t index,const uint8_t *w,size_t wn,const uint8_t *r,size_t rn){
    if(!params || !direct_dsp_params_write_matches(w,wn,r,rn))return false;
    direct_dsp_params_t next=*params;uint8_t *p=next.frames,*q=p+25;
    switch(w[2]){
    case 7:put(p,3,get(w,3));break;
    case 8:put(p,5,get(w,3));break;
    case 0x15:
        put(p,11,w[3]);put(q,9,(uint16_t)((get(q,9)&~0x0200U)|(w[3]==4?0x0200U:0U)));
        put(q,19,(uint16_t)((get(q,19)&0xff00U)|w[4]));break;
    case 0x17:
        if(index==1)put(p,15,get(w,3));
        else if(index==10)put(p,13,get(w,5));
        else return false;
        break;
    case 0x16:put(p,21,get(w,3));put(q,3,get(w,5));break;
    case 0x18:put(p,19,w[3]);put(p,17,w[4]);break;
    case 0x24:put(q,13,get(w,3));put(q,15,get(w,5));put(q,17,get(w,7));break;
    case 0x2f:put(p,7,get(w,5));put(p,9,get(w,7));break;
    default:return false;
    }
    seal(p,25);seal(q,23);direct_dsp_params_t checked;
    if(!direct_dsp_params_decode(next.frames,64,&checked) || !memcmp(params,&next,sizeof(next)))return false;
    *params=next;return true;
}
bool direct_dsp_params_collision_hint(const uint8_t *bytes,size_t n){
    if(!bytes || !n || n%5)return false;
    for(size_t i=0;i<n;i+=5){
        if(bytes[i]!=1 || (bytes[i+1]!=18 && bytes[i+1]!=65) || bytes[i+2]>2)return false;
        uint8_t request[3]={1,1,bytes[i+2]};uint16_t crc=direct_crc16(request,3);
        if(bytes[i+3]!=(uint8_t)crc || bytes[i+4]!=(uint8_t)(crc>>8))return false;
    }
    return true;
}
