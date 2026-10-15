"""Exercise actual BIN parsing and backup/write flow without hardware."""
import sys,tempfile,unittest,hashlib,io,contextlib
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
import flash_single_bin as t
root=Path(__file__).resolve().parents[1]
release=root/'release/esp32c3_v0.6.2_production_dev_20261013/device'

class Test(unittest.TestCase):
 def test_real_app_and_merged_image_types(self):
  app=t.describe(release/'app.bin','app');self.assertEqual(app['offset'],0x20000)
  merged=t.describe(next(release.glob('*4MB.bin')),'merged');self.assertEqual(merged['offset'],0)
  with self.assertRaises(ValueError):t.describe(release/'bootloader.bin','app')
  with self.assertRaises(ValueError):t.describe(release/'app.bin','merged')
 def test_wrong_chip_and_corruption(self):
  app=bytearray((release/'app.bin').read_bytes());app[12]=9
  with self.assertRaises(ValueError):t.validate_image(app,True)
  app=bytearray((release/'app.bin').read_bytes());app[-1]^=1
  with self.assertRaises(ValueError):t.validate_image(app,True)
  with self.assertRaises(Exception):t.validate_image(app[:100],True)
 def test_merged_partition_integrity(self):
  table=bytearray((release/'partition-table.bin').read_bytes());table[15]^=1
  with self.assertRaises(ValueError):t.partitions(table)
 def run_flash(self,kind='app',mismatch=False,partial=False,changed=False,corrupt_identity=False):
  with tempfile.TemporaryDirectory() as folder:
   source=release/'app.bin' if kind=='app' else next(release.glob('*4MB.bin'))
   info=t.describe(source,kind);flash=bytearray(next(release.glob('*4MB.bin')).read_bytes())
   flash[0x3f8000:]=b'K'*0x8000;calls=[]
   if mismatch:flash[0x8000]^=1
   def command(port,*args,baud):
    self.assertEqual(port,'SIMULATED');self.assertEqual(baud,2000000);calls.append(args)
    if args[0]=='read_flash':
     start=int(args[1],0);size=int(args[2],0)
     if corrupt_identity and start==0x3f8000:flash[start]^=1
     output=flash[start:start+size]
     Path(args[3]).write_bytes(output[:-1] if partial and start==0 else output)
    if args[0]=='write_flash':
     for i in range(3,len(args),2):
      start=int(args[i],0);data=Path(args[i+1]).read_bytes();flash[start:start+len(data)]=data
    if args[0]=='verify_flash':
     for i in range(1,len(args),2):
      start=int(args[i],0);data=Path(args[i+1]).read_bytes();self.assertEqual(flash[start:start+len(data)],data)
   job=dict(bin_file=str(source),bin_kind=kind,bin_sha256='0'*64 if changed else info['sha256'],layout=str(release),backup_dir=folder,port='SIMULATED',baud=2000000)
   error=None;report=None
   with contextlib.redirect_stdout(io.StringIO()):
    try:report=t.execute(job,command)
    except RuntimeError as e:error=str(e)
   return calls,error,report
 def test_app_preserves_identity_and_resets_boot_selection(self):
  calls,error,report=self.run_flash();self.assertIsNone(error);self.assertEqual(report['offset'],0x20000)
  self.assertEqual([c[0] for c in calls],['read_flash','write_flash','verify_flash','read_flash','read_flash'])
  self.assertEqual(calls[1][3],'0x20000');self.assertEqual(calls[1][5],'0x10000')
 def test_merged_requires_full_backup_then_writes_at_zero(self):
  calls,error,report=self.run_flash('merged');self.assertIsNone(error)
  self.assertEqual([c[0] for c in calls],['read_flash','write_flash','verify_flash']);self.assertEqual(calls[1][3],'0x0')
 def test_incompatible_layout_and_partial_backup_never_write(self):
  for flags in (dict(mismatch=True),dict(partial=True)):
   calls,error,_=self.run_flash(**flags);self.assertIsNotNone(error);self.assertEqual([c[0] for c in calls],['read_flash'])
 def test_confirmation_hash_checked_before_hardware(self):
  calls,error,_=self.run_flash(changed=True);self.assertIn('变化',error);self.assertFalse(calls)
 def test_identity_readback_detects_unexpected_change(self):
  _,error,_=self.run_flash(corrupt_identity=True);self.assertIn('意外变化',error)

if __name__=='__main__':unittest.main()
