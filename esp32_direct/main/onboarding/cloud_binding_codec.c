#include "cloud_binding_codec.h"
#include "monocypher-ed25519.h"
#include <stdio.h>
#include <string.h>
#include <limits.h>
static int digit(char c){if(c>='0'&&c<='9')return c-'0';if(c>='a'&&c<='f')return c-'a'+10;return -1;}
static bool hex(const char *s,uint8_t *out,size_t n){
 if(!s||strlen(s)!=n*2)return false;
 for(size_t i=0;i<n;i++){int a=digit(s[2*i]),b=digit(s[2*i+1]);if(a<0||b<0)return false;out[i]=(uint8_t)((a<<4)|b);}return true;
}
bool cloud_binding_verify(const char *sn,const char *state,const char *revision,
 const char *nonce,const char *signature,const char *public_key,uint64_t *out_revision){
 if(!sn||strlen(sn)!=16||!state||!revision||!nonce||!out_revision)return false;
 for(unsigned i=0;i<16;i++)if(!((sn[i]>='A'&&sn[i]<='Z')||(sn[i]>='0'&&sn[i]<='9')))return false;
 if(strcmp(state,"BOUND")&&strcmp(state,"UNBOUND")&&strcmp(state,"UNKNOWN"))return false;
 size_t n=strlen(revision);if(!n||n>20||(n>1&&revision[0]=='0'))return false;
 uint64_t rev=0;for(size_t i=0;i<n;i++){if(revision[i]<'0'||revision[i]>'9')return false;unsigned d=(unsigned)(revision[i]-'0');if(rev>(UINT64_MAX-d)/10)return false;rev=rev*10+d;}
 uint8_t nonce_bytes[16],sig[64],key[32];
 if(!hex(nonce,nonce_bytes,16)||!hex(signature,sig,64)||!hex(public_key,key,32))return false;
 unsigned any=0;for(unsigned i=0;i<32;i++)any|=key[i];if(!any)return false;
 char message[144];int length=snprintf(message,sizeof(message),"CSERGY-BINDING-V1\n%s\n%s\n%s\n%s\n",sn,state,revision,nonce);
 if(length<=0||(size_t)length>=sizeof(message)||crypto_ed25519_check(sig,key,(const uint8_t*)message,(size_t)length))return false;
 *out_revision=rev;return true;
}
