#include "admin_sn_command.h"
#include "factory_wire.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static factory_parser_t parser;
static factory_frame_t request;
static admin_sn_input_t admin_input;
static unsigned commands,auth_calls,frames;
static admin_sn_result_t last_result;
static char saved[17];
static char saved_pin[7];
static uint8_t saved_bundle[64];
static bool admin_sn_password_valid(const char *p){auth_calls++;return !strcmp(p,"test-pass");}
static void process(void){frames++;}
static void serial_result(admin_sn_result_t result,const char *sn){if(result==ADMIN_SN_NONE)return;last_result=result;commands++;if(result==ADMIN_SN_VALID)snprintf(saved,sizeof(saved),"%s",sn);if(result==ADMIN_SN_PIN_VALID)memcpy(saved_pin,admin_input.pin,7);if(result==ADMIN_SN_DSP_VALID)memcpy(saved_bundle,admin_input.dsp_bundle,64);}
static void feed(const unsigned char *rx,int n){
#include "../tmp/serial_mux_under_test.inc"
}
static void line(const char *s){feed((const unsigned char*)s,strlen(s));}
int main(void){
 line("set_sn test-pass H1ZZX00139000000\r\n");assert(commands==1&&last_result==ADMIN_SN_VALID&&!strcmp(saved,"H1ZZX00139000000"));
 assert(!admin_input.used);for(unsigned i=0;i<sizeof(admin_input.line);i++)assert(!admin_input.line[i]);
 line("set_sn wrong SECRET123\n");assert(last_result==ADMIN_SN_AUTH_FAILED&&!strcmp(saved,"H1ZZX00139000000"));
 line("set_sn test-pass BAD_SN\n");assert(last_result==ADMIN_SN_INVALID_ARGS);
 line("set_sn test-pass ABCDEFGHIJKLMNOPQ\n");assert(last_result==ADMIN_SN_INVALID_ARGS);
 line("set_sn test-pass NEW123 extra\n");assert(last_result==ADMIN_SN_INVALID_ARGS);
 line("status\n");assert(last_result==ADMIN_SN_STATUS);
 line("set_pin test-pass H1ZZX00139000000 000012\r\n");assert(last_result==ADMIN_SN_PIN_VALID&&!strcmp(saved_pin,"000012")&&!admin_input.pin[0]);
 line("set_pin wrong H1ZZX00139000000 000012\n");assert(last_result==ADMIN_SN_AUTH_FAILED);
 line("set_pin test-pass H1ZZX00139000000 00001x\n");assert(last_result==ADMIN_SN_INVALID_ARGS);
 factory_frame_t frame={.command=4,.id=9};const char *payload="set_sn test-pass FAKE123\n";
 memcpy(frame.payload,payload,strlen(payload));frame.length=(uint16_t)strlen(payload);unsigned char wire[FACTORY_FRAME_MAX];size_t n=factory_frame_encode(&frame,wire,sizeof(wire));
 unsigned before=commands,auth_before=auth_calls;feed(wire,n);assert(frames==1&&commands==before&&auth_calls==auth_before);
 wire[n-1]^=1;feed(wire,n);assert(frames==1&&commands==before);
 line("set_sn test-pass NEW123\n");assert(last_result==ADMIN_SN_VALID&&!strcmp(saved,"NEW123"));
 unsigned char noise[sizeof(admin_input.line)+10];memset(noise,'A',sizeof(noise));feed(noise,sizeof(noise));line("\n");assert(last_result==ADMIN_SN_TOO_LONG);
 char command[256];char hex[129];memset(hex,'A',128);hex[128]=0;
 snprintf(command,sizeof(command),"set_dsp test-pass H1ZZX00139000079 %s\n",hex);line(command);
 assert(last_result==ADMIN_SN_DSP_VALID && saved_bundle[0]==0xaa && saved_bundle[63]==0xaa);
 for(unsigned i=0;i<64;i++)assert(!admin_input.dsp_bundle[i]);
 snprintf(command,sizeof(command),"set_dsp wrong H1ZZX00139000079 %s\n",hex);line(command);assert(last_result==ADMIN_SN_AUTH_FAILED);
 hex[127]='z';snprintf(command,sizeof(command),"set_dsp test-pass H1ZZX00139000079 %s\n",hex);line(command);assert(last_result==ADMIN_SN_INVALID_ARGS);
 line("dsp_params\n");assert(last_result==ADMIN_SN_DSP_STATUS);
 line("dsp_boot test-pass\n");assert(last_result==ADMIN_SN_DSP_BOOT);
 line("dsp_run test-pass\n");assert(last_result==ADMIN_SN_DSP_RUN);
 line("dsp_reboot test-pass\n");assert(last_result==ADMIN_SN_DSP_REBOOT);
 line("dsp_boot wrong\n");assert(last_result==ADMIN_SN_AUTH_FAILED);
 line("dsp_run test-pass extra\n");assert(last_result==ADMIN_SN_INVALID_ARGS);
 for(unsigned i=0;i<sizeof(admin_input.line);i++)assert(!admin_input.line[i]);
 line("set_sn test-pass OK9X\b\n");assert(last_result==ADMIN_SN_VALID&&!strcmp(saved,"OK9"));
 puts("PASS serial administrator: syntax/auth/length/cleartext wipe/CRLF/backspace; actual binary/text mux rejects binary command injection");
}
