
const fs = require('fs');
const path = require('path');
const sharp = require('C:/Users/29538/.cache/codex-runtimes/codex-primary-runtime/dependencies/node/node_modules/sharp');
const out = __dirname;
const C={bg:'#070b10',fg:'#eff5fb',muted:'#91a0af',dim:'#42505d',line:'#26333f',green:'#72e0ac',gold:'#e6ba69',blue:'#83bfff',red:'#ff807c'};
const text=(x,y,s,size=12,color=C.fg,weight=400,anchor='start')=>'<text x="'+x+'" y="'+y+'" font-size="'+size+'" fill="'+color+'" font-weight="'+weight+'" text-anchor="'+anchor+'">'+s+'</text>';
const line=(d,color=C.dim,width=1.5,extra='')=>'<path d="'+d+'" fill="none" stroke="'+color+'" stroke-width="'+width+'" stroke-linecap="round" stroke-linejoin="round" '+extra+'/>';
const rect=(x,y,w,h,color,rx=0,stroke='none')=>'<rect x="'+x+'" y="'+y+'" width="'+w+'" height="'+h+'" rx="'+rx+'" fill="'+color+'" stroke="'+stroke+'"/>';
const circle=(x,y,r,color)=>'<circle cx="'+x+'" cy="'+y+'" r="'+r+'" fill="'+color+'"/>';
const icon=(name,x,y,color=C.fg,scale=1)=>{
 let d='';
 switch(name){
 case 'grid':d='M4 31L15 2L26 31M15 2V31M7 23H23M9 15H21M4 10H26M7 5H23M9 15L21 23M21 15L7 23M7 23L21 31M23 23L9 31M2 10V14M28 10V14M5 5V8M25 5V8M1 31H29';break;
 case 'solar':d='M2 13H29L34 30H-3ZM11 13L8 30M20 13L23 30M0 21H31M12 31V34M20 31V34M8 34H24M30 3V0M30 9V6M24 4H21M39 4H36M25 0L23 -2M35 8L37 10';break;
 case 'house':d='M1 17L18 3L35 17M6 14V34H30V14M14 34V22H22V34M8 19H12V24H8ZM25 19H28V24H25';break;
 case 'battery':d='M1 3H34V21H1ZM35 8H39V16H35M6 7H11V17H6ZM14 7H19V17H14ZM22 7H27V17H22';break;
 case 'generator':d='M1 9H33V30H1ZM6 3H24L29 9M6 14H14V19H6ZM6 23H14M21 12L18 20H24L21 28M5 31V34M28 31V34';break;
 case 'inverter':d='M1 1H36V48H1ZM5 7H32V20H5ZM8 14C12 5 15 23 19 14S25 6 29 14M6 27H30M6 32H30M6 37H22M6 43H8M12 43H14';break;
 case 'arrow':d='M0 0H11M7 -3L11 0L7 3';break;
 case 'check':d='M0 6L4 10L12 1';break;
 case 'warn':d='M12 1L24 22H0ZM12 8V14M12 18V19';break;
 }
 return '<g transform="translate('+x+' '+y+') scale('+scale+')" stroke="'+color+'" stroke-width="1.5" fill="none" stroke-linecap="round" stroke-linejoin="round">'+line(d,color,1.5)+'</g>';
};
const base=(body)=>'<svg xmlns="http://www.w3.org/2000/svg" width="320" height="240" viewBox="0 0 320 240"><defs><style>text{font-family:"Segoe UI","Microsoft YaHei",sans-serif;font-variant-numeric:tabular-nums}</style></defs>'+rect(0,0,320,240,C.bg)+body+'</svg>';
const header=(title,right)=>text(12,18,title,12,C.fg,600)+text(308,18,right,11,C.muted,400,'end')+line('M12 27H308',C.line,1);
const foot=(left,right)=>line('M12 221H308',C.line,1)+text(12,235,left,10,C.muted)+text(308,235,right,10,C.muted,400,'end');
let h=header('CS-L10  /  6K2','18:09');
h+=circle(108,14,2.5,C.green)+text(115,18,'DSP',10,C.muted)+line('M144 17V14M149 17V11M154 17V8',C.blue,2);
// Four cardinal nodes. Flow segments are strictly horizontal or vertical.
h+=line('M60 124H134',C.green,2)+icon('arrow',116,124,C.green);
h+=line('M160 65V95',C.gold,2)+line('M157 90L160 95L163 90',C.gold,2);
h+=line('M184 124H247',C.green,2)+icon('arrow',229,124,C.green);
h+=line('M160 153V176',C.green,2)+line('M157 171L160 176L163 171',C.green,2);
h+=icon('grid',22,108,C.blue)+text(38,98,'AC INPUT',10,C.muted,400,'middle')+text(38,164,'650',21,C.fg,600,'middle')+text(62,164,'W',10,C.muted)+text(38,183,'230 V',11,C.muted,400,'middle');
h+=icon('solar',146,30,C.gold,.82)+text(127,52,'PV',11,C.muted,400,'end')+text(190,55,'1800',20,C.fg,600)+text(240,55,'W',10,C.muted)+text(190,72,'310 V',11,C.muted);
h+=icon('inverter',140,100,C.fg,1.08);
h+=icon('house',254,108,C.fg)+text(272,98,'LOAD',10,C.muted,400,'middle')+text(265,166,'2050',25,C.fg,600,'middle')+text(301,166,'W',10,C.muted);

