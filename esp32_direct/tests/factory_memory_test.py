"""The USB service must keep one full-capacity wire buffer, not a copied request."""
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]


class FactoryMemoryTest(unittest.TestCase):
    def test_single_full_capacity_wire_storage(self):
        source = (ROOT / 'main/factory/factory_service.c').read_text(encoding='utf-8')
        self.assertNotRegex(source, r'static\s+factory_frame_t\s+request\s*;',
                            'USB request still duplicates the parser 4096-byte payload')
        self.assertRegex(source, r'static\s+factory_frame_view_t\s+request\s*;')
        self.assertIn('factory_parser_feed_view(&parser,rx[i],&request)', source)
        self.assertIn('factory_frame_view_encode(', source)
        # Schema-2 provisioning and serial recovery remain available at full capacity.
        self.assertIn('factory_record_decode(request.payload,request.length,&r)', source)
        self.assertIn('serial_result(result,sn)', source)
        header = (ROOT / 'main/factory/factory_wire.h').read_text(encoding='utf-8')
        self.assertRegex(header, r'#define\s+FACTORY_PAYLOAD_MAX\s+4096\b')


if __name__ == '__main__':
    unittest.main()
