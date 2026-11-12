"""Execute production HMAC PIN derivation and real LCD SN-invalidation decisions."""
import hashlib
import hmac
import importlib.util
from pathlib import Path
import subprocess
import sys

root=Path(__file__).resolve().parents[1]
folder=root/'tmp/pairing';folder.mkdir(parents=True,exist_ok=True)
spec=importlib.util.spec_from_file_location('pairing_generator',root/'tools/generate_pairing_secret.py')
generator=importlib.util.module_from_spec(spec);spec.loader.exec_module(generator)
legacy=root.parent/'tools/pin_test_vectors.py'
secret=generator.legacy_secret(legacy)
header=folder/'panel_pairing_secret.h'
assert generator.generate(header,legacy,{})=='legacy-vectors'
original=header.read_bytes();mtime=header.stat().st_mtime_ns
assert generator.generate(header,legacy,{})=='legacy-vectors' and header.stat().st_mtime_ns==mtime
assert generator.generate(header,folder/'missing.py',{'PRODUCT_SECRET':'unit-test-override'})=='environment'
assert header.read_bytes()!=original
for bad in ['', '\0bad-key','short']:
    try:generator.generate(header,legacy,{'PRODUCT_SECRET':bad})
    except generator.SecretError:pass
    else:raise AssertionError('Invalid configuration accepted')
malicious=folder/'untrusted.py'
malicious.write_text('raise RuntimeError("must not execute")\nS1="unit-test"\nS2="-only"\nSECRET=(S1+S2).encode("utf-8")\n',encoding='utf-8')
assert generator.legacy_secret(malicious)==b'unit-test-only'
malicious.write_text('SECRET=__import__("os").getcwd()\n',encoding='utf-8')
try:generator.legacy_secret(malicious)
except generator.SecretError:pass
else:raise AssertionError('Unsupported secret expression accepted')
generator.generate(header,legacy,{})

sdk=Path('D:/.espressif/v5.5.4/esp-idf/components/mbedtls/mbedtls')
(folder/'pairing_mbedtls_config.h').write_text('#define MBEDTLS_MD_C\n#define MBEDTLS_SHA256_C\n#define MBEDTLS_PLATFORM_C\n',encoding='utf-8')
flags=['-std=c11','-Os','-I'+str(folder),'-I'+str(root/'full'),'-I'+str(root.parent/'esp32_direct/main/onboarding'),
       '-I'+str(sdk/'include'),'-I'+str(sdk/'library'),'-DMBEDTLS_CONFIG_FILE="pairing_mbedtls_config.h"']
objects=[]
for source in [sdk/'library'/name for name in ['md.c','sha256.c','platform_util.c','platform.c']]:
    target=folder/(source.stem+'.o');objects.append(target)
    subprocess.run([sys.argv[1],*flags,'-c',str(source),'-o',str(target)],check=True)
production=folder/'panel_pairing.o';objects.append(production)
subprocess.run([sys.argv[1],*flags,'-Wall','-Wextra','-Werror',
    '-Dmbedtls_md_info_from_type=pairing_test_md_info','-Dmbedtls_md_hmac=pairing_test_hmac',
    '-c',str(root/'full/panel_pairing.c'),'-o',str(production)],check=True)

samples=['H1CNA00135000014','H1CNA00135000015','H1CNA00135000016','H1DEA0012A00088X','MB00000000000000']
def pin(sn):return f'{int.from_bytes(hmac.new(secret,sn.encode("ascii"),hashlib.sha256).digest()[:3],"big")%1000000:06d}'
for n in range(1000):
    candidate=f'MB00{n:012X}'
    if pin(candidate).startswith('0'):
        samples.append(candidate);break
