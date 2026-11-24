"""Rasterize the board's MiSans Medium glyphs at the sizes used on the LCD.

Pass MiSans/ttf/MiSans-Medium.ttf from the official MiSans.zip download.
"""
import argparse
import hashlib
import heapq
import re
from collections import Counter
from pathlib import Path

from fontTools.ttLib import TTFont
from PIL import Image, ImageDraw, ImageFont

root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('font', type=Path, help='Official MiSans-Medium.ttf')
args = parser.parse_args()
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
font_sha256 = hashlib.sha256(args.font.read_bytes()).hexdigest()
with TTFont(args.font) as source:
    cmap = source.getBestCmap()
missing = sorted(needed - cmap.keys())
if missing:
    parser.error('MiSans is missing required glyphs: ' + ', '.join(f'U+{cp:04X}' for cp in missing))
if ImageFont.truetype(args.font, 16).getname() != ('MiSans', 'Medium'):
    parser.error('Expected the official MiSans Medium face')

lines=['/* Generated board glyphs from MiSans Medium.',
       ' * Copyright 2020-2025 Beijing Xiaomi Mobile Software Co., Ltd.',
       ' * https://hyperos.mi.com/font/download',
       f' * Source font SHA256: {font_sha256} */',
       '#include "direct_gfx_assets.h"','#include <string.h>',
       'typedef struct {uint16_t cp;uint8_t width,height,stride,advance;uint16_t offset;} entry_t;',
       '_Static_assert(sizeof(entry_t)==8,"packed flash metrics");']
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


def rasterize(face, size, height, baseline, codepoints):
    font = ImageFont.truetype(args.font, size, layout_engine=ImageFont.Layout.BASIC)
    digits = '0123456789'
    digit_width = max(round(font.getlength(digit)) for digit in digits)
    metrics = {}
    for cp in sorted(codepoints):
        char = chr(cp)
        left, top, right, bottom = font.getbbox(char, anchor='ls')
        assert -baseline <= top <= bottom <= height - baseline, (size, char, top, bottom)
        advance = max(1, round(font.getlength(char)))
        x = max(0, -left)
        if char in digits:
            x += (digit_width - advance) // 2
            advance = digit_width
        width = max(advance, right + x)
        stride = (width + 1) // 2
        if size == 32 and char == ' ':
            width = stride = glyph_height = 0
            bitmap = b''
        else:
            glyph_height = height
            image = Image.new('L', (stride * 2, height))
            ImageDraw.Draw(image).text((x, baseline), char, font=font, fill=255, anchor='ls')
            alpha = image.tobytes()
            bitmap = bytes((alpha[i]*15+127)//255 << 4 | (alpha[i+1]*15+127)//255
                           for i in range(0, len(alpha), 2))
        assert stride * glyph_height == len(bitmap) <= (256 if size == 16 else 512)
        name = f'{face}_{cp:x}'
        encoded(name, bitmap)
        metrics[cp] = (width, glyph_height, stride, advance, name, 4)
    return metrics


def write_table(name, glyphs):
    lines.append(f'static const entry_t {name}[]={{')
    for cp, fields in glyphs.items():
        lines.append(f'{{0x{cp:x},' + ','.join(map(str, fields)) + '},')
    lines.append('};')


def update_digest(digest, cp, glyph):
    width, height, stride, advance, name, bpp = glyph
    digest.update(cp.to_bytes(4, 'little') + bytes((width, height, stride, advance, bpp))
                  + raw_glyphs[name])


body = rasterize('body', 16, 20, 15, needed)
write_table('body', body)
digest = hashlib.sha256()
for cp, glyph in body.items():
    update_digest(digest, cp, glyph)
lines.append(f'/* Body bitmap SHA256: {digest.hexdigest()} */')

# Pairing PINs, power readings and other numeric labels use native-size glyphs.
small = rasterize('small', 12, 12, 10, map(ord, '0123456789 .%+-:/WVAkHz'))
large = rasterize('large24', 24, 24, 21, map(ord, '0123456789 .-+%VWAmkHzhdins'))
headline = rasterize('headline', 32, 32, 29, map(ord, '0123456789 .%-VHzAkW'))
write_table('small12', small)
write_table('large24', large)
write_table('headline32', headline)
face_digest = hashlib.sha256()
for size, face in [(12, small), (24, large), (32, headline)]:
    for cp in range(32, 127):
        face_digest.update(size.to_bytes(4, 'little'))
        update_digest(face_digest, cp, face.get(cp, body[cp]))
lines.append(f'/* Face bitmap SHA256: {face_digest.hexdigest()} */')
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
lines=[re.sub(r',((?:body|small|large24|headline)_[0-9a-f]+),4(?=\})',
              lambda m:','+str(offsets[m[1]]),line) for line in lines]
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
assert all(cp<=0xffff for cp in body)
(root/'main/panel_fonts.c').write_text('\n'.join(lines)+'\n',encoding='utf-8',newline='\r\n')
print(f'Generated {len(body)} MiSans body glyphs and {len(small) + len(large) + len(headline)} numeric glyphs')
