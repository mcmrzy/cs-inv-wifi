"""Build a board-owned glyph subset from reachable product text.

Reuse existing licensed Noto glyphs; rasterize only missing CJK supplements.
Never link the original controller's entire parameter/alarm font dictionary.
"""
from pathlib import Path
import re
import hashlib
import heapq
from collections import Counter
from PIL import Image, ImageDraw, ImageFont
root=Path(__file__).resolve().parents[1]
base=(root.parent/'esp32_direct/main/direct/direct_gfx_assets.c').read_text(encoding='utf-8')
paths=[p for p in (root/'main').glob('*.c') if p.name!='panel_fonts.c']
paths+=list((root/'main').glob('panel_locale*.inc'))
paths+=[root/'full'/name for name in ['direct_ui.c','direct_ui_scene.c','direct_ui_locale.c','full_panel_runtime.inc','panel_lvgl.c','panel_wifi_ui.inc','panel_firmware_ui.inc','panel_weather_ui.inc','panel_weather_art.inc']]
paths+=[root.parent/'esp32_direct/main/direct/direct_onboarding_scene.c']
paths+=[root/'full'/'panel_product_ui.inc']
literals=[]
for path in paths:
    source=path.read_text(encoding='utf-8')
    literals+=re.findall(r'"(?:[^"\\]|\\.)*"',source)
needed=set(range(32,127))|{ord(c) for c in ''.join(literals) if ord(c)>=128}
body=re.search(r'static const entry_t body\[\]=\{(.*?)\};',base,re.S).group(1)
entries={int(cp,16):(cp,value) for cp,value in re.findall(r'\{0x([0-9a-f]+),(\{[^}]+\})\}',body)}
definitions={name:text for name,text in re.findall(r'(body_[0-9a-f]+)(\[\]=\{[^;]*;)',base)}
lines=['/* Generated board-only glyph subset. Noto Sans CJK SC Medium: SIL OFL 1.1. */',
       '#include "direct_gfx_assets.h"','#include <string.h>',
       'typedef struct {uint16_t cp;uint8_t width,height,stride,advance;uint16_t offset;} entry_t;',
       '_Static_assert(sizeof(entry_t)==8,"packed flash metrics");']
selected=[]
raw_glyphs={}
encoded_glyphs={}
def encoded(name,raw):
    raw=bytes(raw);raw_glyphs[name]=raw
    out=bytearray();i=0
    while i<len(raw):
        n=1
        while i+n<len(raw) and n<128 and raw[i+n]==raw[i]:n+=1
        if n>=3:out.extend((127+n,raw[i]));i+=n
        else:
            start=i;i+=1
            while i<len(raw) and i-start<128:
                if i+2<len(raw) and raw[i]==raw[i+1]==raw[i+2]:break
                i+=1
            out.append(i-start-1);out.extend(raw[start:i])
    encoded_glyphs[name]=bytes(out)
    return '@@'+name+'@@'
for cp in sorted(needed & entries.keys()):
    key,value=entries[cp];name='body_'+key
    raw=re.search(r'\{([^}]+)\}',definitions[name]).group(1)
    lines.append(encoded(name,[int(x,0) for x in raw.split(',') if x.strip()]));selected.append((cp,value))
