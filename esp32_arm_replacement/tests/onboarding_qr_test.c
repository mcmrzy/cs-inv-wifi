#include "onboarding_qr.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
int main(int argc,char **argv) {
    onboarding_qr_t q={0};
    assert(onboarding_qr_update(&q,"H1ZZX0013900001P","001234"));
    assert(strcmp(q.payload,"csinv://bind?sn=H1ZZX0013900001P&pin=001234")==0);
    int size=onboarding_qr_size(&q);assert(size>=21 && size<=41 && 156/(size+8)>=3);
    uint32_t gen=q.generation;assert(onboarding_qr_update(&q,"H1ZZX0013900001P","001234") && q.generation==gen);
    if(argc>1){FILE*f=fopen(argv[1],"wb");assert(f);int scale=3,span=(size+8)*scale;fprintf(f,"P5\n%d %d\n255\n",span,span);for(int y=0;y<span;y++)for(int x=0;x<span;x++){int xx=x/scale-4,yy=y/scale-4;unsigned char p=onboarding_qr_module(&q,xx,yy)?0:255;fwrite(&p,1,1,f);}fclose(f);}
    assert(!onboarding_qr_update(&q,"short","001234") && !q.ready);
    assert(!onboarding_qr_update(&q,"h1ZZX0013900001P","001234"));
    assert(!onboarding_qr_update(&q,"H1ZZX0013900001P","12345X"));
    assert(onboarding_qr_update(&q,"H1ZZX0013900001P","000000"));
    puts("onboarding QR: PASS (payload, validation, cache, scale)");return 0;
}
