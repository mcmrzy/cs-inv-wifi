import sys, unittest
from pathlib import Path
from types import SimpleNamespace
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
import factory_discovery as discovery

def port(name='COM5',vid=0x303A,pid=0x1001,serial='ONE'):
 return SimpleNamespace(device=name,vid=vid,pid=pid,serial_number=serial,location='1-1',hwid='USB',description='USB Serial')

class DiscoveryTests(unittest.TestCase):
 def test_single_supported_device_is_inspected_once(self):
  d=discovery.Inventory()
  self.assertEqual(d.update([port()],selected='')['inspect'],'COM5')
  self.assertIsNone(d.update([port()],selected='COM5')['inspect'])
 def test_other_serial_ports_are_never_auto_probed(self):
  d=discovery.Inventory()
  self.assertIsNone(d.update([port(vid=0x0403)],selected='')['inspect'])
 def test_multiple_candidates_require_selection(self):
  d=discovery.Inventory()
  r=d.update([port(),port('COM6',serial='TWO')],selected='')
  self.assertTrue(r['multiple']);self.assertIsNone(r['inspect'])
 def test_busy_defers_probe_until_idle(self):
  d=discovery.Inventory()
  self.assertIsNone(d.update([port()],selected='',busy=True)['inspect'])
  self.assertEqual(d.update([port()],selected='',busy=False)['inspect'],'COM5')
 def test_remove_and_same_name_reuse_invalidates_snapshot(self):
  d=discovery.Inventory();d.update([port()],selected='COM5');epoch=d.epoch('COM5')
  r=d.update([],selected='COM5');self.assertIn('COM5',r['invalidated'])
  d.update([port(serial='TWO')],selected='COM5')
  self.assertNotEqual(epoch,d.epoch('COM5'))
 def test_same_port_changed_usb_identity_invalidates_before_probe(self):
  d=discovery.Inventory();d.update([port()],selected='COM5')
  r=d.update([port(serial='TWO')],selected='COM5')
  self.assertIn('COM5',r['invalidated']);self.assertEqual(r['inspect'],'COM5')

if __name__=='__main__':unittest.main()
