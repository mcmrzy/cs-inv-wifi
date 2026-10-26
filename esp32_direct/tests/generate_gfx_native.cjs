/* Deterministic native bitmap subset, no system-font rasterization.
 *
 * ---------------------------------------------------------------------------
 * LATIN_FONT selects the ASCII / Latin / digit source. This is the single
 * switch to flip when trying another Latin pixel font:
 *   'ark16'   Ark Pixel 16px proportional Latin (OFL-1.1, TakWolf)  <- current
 *   'unifont' GNU Unifont 16.0.04 (mixed into the CJK source anyway)
 * CJK always comes from Unifont 16x16: Ark's 16px CJK build is deprecated and
 * has coverage holes. The 12x24 instrument numerals are hand-authored below and
 * are unaffected by this switch.
 * ---------------------------------------------------------------------------
 */
const LATIN_FONT = process.env.DIRECT_LATIN_FONT || 'ark16';
const fs=require('fs'),zlib=require('zlib');
const UNIFONT_SRC=process.env.DIRECT_UNIFONT_PATH || 'tests/font-source/unifont-16.0.04.hex.gz';
const ARK_LATIN_SRC=process.env.DIRECT_ARK_LATIN_PATH || 'tests/font-source/ark-pixel-16px-proportional-latin.bdf';
const ASCII_CELL_H=16;   /* keep the 16-row cell the UI already lays out */
const ASCII_BASELINE=14; /* baseline row in the cell, matching Unifont's */
const all=new Map(zlib.gunzipSync(fs.readFileSync(UNIFONT_SRC)).toString('ascii').trim().split(/\r?\n/).map(l=>{let p=l.split(':');return [parseInt(p[0],16),Buffer.from(p[1],'hex')]}));
/* Minimal BDF reader for the proportional Latin font. */
function readBdf(file){
 const glyphs=new Map();let enc=null,dw=null,bbx=null,rows=null;
 for(const raw of fs.readFileSync(file,'utf8').split(/\r?\n/)){
  const line=raw.trim();
  if(line.startsWith('STARTCHAR')){enc=null;dw=null;bbx=null;rows=null;}
  else if(line.startsWith('ENCODING')){const p=line.split(/\s+/).slice(1).map(Number);enc=p[p.length-1];}
  else if(line.startsWith('DWIDTH')){dw=Number(line.split(/\s+/)[1]);}
  else if(line.startsWith('BBX')){const p=line.split(/\s+/).slice(1).map(Number);bbx={w:p[0],h:p[1],xoff:p[2],yoff:p[3]};}
  else if(line==='BITMAP'){rows=[];}
  else if(line==='ENDCHAR'){
   if(enc!=null&&enc>=0&&bbx&&dw!=null&&rows){
    const stride=Math.ceil(bbx.w/8);
    if(rows.length!==bbx.h)throw new Error(`${file}: row count mismatch U+${enc.toString(16)}`);
    const bytes=Buffer.alloc(Math.max(1,stride*bbx.h));
    rows.forEach((r,y)=>{Buffer.from(r.padEnd(stride*2,'0'),'hex').copy(bytes,y*stride,0,stride);});
    glyphs.set(enc,{w:bbx.w,h:bbx.h,xoff:bbx.xoff,yoff:bbx.yoff,dw,rows:bytes,stride});
   }
   enc=null;rows=null;
  }
  else if(rows&&/^[0-9a-fA-F]+$/.test(line))rows.push(line);
 }
 return glyphs;
}
const ark = LATIN_FONT==='ark16' ? readBdf(ARK_LATIN_SRC) : new Map();
/* Normalize a proportional BDF glyph into the fixed 16-row ASCII cell so the
 * existing layout (row0 drawn at y, 20 px rows) keeps its vertical rhythm. */
function asciiFromBdf(g){
 const width=Math.max(g.dw,g.xoff+g.w,1);
 const stride=Math.ceil(width/8);
 const bytes=Buffer.alloc(stride*ASCII_CELL_H,0);
 for(let r=0;r<g.h;r++){
  const row=ASCII_BASELINE-(g.yoff+g.h-1-r);
  if(row<0||row>=ASCII_CELL_H)continue;
  for(let c=0;c<g.w;c++){
   if(!(g.rows[r*g.stride+(c>>3)]&(0x80>>(c&7))))continue;
   const x=g.xoff+c;
   if(x<0||x>=width)continue;
   bytes[row*stride+(x>>3)]|=0x80>>(x&7);
  }
 }
 return {w:width,h:ASCII_CELL_H,stride,adv:g.dw,data:bytes};
}
function asciiFromUnifont(cp){
 const b=all.get(cp);if(!b)return null;
 const w=b.length===16?8:b.length===32?16:0;if(!w)return null;
 return {w,h:16,stride:w/8,adv:w,data:b};
}
const needed=new Set(Array.from({length:95},(_,i)=>i+32));
for(const ch of '市电光伏逆变器负载电池设置告警返回确定中文主题深色浅色语言')needed.add(ch.codePointAt(0));
for(const f of fs.readdirSync('main/direct').filter(f=>/\.(c|h|inc)$/.test(f) && !f.includes('assets'))){
 for(const ch of fs.readFileSync('main/direct/'+f,'utf8'))if(ch.codePointAt(0)>=0x3000 && ch.codePointAt(0)<=0xffff)needed.add(ch.codePointAt(0));
}
let out='#include "direct_gfx_assets.h"\n';
out+='/* Native 1-bit subset: no scaling, no smoothing, MSB-left rows.\n';
if(LATIN_FONT==='ark16')
 out+=' * Latin and digits: Ark Pixel 16px proportional Latin (OFL-1.1), TakWolf.\n *   source tests/font-source/ark-pixel-16px-proportional-latin.bdf,\n *   licence tests/font-source/ark-pixel-OFL-1.1.txt. Normalized into the\n *   16-row ASCII cell the UI lays out, baseline row '+ASCII_BASELINE+'.\n';
