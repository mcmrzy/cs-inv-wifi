/* Generate main/direct/direct_boot_logo.c/.h from the CSERGY combined logo.
 *
 * Source: C:/Users/29538/Pictures/csergy/reference/brand/logo-combined-white-bg.png
 * Transform: flatten onto white -> grayscale -> ink mask where luminance < 200
 * -> crop to ink bbox -> split at the widest fully-empty column run (part 0 =
 * "CSERGY", part 1 = "\u8fb0\u70c1\u79d1\u6280") -> independently crop vertical bounds
 * -> stack centred at widths 276/176 px with an 18 px gap -> coverage downsample
 * of the original 1-bit mask (ink when
 * coverage >= COVER) -> 4-neighbour morphological close -> keep-strokes re-add
 * (ink when coverage >= KEEP). Strictly 1 bit, MSB-left, stride = ceil(width/8).
 *
 * Run from the project root: node tests/generate_boot_logo.cjs
 */
const fs=require('fs');
const sharp=require('C:/Users/29538/.cache/codex-runtimes/codex-primary-runtime/dependencies/node/node_modules/sharp');

const SRC=process.env.CSERGY_LOGO || 'C:/Users/29538/Pictures/csergy/reference/brand/logo-combined-white-bg.png';
const TARGET_W=Number(process.env.LOGO_PART_W || 276); /* Large upper CSERGY wordmark. */
const CJK_W=Number(process.env.LOGO_CJK_W || 176); /* Independent lower brand crop. */
const INK=Number(process.env.LOGO_INK || 200);         /* ink threshold on the flattened grayscale */
/* Per-part tuning: [0] = "CSERGY" (sparse, thick strokes), [1] = "\u8fb0\u70c1\u79d1\u6280"
 * (dense strokes, needs a higher threshold and no close or the counters blob). */
const COVER=[Number(process.env.LOGO_COVER || 0.38),Number(process.env.LOGO_COVER_CJK || 0.50)];
const KEEP=[Number(process.env.LOGO_KEEP || 0.50),Number(process.env.LOGO_KEEP_CJK || 0.50)];
const CLOSE=[process.env.LOGO_CLOSE!=='0',process.env.LOGO_CLOSE_CJK==='1'];

function dilate4(m,w,h)
{
    const o=new Uint8Array(m);
    for(let y=0;y<h;y++)for(let x=0;x<w;x++){
        if(m[y*w+x])continue;
        if((x>0&&m[y*w+x-1])||(x<w-1&&m[y*w+x+1])||(y>0&&m[(y-1)*w+x])||(y<h-1&&m[(y+1)*w+x]))o[y*w+x]=1;
    }
    return o;
}
function erode4(m,w,h)
{
    const o=new Uint8Array(m);
    for(let y=0;y<h;y++)for(let x=0;x<w;x++){
        if(!m[y*w+x])continue;
        /* Outside the glyph box counts as background, so the close cannot weld
         * the mark to the border. A pixel with 3 of 4 neighbours inked is kept
         * instead of eroded, which preserves thin strokes. */
        const up=y>0?m[(y-1)*w+x]:0, down=y<h-1?m[(y+1)*w+x]:0;
        const left=x>0?m[y*w+x-1]:0, right=x<w-1?m[y*w+x+1]:0;
        const n=up+down+left+right;
        o[y*w+x]=(n===4||n===3)?1:0;
    }
    return o;
}
/* Coverage-based downsample of a 1-bit source region. */
function coverageDownsample(mask,sw,sh,tw,th,cover,keep,close)
{
    const cov=new Float32Array(tw*th);
    for(let oy=0;oy<th;oy++){
        const y0=Math.floor(oy*sh/th), y1=Math.max(y0+1,Math.ceil((oy+1)*sh/th));
        for(let ox=0;ox<tw;ox++){
            const x0=Math.floor(ox*sw/tw), x1=Math.max(x0+1,Math.ceil((ox+1)*sw/tw));
            let ink=0,tot=0;
            for(let y=y0;y<y1&&y<sh;y++)for(let x=x0;x<x1&&x<sw;x++){tot++;if(mask[y*sw+x])ink++;}
            cov[oy*tw+ox]=tot?ink/tot:0;
        }
    }
    let out=new Uint8Array(tw*th);
    for(let i=0;i<out.length;i++)out[i]=cov[i]>=cover?1:0;
    if(close)out=erode4(dilate4(out,tw,th),tw,th);
    for(let i=0;i<out.length;i++)if(cov[i]>=keep)out[i]=1;
    return out;
}

