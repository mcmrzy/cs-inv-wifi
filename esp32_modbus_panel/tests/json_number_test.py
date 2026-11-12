"""Use the real IDF cJSON serializer to check the scoped numeric adapter."""
from pathlib import Path
import subprocess
import sys
root=Path(__file__).resolve().parents[1]
sdk=Path('D:/.espressif/v5.5.4/esp-idf/components/json/cJSON')
source=r'''
#include "cJSON.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <float.h>
int panel_json_scanf(const char *,const char *,...);
static void roundtrip(double n){
 cJSON *in=cJSON_CreateNumber(n);assert(in);
 char *s=cJSON_PrintUnformatted(in);assert(s);
 cJSON *out=cJSON_Parse(s);assert(out&&cJSON_IsNumber(out));
 if(isfinite(out->valuedouble))assert(fabs(out->valuedouble-n)<=fabs(n)*DBL_EPSILON);
 puts(s);cJSON_Delete(in);cJSON_Delete(out);free(s);
}
int main(void){
 const double cases[]={0,-0.0,1,-1,12.34,-52.7,0.001,1e30,1e-30,
  9007199254740991.,2147483648.,DBL_MAX,DBL_MIN,0.12345678901234567};
 for(unsigned i=0;i<sizeof(cases)/sizeof(*cases);i++)roundtrip(cases[i]);
 for(unsigned i=1;i<2000;i++)roundtrip((double)i*0.12345678901234567);
 double n=0;assert(panel_json_scanf("1.23e5","%lg",&n)==1&&n==123000);
 assert(panel_json_scanf("invalid","%lg",&n)==0);
 assert(panel_json_scanf("1","%d",&n)==0);
}
'''
path=root/'tmp/json_number_test.c';path.write_text(source,encoding='utf-8')
outputs=[]
for name,flags in [('original',[]),('adapted',['-Dsscanf=panel_json_scanf'])]:
 obj=root/f'tmp/json_number_{name}.o';exe=root/f'tmp/json_number_{name}.exe'
 subprocess.run([sys.argv[1],'-std=c11','-Os',*flags,'-I'+str(sdk),'-c',str(sdk/'cJSON.c'),'-o',str(obj)],check=True)
 subprocess.run([sys.argv[1],'-std=c11','-Wall','-Wextra','-Werror','-I'+str(sdk),str(path),str(root/'full/panel_json_number.c'),str(obj),'-o',str(exe)],check=True)
 outputs.append(subprocess.check_output([str(exe)]))
assert outputs[0]==outputs[1],'Numeric JSON output differs from original cJSON'
print('PASS JSON numeric formatting: 2013 real cJSON round trips match original, precision, exponents and invalid input')