h+=icon('battery',142,180,C.green,.9)+text(210,200,'76',25,C.fg,600)+text(241,200,'%',11,C.muted)+text(160,216,'51.8 V',12,C.muted,400,'middle')+text(125,198,'BATTERY',10,C.muted,400,'end');
h+=line('M12 221H308',C.line,1)+circle(14,231,2,C.green)+text(22,235,'Normal',10,C.muted)+text(308,235,'ENTER Menu   → Alarm',10,C.muted,400,'end');
const home=base(h);
let s=header('Settings / Output','01 / 04');
s+=text(14,47,'OUTPUT PARAMETERS',10,C.muted,600);
s+=rect(10,56,300,44,'#11231c',5)+rect(10,62,3,32,C.green,1.5)+text(22,75,'Output voltage',13,C.fg,600)+text(296,85,'230 V',22,C.green,600,'end');
s+=text(22,120,'Frequency',13,C.fg)+text(296,120,'50 Hz',15,C.fg,500,'end')+line('M22 133H296',C.line,1);
s+=text(22,157,'Output priority',13,C.fg)+text(296,157,'SBU',15,C.fg,500,'end')+line('M22 170H296',C.line,1);
s+=icon('check',22,185,C.green,.8)+text(40,194,'DSP readback confirmed',11,C.green);
s+=foot('↑↓ Select   ←→ Edit','ENTER Save   ESC Back');
const settings=base(s);
let b=header('Battery / Cells','01–16');
b+=text(12,61,'76',29,C.fg,600)+text(51,61,'%',12,C.muted)+text(90,45,'PACK',9,C.muted)+text(90,63,'51.8 V',19,C.fg,600)+text(217,45,'CELL DELTA',9,C.muted)+text(217,63,'18',19,C.gold,600)+text(244,63,'mV',10,C.muted);
b+=text(12,85,'CELL VOLTAGES',9,C.muted,600)+circle(224,81,2.5,C.green)+text(232,85,'BMS ONLINE',9,C.green);
const cells=['3.238','3.241','3.239','3.242','3.236','3.244','3.240','3.226','3.239','3.241','3.239','3.238','3.240','3.237','3.241','3.239'];
for(let i=0;i<16;i++){
 const x=12+(i%4)*75,y=96+Math.floor(i/4)*28;
 const special=i===5||i===7,co=i===5?C.gold:i===7?C.blue:C.fg;
 b+=text(x,y+9,String(i+1).padStart(2,'0'),9,C.muted)+text(x+66,y+9,cells[i],13,co,500,'end')+line('M'+x+' '+(y+16)+'h66',C.line,1);
}
b+=foot('MIN 08   MAX 06','ESC Back');
const battery=base(b);
let a=header('Alarms','02 ACTIVE');
a+=rect(12,42,296,51,'#271c0f',4)+icon('warn',22,55,C.gold,.85)+text(54,60,'BATTERY LOW',13,C.gold,600)+text(54,79,'Check battery voltage / SOC',11,C.muted);
a+=text(14,115,'PV INPUT LOW',13,C.fg,600)+text(14,134,'Check solar input and connection',11,C.muted)+line('M14 146H306',C.line,1);
a+=text(14,167,'Communication',12,C.muted)+circle(229,163,2.5,C.green)+text(239,167,'Online',12,C.green);
a+=text(14,193,'1 / 2',11,C.muted)+text(306,193,'Warning · No fault',11,C.gold,400,'end');
a+=foot('↑↓ Select   ENTER Detail','ESC Back');
const alarms=base(a);
const screens={home,settings,battery,alarms};
(async()=>{
 for(const [name,svg] of Object.entries(screens)){
 fs.writeFileSync(path.join(out,name+'-v2.svg'),svg);
 await sharp(Buffer.from(svg)).png().toFile(path.join(out,name+'-v2-320.png'));
 await sharp(Buffer.from(svg)).resize(960,720,{kernel:'nearest'}).png().toFile(path.join(out,name+'-v2.png'));
 }
 let board='<svg xmlns="http://www.w3.org/2000/svg" width="1080" height="880"><rect width="1080" height="880" fill="#e9eef3"/><style>text{font-family:"Segoe UI","Microsoft YaHei",sans-serif}</style><text x="32" y="38" font-size="23" fill="#172332">CS-L10 · 图形界面第二版</text><text x="32" y="65" font-size="13" fill="#617385">320 × 240 实际像素 / 六键操作 / 数值与告警为设计示例，非实机数据</text>';
 const placements=[['home',32,108,'01   能流主屏'],['settings',568,108,'02   设置与参数'],['battery',32,500,'03   电池与单体'],['alarms',568,500,'04   告警与诊断']];
 for(const [name,x,y,label] of placements){
  board+='<text x="'+x+'" y="'+(y-12)+'" font-size="15" fill="#293b4b">'+label+'</text>';
  const png=fs.readFileSync(path.join(out,name+'-v2-320.png')).toString('base64');
  board+='<image x="'+x+'" y="'+y+'" width="480" height="360" href="data:image/png;base64,'+png+'" style="image-rendering:pixelated"/>';
 }
 board+='</svg>';
 await sharp(Buffer.from(board)).png().toFile(path.join(out,'ui-v2-overview.png'));
 fs.writeFileSync(path.join(out,'ui-v2-overview.svg'),board);
 console.log('Rendered four native 320x240 pages and overview');
})().catch(e=>{console.error(e);process.exit(1)});
