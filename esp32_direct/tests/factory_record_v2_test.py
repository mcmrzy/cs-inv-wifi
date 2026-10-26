"""Exercise both host codec and the actual firmware C codec on identical bytes."""
import importlib.util
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import unittest
import zlib

ROOT=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('factory_codec',ROOT/'tools/factory_tool.py')
t=importlib.util.module_from_spec(spec);spec.loader.exec_module(t)
GCC=Path(r'C:\Users\29538\AppData\Local\Microsoft\WinGet\Packages\BrechtSanders.WinLibs.POSIX.UCRT_Microsoft.Winget.Source_8wekyb3d8bbwe\mingw64\bin\gcc.exe')

def profile():
 return dict(sn='H1ZZX0013900001P',hw_system='1.0',hw_controller='1.1',batch='test',production_time='2026-10-12T00:00:00Z')

class Test(unittest.TestCase):
 def test_schema_and_limits(self):
  self.assertEqual(len(t.record(profile(),'02:03:04:05:06:07')),736)
  self.assertEqual(len(t.record(profile(),'02:03:04:05:06:07',schema=1)),336)
  for sn in ('short','abcdefghijklmnop','H1ZZX0013900001-', 'Ａ1ZZX0013900001P'):
   with self.subTest(sn=sn),self.assertRaises(ValueError):t.record({**profile(),'sn':sn},'02:03:04:05:06:07')
  with self.assertRaises(ValueError):t.record({**profile(),'work_order':'WO'},'02:03:04:05:06:07',schema=1)
  with self.assertRaises(ValueError):t.record(profile(),'02:03:04:05:06:07',schema=3)
 def test_large_frames(self):
  payload=b'{"identity":"'+b'x'*3500+b'"}'
  packet=t.frame(0x81,3,payload);p=t.Parser();self.assertEqual(p.feed(packet[:1500]),[])
  self.assertEqual(p.feed(packet[1500:]),[(0x81,3,payload)])
  self.assertEqual(len(t.Parser().feed(t.frame(1,4,b'x'*4096))),1)
  with self.assertRaises(ValueError):t.frame(1,4,b'x'*4097)
 def test_c_interoperability_and_migration(self):
  compiler=str(GCC) if GCC.exists() else shutil.which('gcc')
  self.assertTrue(compiler,'gcc needed to verify firmware codec')
  (ROOT/'tmp').mkdir(exist_ok=True)
  with tempfile.TemporaryDirectory(dir=ROOT/'tmp') as tmp:
   exe=Path(tmp)/'record.exe'
   subprocess.run([compiler,'-save-temps=obj','-std=c11','-Wall','-Wextra','-Werror','-I'+str(ROOT/'main/factory'),str(ROOT/'tests/factory_record_v2_test.c'),str(ROOT/'main/factory/factory_record.c'),str(ROOT/'main/factory/factory_store.c'),'-o',str(exe)],check=True)
   subprocess.run([str(exe)],check=True)
   for schema in (1,2):
    d=profile()
    if schema==2:
     for name,size in t.FIELDS:
      if name not in dict(t.LEGACY_FIELDS):d[name]=('"\\A'+name+'X'*size)[:size-1]
    wire=t.record(d,'02:03:04:05:06:07',schema=schema)
    self.assertEqual(struct.unpack_from('<H',wire,4)[0],schema)
    self.assertEqual(struct.unpack_from('<I',wire,len(wire)-4)[0],zlib.crc32(wire[:-4]))
    src=Path(tmp)/'input.bin';dst=Path(tmp)/'output.bin';src.write_bytes(wire)
    subprocess.run([str(exe),str(src),str(dst)],check=True)
    self.assertEqual(dst.read_bytes(),wire)

if __name__=='__main__':unittest.main()
