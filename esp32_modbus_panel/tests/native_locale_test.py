"""Require a complete English translation for every native UI/catalog literal."""
from pathlib import Path
import json
import re
import subprocess
import sys
root=Path(__file__).resolve().parents[1]
texts=set()
for name in ['panel_ui.c','panel_catalog.c']:
 for text in re.findall(r'"([^"\n]*)"',(root/'main'/name).read_text(encoding='utf-8')):
  if any(ord(c)>127 for c in text):texts.add(text)
array=',\n'.join(json.dumps(t,ensure_ascii=False) for t in sorted(texts))
test=r'''
#include "panel_locale.h"
#include "direct_gfx.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static const char *const source[]={LITERALS};
int main(void){
 unsigned failures=0;
 for(unsigned i=0;i<sizeof(source)/sizeof(source[0]);i++){
  char out[256];snprintf(out,sizeof(out),"%s",source[i]);panel_locale_english(out,sizeof(out));
  for(const unsigned char *p=(const unsigned char *)out;*p;p++)if(*p>=128){
   fprintf(stderr,"UNTRANSLATED %s -> %s\n",source[i],out);failures++;break;
  }
 }
 assert(!failures);
 printf("PASS native English vocabulary: %u UI/catalog literals translated without Chinese fallback\n",(unsigned)(sizeof(source)/sizeof(source[0])));
}
'''.replace('LITERALS',array)
path=root/'tmp/native_locale_test.c';path.write_text(test,encoding='utf-8')
exe=path.with_suffix('.exe')
subprocess.run([sys.argv[1],'-std=c11','-Wall','-Wextra','-Werror','-I'+str(root/'main'),
 '-I'+str(root.parent/'esp32_direct/main/direct'),str(path),str(root/'main/panel_locale.c'),'-o',str(exe)],check=True)
subprocess.run([str(exe)],check=True)
