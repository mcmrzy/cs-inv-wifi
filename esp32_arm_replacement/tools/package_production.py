"""Package the last successfully built profile. Never flashes hardware."""
from pathlib import Path
import argparse, hashlib, json, subprocess, sys
p=argparse.ArgumentParser();p.add_argument('profile',choices=['device','factory','production']);a=p.parse_args()
root=Path(__file__).resolve().parents[1];version=(root/'version.txt').read_text(encoding='utf-8').strip()
base_version=version.split('-')[0];log_version=base_version.replace('.','')
target=root/f'release/esp32c3_v{version.replace("-","_")}_20261013'/a.profile;target.mkdir(parents=True,exist_ok=True)
(target/'THIRD_PARTY_QRCODEGEN.txt').write_text((root/'components/qrcodegen/qrcodegen.h').read_text(encoding='utf-8').split('*/',1)[0]+'*/\n',encoding='utf-8')
parts=((0,'build/bootloader/bootloader.bin','bootloader.bin'),(0x8000,'build/partition_table/partition-table.bin','partition-table.bin'),(0x10000,'build/ota_data_initial.bin','ota_data_initial.bin'),(0x20000,'build/cs_inv_direct_c3.bin','app.bin'))
config=(root/f'sdkconfig.{a.profile}').read_text(encoding='utf-8')
assert ('CONFIG_DIRECT_IDENTITY_PRODUCTION=y' in config)==(a.profile!='device')
assert ('CONFIG_DIRECT_FACTORY_WRITES=y' in config)==(a.profile=='factory')
assert 'CONFIG_DIRECT_BOOT_DIAGNOSTICS=y' not in config, 'release must disable periodic boot diagnostics'
assert 'CONFIG_BOOTLOADER_LOG_LEVEL=2' in config, 'release bootloader must use warning level'
cache=(root/'build/CMakeCache.txt').read_text(encoding='utf-8')
assert f'sdkconfig.{a.profile}' in cache,'last build profile mismatch'
args=[]
for address,src,name in parts:
 data=(root/src).read_bytes();(target/name).write_bytes(data);args.extend((hex(address),str(root/src)))
merged=target/f'cs_inv_direct_c3_v{base_version}_{a.profile}_4MB.bin'
subprocess.run([sys.executable,'-m','esptool','--chip','esp32c3','merge_bin','--flash_mode','dio','--flash_freq','80m','--flash_size','4MB','--fill-flash-size','4MB','-o',str(merged),*args],check=True)
image=merged.read_bytes();assert len(image)==4194304;end=0
for address,src,name in parts:
 data=(root/src).read_bytes();assert image[end:address]==b'\xff'*(address-end);assert image[address:address+len(data)]==data;end=address+len(data)
assert image[end:]==b'\xff'*(len(image)-end)
app=(target/'app.bin').read_bytes();assert version.encode('ascii') in app
assert b'[BOOT_DIAG]' not in app, 'periodic boot task leaked into release image'
metadata=bytearray(b'CSIV\1\0'+bytes([{'device':1,'factory':2,'production':3}[a.profile]])+b'\0')
metadata.extend(b'CS-L10-6K2'+b'\0'*46)
assert len(metadata)==64 and app[288:352]==metadata,'embedded product/profile descriptor mismatch'
assert (b'H1ZZX0013900001P' in app)==(a.profile=='device'), 'fixed identity must not leak into per-device image'
for marker in (b'Firmware upgrade',b'Manufacturing',b'LCD_BOOT_VISIBLE',b'Factory partition init',b'[SET_SN] OK',b'[STATUS] firmware='):
 assert marker in app,marker
manifest={'version':version,'profile':a.profile,'model':'CS-L10-6K2','factory_partition':{'offset':0x3f8000,'size':0x8000},'requires_wired_partition_migration':True,'sha256':{f.name:hashlib.sha256(f.read_bytes()).hexdigest() for f in target.glob('*.bin')}}
manifest.update(log_mode='release', serial_commands=['status','set_sn','set_pin'],
                dsp_measurement_max_age_ms=2000, hardware_acceptance=False,
                onboarding_qr_v1=True, pairing_pin_preprovision_required=True,
                cloud_binding_public_key_configured=('CONFIG_DIRECT_BINDING_PUBLIC_KEY="' in config and 'CONFIG_DIRECT_BINDING_PUBLIC_KEY=""' not in config))
(target/'flash_manifest.json').write_text(json.dumps(manifest,indent=2),encoding='utf-8')
(target/'SHA256SUMS.txt').write_text(''.join(f'{h}  {name}\n' for name,h in manifest['sha256'].items()),encoding='ascii')
log=root/f'build_v{log_version}_{a.profile}.log'
if log.exists():(target/'build.log').write_bytes(log.read_bytes())
print(f'PASS {a.profile}: complete 4MB, segments + FF gaps, identity/profile/feature markers, SHA256; app={len(app)} bytes')
