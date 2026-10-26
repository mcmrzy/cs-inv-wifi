// Export the actual host C renderer output, without recreating scene geometry.
const fs = require('fs');
const path = require('path');
const sharp = require('C:/Users/29538/.cache/codex-runtimes/codex-primary-runtime/dependencies/node/node_modules/sharp');
async function main() {
  const source = path.resolve('tmp/ui_graphical');
  const target = path.resolve(process.argv[2] || 'docs/ui_redesign/firmware_v030');
  fs.mkdirSync(target, {recursive:true});
  for (const file of fs.readdirSync(source).filter(f=>f.endsWith('.ppm'))) {
    const bytes = fs.readFileSync(path.join(source,file));
    const header = bytes.toString('ascii',0,64).match(/^P6\s+(\d+)\s+(\d+)\s+255\s/);
    if (!header) throw new Error('Unsupported PPM: '+file);
    await sharp(bytes.subarray(header[0].length), {raw:{width:+header[1],height:+header[2],channels:3}}).png().toFile(path.join(target,file.replace('.ppm','.png')));
  }
  const names = ['home_online','menu','editing','bms_cells','alarms','generator_online'];
  const composite = names.map((name,i)=>({input:path.join(target,name+'.png'),left:(i%3)*320,top:Math.floor(i/3)*240}));
  await sharp({create:{width:960,height:480,channels:3,background:'#080810'}}).composite(composite).png().toFile(path.join(target,'firmware_overview.png'));
}
main().catch(e=>{console.error(e);process.exitCode=1;});