(async()=>{
  const {data,info}=await sharp(SRC).flatten({background:'#ffffff'}).grayscale().raw().toBuffer({resolveWithObject:true});
  const W=info.width,H=info.height,CH=info.channels;
  if(CH!==1)throw new Error('expected 1 grayscale channel, got '+CH);
  const lum=(x,y)=>data[y*W+x];

  let minx=W,maxx=-1,miny=H,maxy=-1;
  for(let y=0;y<H;y++)for(let x=0;x<W;x++)if(lum(x,y)<INK){
    if(x<minx)minx=x;if(x>maxx)maxx=x;if(y<miny)miny=y;if(y>maxy)maxy=y;
  }
  if(maxx<0)throw new Error('no ink found');
  const bw=maxx-minx+1,bh=maxy-miny+1;

  const empty=[];
  for(let x=minx;x<=maxx;x++){
    let e=true;
    for(let y=miny;y<=maxy;y++)if(lum(x,y)<INK){e=false;break;}
    empty.push(e);
  }
  let runStart=-1,runLen=0,bestStart=-1,bestLen=0;
  for(let i=0;i<empty.length;i++){
    if(empty[i]){if(runStart<0)runStart=i;runLen++;if(runLen>bestLen){bestLen=runLen;bestStart=runStart;}}
    else{runStart=-1;runLen=0;}
  }
  if(bestLen<1)throw new Error('no fully-empty column run to split on');
  const splitL=minx+bestStart,splitR=splitL+bestLen-1;

  const ranges=[[minx,splitL-1],[splitR+1,maxx]];
  const parts=ranges.map(([a,b])=>{
    let l=b+1,r=a-1;
    for(let x=a;x<=b;x++){
      let any=false;
      for(let y=miny;y<=maxy;y++)if(lum(x,y)<INK){any=true;break;}
      if(any){if(x<l)l=x;if(x>r)r=x;}
    }
    if(r<l)throw new Error('empty part');
    let t=H,bottom=-1;
    for(let y=miny;y<=maxy;y++)for(let x=l;x<=r;x++)if(lum(x,y)<INK){t=Math.min(t,y);bottom=Math.max(bottom,y);}
    return {l,r,w:r-l+1,t,h:bottom-t+1};
  });

  const outParts=[];
  for(let pi=0;pi<parts.length;pi++){
    const p=parts[pi];
    const Wt=pi===0?TARGET_W:CJK_W;
    const Ht=Math.max(1,Math.round(p.h*Wt/p.w));
    const src=new Uint8Array(p.w*p.h);
    for(let y=0;y<p.h;y++)for(let x=0;x<p.w;x++)src[y*p.w+x]=lum(p.l+x,p.t+y)<INK?1:0;
    const mask=coverageDownsample(src,p.w,p.h,Wt,Ht,COVER[pi],KEEP[pi],CLOSE[pi]);
    const stride=Math.ceil(Wt/8);
    const bytes=Buffer.alloc(stride*Ht,0);
    for(let y=0;y<Ht;y++)for(let x=0;x<Wt;x++)if(mask[y*Wt+x])bytes[y*stride+(x>>3)]|=0x80>>(x&7);
    outParts.push({x_offset:Math.round((TARGET_W-Wt)/2),y_offset:pi===0?0:outParts[0].height+18,width:Wt,height:Ht,stride,bytes});
  }

  const bytesToC=b=>[...b].map(v=>'0x'+v.toString(16).padStart(2,'0')).join(',');
  const names=['cser_0','cser_1'];
  const desc=['"CSERGY"','"\u8fb0\u70c1\u79d1\u6280"'];

  let c='#include "direct_boot_logo.h"\n';
  c+='/* GNU/CSERGY boot mark, generated by tests/generate_boot_logo.cjs.\n';
  c+=' * Source: '+SRC+'\n';
  c+=' * Transform: flatten white -> grayscale -> ink lum<200 -> crop to ink box\n';
  c+=' * -> split at widest empty column run -> independent stacked crops\n';
  c+=' * '+TARGET_W+' px -> coverage downsample (CSERGY >= '+COVER[0].toFixed(2)+
     (CLOSE[0]?' + close':'')+', \u8fb0\u70c1\u79d1\u6280 >= '+COVER[1].toFixed(2)+
     (CLOSE[1]?' + close':'')+') -> keep-strokes re-add (>= '+KEEP[0].toFixed(2)+
     ' / '+KEEP[1].toFixed(2)+'). Strictly 1 bit, MSB-left rows.\n';
  c+=' * Part 0 = "CSERGY", part 1 = "\u8fb0\u70c1\u79d1\u6280". */\n';
  outParts.forEach((p,i)=>{
    c+=`static const uint8_t ${names[i]}[]={${bytesToC(p.bytes)}};\n`;
  });
  c+='const direct_boot_logo_part_t direct_boot_logo_parts[2]={\n';
  outParts.forEach((p,i)=>{
    c+=` {${p.x_offset},${p.y_offset},${p.width},${p.height},${p.stride},${names[i]}}, /* ${desc[i]} */\n`;
  });
  c+='};\n';
  const markWidth=Math.max(...outParts.map(p=>p.x_offset+p.width));
  c+=`const uint16_t direct_boot_logo_width=${markWidth};\n`;
  c+=`const uint16_t direct_boot_logo_height=${Math.max(...outParts.map(p=>p.y_offset+p.height))};\n`;
  c+='const char direct_boot_logo_marker[]="direct_boot_logo stacked parts=2 src=logo-combined-white-bg";\n';

  let h='#ifndef DIRECT_BOOT_LOGO_H\n#define DIRECT_BOOT_LOGO_H\n#include <stdint.h>\n';
  h+='/* CSERGY boot logo (1 bit per pixel, MSB-left rows, stride=ceil(width/8)).\n';
  h+=' * Generated by tests/generate_boot_logo.cjs from the brand PNG; part 0 is\n';
  h+=' * "CSERGY", part 1 is "\u8fb0\u70c1\u79d1\u6280". x_offset is relative to the mark. */\n';
  h+='typedef struct { uint16_t x_offset,y_offset; uint16_t width; uint16_t height; uint8_t stride; const uint8_t *data; } direct_boot_logo_part_t;\n';
  h+='extern const direct_boot_logo_part_t direct_boot_logo_parts[2];\n';
  h+='extern const uint16_t direct_boot_logo_width;\n';
  h+='extern const uint16_t direct_boot_logo_height;\n';
  h+='extern const char direct_boot_logo_marker[];\n';
  h+='#endif\n';

  fs.writeFileSync('main/direct/direct_boot_logo.c',c);
  fs.writeFileSync('main/direct/direct_boot_logo.h',h);
  const bytesTotal=outParts.reduce((a,p)=>a+p.bytes.length,0);
  console.log(`source ${W}x${H} inkbox ${bw}x${bh} splitcolumns ${splitL}..${splitR} (${bestLen} wide) cover=${COVER} keep=${KEEP} close=${CLOSE}`);
  outParts.forEach((p,i)=>console.log(`part${i} ${desc[i]} x_offset=${p.x_offset} width=${p.width} height=${p.height} stride=${p.stride} bytes=${p.bytes.length}`));
  console.log(`stacked logo width=${markWidth} height=${Math.max(...outParts.map(p=>p.y_offset+p.height))} total_bytes=${bytesTotal}`);
})().catch(e=>{console.error(e);process.exit(1);});
