import sys, unittest, tempfile, json, hashlib
from pathlib import Path
from unittest.mock import patch
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
import factory_gui_backend as b

class FakeClient:
 calls=[]
 fail=False
 closed=False
 def __init__(self,port):assert port=='SIMULATED'
 def request(self,command,payload=b'',timeout=8):
  self.calls.append((command,payload))
  if command==4:
   if self.fail:raise TimeoutError('lost reply')
   return {'result':'OK','flash_bytes':4194304,'dsp_online':True,'bms_online':True,'keys':63,'measurements':{'battery_v':48},'calibration_write':'unsupported'}
  return {'result':'OK','mac':'02:03:04:05:06:07','factory_write_enabled':True}
 def close(self):type(self).closed=True

class Test(unittest.TestCase):
 def setUp(self):FakeClient.calls=[];FakeClient.fail=FakeClient.closed=False
 def test_missing_port_never_opens_device(self):
  with patch.object(b.factory,'Client') as c:
   with self.assertRaises(ValueError):b.execute({'operation':'inspect','port':''})
   c.assert_not_called()
 def test_test_report_never_fabricates_acceptance(self):
  with patch.object(b.factory,'Client',FakeClient):
   r=b.execute({'operation':'test','port':'SIMULATED','evidence':{'display':'PASS','buzzer':'FAIL'}})
  self.assertEqual(r['overall'],'FAIL');self.assertEqual(r['checks']['calibration_write']['status'],'PENDING')
  self.assertEqual(FakeClient.calls[-1],(6,b'\0'));self.assertTrue(FakeClient.closed)
 def test_test_failure_stops_diagnostics_and_closes(self):
  FakeClient.fail=True
  with patch.object(b.factory,'Client',FakeClient):
   with self.assertRaises(TimeoutError):b.execute({'operation':'test','port':'SIMULATED'})
  self.assertEqual(FakeClient.calls[-1],(6,b'\0'));self.assertTrue(FakeClient.closed)
 def test_profile_requires_real_sn(self):
  p=b.default_profile();self.assertEqual(p['sn'],'');self.assertEqual(p['model'],'CS-L10-6K2')
  with self.assertRaises(ValueError):b.factory.record(p,'02:03:04:05:06:07')
 def test_worker_command_handles_frozen_executable(self):
  with patch.object(sys,'frozen',True,create=True),patch.object(sys,'executable','C:/工位/工具.exe'):
   self.assertEqual(b.worker_command(Path('C:/工位/job.json')),['C:/工位/工具.exe','--worker',str(Path('C:/工位/job.json'))])
 def test_flash_hash_failure_before_packaged_esptool_access(self):
  with tempfile.TemporaryDirectory() as folder:
   root=Path(folder);hashes={}
   for name in ('app.bin','bootloader.bin','partition-table.bin','ota_data_initial.bin'):
    (root/name).write_bytes(b'test');hashes[name]=hashlib.sha256(b'test').hexdigest()
   hashes['app.bin']='0'*64
   (root/'flash_manifest.json').write_text(json.dumps({'sha256':hashes}),encoding='utf-8')
   with patch('esptool.main') as main:
    with self.assertRaisesRegex(RuntimeError,'hash mismatch'):
     b.execute({'operation':'flash','port':'SIMULATED','release':str(root),'backup_dir':str(root/'backups')})
    main.assert_not_called()
 def test_provision_uses_device_mac_and_saves_readback_state(self):
  profile={**b.default_profile(),'sn':'UNIT00001','batch':'BATCH1'}
  class Writer(FakeClient):
   def request(self,command,payload=b'',timeout=8):
    self.calls.append((command,payload))
    if command==2:
     self.assert_wire=payload
     return {'result':'OK','mac':'02:03:04:05:06:07','identity':profile.copy()}
    return {'result':'OK','mac':'02:03:04:05:06:07','factory_write_enabled':True}
  with tempfile.TemporaryDirectory() as folder,patch.object(b.factory,'Client',Writer):
   registry=str(Path(folder)/'registry.db')
   r=b.execute({'operation':'provision','port':'SIMULATED','profile':profile,'registry':registry})
   self.assertIn('断电重启',r['next_step'])
   with b.contextlib.closing(b.sqlite3.connect(registry)) as db:
    self.assertEqual(db.execute('SELECT sn,mac,status FROM devices').fetchone(),('UNIT00001','02:03:04:05:06:07','written_waiting_reboot'))
 def test_old_device_diagnostic_preflight_never_sends_command6(self):
  class OldDevice(FakeClient):
   def request(self,command,payload=b'',timeout=8):
    self.calls.append((command,payload));return {'result':'OK','factory_write_enabled':False,'firmware_profile':'device'}
  with patch.object(b.factory,'Client',OldDevice):
   with self.assertRaises(b.factory.FirmwareError) as error:b.execute({'operation':'diagnostic-start','port':'SIMULATED'})
  self.assertEqual(FakeClient.calls,[(1,b'')]);self.assertIn('v0.6.2',b.friendly_error(error.exception,'diagnostic-start'))
  self.assertTrue(OldDevice.closed)
 def test_new_device_diagnostics_do_not_unlock_identity(self):
  caps=b.capabilities({'factory_write_enabled':False,'diagnostics_enabled':True,'firmware_profile':'device'})
  self.assertTrue(caps['diagnostics']);self.assertFalse(caps['identity_write'])
 def test_packaged_esptool_uses_selected_baud_and_default_two_megabaud(self):
  for selected in (None,921600):
   job={'operation':'flash','port':'SIMULATED','release':'unused','backup_dir':'unused'}
   if selected:job['baud']=selected
   def simulated_flash():
    baud=int(sys.argv[sys.argv.index('--baud')+1])
    b.flasher.command('SIMULATED','read_flash','0','32','unused.bin',baud=baud)
   with patch.object(b.flasher,'main',simulated_flash),patch('esptool.main') as main:
    report=b.execute(job)
   args=main.call_args.args[0]
   self.assertEqual(args[args.index('--baud')+1],str(selected or 2000000))
   self.assertEqual(report['baud'],selected or 2000000)
 def test_bin_flash_keeps_application_stopped_during_preservation_checks(self):
  import flash_single_bin
  def simulated(job,command):
   command('SIMULATED','read_flash','0','32','unused.bin',baud=2000000)
   return {'result':'OK'}
  with patch.object(flash_single_bin,'execute',simulated),patch('esptool.main') as main:
   b.execute({'operation':'flash-bin','port':'SIMULATED'})
  args=main.call_args.args[0]
  self.assertEqual(args[args.index('--after')+1],'no_reset')
  self.assertEqual(args[args.index('--baud')+1],'2000000')
