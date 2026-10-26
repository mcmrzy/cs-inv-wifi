"""Wired flashing helper with default factory identity preservation.
Requires esptool installed. Does not guess COM port. Explicit first-install
migration may erase the former cache area that overlaps the new factory area.
First use: --port COM11 --release <profile-folder> --first-install
Existing new-layout devices: omit --first-install (backup/readback factory).
"""
import argparse, contextlib, datetime, hashlib, json, subprocess, sys
from pathlib import Path
import flash_identify
SEGMENTS=((0,'bootloader.bin'),(0x8000,'partition-table.bin'),(0x20000,'app.bin'))
DEFAULT_BAUD=2000000
BAUD_RATES=(115200,460800,921600,1000000,2000000)

def esptool_arguments(port,*args,baud=DEFAULT_BAUD):
 if baud not in BAUD_RATES:raise ValueError('unsupported flashing baud rate')
 return ['--chip','esp32c3','--port',port,'--baud',str(baud),*map(str,args)]

def command(port,*args,baud=DEFAULT_BAUD):
 cmd=[sys.executable,'-m','esptool',*esptool_arguments(port,*args,baud=baud)]
 subprocess.run(cmd,check=True)

_DEFAULT_COMMAND=command

@contextlib.contextmanager
def flash_session(port,baud,expected_mac=None,tool_main=None):
 """Keep one serial handle and stub alive across identity/backup/write/readback.

 esptool 4 supports main(argv, esp=connected_loader). --no-stub prevents
 re-upload, while no_reset_stub keeps the same stub and application stopped.
 """
 if baud not in BAUD_RATES:raise ValueError('unsupported flashing baud rate')
 loader=flash_identify.open_rom(port)
 try:
  flash_identify.read_identity(loader,expected_mac,require_4mb=True)
  loader=loader.run_stub()
  if baud!=115200:loader.change_baud(baud)
  if tool_main is None:
   import esptool
   tool_main=esptool.main
  def run(selected,*args,baud=baud):
   if selected!=port:raise RuntimeError('serial port changed within flashing session')
   argv=esptool_arguments(port,'--before','no_reset_no_sync','--after','no_reset_stub','--no-stub',*args,baud=baud)
   tool_main(argv,esp=loader)
  yield run
 finally:loader._port.close()

def flash_transport(port,baud,expected_mac,override):
 # Preserve the existing no-hardware injection API. Every physical default
 # command, and every GUI job bound to an expected MAC, uses a guarded session.
 if expected_mac is not None or override is _DEFAULT_COMMAND:return flash_session(port,baud,expected_mac)
 return contextlib.nullcontext(override)

def validate_manifest(manifest):
 if 'profile' in manifest and manifest['profile'] not in ('device','factory','production'):
  raise RuntimeError('unsupported firmware profile')
 if 'model' in manifest and manifest['model']!='CS-L10-6K2':raise RuntimeError('firmware model does not match this tool')
 if 'chip' in manifest and str(manifest['chip']).lower().replace('-','')!='esp32c3':raise RuntimeError('firmware chip mismatch')
 if 'factory_partition' in manifest and manifest['factory_partition']!={'offset':0x3f8000,'size':0x8000}:
  raise RuntimeError('unexpected factory partition')

def validate_release_images(images):
 from flash_single_bin import validate_image,partitions
 validate_image(images['bootloader.bin'])
 validate_image(images['app.bin'],True)
 if len(images['bootloader.bin'])>0x8000 or len(images['partition-table.bin'])>0x1000 or len(images['ota_data_initial.bin'])!=0x2000 or len(images['app.bin'])>0x1a0000:
  raise RuntimeError('firmware segment does not fit the expected layout')
 entries=partitions(images['partition-table.bin'])
 required={(1,2,0x9000,0x6000),(1,0,0x10000,0x2000),(0,16,0x20000,0x1a0000),(1,2,0x3f8000,0x8000)}
 actual={(e['type'],e['subtype'],e['offset'],e['size']) for e in entries}
 if not required<=actual:raise RuntimeError('firmware partition layout is not supported')