else
 out+=' * Latin and digits: GNU Unifont 16.0.04 (same source as the CJK glyphs).\n';
out+=' * CJK 16x16: GNU Unifont 16.0.04, OFL-1.1 or GPL-2+ with font embedding\n *   exception. Authors: Roman Czyborra, David Starner, Qianqian Fang,\n *   Paul Hardy, Andrew Miller and contributors. Notices: tests/font-license/.\n';
out+=' * Instrument numerals 12x24 are hand-authored proportional segments. */\n';
const entries=[];let total=0;
for(const cp of [...needed].sort((a,b)=>a-b)){
 let g=null,name=null;
 if(cp<0x2E80 && LATIN_FONT==='ark16'){
  const e=ark.get(cp);
  if(e){g=asciiFromBdf(e);name='a_'+cp.toString(16);}
  else console.error('WARN Ark Latin missing U+'+cp.toString(16)+', using Unifont');
 }
 if(!g){const u=asciiFromUnifont(cp);if(u){g=u;name='u_'+cp.toString(16);}}
 if(!g)throw Error('Missing Unifont U+'+cp.toString(16));
 if(g.w>255||g.h>255||g.stride>255||g.adv>255)throw Error('Metric overflow U+'+cp.toString(16));
 out+=`static const uint8_t ${name}[]={${[...g.data].map(v=>'0x'+v.toString(16).padStart(2,'0')).join(',')}};\n`;
 entries.push(`{0x${cp.toString(16)}, {${g.w},${g.h},${g.stride},${g.adv},${name}}}`);total+=g.data.length;
}
out+='typedef struct { uint32_t cp; direct_gfx_glyph_t glyph; } entry_t;\n';
out+=`static const entry_t native_font[]={${entries.join(',')}};\n`;
/* Native instrument numerals authored as fixed pixel segments.
   Each glyph is cropped to its tight horizontal ink box so cells are
   proportional (stride=ceil(width/8), advance=width+1). Rows stay 24 tall so
   the baseline is preserved (the glyph format carries no vertical offset). */
const digits=['abcdef','bc','abdeg','abcdg','bcfg','acdfg','acdefg','abc','abcdefg','abcdfg'],large=[];
for(const ch of '0123456789.-%'){
 const p=Array.from({length:24},()=>Array(12).fill(0));
 const rect=(x,y,w,h)=>{for(let yy=y;yy<y+h;yy++)for(let xx=x;xx<x+w;xx++)p[yy][xx]=1};
 const seg={a:[2,1,8,2],b:[9,3,2,8],c:[9,13,2,8],d:[2,21,8,2],e:[1,13,2,8],f:[1,3,2,8],g:[2,11,8,2]};
 if(ch>='0' && ch<='9')for(const s of digits[+ch])rect(...seg[s]);
 else if(ch==='.')rect(4,21,3,3);
 else if(ch==='-')rect(2,11,8,2);
 else {rect(1,2,4,5);rect(7,17,4,5);for(let yy=4;yy<=20;yy++){let xx=10-Math.floor((yy-4)*9/16);rect(xx,yy,2,1);}}
 let minx=12,maxx=-1;for(let y=0;y<24;y++)for(let x=0;x<12;x++)if(p[y][x]){if(x<minx)minx=x;if(x>maxx)maxx=x;}
 if(maxx<minx){minx=0;maxx=11;}
 const w=maxx-minx+1,stride=Math.ceil(w/8);
 const b=[];for(const row of p){const packed=new Array(stride).fill(0);for(let x=minx;x<=maxx;x++)if(row[x])packed[(x-minx)>>3]|=0x80>>((x-minx)&7);for(const byte of packed)b.push(byte);}
 const name='large_'+ch.codePointAt(0);out+=`static const uint8_t ${name}[]={${b.join(',')}};\n`;
 large.push(`{${ch.codePointAt(0)}, {${w},24,${stride},${w+1},${name}}}`);total+=b.length;
}
out+=`static const entry_t numerals[]={${large.join(',')}};\n`;
out+='static const direct_gfx_glyph_t *lookup(uint32_t cp){unsigned l=0,r=sizeof(native_font)/sizeof(native_font[0]);while(l<r){unsigned m=l+(r-l)/2;if(native_font[m].cp<cp)l=m+1;else r=m;}return l<sizeof(native_font)/sizeof(native_font[0]) && native_font[l].cp==cp?&native_font[l].glyph:0;}\n';
out+='int direct_gfx_has_glyph(uint32_t cp){return lookup(cp)!=0;}\n';
out+='const direct_gfx_glyph_t *direct_gfx_glyph(uint32_t cp,int size){if(size==22 || size==28)for(unsigned i=0;i<sizeof(numerals)/sizeof(numerals[0]);i++)if(numerals[i].cp==cp)return &numerals[i].glyph;const direct_gfx_glyph_t *g=lookup(cp);return g?g:lookup(63);}\n';
fs.writeFileSync('main/direct/direct_gfx_assets.c',out);
console.log(`Native glyphs ${needed.size}, bitmap bytes ${total}; large digits native 12x24.`);