else:raise AssertionError('No leading-zero fixture')
vectors=','.join('{"'+s+'","'+pin(s)+'"}' for s in samples)
runtime=(root/'full/full_panel_runtime.inc').read_text(encoding='utf-8')
start=runtime.index('static onboarding_inputs_t panel_onboarding_inputs(')
end=runtime.index('static void panel_onboarding_close(',start)
onboarding=runtime[start:end]
assert 'panel_pairing_pin(g_device_sn,s_pair_pin)' in onboarding
assert '"pairing_ready",panel_pairing_pin(g_device_sn,pin)' in (root/'full/factory_service.c').read_text(encoding='utf-8')
source=r'''
#include "panel_pairing.h"
#include "onboarding_state.h"
#include "mbedtls/md.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
static unsigned hmac_calls;
static bool fail_md,fail_hmac;
const mbedtls_md_info_t *pairing_test_md_info(mbedtls_md_type_t type){
 return fail_md?NULL:mbedtls_md_info_from_type(type);
}
int pairing_test_hmac(const mbedtls_md_info_t *md,const unsigned char *key,size_t key_size,
 const unsigned char *input,size_t input_size,unsigned char *output){
 hmac_calls++;assert(key_size>=8&&input_size==16);
 if(fail_hmac)return -1;
 return mbedtls_md_hmac(md,key,key_size,input,input_size,output);
}
typedef enum {CLOUD_BINDING_UNKNOWN,CLOUD_BINDING_BOUND} cloud_binding_state_t;
static cloud_binding_state_t cloud_binding_get(void){return CLOUD_BINDING_UNKNOWN;}
static uint32_t clock_ms=1000;
static uint32_t now_ms(void){return clock_ms;}
static bool storage_has_wifi_config(void){return true;}
#define WIFI_DEFAULT_SSID ""
enum {UI_HOME,UI_PHONE_SETUP,UI_REVIEW,UI_MENU};
static struct {bool editing,pending;unsigned page;} s_ui;
static struct {bool ota_busy;} s_ui_data;
static bool direct_ui_has_alarm(const void *d){(void)d;return false;}
static char g_device_sn[33],s_pair_pin[7],s_pair_sn[17],s_dismissed_sn[33];
static bool s_pair_ready,s_pair_loaded,s_has_wifi,s_bound;
static uint32_t s_pair_refresh_ms;
static struct {bool ready;char payload[96];} s_onboarding_qr;
ONBOARDING
int main(void){
 static const struct {const char *sn,*expected;} vectors[]={VECTORS};
 char result[7];
 for(unsigned i=0;i<sizeof(vectors)/sizeof(*vectors);i++){
  assert(panel_pairing_pin(vectors[i].sn,result)&&!strcmp(result,vectors[i].expected));
 }
 assert(strlen(result)==6&&result[0]=='0');
 const char *bad[]={"","SHORT","h1cna00135000014","H1CNA0013500001-","H1CNA001350000140"};
 for(unsigned i=0;i<sizeof(bad)/sizeof(*bad);i++){
  strcpy(result,"123456");assert(!panel_pairing_pin(bad[i],result));
  for(unsigned j=0;j<7;j++)assert(result[j]==0);
 }
 strcpy(result,"123456");assert(!panel_pairing_pin(NULL,result)&&!result[0]);
 assert(!panel_pairing_pin(vectors[0].sn,NULL));
 fail_md=true;assert(!panel_pairing_pin(vectors[0].sn,result)&&!result[0]);fail_md=false;
 fail_hmac=true;assert(!panel_pairing_pin(vectors[0].sn,result)&&!result[0]);fail_hmac=false;
 strcpy(g_device_sn,vectors[0].sn);onboarding_inputs_t input=panel_onboarding_inputs(false);
 assert(input.identity_ready&&s_pair_ready&&!strcmp(s_pair_pin,vectors[0].expected));
 unsigned before=hmac_calls;clock_ms++;panel_onboarding_inputs(false);assert(hmac_calls==before);
 s_onboarding_qr.ready=true;strcpy(s_onboarding_qr.payload,"old-qr");
 strcpy(g_device_sn,vectors[1].sn);input=panel_onboarding_inputs(false);
 assert(input.identity_ready&&!strcmp(s_pair_pin,vectors[1].expected));
 assert(!s_onboarding_qr.ready&&!s_onboarding_qr.payload[0]&&hmac_calls==before+1);
 s_onboarding_qr.ready=true;strcpy(g_device_sn,"INVALID");input=panel_onboarding_inputs(false);
 assert(!input.identity_ready&&!s_pair_ready&&!s_pair_pin[0]&&!s_onboarding_qr.ready);
 strcpy(g_device_sn,vectors[0].sn);fail_hmac=true;input=panel_onboarding_inputs(false);
 assert(!input.identity_ready&&!s_pair_pin[0]);fail_hmac=false;
 clock_ms+=1000;input=panel_onboarding_inputs(false);assert(input.identity_ready);
 puts("PASS pairing: actual mbedTLS legacy vectors, leading zero, invalid SN, crypto failure and immediate SN/PIN/QR invalidation");
}
'''.replace('VECTORS',vectors).replace('ONBOARDING',onboarding)
test=folder/'pairing_test.c';test.write_text(source,encoding='utf-8')
exe=folder/'pairing_test.exe'
subprocess.run([sys.argv[1],*flags,'-Wall','-Wextra','-Werror',str(test),*map(str,objects),'-o',str(exe)],check=True)
subprocess.run([str(exe)],check=True)
print('PASS pairing build configuration: AST-only legacy import, environment override, unchanged-file reuse and invalid input rejection')
