#include "diag_core.h"
#include "panel_modbus.h"
#include <ctype.h>
#include <string.h>

static int digit(char c)
{
    if(c>='0' && c<='9')return c-'0';
    c=(char)tolower((unsigned char)c);
    return c>='a' && c<='f'?c-'a'+10:-1;
}
static bool number(const char *s,unsigned base,uint32_t min,uint32_t max,uint32_t *out)
{
    if(base==16 && s[0]=='0' && (s[1]=='x' || s[1]=='X'))s+=2;
    if(!*s)return false;
    uint32_t n=0;
    for(;*s;s++){
        int d=digit(*s);
        if(d<0 || (unsigned)d>=base || n>(UINT32_MAX-(unsigned)d)/base)return false;
        n=n*base+(unsigned)d;
    }
    if(n<min || n>max)return false;
    *out=n;return true;
}
static size_t finish(uint8_t *out,size_t n)
{uint16_t c=panel_mb_crc(out,n);out[n]=(uint8_t)c;out[n+1]=(uint8_t)(c>>8);return n+2;}

bool diag_command(const char *line,diag_command_t *out)
{
    if(!line || !out)return false;
    size_t length=0;while(length<DIAG_LINE_MAX && line[length])length++;
    if(!length || length==DIAG_LINE_MAX)return false;
    char copy[DIAG_LINE_MAX];memcpy(copy,line,length+1);
    char *tokens[DIAG_TX_MAX+1];unsigned n=0;
    for(char *p=copy;*p;){
        while(isspace((unsigned char)*p))p++;
        if(!*p)break;
        if(n==DIAG_TX_MAX+1)return false;
        tokens[n++]=p;
        while(*p && !isspace((unsigned char)*p))p++;
        if(*p)*p++=0;
    }
    if(!n)return false;
    for(char *p=tokens[0];*p;p++)*p=(char)tolower((unsigned char)*p);
    memset(out,0,sizeof(*out));
    const struct {const char *name;diag_kind_t kind;} simple[]={
        {"help",CMD_HELP},{"?",CMD_HELP},{"status",CMD_STATUS},{"stats",CMD_STATS},
        {"clear",CMD_CLEAR},{"reboot",CMD_REBOOT},{"stop",CMD_STOP},{"q",CMD_STOP},
        {"swap",CMD_SWAP},{"loopback",CMD_LOOPBACK},{"legacy",CMD_LEGACY}};
    for(unsigned i=0;i<sizeof(simple)/sizeof(simple[0]);i++)if(!strcmp(tokens[0],simple[i].name)){
        out->kind=simple[i].kind;return n==1;
    }
    const struct {const char *name;diag_kind_t kind;uint32_t min,max;} scalar[]={
        {"baud",CMD_BAUD,300,1000000},{"addr",CMD_ADDR,1,247},{"fc",CMD_FC,3,4},
        {"timeout",CMD_TIMEOUT,50,5000},{"verbose",CMD_VERBOSE,0,1}};
    for(unsigned i=0;i<sizeof(scalar)/sizeof(scalar[0]);i++)if(!strcmp(tokens[0],scalar[i].name)){
        out->kind=scalar[i].kind;
        return n==2 && number(tokens[1],10,scalar[i].min,scalar[i].max,&out->arg[0]);
    }
    if(!strcmp(tokens[0],"pins")){
        out->kind=CMD_PINS;
        return n==3 && number(tokens[1],10,0,1,&out->arg[0]) &&
            number(tokens[2],10,0,1,&out->arg[1]) && out->arg[0]!=out->arg[1];
    }
    if(!strcmp(tokens[0],"format")){
        out->kind=CMD_FORMAT;
        if(n!=2 || strlen(tokens[1])!=3 || tokens[1][0]!='8')return false;
        out->parity=(char)toupper((unsigned char)tokens[1][1]);
        out->arg[0]=(unsigned)(tokens[1][2]-'0');
        return strchr("NEO",out->parity)!=NULL && (out->arg[0]==1 || out->arg[0]==2);
    }
    if(!strcmp(tokens[0],"invert")){
        out->kind=CMD_INVERT;
        const char *const names[]={"none","rx","tx","both"};
        if(n!=2)return false;
        for(unsigned i=0;i<4;i++)if(!strcmp(tokens[1],names[i])){out->arg[0]=i;return true;}
        return false;
    }
    if(!strcmp(tokens[0],"read") || !strcmp(tokens[0],"poll")){
        bool poll=!strcmp(tokens[0],"poll");out->kind=poll?CMD_POLL:CMD_READ;
        out->arg[1]=1;out->arg[2]=poll?1000:0;
        if(n<2 || n>4 || !number(tokens[1],16,0,65535,&out->arg[0]))return false;
        if(n>=3 && !number(tokens[2],10,1,125,&out->arg[1]))return false;
        if(n==4 && !number(tokens[3],10,poll?50:1,poll?60000:247,&out->arg[2]))return false;
        return out->arg[0]+out->arg[1]<=65536;
    }
    if(!strcmp(tokens[0],"scan") || !strcmp(tokens[0],"hunt")){
        bool hunt=!strcmp(tokens[0],"hunt");out->kind=hunt?CMD_HUNT:CMD_SCAN;
        out->arg[0]=1;out->arg[1]=hunt?16:247;out->arg[2]=0x42;
        if(n>(hunt?3U:5U))return false;
        if(n>=2 && !number(tokens[1],10,1,247,&out->arg[0]))return false;
        if(n>=3 && !number(tokens[2],10,1,247,&out->arg[1]))return false;
        if(n>=4 && !number(tokens[3],16,0,65535,&out->arg[2]))return false;
        if(n>=5 && !number(tokens[4],10,3,4,&out->arg[3]))return false;
        return out->arg[0]<=out->arg[1];
    }
    if(!strcmp(tokens[0],"listen") || !strcmp(tokens[0],"levels")){
        bool levels=!strcmp(tokens[0],"levels");out->kind=levels?CMD_LEVELS:CMD_LISTEN;
        out->arg[0]=levels?1000:10000;
        return n==1 || (n==2 && number(tokens[1],10,1,levels?10000:600000,&out->arg[0]));
    }
    if(!strcmp(tokens[0],"tx") || !strcmp(tokens[0],"txcrc") || !strcmp(tokens[0],"crc")){
        out->kind=!strcmp(tokens[0],"tx")?CMD_TX:!strcmp(tokens[0],"txcrc")?CMD_TXCRC:CMD_CRC;
        size_t limit=out->kind==CMD_TXCRC?DIAG_TX_MAX-2:DIAG_TX_MAX;
        if(n<2)return false;
        for(unsigned i=1;i<n;i++){
            size_t size=strlen(tokens[i]);
            if(size%2)return false;
            for(size_t j=0;j<size;j+=2){
                int high=digit(tokens[i][j]),low=digit(tokens[i][j+1]);
                if(high<0 || low<0 || out->length>=limit)return false;
                out->data[out->length++]=(uint8_t)(high*16+low);
            }
        }
        if(out->kind==CMD_TXCRC)out->length=finish(out->data,out->length);
        return out->length>0;
    }
    return false;
}

