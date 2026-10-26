"""Run the real boot SN selection for Device/Factory/Production profiles."""
from pathlib import Path
import subprocess,tempfile,unittest
ROOT=Path(__file__).resolve().parents[1]
GCC=r'C:\Users\29538\AppData\Local\Microsoft\WinGet\Packages\BrechtSanders.WinLibs.POSIX.UCRT_Microsoft.Winget.Source_8wekyb3d8bbwe\mingw64\bin\gcc.exe'
class Test(unittest.TestCase):
 def test_boot_prefers_persisted_factory_identity(self):
  text=(ROOT/'main/app/app_main.c').read_text(encoding='utf-8')
  start=text.index('static void ensure_sn(');end=text.index('/* ---- Key:',start)
  code=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "factory_record.h"
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define DEVICE_FIXED_SN "H1ZZX0013900001P"
#define SN_MAX_LEN 32
static char g_device_sn[32];
static bool persisted,override;
factory_result_t factory_identity_read(factory_record_t *r){if(!persisted)return FACTORY_UNPROVISIONED;strcpy(r->sn,"FACTORY000000001");return FACTORY_OK;}
#if !CONFIG_DIRECT_IDENTITY_PRODUCTION
static bool factory_identity_admin_sn(char *out,size_t n){if(override)snprintf(out,n,"ADMIN00000000001");return override;}
static bool storage_get_sn(char *out,size_t n){(void)out;(void)n;return false;}
static bool storage_set_sn(const char *s){assert(!strcmp(s,DEVICE_FIXED_SN));return true;}
#endif
FUNCTION
int main(void){
 ensure_sn();
#if CONFIG_DIRECT_IDENTITY_PRODUCTION
 assert(!g_device_sn[0]);
#else
 assert(!strcmp(g_device_sn,DEVICE_FIXED_SN));
#endif
 persisted=true;ensure_sn();assert(!strcmp(g_device_sn,"FACTORY000000001"));
 override=true;ensure_sn();
#if !CONFIG_DIRECT_IDENTITY_PRODUCTION
 assert(!strcmp(g_device_sn,"ADMIN00000000001"));
#endif
 return 0;
}
'''.replace('FUNCTION',text[start:end])
  with tempfile.TemporaryDirectory(dir=ROOT/'tmp') as folder:
   source=Path(folder)/'sn.c';exe=Path(folder)/'sn.exe';source.write_text(code,encoding='utf-8')
   for prod in (0,1):
    subprocess.run([GCC,'-save-temps=obj','-std=c11','-Wall','-Wextra','-Werror',f'-DCONFIG_DIRECT_IDENTITY_PRODUCTION={prod}','-I'+str(ROOT/'main/factory'),str(source),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
if __name__=='__main__':unittest.main()
