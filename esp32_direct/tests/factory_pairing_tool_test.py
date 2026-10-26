import os
import sys
import unittest
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import factory_pairing

class PairingTests(unittest.TestCase):
    def test_derivation_and_zero(self):
        import hashlib,hmac
        sn='H1ZZX00139000000'; secret='unit-test-only'
        want=f'{int.from_bytes(hmac.new(secret.encode(),sn.encode(),hashlib.sha256).digest()[:3],"big")%1000000:06d}'
        self.assertEqual(factory_pairing.compute_pin(sn,secret),want)
        self.assertEqual(factory_pairing.payload(sn,'000012')[-7:],b'000012\0')
    def test_missing_secret(self):
        with self.assertRaises(ValueError): factory_pairing.compute_pin('H1ZZX00139000000','')
    def test_invalid(self):
        for sn,pin in [('h1zzx00139000000','123456'),('SHORT','123456'),('H1ZZX00139000000','12345x')]:
            with self.assertRaises(ValueError):factory_pairing.payload(sn,pin)
if __name__=='__main__':unittest.main()