size_t diag_read_request(uint8_t a,uint8_t f,uint16_t reg,uint16_t count,uint8_t out[8])
{
    if(!out || a<1 || a>247 || (f!=3 && f!=4) || count<1 || count>125 ||
       (uint32_t)reg+count>65536)return 0;
    out[0]=a;out[1]=f;out[2]=(uint8_t)(reg>>8);out[3]=(uint8_t)reg;
    out[4]=(uint8_t)(count>>8);out[5]=(uint8_t)count;return finish(out,6);
}
size_t diag_legacy_request(uint8_t out[9])
{
    if(!out)return 0;
    memset(out,0,9);out[0]=1;out[1]=5;out[2]=4;return finish(out,7);
}
diag_reply_t diag_analyze(const uint8_t *tx,size_t tn,const uint8_t *rx,size_t rn,
                         uint8_t address,uint8_t function,uint16_t count,bool legacy)
{
    diag_reply_t out={.result=rn?DIAG_UNMATCHED:DIAG_NO_RX};
    if(!rx || !rn)return out;
    bool bad_crc=false;size_t non_echo=0;
    for(size_t i=0;i<rn;i++){
        if(tx && tn && rn-i>=tn && !memcmp(rx+i,tx,tn)){
            out.echo=true;i+=tn-1;continue;
        }
        non_echo++;
        size_t available=rn-i,len=0;const uint8_t *p=rx+i;
        bool exception=false;
        if(legacy){
            /* Original direct_protocol.h: CMD04_LEN=69, MAX_FRAME=128. */
            if(available<3 || p[0]!=1 || p[2]!=4 || p[1]<65 || p[1]>124)continue;
            len=(size_t)p[1]+4;
        }else{
            if((function!=3 && function!=4) || !count || count>125 || available<3 || p[0]!=address)continue;
            exception=p[1]==(uint8_t)(function|0x80);
            if(exception){if(!p[2] || p[2]>0x0b)continue;len=5;}
            else if(p[1]==function && p[2]==count*2)len=(size_t)p[2]+5;
            else continue;
        }
        if(available<len)continue;
        if(panel_mb_crc(p,len)!=0){bad_crc=true;continue;}
        out.result=legacy?DIAG_LEGACY_OK:exception?DIAG_EXCEPTION:DIAG_READ_OK;
        out.offset=i;out.length=len;out.exception=exception?p[2]:0;
        if(!legacy && !exception){
            out.count=count;
            for(unsigned j=0;j<count;j++)out.values[j]=(uint16_t)(((uint16_t)p[3+j*2]<<8)|p[4+j*2]);
        }
        return out;
    }
    out.result=bad_crc?DIAG_CRC_BAD:out.echo && !non_echo?DIAG_ECHO:DIAG_UNMATCHED;
    return out;
}
const char *diag_result_name(diag_result_t result)
{
    static const char *const names[]={"NO_RX","ECHO_ONLY","CRC_BAD","UNMATCHED","READ_OK","EXCEPTION","LEGACY_OK"};
    return (unsigned)result<sizeof(names)/sizeof(names[0])?names[result]:"UNKNOWN";
}