def validate_release_metadata(manifest,images):
 if 'profile' not in manifest and 'model' not in manifest:return
 descriptor=images['app.bin'][288:352]
 if len(descriptor)!=64 or descriptor[:6]!=b'CSIV\1\0' or descriptor[7]!=0:
  raise RuntimeError('firmware product/profile descriptor is missing or invalid')
 if 'profile' in manifest and descriptor[6]!={'device':1,'factory':2,'production':3}[manifest['profile']]:
  raise RuntimeError('firmware profile differs from manifest')
 if 'model' in manifest and descriptor[8:].split(b'\0',1)[0]!=manifest['model'].encode('ascii'):
  raise RuntimeError('firmware model differs from manifest')
def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--port',required=True);p.add_argument('--release',type=Path,required=True);p.add_argument('--backup-dir',type=Path,default=Path('factory_backups'));p.add_argument('--first-install',action='store_true');p.add_argument('--expected-mac');p.add_argument('--baud',type=int,choices=BAUD_RATES,default=DEFAULT_BAUD);args=p.parse_args()
 manifest=json.loads((args.release/'flash_manifest.json').read_text(encoding='utf-8'))
 validate_manifest(manifest)
 images={}
 for _,name in SEGMENTS+((0x10000,'ota_data_initial.bin'),):
  path=args.release/name
  images[name]=path.read_bytes()
  if hashlib.sha256(images[name]).hexdigest()!=manifest['sha256'][name]:raise RuntimeError(f'hash mismatch {name}')
 validate_release_images(images)
 validate_release_metadata(manifest,images)
 # Unique directory each run; backup never overwritten by an FF result from a retry.
 stamp=datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%S%fZ')
 backup=args.backup_dir/(args.port+'_'+stamp);backup.mkdir(parents=True,exist_ok=False)
 staged=backup/'firmware';staged.mkdir()
 for name,data in images.items():(staged/name).write_bytes(data)
 print(f'Flash transfer baud: {args.baud}; full backup, write and readback use this rate.',flush=True)
 with flash_transport(args.port,args.baud,args.expected_mac,command) as run:
  flash_validated(args,images,backup,staged,run)

def flash_validated(args,images,backup,staged,command):
 command(args.port,'read_flash','0','0x400000',backup/'before_4MB.bin',baud=args.baud)
 full=(backup/'before_4MB.bin').read_bytes();old_table=full[0x8000:0x9000]
 if len(full)!=0x400000:raise RuntimeError('full Flash backup is incomplete; stop before erase/write')
 new_table=images['partition-table.bin']
 # Existing layout must already describe our factory NVS; otherwise tail bytes
 # belong to old SPIFFS, not identity. Explicit first-install permits migration.
 same=old_table[:len(new_table)]==new_table
 if not same and not args.first_install:raise RuntimeError('partition layout differs: full backup saved; use reviewed first-install migration')
 old_factory=full[0x3f8000:0x400000]
 (backup/'factory_before.bin').write_bytes(old_factory)
 if args.first_install and not same:
  print('First-install migration: cache format changes; previous full flash is saved.')
  # Erase only old SPIFFS region, which overlaps the new factory partition.
  # First-install is explicit permission for this migration, not routine repair.
  command(args.port,'erase_region','0x360000','0xA0000',baud=args.baud)
 pairs=[]
 for offset,name in SEGMENTS:pairs.extend((hex(offset),staged/name))
 # Reset OTA selection for full wired application replacement at ota_0, but preserve NVS.
 ota=staged/'ota_data_initial.bin'
 pairs.extend(('0x10000',ota))
 command(args.port,'write_flash','--flash_mode','dio','--flash_freq','80m','--flash_size','4MB',*pairs,baud=args.baud)
 command(args.port,'verify_flash',*pairs,baud=args.baud)
 command(args.port,'read_flash','0x3f8000','0x8000',backup/'factory_after.bin',baud=args.baud)
 after=(backup/'factory_after.bin').read_bytes()
 if same and after!=old_factory:raise RuntimeError('factory bytes changed unexpectedly')
 command(args.port,'read_flash','0x9000','0x6000',backup/'nvs_after.bin',baud=args.baud)
 if (backup/'nvs_after.bin').read_bytes()!=full[0x9000:0xf000]:raise RuntimeError('user NVS bytes changed unexpectedly')
 # First boot may initialize blank NVS metadata; use USB inspect to verify UNPROVISIONED.
 print('Verified firmware segments and factory preservation. Next: power-cycle, inspect/verify identity; first-install requires provisioning.')
if __name__=='__main__':main()
