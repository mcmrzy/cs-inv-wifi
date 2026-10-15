#include "direct_ui.h"
#include "direct_ui_locale.h"
#include "direct_gfx.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>
static void localized_fits(const char *text){
 char zh[256];const char *translated=direct_ui_translate(text,true,zh,sizeof(zh));
 assert(text[0] && strcmp(text,translated));
 assert(direct_gfx_text_width(text,16)<=292);
 assert(direct_gfx_text_width(translated,16)<=292);
}
int main(void){
 direct_ui_state_t u;direct_ui_init(&u);direct_ui_data_t d={0};direct_ui_view_t v;
 d.ota_busy=true;d.ota.state=OTA_STATE_RECEIVING;d.ota.progress=50;d.ota.total=100;d.ota.received=50;
 u.page=UI_FACTORY;u.confirm=true;assert(direct_ui_key(&u,2,0,&d).action==UI_ACT_NONE);
 u.page=UI_CONFIG;u.group=7;u.editing=true;u.draft=2;d.dsp_online=true;d.valid[0]=d.writable[0]=true;
 assert(direct_ui_key(&u,2,0,&d).action==UI_ACT_NONE);u.page=UI_UPGRADE;
 assert(direct_ui_key(&u,2,0,&d).action==UI_ACT_OTA_CANCEL);
 direct_ui_build(&u,&d,&v);assert(strstr(v.title,"upgrade"));
 d.ota.state=OTA_STATE_INSTALLING;assert(direct_ui_key(&u,2,0,&d).action==UI_ACT_NONE);
 strcpy(d.ota.stage,"transferring");direct_ui_build(&u,&d,&v);
 localized_fits(v.rows[0]);localized_fits(v.rows[4]);localized_fits(v.rows[6]);
 for(unsigned state=0;state<=OTA_STATE_ROLLED_BACK;state++){
  d.ota.state=(OtaState)state;d.ota.stage[0]=0;d.ota.message[0]=0;d.ota.error_code[0]=0;
  direct_ui_build(&u,&d,&v);localized_fits(v.rows[0]);localized_fits(v.rows[6]);
 }
 static const char *codes[]={"DUPLICATE","INVALID_PARAMS","INVALID_TARGET","TLS_REQUIRED","SIGNATURE_REQUIRED","SIGNATURE_INVALID","SECURITY_VERSION_READ_FAILED","ROLLBACK_REJECTED","OTA_BUSY","FILE_TOO_LARGE","SIZE_MISMATCH","HASH_MISMATCH","HTTP_ERROR","TIMEOUT","READ_FAILED","FLASH_WRITE_FAILED","TARGET_TRANSFER_FAILED","PERSIST_FAILED","OTA_NOT_CANCELLABLE","TASK_CREATE_FAILED","INTERNAL_ERROR","TARGET_UNSUPPORTED","INSUFFICIENT_STORAGE","DSP_TRANSFER_FAILED","BMS_TRANSFER_FAILED","IMAGE_FORMAT_INVALID","TARGET_NOT_READY","FUTURE_UNKNOWN_ERROR"};
 d.ota.state=OTA_STATE_FAILED;
 strcpy(d.ota.error_code,"TASK_REPLAY_BLOCKED");direct_ui_build(&u,&d,&v);
 assert(!strcmp(v.rows[4],"Upgrade already requested"));localized_fits(v.rows[4]);
 for(unsigned i=0;i<sizeof(codes)/sizeof(*codes);i++){
  strcpy(d.ota.error_code,codes[i]);strcpy(d.ota.message,"HTTP client init failed free=1234 max=5678 | https needs TLS heap");
  OtaSnapshot original=d.ota;direct_ui_build(&u,&d,&v);
  assert(!memcmp(&original,&d.ota,sizeof(original)));assert(!strcmp(v.rows[5],codes[i]));
  localized_fits(v.rows[4]);localized_fits(v.rows[6]);assert(!strstr(v.rows[6],"free="));
 }
 d.ota.state=OTA_STATE_SUCCEEDED;d.ota.target=OTA_TARGET_DSP;strcpy(d.ota.error_code,"OK");
 const char *confirmation_codes[]={"TARGET_RESPONSE_TIMEOUT","TARGET_VERSION_UNCONFIRMED","DIAGNOSTIC_ONLY"};
 const char *confirmation_labels[]={"Target did not restart","Upgrade unconfirmed","Diagnostic finished"};
 for(unsigned i=0;i<3;i++){
  d.ota.state=OTA_STATE_FAILED;strcpy(d.ota.error_code,confirmation_codes[i]);
  direct_ui_build(&u,&d,&v);assert(!strcmp(v.rows[4],confirmation_labels[i]));
  localized_fits(v.rows[4]);localized_fits(v.rows[6]);
 }
 d.ota.state=OTA_STATE_SUCCEEDED;strcpy(d.ota.error_code,"OK");
 strcpy(d.ota.message,"Target transfer complete; target response not observed");
 direct_ui_build(&u,&d,&v);assert(!strcmp(v.rows[0],"Transfer complete"));
 assert(!strcmp(v.rows[6],"Target response not observed"));assert(!v.rows[5][0]);
 strcpy(d.ota.message,"Target transfer complete; new App responded");direct_ui_build(&u,&d,&v);
 assert(!strcmp(v.rows[6],"Target response received"));
 strcpy(d.ota.message,"Target transfer complete; application responded");direct_ui_build(&u,&d,&v);
 assert(!strcmp(v.rows[0],"Transfer complete")&&!strcmp(v.rows[6],"Target response received"));
 strcpy(d.ota.message,"Some future diagnostic message");direct_ui_build(&u,&d,&v);
 assert(!strcmp(v.rows[6],"Target response not confirmed"));
 d.ota.target=OTA_TARGET_ARM;direct_ui_build(&u,&d,&v);assert(strstr(v.rows[1],"ARM"));
 puts("PASS upgrade safety, all state/error localization, width limits, raw diagnostics retained, target response evidence");
}
