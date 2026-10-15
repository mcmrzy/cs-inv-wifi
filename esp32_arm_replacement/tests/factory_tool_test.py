import importlib.util, unittest, struct, sqlite3
from pathlib import Path
from unittest.mock import patch
spec=importlib.util.spec_from_file_location("tool",Path(__file__).resolve().parents[1]/"tools/factory_tool.py")
t=importlib.util.module_from_spec(spec);spec.loader.exec_module(t)
class Test(unittest.TestCase):
 def test_optional_bms_query(self):
  class Client:
   def __init__(self):self.calls=[]
   def request(self,command,payload):
    self.calls.append((command,payload))
    return {'pending':len(self.calls)<3,'valid_mask':0,'capability':'QUERYING' if len(self.calls)<3 else 'UNSUPPORTED_OR_NO_REPLY','upgrade_authorized':False}
  c=Client()
  with patch.object(t.time,'sleep'):
   report=t.query_bms_identity(c)
  self.assertEqual(c.calls,[(7,b'\1'),(7,b'\0'),(7,b'\0')]);self.assertFalse(report['upgrade_authorized'])
  self.assertEqual(report['capability'],'UNSUPPORTED_OR_NO_REPLY')
 def test_registry_reservation(self):
  with sqlite3.connect(':memory:') as db:
   t.reserve_identity(db,'SN1','MAC1')
   t.reserve_identity(db,'SN1','MAC1')
   with self.assertRaises(RuntimeError):t.reserve_identity(db,'SN1','MAC2')
   with self.assertRaises(RuntimeError):t.reserve_identity(db,'SN2','MAC1')
   self.assertEqual(db.execute('SELECT sn,mac FROM devices').fetchall(),[('SN1','MAC1')])
 def test_wire_noise(self):
  f=t.frame(0x81,123,b'{"result":"OK"}');p=t.Parser();self.assertEqual(p.feed(b"logs\n"+f)[0],(0x81,123,b'{"result":"OK"}'))
  f=bytearray(f);f[-1]^=1;self.assertFalse(p.feed(f));self.assertEqual(len(p.feed(t.frame(0x81,124,b'{}'))),1)
 def test_record(self):
  d={"sn":"H1ZZX0013900001P","model":"CS-L10-6K2","hw_system":"UNSET","hw_controller":"UNSET","batch":"test","production_time":"2026-10-12T00:00:00Z"}
  wire=t.record(d,"02:03:04:05:06:07");self.assertEqual(len(wire),336);self.assertEqual(wire[14:20],bytes([2,3,4,5,6,7]))
  self.assertRaises(ValueError,t.record,{**d,"sn":"X"*32},"02:03:04:05:06:07")
 def test_limits(self):
  self.assertEqual(t.evaluate(48,48,None)["status"],"PENDING")
  self.assertEqual(t.evaluate(48.1,48,{"absolute":0.2})["status"],"PASS")
  self.assertEqual(t.evaluate(49,48,{"absolute":0.2})["status"],"FAIL")
  self.assertEqual(t.evaluate(None,48,{"absolute":0.2})["status"],"PENDING")
  self.assertRaises(ValueError,t.evaluate,48,48,{"absolute":-1})
if __name__=="__main__":unittest.main()
