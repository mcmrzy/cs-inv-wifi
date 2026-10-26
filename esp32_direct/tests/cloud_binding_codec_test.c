#include "cloud_binding_codec.h"
#include "monocypher-ed25519.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static void to_hex(char *out,const uint8_t *b,unsigned n){for(unsigned i=0;i<n;i++)sprintf(out+2*i,"%02x",b[i]);}
int main(void){
 uint8_t seed[32]={1},secret[64],pub[32],signature[64];char key[65],sig[129];uint64_t revision;
 crypto_ed25519_key_pair(secret,pub,seed);to_hex(key,pub,32);
 const char *sn="H1ZZX00139000000",*nonce="0123456789abcdef0123456789abcdef";
 const char *text="CSERGY-BINDING-V1\nH1ZZX00139000000\nUNBOUND\n1\n0123456789abcdef0123456789abcdef\n";
 crypto_ed25519_sign(signature,secret,(const uint8_t*)text,strlen(text));to_hex(sig,signature,64);
 assert(cloud_binding_verify(sn,"UNBOUND","1",nonce,sig,key,&revision)&&revision==1);
 assert(!cloud_binding_verify(sn,"BOUND","1",nonce,sig,key,&revision));
 assert(!cloud_binding_verify(sn,"UNBOUND","01",nonce,sig,key,&revision));
 assert(!cloud_binding_verify(sn,"UNBOUND","18446744073709551616",nonce,sig,key,&revision));
 assert(!cloud_binding_verify(sn,"UNBOUND","1","wrong",sig,key,&revision));
 assert(!cloud_binding_verify(sn,"UNBOUND","1",nonce,sig,"",&revision));
 sig[0]=sig[0]=='0'?'1':'0';assert(!cloud_binding_verify(sn,"UNBOUND","1",nonce,sig,key,&revision));
 assert(cloud_binding_verify("H1ZZX0013900001P","UNBOUND","17",nonce,
 "34655de51d188cc0f8d88c2aa01019973985826609ac61a916abaab29f4afec5b6aa2d02c728d0c82d0f13e6c1770d9ce510c20fce36ea98fb6a199b1106d60b",
 "3b6a27bcceb6a42d62a3a8d02a6f0d73653215771de243a63ac048a18b59da29",&revision)&&revision==17);
 puts("PASS actual Ed25519 binding codec: tamper, nonce format, key, revision canonical/overflow");
}