font=ImageFont.truetype(str(root.parent/'esp32_direct/tests/font-source/noto/NotoSansCJKsc-Medium.otf'),16)
missing=sorted(needed-entries.keys())
for cp in missing:
    width=8 if cp<128 else 16
    # Share the original atlas' 20px cell and baseline. Ink-top anchoring puts
    # punctuation at the top and gives each supplemental glyph a new baseline.
    im=Image.new('L',(width,20));ImageDraw.Draw(im).text((0,15),chr(cp),font=font,fill=255,anchor='ls')
    pixels=list(im.tobytes())
    packed=[(pixels[i]*15//255<<4)|(pixels[i+1]*15//255) for i in range(0,width*20,2)]
    name=f'body_{cp:x}'
    lines.append(encoded(name,packed))
    selected.append((cp,'{'+f'{width},20,{width//2},{width},{name},4'+'}'))
lines.append('static const entry_t body[]={')
lines += [f'{{0x{cp:x},{value}}},' for cp,value in sorted(selected)]
lines.append('};')
digest=hashlib.sha256()
for cp,value in sorted(selected):
    fields=value.strip('{}').split(',');w,h,stride,advance=map(int,fields[:4]);name=fields[4];bpp=int(fields[5])
    assert stride*h==len(raw_glyphs[name]) and stride*h<=256
    digest.update(cp.to_bytes(4,'little')+bytes([w,h,stride,advance,bpp])+raw_glyphs[name])
lines.append('/* Body bitmap SHA256: '+digest.hexdigest()+' */')
# Phone pairing PIN and LCD test use native 22/24px numeric glyphs.
large=re.search(r'(static const uint8_t large24_.*?)(?=static const uint8_t large28_)',base,re.S).group(1)
large=re.sub(r'static const uint8_t (large24_[0-9a-f]+)\[\]=\{([^}]+)\};',
             lambda m:encoded(m[1],[int(x,0) for x in m[2].split(',') if x.strip()]),large)
lines.append(large)
small_font=ImageFont.truetype(str(root.parent/'esp32_direct/tests/font-source/noto/NotoSansCJKsc-Medium.otf'),12)
small=[]
for cp in sorted(set(map(ord,'0123456789 .%+-:/WVAkHz'))):
    ch=chr(cp);width=max(3,int(small_font.getlength(ch)+1));stride=(width+1)//2
    im=Image.new('L',(stride*2,12));ImageDraw.Draw(im).text((0,10),ch,font=small_font,fill=255,anchor='ls')
    pixels=list(im.tobytes());data=[(pixels[i]*15//255<<4)|(pixels[i+1]*15//255) for i in range(0,len(pixels),2)]
    name=f'small_{cp:x}';lines.append(encoded(name,data))
    small.append(f'{{0x{cp:x},{{{width},12,{stride},{width},{name},4}}}}')
lines.append('static const entry_t small12[]={'+','.join(small)+'};')
headline_font=ImageFont.truetype(str(root.parent/'esp32_direct/tests/font-source/noto/NotoSansCJKsc-Medium.otf'),32)
headline=[]
for cp in sorted(set(map(ord,'0123456789 .%-VHzAkW'))):
    ch=chr(cp);width=max(5,int(headline_font.getlength(ch)+1));stride=(width+1)//2
    im=Image.new('L',(stride*2,32));ImageDraw.Draw(im).text((0,29),ch,font=headline_font,fill=255,anchor='ls')
    pixels=list(im.tobytes());data=[(pixels[i]*15//255<<4)|(pixels[i+1]*15//255) for i in range(0,len(pixels),2)]
    name=f'headline_{cp:x}'
    if cp==32:
        lines.append(encoded(name,[]))
        headline.append(f'{{0x{cp:x},{{0,0,0,{width},{name},4}}}}')
    else:
        lines.append(encoded(name,data))
        headline.append(f'{{0x{cp:x},{{{width},32,{stride},{width},{name},4}}}}')
lines.append('static const entry_t headline32[]={'+','.join(headline)+'};')
def table_map(text):
    return {int(cp,16):fields.split(',') for cp,fields in re.findall(r'\{0x([0-9a-f]+),\{([^}]+)\}\}',text)}
body_map={cp:value.strip('{}').split(',') for cp,value in selected}
face_digest=hashlib.sha256()
for size,face_entries in [(12,table_map(','.join(small))),(24,table_map(large)),(32,table_map(','.join(headline)))]:
    for cp in range(32,127):
        fields=face_entries.get(cp,body_map[cp]);w,h,stride,advance=map(int,fields[:4]);bpp=int(fields[5])
        assert bpp==4 and stride*h<=512
        face_digest.update(size.to_bytes(4,'little')+cp.to_bytes(4,'little')+
                           bytes([w,h,stride,advance,bpp])+raw_glyphs[fields[4]])
lines.append('/* Face bitmap SHA256: '+face_digest.hexdigest()+' */')
counts=Counter(b for data in encoded_glyphs.values() for b in data)

def prefix_codes(frequencies):
    """Canonical codes for common RLE bytes and an escape for other bytes."""
    for limit in (64, 48, 32, 16):
        common=sorted(frequencies,key=lambda b:(-frequencies[b],b))[:limit]
        heap=[(frequencies[b],(b,)) for b in common]
        heap.append((sum(frequencies.values())-sum(frequencies[b] for b in common),(256,)))
        heapq.heapify(heap)
        lengths=Counter()
        while len(heap)>1:
            left,left_symbols=heapq.heappop(heap)
            right,right_symbols=heapq.heappop(heap)
            merged=left_symbols+right_symbols
            lengths.update(merged)
            heapq.heappush(heap,(left+right,merged))
        if max(lengths.values())<=8:
            break
    assert max(lengths.values())<=8
    codes={}
    prefix=previous_length=0
    for symbol,length in sorted(lengths.items(),key=lambda entry:(entry[1],entry[0])):
        prefix<<=length-previous_length
        codes[symbol]=(prefix,length)
        prefix+=1
        previous_length=length
    return codes


def pack_prefix(data,codes):
    result=bytearray()
    bits=count=0
    for b in data:
        prefix,length=codes.get(b,codes[256])
        if b not in codes:
            prefix=(prefix<<8)|b
            length+=8
        bits=(bits<<length)|prefix
        count+=length
        while count>=8:
            count-=8
            result.append(bits>>count)
            bits&=(1<<count)-1
    if count:
        result.append(bits<<(8-count))
    return result


codes=prefix_codes(counts)
prefix_table=[0]*256
for symbol,(prefix,length) in codes.items():
    repeat=1<<(8-length)
    entry=(length<<8)|(0x1000 if symbol==256 else symbol)
    prefix_table[prefix*repeat:(prefix+1)*repeat]=[entry]*repeat
assert all(prefix_table)
lines.append('static const uint16_t font_prefix[256]={'+','.join(map(str,prefix_table))+'};')
blobs={face:bytearray() for face in ('body','small','large24','headline')}
offsets={}
for name,data in encoded_glyphs.items():
    packed=pack_prefix(data,codes)
    raw=raw_glyphs[name];pixels=[n for b in raw for n in [b>>4,b&15]];runs=[];i=0
    while i<len(pixels):
        n=1
        while i+n<len(pixels) and n<16 and pixels[i+n]==pixels[i]:n+=1
        runs.append(((n-1)<<4)|pixels[i]);i+=n
    mode,payload=min(enumerate([data,packed,bytes(runs),raw]),key=lambda item:len(item[1]))
    blob=blobs[name.split('_',1)[0]]
    offsets[name]=len(blob)
    blob.extend([mode,*payload])
lines=[re.sub(r'@@[a-z0-9_]+@@\n?', '', re.sub(r',((?:body|small|large24|headline)_[0-9a-f]+),4(?=\}\})',
              lambda m:','+str(offsets[m[1]]),line)) for line in lines]
for face,blob in blobs.items():
    blob.append(0)  # The prefix decoder peeks one byte ahead.
    assert len(blob)<=65536
    lines.append('static const uint8_t '+face+'_blob[]={')
    lines.extend(','.join(map(str,blob[start:start+24]))+',' for start in range(0,len(blob),24))
    lines.append('};')
lines+=['static const entry_t *lookup(const entry_t *t,unsigned n,uint32_t cp){unsigned l=0,r=n;while(l<r){unsigned m=l+(r-l)/2;if(t[m].cp<cp)l=m+1;else r=m;}return l<n&&t[l].cp==cp?&t[l]:0;}',
'#define FIND(t,cp) lookup(t,sizeof(t)/sizeof(t[0]),cp)',
'int direct_gfx_has_glyph(uint32_t cp){return FIND(body,cp)!=0;}',
'''/* The LCD owner consumes one glyph at a time. Cache a losslessly decoded
 * bitmap instead of reserving a second full font or reducing glyph quality. */
const direct_gfx_glyph_t *panel_font_glyph_info(uint32_t cp,int size){
 const entry_t *g=0;const uint8_t *base=body_blob;
 if(size==32){g=FIND(headline32,cp);if(g)base=headline_blob;}
 if(size==12){g=FIND(small12,cp);if(g)base=small_blob;}
 if(size==22||size==24){g=FIND(large24,cp);if(g)base=large24_blob;}
 if(!g)g=FIND(body,cp);
 if(!g)g=FIND(body,63);
 static direct_gfx_glyph_t metrics;
 metrics=(direct_gfx_glyph_t){g->width,g->height,g->stride,g->advance,base+g->offset,4};
 return &metrics;
}
static unsigned font_peek(const uint8_t *at,unsigned shift){
 return ((((unsigned)at[0]<<8)|at[1])>>(8-shift))&255U;
}
static uint8_t font_byte(const uint8_t **at,unsigned *shift){
 unsigned entry=font_prefix[font_peek(*at,*shift)];
 unsigned bits=*shift+((entry>>8)&15U);
 *at+=bits/8;*shift=bits%8;
 if(entry&0x1000U){
  unsigned value=font_peek(*at,*shift);
  (*at)++;
  return (uint8_t)value;
 }
 return (uint8_t)entry;
}
const direct_gfx_glyph_t *direct_gfx_glyph(uint32_t cp,int size){
 const direct_gfx_glyph_t *g=panel_font_glyph_info(cp,size);
 static direct_gfx_glyph_t decoded;static uint8_t pixels[512];
 unsigned length=g->stride*g->height,out=0,shift=0;const uint8_t *at=g->data;
 unsigned mode=*at++;
 if(mode==3)memcpy(pixels,at,length);
 else if(mode==2){
  unsigned pixel=0;
  while(pixel<length*2){unsigned token=*at++,count=(token>>4)+1U;
   while(count-- && pixel<length*2){
    if(pixel&1U)pixels[pixel/2]|=token&15U;
    else pixels[pixel/2]=(uint8_t)(token<<4);
    pixel++;
   }
  }
 }else while(out<length){unsigned header=mode?font_byte(&at,&shift):*at++,count=(header&127U)+1U;
  if(header&128U){uint8_t value=mode?font_byte(&at,&shift):*at++;while(count-- && out<length)pixels[out++]=value;}
  else while(count-- && out<length)pixels[out++]=mode?font_byte(&at,&shift):*at++;
 }
 decoded=*g;decoded.data=pixels;
 return &decoded;
}''']
assert all(cp<=0xffff for cp,value in selected)
result='\n'.join(lines)+'\n'
result=re.sub(r'\{(0x[0-9a-f]+),\{([^}]+)\}\}',r'{\1,\2}',result)
(root/'main/panel_fonts.c').write_text(result,encoding='utf-8')
print(f'Generated board glyph subset: {len(selected)} body glyphs, {len(entries)-len(needed&entries.keys())} old body glyphs removed, {len(missing)} supplements')