class PairingBackendTests(unittest.TestCase):
 def setUp(self):FakeClient.calls=[];FakeClient.fail=False;FakeClient.closed=False
 def snapshot(self,**extra):return {'result':'OK','pairing_identity_v1':True,'factory_write_enabled':True,'runtime_sn':'H1ZZX00139000000','pairing_ready':True,'identity':{'sn':'H1ZZX00139000000','hw_system':'1.0','hw_controller':'1.0','calibration_summary':'report'},**extra}
 def test_pairing_sequence_and_readback(self):
  initial=self.snapshot(pairing_ready=False);readback=self.snapshot();owner=self
  class PairClient(FakeClient):
   def request(self,command,payload=b'',timeout=8):
    self.calls.append((command,payload))
    if command==8:return {'result':'OK'}
    return initial if len(self.calls)==1 else readback
  with patch.object(b.factory,'Client',PairClient),patch.dict('os.environ',{'PRODUCT_SECRET':'unit-test-only'}):
   report=b.execute({'operation':'pairing','port':'SIMULATED'})
  self.assertTrue(report['pairing_ready']);self.assertEqual([c for c,p in FakeClient.calls],[1,8,1]);self.assertEqual(len(FakeClient.calls[1][1]),23)
 def test_old_capability_no_write(self):
  class OldClient(FakeClient):
   def request(self,command,payload=b'',timeout=8):self.calls.append((command,payload));return {'result':'OK','factory_write_enabled':True}
  with patch.object(b.factory,'Client',OldClient):
   with self.assertRaises(b.factory.FirmwareError):b.execute({'operation':'pairing','port':'SIMULATED'})
  self.assertEqual(FakeClient.calls,[(1,b'')])
 def test_missing_secret_and_reboot_guard(self):
  for snap in (self.snapshot(),self.snapshot(runtime_sn='H1ZZX0013900001P')):
   class Client(FakeClient):
    def request(self,command,payload=b'',timeout=8):self.calls.append((command,payload));return snap
   FakeClient.calls=[]
   with patch.object(b.factory,'Client',Client),patch.dict('os.environ',{},clear=True):
    with self.assertRaises(ValueError):b.execute({'operation':'pairing','port':'SIMULATED'})
   self.assertEqual(FakeClient.calls,[(1,b'')])
 def test_partial_pairing_readback_rejected(self):
  for final in (self.snapshot(pairing_ready=False),self.snapshot(runtime_sn='H1ZZX0013900001P')):
   initial=self.snapshot(pairing_ready=False)
   class Client(FakeClient):
    def request(self,command,payload=b'',timeout=8):
     self.calls.append((command,payload));return {'result':'OK'} if command==8 else initial if len(self.calls)==1 else final
   FakeClient.calls=[]
   with patch.object(b.factory,'Client',Client),patch.dict('os.environ',{'PRODUCT_SECRET':'unit-test-only'}):
    with self.assertRaises(b.factory.FirmwareError):b.execute({'operation':'pairing','port':'SIMULATED'})
 def test_new_lock_requires_pairing_ready_old_compatible(self):
  for supports,writes in ((True,False),(False,True)):
   snap=self.snapshot(pairing_ready=False,pairing_identity_v1=supports)
   class Client(FakeClient):
    def request(self,command,payload=b'',timeout=8):self.calls.append((command,payload));return snap
   FakeClient.calls=[]
   with patch.object(b.factory,'Client',Client):
    if supports:
     with self.assertRaises(ValueError):b.execute({'operation':'lock','port':'SIMULATED'})
    else:b.execute({'operation':'lock','port':'SIMULATED'})
   self.assertEqual(any(c==3 for c,p in FakeClient.calls),writes)
if __name__=='__main__':unittest.main()
