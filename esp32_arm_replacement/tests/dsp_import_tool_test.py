import importlib.util
import json
import tempfile
import unittest
from pathlib import Path
root=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('dsp_import',root/'tools/dsp_params_import.py')
tool=importlib.util.module_from_spec(spec);spec.loader.exec_module(tool)
class ImportTest(unittest.TestCase):
    def profile(self):
        p={'sn':'H1ZZX00139000079'}
        for cmd,n in enumerate((25,23,16)):
            data=bytes((1,n-4,cmd))+bytes(n-5);data+=tool.crc16(data).to_bytes(2,'little');p[f'cmd{cmd:02}']=data.hex()
        return p
    def load(self,p):
        with tempfile.TemporaryDirectory(dir=root/'tmp') as folder:
            file=Path(folder)/'parameters.json';file.write_text(json.dumps(p),encoding='utf-8');return tool.load_profile(file)
    def test_exact(self):
        sn,frames=self.load(self.profile());self.assertEqual(sn,'H1ZZX00139000079');self.assertEqual(list(map(len,frames)),[25,23,16])
    def test_reject(self):
        for field,value in [('sn','wrong'),('cmd00','0141002190'),('cmd01',''),('cmd02','abc')]:
            p=self.profile();p[field]=value
            with self.assertRaises(ValueError):self.load(p)
if __name__=='__main__':unittest.main()
