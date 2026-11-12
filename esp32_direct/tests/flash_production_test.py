"""Exercise the actual flashing CLI with simulated esptool; never accesses USB."""
import contextlib, hashlib, importlib.util, io, json, sys, tempfile, unittest
from pathlib import Path
from unittest.mock import patch
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))

spec=importlib.util.spec_from_file_location('flasher',Path(__file__).resolve().parents[1]/'tools/flash_production.py')
t=importlib.util.module_from_spec(spec);spec.loader.exec_module(t)

class Test(unittest.TestCase):
 def test_default_transport_is_two_megabaud(self):
  with patch.object(t.subprocess,'run') as run:
   t.command('SIMULATED','read_flash','0','32','unused.bin')
  cmd=run.call_args.args[0]
  self.assertIn('--baud',cmd)
  self.assertEqual(cmd[cmd.index('--baud')+1],'2000000')
 def run_flash(self,same=True,first=False,bad_hash=False,changed_identity=False,baud=None,
               model=None,check=False,existing_model=None):
  with tempfile.TemporaryDirectory() as folder:
   root=Path(folder);release=root/'release';release.mkdir();calls=[]
   data={'bootloader.bin':b'BOOT','partition-table.bin':b'PARTITIONS','app.bin':b'APP','ota_data_initial.bin':b'OTA'}
   for name,value in data.items():(release/name).write_bytes(value)
   hashes={name:hashlib.sha256(value).hexdigest() for name,value in data.items()}
   if bad_hash:hashes['app.bin']='0'*64
   manifest={'sha256':hashes}
   if model:manifest['model']=model
   (release/'flash_manifest.json').write_text(json.dumps(manifest),encoding='utf-8')
   flash=bytearray(b'\xff'*0x400000);flash[0x3f8000:]=b'K'*0x8000
   if same:flash[0x8000:0x8000+len(data['partition-table.bin'])]=data['partition-table.bin']
   if existing_model:
    flash[0x1c0000]=0xe9
    descriptor=b'CSIV\1\0\1\0'+existing_model.encode('ascii')
    flash[0x1c0000+288:0x1c0000+352]=descriptor.ljust(64,b'\0')
   self.rates=[]
   def command(port,*args,baud=t.DEFAULT_BAUD):
    self.rates.append(baud)
    self.assertEqual(port,'SIMULATED');calls.append(tuple(map(str,args)))
    if args[0]=='read_flash':
     address=int(str(args[1]),0);size=int(str(args[2]),0)
     if changed_identity and address==0x3f8000:flash[address]^=1
     Path(args[3]).write_bytes(flash[address:address+size])
    elif args[0]=='erase_region':
     address=int(str(args[1]),0);size=int(str(args[2]),0);flash[address:address+size]=b'\xff'*size
   argv=['flash_production.py','--port','SIMULATED','--release',str(release),'--backup-dir',str(root/'backup')]
   if first:argv.append('--first-install')
   if baud is not None:argv.extend(('--baud',str(baud)))
   if model:argv.extend(('--model',model))
   if check:argv.extend(('--check',))
   error=None
   with patch.object(sys,'argv',argv),patch.object(t,'command',command),patch.object(t,'validate_release_images'),patch.object(t,'validate_release_metadata'),contextlib.redirect_stdout(io.StringIO()):
    try:t.main()
    except RuntimeError as e:error=str(e)
   return calls,error
 def test_rate_applies_to_backup_write_verify_and_identity_readback(self):
  for selected in (None,921600):
   calls,error=self.run_flash(baud=selected)
   self.assertIsNone(error)
   self.assertEqual([c[0] for c in calls],['read_flash','write_flash','verify_flash','read_flash','read_flash'])
   self.assertEqual(self.rates,[selected or 2000000]*5)
 def test_transport_arguments_validate_before_launch(self):
  self.assertIn('921600',t.esptool_arguments('SIMULATED','read_flash',baud=921600))
  with patch.object(t.subprocess,'run') as run:
   with self.assertRaises(ValueError):t.command('SIMULATED','read_flash',baud=-1)
   run.assert_not_called()
 def test_hash_before_any_device_command(self):
  calls,error=self.run_flash(bad_hash=True);self.assertIn('hash mismatch',error);self.assertEqual(calls,[])
 def test_unknown_layout_backup_then_stop(self):
  calls,error=self.run_flash(same=False);self.assertIn('partition layout differs',error);self.assertEqual([c[0] for c in calls],['read_flash'])
 def test_explicit_migration_erases_only_cache_after_backup(self):
  calls,error=self.run_flash(same=False,first=True);self.assertIsNone(error)
  self.assertEqual(calls[0][:3],('read_flash','0','0x400000'));self.assertEqual(calls[1],('erase_region','0x360000','0xA0000'))
  self.assertEqual([c[0] for c in calls],['read_flash','erase_region','write_flash','verify_flash','read_flash','read_flash'])
 def test_routine_update_preserves_identity_and_detects_change(self):
  calls,error=self.run_flash();self.assertIsNone(error);self.assertNotIn('erase_region',[c[0] for c in calls])
  calls,error=self.run_flash(changed_identity=True);self.assertIn('factory bytes changed',error)
 def test_modbus_manifest_requires_explicit_product(self):
  manifest={'model':'CS-MB-PANEL','chip':'esp32c3','profile':'device'}
  with self.assertRaisesRegex(RuntimeError,'selected product'):
   t.validate_manifest(manifest)
  t.validate_manifest(manifest,'CS-MB-PANEL')
 def test_modbus_check_never_opens_serial(self):
  calls,error=self.run_flash(model='CS-MB-PANEL',check=True)
  self.assertIsNone(error);self.assertEqual(calls,[])
 def test_modbus_update_stops_after_backup_if_other_product_exists(self):
  calls,error=self.run_flash(model='CS-MB-PANEL',existing_model='CS-L10-6K2')
  self.assertIn('another or unknown product',error)
  self.assertEqual([c[0] for c in calls],['read_flash'])
 def test_modbus_update_preserves_existing_matching_product(self):
  calls,error=self.run_flash(model='CS-MB-PANEL',existing_model='CS-MB-PANEL')
  self.assertIsNone(error);self.assertIn('verify_flash',[c[0] for c in calls])
if __name__=='__main__':unittest.main()
