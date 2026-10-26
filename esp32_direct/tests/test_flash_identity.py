"""ROM identity and same-connection flashing tests; no hardware access."""
import contextlib
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tools'))
import flash_identify as identity
import flash_production as production

class Loader:
 CHIP_NAME='ESP32-C3'
 def __init__(self,port,baud):
  self.port=port;self.baud=baud;self.closed=False;self.connected=0;self.stub_count=0;self._port=self
 def connect(self):self.connected+=1
 def close(self):self.closed=True
 def read_mac(self):return (2,3,4,5,6,7)
 def flash_spi_attach(self,n):assert n==0
 def flash_id(self):return 0x1640ef
 def run_stub(self):self.stub_count+=1;return self
 def change_baud(self,baud):self.baud=baud

class Test(unittest.TestCase):
 def test_manifest_matches_embedded_product_and_profile(self):
  app=bytearray(352);app[288:296]=b'CSIV\1\0\2\0';app[296:306]=b'CS-L10-6K2'
  production.validate_release_metadata({'profile':'factory','model':'CS-L10-6K2'},{'app.bin':bytes(app)})
  for manifest in ({'profile':'device','model':'CS-L10-6K2'},{'profile':'factory','model':'OTHER'}):
   with self.subTest(manifest=manifest),self.assertRaises(RuntimeError):production.validate_release_metadata(manifest,{'app.bin':bytes(app)})
 def test_rom_identification_is_explicit_and_closes(self):
  loader=Loader('SIMULATED',115200)
  with patch.object(identity,'open_rom',return_value=loader):report=identity.identify('SIMULATED')
  self.assertEqual(report['identification_mode'],'rom');self.assertEqual(report['chip'],'esp32c3')
  self.assertEqual(report['mac'],'02:03:04:05:06:07');self.assertEqual(report['flash_bytes'],0x400000)
  self.assertTrue(loader.closed);self.assertEqual(loader.stub_count,0)
  self.assertNotIn('identity',report)
 def test_session_guards_before_operation_and_closes(self):
  for setting in ('mac','chip','flash'):
   loader=Loader('SIMULATED',115200)
   if setting=='chip':loader.CHIP_NAME='ESP32-S3'
   if setting=='flash':loader.flash_id=lambda:0x1740ef
   expected='02:03:04:05:06:08' if setting=='mac' else '02:03:04:05:06:07'
   with self.subTest(setting=setting),patch.object(identity,'open_rom',return_value=loader),self.assertRaises(RuntimeError):
    with production.flash_session('SIMULATED',921600,expected,tool_main=lambda *_args,**_kwargs:self.fail('guard bypass')):self.fail('guard bypass')
   self.assertTrue(loader.closed);self.assertEqual(loader.stub_count,0)
 def test_all_operations_share_one_loader_without_reset(self):
  loader=Loader('SIMULATED',115200);calls=[]
  def main(argv,esp):calls.append((argv,esp))
  with patch.object(identity,'open_rom',return_value=loader):
   with production.flash_session('SIMULATED',921600,'02:03:04:05:06:07',tool_main=main) as command:
    command('SIMULATED','read_flash','0','4','test.bin',baud=921600)
    command('SIMULATED','verify_flash','0','image.bin',baud=921600)
  self.assertTrue(loader.closed);self.assertEqual(loader.stub_count,1)
  self.assertEqual(len(calls),2)
  for argv,actual in calls:
   self.assertIs(actual,loader)
   self.assertEqual(argv[argv.index('--after')+1],'no_reset_stub')
   self.assertEqual(argv[argv.index('--before')+1],'no_reset_no_sync')
 def run_production(self,partial=False,change_nvs=False,change_source=False,bad_profile=False):
  with tempfile.TemporaryDirectory(dir=ROOT/'tmp') as folder:
   root=Path(folder);release=root/'release';release.mkdir();calls=[]
   data={'bootloader.bin':b'BOOT','partition-table.bin':b'PARTITIONS','app.bin':b'APP','ota_data_initial.bin':b'OTA'}
   for name,value in data.items():(release/name).write_bytes(value)
   manifest={'sha256':{n:hashlib.sha256(v).hexdigest() for n,v in data.items()}}
   if bad_profile:manifest['profile']='unrelated'
   (release/'flash_manifest.json').write_text(json.dumps(manifest))
   flash=bytearray(b'\xff'*0x400000);flash[0x8000:0x8000+len(data['partition-table.bin'])]=data['partition-table.bin']
   def command(port,*args,baud):
    calls.append(args)
    if args[0]=='read_flash':
     start=int(args[1],0);size=int(args[2],0)
     if change_nvs and start==0x9000:flash[start]^=1
     Path(args[3]).write_bytes(flash[start:start+size-(1 if partial and start==0 else 0)])
     if change_source and start==0:(release/'app.bin').write_bytes(b'REPLACED')
    elif args[0]=='write_flash':
     # The firmware copy must be the prevalidated snapshot after source replacement.
     app=Path(args[args.index('0x20000')+1]);self.assertEqual(app.read_bytes(),b'APP')
   @contextlib.contextmanager
   def session(port,baud,expected):
    self.assertEqual(expected,'02:03:04:05:06:07');yield command
   argv=['flash','--port','SIMULATED','--release',str(release),'--backup-dir',str(root/'backups'),'--expected-mac','02:03:04:05:06:07']
   with patch.object(sys,'argv',argv),patch.object(production,'flash_session',session),patch.object(production,'validate_release_images'),contextlib.redirect_stdout(io.StringIO()):
    try:production.main();error=None
    except RuntimeError as exc:error=str(exc)
   return calls,error
 def test_partial_backup_never_writes(self):
  calls,error=self.run_production(partial=True)
  self.assertIsNotNone(error);self.assertEqual([c[0] for c in calls],['read_flash'])
 def test_nvs_preservation_verified(self):
  _,error=self.run_production(change_nvs=True);self.assertIn('NVS',error)
 def test_source_replacement_does_not_change_staged_firmware(self):
  _,error=self.run_production(change_source=True);self.assertIsNone(error)
 def test_invalid_manifest_profile_rejected_before_hardware(self):
  calls,error=self.run_production(bad_profile=True);self.assertIn('profile',error);self.assertFalse(calls)

if __name__=='__main__':unittest.main()
