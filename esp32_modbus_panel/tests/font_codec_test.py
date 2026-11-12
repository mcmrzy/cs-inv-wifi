"""Verify every C-decoded glyph against the pre-compression bitmap digest."""
from pathlib import Path
import hashlib,re,subprocess,sys
root=Path(__file__).resolve().parents[1]
source=(root/'main/panel_fonts.c').read_text(encoding='utf-8')
expected=re.search(r'Body bitmap SHA256: ([0-9a-f]{64})',source).group(1)
test=root/'tmp/font_codec_test.c'
test.write_text(r'''
#include "direct_gfx_assets.h"
#include <assert.h>
#include <stdio.h>
#include <fcntl.h>
#include <io.h>
int main(int argc,char **argv){
 (void)argv;
 _setmode(_fileno(stdout),_O_BINARY);
 if(argc>1){
  const unsigned sizes[]={12,24,32};
  for(unsigned i=0;i<3;i++)for(uint32_t cp=32;cp<127;cp++){
   unsigned size=sizes[i];const direct_gfx_glyph_t *g=direct_gfx_glyph(cp,(int)size);
   unsigned char header[]={size,0,0,0,cp,0,0,0,g->width,g->height,g->stride,g->advance,g->bpp};
   assert(fwrite(header,1,sizeof(header),stdout)==sizeof(header));
   assert(fwrite(g->data,1,g->stride*g->height,stdout)==(unsigned)g->stride*g->height);
  }
  return 0;
 }
 for(uint32_t cp=0;cp<0x110000;cp++)if(direct_gfx_has_glyph(cp)){
  const direct_gfx_glyph_t *g=direct_gfx_glyph(cp,16);
  unsigned char header[]={cp,cp>>8,cp>>16,cp>>24,g->width,g->height,g->stride,g->advance,g->bpp};
  assert(fwrite(header,1,sizeof(header),stdout)==sizeof(header));
  assert(fwrite(g->data,1,g->stride*g->height,stdout)==(unsigned)g->stride*g->height);
  /* Exercise the mixed body/large fallback used by LVGL numeric labels. */
  direct_gfx_glyph('8',24);direct_gfx_glyph('W',24);direct_gfx_glyph(cp,16);
 }
 const direct_gfx_glyph_t *comma=direct_gfx_glyph(0xff0c,16);
 assert(comma->height==20);
 unsigned first=20,last=0;
 for(unsigned y=0;y<comma->height;y++)for(unsigned x=0;x<comma->width;x++)
  if((comma->data[y*comma->stride+x/2]>>(x%2?0:4))&15){if(y<first)first=y;last=y;}
 assert(first>=10 && last>=16); /* Noto comma straddles the baseline in the lower half of the cell. */
 return 0;
}
''',encoding='utf-8')
exe=root/'tmp/font_codec_test.exe'
subprocess.run([sys.argv[1],'-std=c11','-Wall','-Wextra','-Werror',
 '-I'+str(root.parent/'esp32_direct/main/direct'),str(test),str(root/'main/panel_fonts.c'),'-o',str(exe)],check=True)
data=subprocess.check_output([str(exe)])
assert hashlib.sha256(data).hexdigest()==expected
faces=subprocess.check_output([str(exe),'faces'])
assert hashlib.sha256(faces).hexdigest()==re.search(r'Face bitmap SHA256: ([0-9a-f]{64})',source).group(1)
print('PASS font codec: every CJK/Latin glyph losslessly restored, mixed numeric fallback and cache changes')
