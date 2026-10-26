import sys, tempfile, unittest
import time
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch,Mock
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
import factory_gui as gui
import tkinter as tk
import customtkinter as ct

class GuiDiscoveryTests(unittest.TestCase):
 def setUp(self):
  self.folder=tempfile.TemporaryDirectory();self.root=ct.CTk();self.root.withdraw()
  self.app=gui.App(self.root,self.folder.name,scan=False)
 def tearDown(self):
  for timer in self.root.tk.call('after','info'):self.root.tk.call('after','cancel',timer)
  self.root.destroy();self.folder.cleanup()
 def usb(self,serial='ONE'):
  return SimpleNamespace(device='COM5',vid=0x303A,pid=0x1001,serial_number=serial,location='1',hwid='USB',description='ESP USB')
 def test_poll_auto_inspect_only_once(self):
  with patch('serial.tools.list_ports.comports',return_value=[self.usb()]),patch.object(self.app,'run') as run:
   self.app.refresh_ports();self.app.refresh_ports()
   self.assertEqual(self.app.port.get(),'COM5');run.assert_called_once_with('inspect',automatic=True)
 def test_removal_clears_identity_permissions_and_result(self):
  self.app.port.set('COM5')
  with patch('serial.tools.list_ports.comports',return_value=[self.usb()]),patch.object(self.app,'run'):
   self.app.refresh_ports()
  self.app.render_report({'firmware':'new','mac':'02:03:04:05:06:07','factory_api':3,'identity_schema':2,'provisioning_identity_v2':True,'factory_write_enabled':True},'inspect','COM5')
  self.app.last_report={'identity':{'sn':'OLD'}}
  self.app.fields['sn'].set('H1ZZX00139000000');self.app.fields['controller_sn'].set('PRIOR-BOARD')
  with patch('serial.tools.list_ports.comports',return_value=[]):self.app.refresh_ports()
  self.assertIsNone(self.app.snapshot);self.assertIsNone(self.app.last_report)
  self.assertEqual(self.app.peer_port,'');self.assertEqual(self.app.stats['sn'].get(),'等待识别')
  self.assertEqual(self.app.fields['sn'].get(),'');self.assertEqual(self.app.fields['controller_sn'].get(),'')
 def test_busy_does_not_probe_or_replace_selected_port(self):
  self.app.process=object()
  with patch('serial.tools.list_ports.comports',return_value=[self.usb()]),patch.object(self.app,'run') as run:
   self.app.refresh_ports();run.assert_not_called();self.assertEqual(self.app.port.get(),'')
  self.app.process=None
 def test_rom_identification_binds_mac_but_no_identity_rights(self):
  self.app.port.set('COM5')
  self.app.render_report({'result':'OK','mac':'02:03:04:05:06:07','chip':'esp32c3','identification_mode':'rom','flash_bytes':4194304},'identify-rom','COM5')
  self.assertEqual(self.app.snapshot['mac'],'02:03:04:05:06:07')
  self.assertEqual(self.app.peer_port,'COM5');self.assertFalse(gui.backend.capabilities(self.app.snapshot)['identity_write'])
 def test_full_forms_have_localized_labels(self):
  for name,_ in gui.backend.factory.FIELDS:
   self.assertIn(name,self.app.fields);self.assertIn(name,gui.FIELD_LABELS)
 def test_provision_job_binds_current_mac_and_epoch(self):
  self.app.port.set('COM5')
  snap={'firmware':'new','mac':'02:03:04:05:06:07','factory_api':3,'identity_schema':2,'provisioning_identity_v2':True,'factory_write_enabled':True}
  self.app.render_report(snap,'inspect','COM5')
  self.app.fields['sn'].set('H1ZZX00139000000');self.app.fields['batch'].set('B1')
  with patch.object(self.app.message,'askyesno',return_value=True),patch('subprocess.Popen') as process:
   self.app.run('provision')
  self.assertEqual(self.app.job['expected_mac'],snap['mac']);self.assertEqual(self.app.job['port_epoch'],self.app.inventory.epoch('COM5'))
  self.app.process=None
 def test_stale_worker_result_cannot_restore_disconnected_identity(self):
  self.app.port.set('COM5');self.app.next_inventory=float('inf')
  output=Path(self.folder.name)/'out.json';log=Path(self.folder.name)/'out.log'
  gui.backend.factory.save(output,{'result':'OK','firmware':'old peer','mac':'02:03:04:05:06:07'})
  self.app.job={'operation':'inspect','port':'COM5','port_epoch':0,'log':str(log),'output':str(output)}
  self.app.inventory.revisions['COM5']=2;self.app.process=Mock();self.app.process.poll.return_value=0
  self.app.poll()
  self.assertIsNone(self.app.snapshot);self.assertIsNone(self.app.last_report);self.assertIsNone(self.app.process)
 def test_pending_persisted_sn_never_appears_as_running_sn(self):
  self.app.port.set('COM5')
  self.app.render_report({'firmware':'new','runtime_sn':'','runtime_generation':0,'identity_pending_reboot':True,
                          'identity':{'sn':'H1ZZX00139000000','generation':4}},'inspect','COM5')
  self.assertNotIn('H1ZZX00139000000',self.app.stats['sn'].get())
  self.assertIn('H1ZZX00139000000',self.app.stats['configuration'].get())
  self.assertIn('等待重启',self.app.stats['configuration'].get())

class SelfTestProof(unittest.TestCase):
 def test_self_test_records_v2_fields_pending_and_discovery_checks(self):
  with tempfile.TemporaryDirectory() as directory:
   output=Path(directory)/'proof.json';gui.self_test(output);proof=gui.backend.load_json(output)
  for name in ('extended_identity_fields_checked','pending_identity_ui_checked','usb_auto_discovery_checked','firmware_v2_gate_checked'):
   self.assertIs(proof.get(name),True,name)
  self.assertEqual(proof['identity_field_count'],len(gui.backend.factory.FIELDS))
  self.assertFalse(proof['hardware_accessed'])

if __name__=='__main__':unittest.main()
