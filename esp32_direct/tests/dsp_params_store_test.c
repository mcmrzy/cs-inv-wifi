#include "direct_dsp_params_store.h"
#include "nvs.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static uint8_t committed[DIRECT_DSP_PARAMS_RECORD_SIZE],pending[DIRECT_DSP_PARAMS_RECORD_SIZE];static bool exists;
static size_t record_length=DIRECT_DSP_PARAMS_RECORD_SIZE;
static uint8_t profiles[DIRECT_DSP_PROFILES_RECORD_SIZE],pending_profiles[DIRECT_DSP_PROFILES_RECORD_SIZE];
static bool profiles_exist;
static unsigned commits, closes;static int fail;static bool legacy;
esp_err_t nvs_open(const char *name,int mode,nvs_handle_t *h){
 if(!strcmp(name,"direct_ui")){assert(mode==NVS_READONLY);*h=2;return legacy?ESP_OK:ESP_ERR_NVS_NOT_FOUND;}
 assert(!strcmp(name,"dsp_params"));assert(mode==0 || mode==1);*h=1;return fail==1?-1:fail==4 && mode==NVS_READONLY?ESP_ERR_NVS_NOT_FOUND:0;
}
esp_err_t nvs_get_u16(nvs_handle_t h,const char *key,uint16_t *out){assert(h==2);*out=!strcmp(key,"blank")?1:0;return ESP_OK;}
esp_err_t nvs_get_blob(nvs_handle_t h,const char *k,void *p,size_t *n){assert(h==1);if(!strcmp(k,"battery")){if(!profiles_exist)return ESP_ERR_NVS_NOT_FOUND;assert(*n>=sizeof(profiles));memcpy(p,profiles,sizeof(profiles));*n=sizeof(profiles);return ESP_OK;}assert(!strcmp(k,"record"));if(fail==5)return -1;if(!exists)return ESP_ERR_NVS_NOT_FOUND;if(*n<record_length){*n=record_length;return ESP_ERR_NVS_INVALID_LENGTH;}memcpy(p,committed,record_length);*n=record_length;return ESP_OK;}
esp_err_t nvs_set_blob(nvs_handle_t h,const char *k,const void *p,size_t n){assert(h==1);if(!strcmp(k,"battery")){assert(n==sizeof(profiles));memcpy(pending_profiles,p,n);}else{assert(!strcmp(k,"record")&&n==DIRECT_DSP_PARAMS_RECORD_SIZE);memcpy(pending,p,n);}return fail==2?1:0;}
esp_err_t nvs_commit(nvs_handle_t h){assert(h==1);commits++;if(fail==3)return 1;memcpy(committed,pending,sizeof(committed));memcpy(profiles,pending_profiles,sizeof(profiles));profiles_exist=true;record_length=sizeof(committed);exists=true;return 0;}
void nvs_close(nvs_handle_t h){assert(h==1 || h==2);closes++;}
static void word(uint8_t *p,unsigned at,uint16_t v){p[at]=(uint8_t)(v>>8);p[at+1]=(uint8_t)v;}
static void seal(uint8_t *p,size_t n){uint16_t c=direct_crc16(p,n-2);p[n-2]=(uint8_t)c;p[n-1]=(uint8_t)(c>>8);}
int main(void){
    direct_dsp_params_t p={0},loaded={0};p.revision=1;uint8_t *b=p.frames;b[0]=1;b[1]=21;b[2]=0;
    uint16_t v[]={2300,5000,540,400,1,60,140,3,1,480};for(unsigned i=0;i<10;i++)word(b,3+2*i,v[i]);seal(b,25);
    b+=25;b[0]=1;b[1]=19;b[2]=1;uint16_t c[]={576,2800,900,0x1dda,1,50,95,5,0x321};for(unsigned i=0;i<9;i++)word(b,3+2*i,c[i]);seal(b,23);
    b+=23;b[0]=1;b[1]=12;b[2]=2;seal(b,16);
    const char *sn="H1ZZX00139000079";
    assert(!direct_dsp_params_load(sn,&loaded));
    bool seeded=false;
    assert(direct_dsp_params_bootstrap(sn,&loaded,&seeded) && seeded && commits==1);
    assert(loaded.frames[61]==0xb8); /* original ARM generator default: 3000W */
    assert(direct_dsp_params_bootstrap(sn,&loaded,&seeded) && !seeded && commits==1);
    exists=false;commits=0;fail=4;
    assert(direct_dsp_params_bootstrap(sn,&loaded,&seeded) && seeded); /* absent namespace */
    fail=0;exists=false;commits=0;
    for(fail=1;fail<=3;fail++){
        memset(&loaded,0xa5,sizeof(loaded));
        assert(!direct_dsp_params_bootstrap(sn,&loaded,&seeded) && !seeded && !exists);
        assert(loaded.frames[0]==0xa5); /* save failure never enables RAM defaults */
    }
    fail=0;commits=0;
    assert(direct_dsp_params_save(sn,&p));assert(commits==1);
    assert(direct_dsp_params_bootstrap(sn,&loaded,&seeded) && !seeded && !memcmp(&p,&loaded,sizeof(p)) && commits==1);
    assert(direct_dsp_params_bootstrap("H1ZZX00139000081",&loaded,&seeded) && seeded && commits==2);
    assert(direct_dsp_params_load("H1ZZX00139000081",&loaded));
    assert(!direct_dsp_params_load(sn,&loaded));
    assert(direct_dsp_params_save(sn,&p));
    assert(direct_dsp_params_load(sn,&loaded) && !memcmp(&p,&loaded,sizeof(p)));
    p.revision=17;assert(direct_dsp_params_set(&p,7,2400) && direct_dsp_params_save(sn,&p));
    assert(direct_dsp_params_bootstrap(sn,&loaded,&seeded) && !seeded && !memcmp(&p,&loaded,sizeof(p)));
    uint16_t value;assert(direct_dsp_params_get(&loaded,7,&value) && value==2400);
    assert(loaded.revision==17);
    committed[4]=1;committed[21]=0;seal(committed,88);record_length=88;legacy=true;
    assert(direct_dsp_params_bootstrap(sn,&loaded,&seeded) && !seeded && committed[4]==3);
    assert(direct_dsp_params_get(&loaded,7,&value) && value==2400);
    assert(direct_dsp_params_get(&loaded,14,&value) && value==0);
    assert(direct_dsp_params_get(&loaded,15,&value) && value==1);
    assert(direct_dsp_params_get(&loaded,41,&value) && value==0);
    legacy=false;p=loaded;
    assert(!direct_dsp_params_load("H1ZZX00139000081",&loaded));
    uint8_t before[DIRECT_DSP_PARAMS_RECORD_SIZE];memcpy(before,committed,sizeof(before));
    for(fail=1;fail<=3;fail++){assert(!direct_dsp_params_save(sn,&p));assert(!memcmp(before,committed,sizeof(before)));}
    fail=0;committed[30]^=1;assert(!direct_dsp_params_load(sn,&loaded));
    direct_dsp_params_t defaults;assert(direct_dsp_params_arm_defaults(&defaults));
    uint8_t valid[DIRECT_DSP_PARAMS_RECORD_SIZE];assert(direct_dsp_params_pack(&p,sn,valid));
    for(unsigned fault=0;fault<8;fault++){
        memcpy(committed,valid,sizeof(valid));record_length=sizeof(valid);
        switch(fault){
        case 0:committed[30]^=1;break; /* CRC mismatch */
        case 1:committed[0]='X';seal(committed,sizeof(committed));break;
        case 2:committed[4]=99;seal(committed,sizeof(committed));break;
        case 3:record_length=12;break; /* truncated record */
        case 4:record_length=sizeof(committed)+1;break; /* NVS reports oversized blob */
        case 5:memset(committed+86,0,4);seal(committed,sizeof(committed));break; /* zero revision */
        case 6:word(committed+22,3,0);seal(committed+22,25);seal(committed,sizeof(committed));break; /* invalid output voltage */
        case 7:memcpy(committed+5,"H1ZZX00139000081",16);seal(committed,sizeof(committed));break;
        }
        assert(!direct_dsp_params_load(sn,&loaded));
        unsigned before_commits=commits;
        assert(direct_dsp_params_bootstrap(sn,&loaded,&seeded) && seeded && commits==before_commits+1);
        assert(!memcmp(&loaded,&defaults,sizeof(defaults)));
        assert(direct_dsp_params_load(sn,&loaded) && !memcmp(&loaded,&defaults,sizeof(defaults)));
        assert(direct_dsp_params_bootstrap(sn,&loaded,&seeded) && !seeded && commits==before_commits+1);
    }
    for(fail=1;fail<=5;fail++){
        if(fail==4)continue; /* absent namespace is already tested */
        memcpy(committed,valid,sizeof(valid));committed[30]^=1;record_length=sizeof(valid);
        memcpy(before,committed,sizeof(before));
        memset(&loaded,0xa5,sizeof(loaded));seeded=true;
        assert(!direct_dsp_params_bootstrap(sn,&loaded,&seeded) && !seeded);
        assert(loaded.frames[0]==0xa5 && !memcmp(committed,before,sizeof(before)));
    }
    fail=0;memcpy(committed,valid,sizeof(valid));
    unsigned before_commits=commits;
    assert(!direct_dsp_params_bootstrap("invalid-sn",&loaded,&seeded) && !seeded && commits==before_commits);
    assert(!memcmp(committed,valid,sizeof(valid)));
    assert(closes>0);puts("PASS actual DSP NVS store: exact single blob+commit, reboot load, SN/corruption rejection, open/set/commit failures");
    puts("PASS ARM defaults recovery: missing, wrong SN, CRC/header/version/length/revision/range errors recover once and survive reboot");
    puts("PASS recovery failures: open/read/set/commit errors and invalid caller SN never enable unsaved defaults or replace committed data");
    puts("PASS legacy local preferences: valid V1 record migrates into V3 without replacing DSP settings; user revision survives reboot");
}
