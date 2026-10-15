"""Wired flashing helper with default factory identity preservation.
Requires esptool installed. Does not guess COM port. Explicit first-install
migration may erase the former cache area that overlaps the new factory area.
First use: --port COM11 --release <profile-folder> --first-install
Existing new-layout devices: omit --first-install (backup/readback factory).
"""
import argparse, datetime, hashlib, json, subprocess, sys
from pathlib import Path
SEGMENTS=((0,'bootloader.bin'),(0x8000,'partition-table.bin'),(0x20000,'app.bin'))
DEFAULT_BAUD=2000000
BAUD_RATES=(115200,460800,921600,1000000,2000000)

def esptool_arguments(port,*args,baud=DEFAULT_BAUD):
 if baud not in BAUD_RATES:raise ValueError('unsupported flashing baud rate')
 return ['--chip','esp32c3','--port',port,'--baud',str(baud),*map(str,args)]

def command(port,*args,baud=DEFAULT_BAUD):
 cmd=[sys.executable,'-m','esptool',*esptool_arguments(port,*args,baud=baud)]
 subprocess.run(cmd,check=True)
def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--port',required=True);p.add_argument('--release',type=Path,required=True);p.add_argument('--backup-dir',type=Path,default=Path('factory_backups'));p.add_argument('--first-install',action='store_true');p.add_argument('--baud',type=int,choices=BAUD_RATES,default=DEFAULT_BAUD);args=p.parse_args()
 manifest=json.loads((args.release/'flash_manifest.json').read_text(encoding='utf-8'))
 for _,name in SEGMENTS+((0x10000,'ota_data_initial.bin'),):
  path=args.release/name
  if hashlib.sha256(path.read_bytes()).hexdigest()!=manifest['sha256'][name]:raise RuntimeError(f'hash mismatch {name}')
 # Unique directory each run; backup never overwritten by an FF result from a retry.
 stamp=datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%S%fZ')
 backup=args.backup_dir/(args.port+'_'+stamp);backup.mkdir(parents=True,exist_ok=False)
 print(f'Flash transfer baud: {args.baud}; full backup, write and readback use this rate.',flush=True)
 command(args.port,'read_flash','0','0x400000',backup/'before_4MB.bin',baud=args.baud)
 full=(backup/'before_4MB.bin').read_bytes();old_table=full[0x8000:0x9000]
 new_table=(args.release/'partition-table.bin').read_bytes()
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
 for offset,name in SEGMENTS:pairs.extend((hex(offset),args.release/name))
 # Reset OTA selection for full wired application replacement at ota_0, but preserve NVS.
 ota=args.release/'ota_data_initial.bin'
 if hashlib.sha256(ota.read_bytes()).hexdigest()!=manifest['sha256']['ota_data_initial.bin']:raise RuntimeError('otadata hash mismatch')
 pairs.extend(('0x10000',ota))
 command(args.port,'write_flash','--flash_mode','dio','--flash_freq','80m','--flash_size','4MB',*pairs,baud=args.baud)
 command(args.port,'verify_flash',*pairs,baud=args.baud)
 command(args.port,'read_flash','0x3f8000','0x8000',backup/'factory_after.bin',baud=args.baud)
 after=(backup/'factory_after.bin').read_bytes()
 if same and after!=old_factory:raise RuntimeError('factory bytes changed unexpectedly')
 # First boot may initialize blank NVS metadata; use USB inspect to verify UNPROVISIONED.
 print('Verified firmware segments and factory preservation. Next: power-cycle, inspect/verify identity; first-install requires provisioning.')
if __name__=='__main__':main()
