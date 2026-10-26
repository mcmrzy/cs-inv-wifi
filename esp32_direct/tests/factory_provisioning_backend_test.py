import sys, tempfile, unittest
from pathlib import Path
from unittest.mock import patch
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
import factory_gui_backend as b

MAC='02:03:04:05:06:07'
PROFILE={**b.default_profile(),'sn':'H1ZZX00139000000','batch':'B1'}
def snapshot(**extra):
 return {'result':'OK','mac':MAC,'factory_api':3,'identity_schema':2,'provisioning_identity_v2':True,'factory_write_enabled':True,
         'runtime_sn':PROFILE['sn'],'runtime_generation':2,'identity_pending_reboot':False,
         'identity':{**PROFILE,'generation':2},**extra}

class Guards(unittest.TestCase):
 def run_job(self,operation,snap,**extra):
  self.calls=[];owner=self
  class Client:
   def __init__(self,port):pass
   def request(self,command,payload=b'',timeout=8):owner.calls.append(command);return snap
   def close(self):pass
  with tempfile.TemporaryDirectory() as directory,patch.object(b.factory,'Client',Client):
   return b.execute({'operation':operation,'port':'SIM','expected_mac':MAC,'profile':PROFILE,'registry':str(Path(directory)/'db'),**extra})
 def test_changed_mac_rejected_before_provision(self):
  with self.assertRaisesRegex(ValueError,'MAC'):self.run_job('provision',snapshot(mac='02:03:04:05:06:08'))
  self.assertEqual(self.calls,[1])
 def test_old_firmware_rejected_before_extended_write(self):
  with self.assertRaisesRegex(ValueError,'固件'):self.run_job('provision',snapshot(provisioning_identity_v2=False))
  self.assertEqual(self.calls,[1])
 def test_missing_expected_mac_rejected_before_mutation(self):
  with self.assertRaisesRegex(ValueError,'MAC'):self.run_job('diagnostic-start',snapshot(),expected_mac='')
  self.assertNotIn(6,self.calls)
 def test_verify_requires_reboot_and_runtime_generation(self):
  for extra in ({'identity_pending_reboot':True},{'runtime_generation':1},{'runtime_sn':'OTHER'}, {'runtime_generation':None}):
   with self.subTest(extra=extra),self.assertRaisesRegex(ValueError,'重启|固件'):
    self.run_job('verify',snapshot(**extra))
 def test_verify_rejects_old_proof_and_accepts_matching_boot(self):
  with self.assertRaisesRegex(ValueError,'固件'):self.run_job('verify',snapshot(provisioning_identity_v2=False))
  self.assertEqual(self.run_job('verify',snapshot())['result'],'OK')
 def test_caps_do_not_offer_new_write_or_verified_old_identity(self):
  caps=b.capabilities({'factory_write_enabled':True,'identity':{'sn':'OLD'}})
  self.assertFalse(caps['identity_write']);self.assertFalse(caps['identity_verify'])
 def test_locked_identity_is_rejected_before_write(self):
  with self.assertRaises(b.factory.FirmwareError):self.run_job('provision',snapshot(identity={**PROFILE,'generation':2,'locked':True}))
  self.assertEqual(self.calls,[1])
 def test_rom_identify_does_not_open_application_protocol(self):
  import flash_identify
  expected={'result':'OK','identification_mode':'rom','mac':MAC,'chip':'esp32c3'}
  with patch.object(flash_identify,'identify',return_value=expected) as identify,patch.object(b.factory,'Client') as client:
   self.assertEqual(b.execute({'operation':'identify-rom','port':'SIM'}),expected)
  identify.assert_called_once_with('SIM');client.assert_not_called()

if __name__=='__main__':unittest.main()
