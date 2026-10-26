#!/usr/bin/env python3
"""Deterministically encode the approved transparent house at native LCD size.

Requires Pillow 12.3.0 for byte-identical regeneration. No cropping, retouching,
generated replacement or background flattening is applied. --check compares the
generated source without writing. Optional --preview renders the indexed image.
"""
import argparse
import hashlib
from pathlib import Path

from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
SOURCE = Path("docs/ui_redesign/lcd_optimization_20261016/assets/home-energy-house-v1.png")
OUTPUT = ROOT / "main/direct/direct_home_asset.c"
WIDTH, HEIGHT = 320, 192

DRAW = r'''
static uint16_t blend(const uint8_t *p, uint16_t bg)
{
 unsigned a=p[3];
 if(a==255)return (uint16_t)(((p[0]>>3)<<11)|((p[1]>>2)<<5)|(p[2]>>3));
 unsigned br=((bg>>11)&31)*255/31, bgc=((bg>>5)&63)*255/63, bb=(bg&31)*255/31;
 unsigned r=(p[0]*a+br*(255-a)+127)/255;
 unsigned g=(p[1]*a+bgc*(255-a)+127)/255;
 unsigned b=(p[2]*a+bb*(255-a)+127)/255;
 return (uint16_t)(((r>>3)<<11)|((g>>2)<<5)|(b>>3));
}

void direct_home_asset_draw(direct_gfx_t *g,int x,int y,uint16_t background)
{
 if(!g || !g->pixels || g->width<=0 || g->height<=0)return;
 int64_t left=x, right=(int64_t)x+DIRECT_HOME_ASSET_WIDTH;
 int64_t top=y, bottom=(int64_t)y+DIRECT_HOME_ASSET_HEIGHT;
 if(left<0)left=0;
 if(right>g->width)right=g->width;
 if(top<g->y0)top=g->y0;
 if(bottom>(int64_t)g->y0+g->height)bottom=(int64_t)g->y0+g->height;
 if(left>=right || top>=bottom)return;
 for(int64_t row=top;row<bottom;row++){
  size_t dst=(size_t)(row-g->y0)*(size_t)g->width+(size_t)left;
  unsigned source_row=(unsigned)(row-y), col=0;
  size_t at=row_offsets[source_row], end=row_offsets[source_row+1];
  unsigned first=(unsigned)(left-x), last=(unsigned)(right-x);
  while(at<end && col<last){
   unsigned header=packed_indices[at++], count=(header&127U)+1U;
   bool repeated=(header&128U)!=0;
   size_t data=at;at+=repeated?1U:count;
   unsigned begin=col>first?col:first, stop=col+count<last?col+count:last;
   if(begin<stop){
    if(repeated){
     const uint8_t *rgba=palette+(size_t)packed_indices[data]*4;
     if(rgba[3]){
      uint16_t color=blend(rgba,background);
      for(unsigned c=begin;c<stop;c++)g->pixels[dst+c-first]=color;
     }
    }else for(unsigned c=begin;c<stop;c++){
     const uint8_t *rgba=palette+(size_t)packed_indices[data+c-col]*4;
     if(rgba[3])g->pixels[dst+c-first]=blend(rgba,background);
    }
   }
   col+=count;
  }
 }
}
'''


def array(name, data):
    rows = [" "+",".join(str(b) for b in data[i:i+24])+"," for i in range(0,len(data),24)]
    return f"static const uint8_t {name}[{len(data)}]={{\n"+"\n".join(rows)+"\n};\n"


def encode_rows(indices, width, height):
    """Literal packets 1..128; repeat packets 3..128, independently per row."""
    assert len(indices) == width*height
    packed, offsets = bytearray(), [0]
    for y in range(height):
        row = indices[y*width:(y+1)*width]
        i = 0
        while i < width:
            count = 1
            while i+count < width and row[i+count] == row[i] and count < 128:
                count += 1
            if count >= 3:
                packed.extend((127+count,row[i]))
                i += count
            else:
                start = i
                i += 1
                while i < width and i-start < 128:
                    if i+2 < width and row[i] == row[i+1] == row[i+2]:
                        break
                    i += 1
                packed.append(i-start-1)
                packed.extend(row[start:i])
        offsets.append(len(packed))
    assert len(packed) <= 65535
    return bytes(packed), offsets


def decode_rows(packed, offsets):
    decoded = bytearray()
    for start,end in zip(offsets,offsets[1:]):
        while start < end:
            header = packed[start]
            start += 1
            count = (header & 127)+1
            if header & 128:
                decoded.extend([packed[start]]*count)
                start += 1
            else:
                decoded.extend(packed[start:start+count])
                start += count
        assert start == end
    return bytes(decoded)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true")
    parser.add_argument("--preview", type=Path)
    args = parser.parse_args()
    if Image.__version__ != "12.3.0":
        raise SystemExit("Use Pillow 12.3.0 for reproducible palette assignment")
    image = Image.open(ROOT / SOURCE).convert("RGBA")
    if image.size != (1619,971):
        raise SystemExit("Approved asset must retain original 1619x971 composition")
    image = image.resize((WIDTH,HEIGHT),Image.Resampling.LANCZOS)
    indexed = image.quantize(colors=256,method=Image.Quantize.FASTOCTREE,dither=Image.Dither.NONE)
    palette = indexed.getpalette("RGBA")
    if len(palette) != 1024:
        raise SystemExit("Expected 256-entry RGBA palette")
    packed, offsets = encode_rows(indexed.tobytes(),WIDTH,HEIGHT)
    assert decode_rows(packed,offsets) == indexed.tobytes(), "Lossless row encoding failed"
    resource_bytes = len(packed)+len(offsets)*2+len(palette)
    source_hash = hashlib.sha256((ROOT / SOURCE).read_bytes()).hexdigest()
    result = (
        "/* Generated by tools/generate_home_asset.py; do not edit pixel data.\n"
        f" * Source: {SOURCE.as_posix()}\n * Source SHA256: {source_hash}\n"
        " * Pillow 12.3.0: LANCZOS 320x192, RGBA FASTOCTREE 256, no dither.\n"
        f" * Lossless row PackBits: {len(packed):,} data + {len(offsets)*2:,} offsets + 1,024 palette = {resource_bytes:,} bytes.\n */\n"
        '#include "direct_home_asset.h"\n#include <stddef.h>\n\n'
        + array("palette",palette)
        + f"static const uint16_t row_offsets[{len(offsets)}]={{"+','.join(map(str,offsets))+"};\n"
        + array("packed_indices",packed) + DRAW
    )
    if args.check:
        if not OUTPUT.exists() or OUTPUT.read_text(encoding="utf-8") != result:
            raise SystemExit("House asset differs; regenerate with pinned Pillow")
    else:
        OUTPUT.write_text(result,encoding="utf-8",newline="\n")
    if args.preview:
        indexed.convert("RGBA").save(args.preview)
    print(f"House asset: {WIDTH}x{HEIGHT}, {resource_bytes} resource bytes, lossless round trip verified, source SHA256 {source_hash}")


if __name__ == "__main__":
    main()
