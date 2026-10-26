"""Boundary and budget tests using small ELF32 fixtures; no SDK required."""
from pathlib import Path
import importlib.util
import contextlib
import io
import json
import struct
import tempfile
import unittest

SCRIPT = Path(__file__).resolve().parents[1] / 'tools/firmware_budget.py'
spec = importlib.util.spec_from_file_location('firmware_budget', SCRIPT)
budget = importlib.util.module_from_spec(spec)
spec.loader.exec_module(budget)


def elf_fixture():
    names = b'\0.shstrtab\0.dram0.data\0.dram0.bss\0.flash.text\0.iram0.text\0.dram0.dummy\0'
    sections = [(0, 0, 0, 0, 0, 0, 0, 0, 0, 0)]
    sections.append((1, 3, 0, 0, 52, len(names), 0, 0, 1, 0))
    payload = names + bytes(64)
    sections.extend([
        (names.index(b'.dram0.data'), 1, 3, 0x3FC80000, 52+len(names), 16, 0, 0, 4, 0),
        (names.index(b'.dram0.bss'), 8, 3, 0x3FC80010, 0, 32, 0, 0, 4, 0),
        (names.index(b'.flash.text'), 1, 6, 0x42000000, 52+len(names)+16, 16, 0, 0, 4, 0),
        (names.index(b'.iram0.text'), 1, 6, 0x40380000, 52+len(names)+32, 16, 0, 0, 4, 0),
        (names.index(b'.dram0.dummy'), 8, 3, 0x3FC80000, 0, 16, 0, 0, 4, 0),
    ])
    header = struct.pack('<16sHHIIIIIHHHHHH', b'\x7fELF\x01\x01\x01'+bytes(9), 2, 243, 1,
                         0, 0, 52+len(payload), 0, 52, 0, 0, 40, len(sections), 1)
    return header + payload + b''.join(struct.pack('<10I', *s) for s in sections)


def partition(size=0x1A0000, offset=0x20000):
    return struct.pack('<HBBII16sI', 0x50AA, 0, 0x10, offset, size, b'ota_0', 0)+bytes([255])*32


def elf_symbols(value=0x3FC80000, length=16, string_index=1):
    original = elf_fixture()
    shoff = struct.unpack_from('<I', original, 32)[0]
    strings = b'\0sample_buffer\0'
    entries = bytes(16)+struct.pack('<IIIBBH', string_index, value, length, 0x11, 0, 2)
    header = bytearray(original[:52]); new_shoff = shoff+len(strings)+len(entries)
    struct.pack_into('<I', header, 32, new_shoff); struct.pack_into('<H', header, 48, 9)
    return bytes(header)+original[52:shoff]+strings+entries+original[shoff:]+struct.pack(
        '<10I', 0, 2, 0, 0, shoff+len(strings), len(entries), 8, 0, 4, 16)+struct.pack(
        '<10I', 0, 3, 0, 0, shoff, len(strings), 0, 0, 1, 0)


class BudgetTests(unittest.TestCase):
    def test_ram_aliases_are_not_double_counted(self):
        parsed = budget.parse_elf(elf_fixture())
        self.assertEqual(parsed['static_ram_bytes'], 48)
        self.assertEqual(parsed['flash_alloc_bytes'], 16)

    def test_bad_elf_inputs(self):
        for data in (b'', b'not elf', elf_fixture()[:51]):
            with self.subTest(length=len(data)), self.assertRaises(ValueError):
                budget.parse_elf(data)

    def test_unsupported_elf_class_endianness_machine(self):
        for pos, value in ((4, 2), (5, 2), (18, 3)):
            data = bytearray(elf_fixture()); data[pos] = value
            with self.subTest(pos=pos), self.assertRaises(ValueError):
                budget.parse_elf(data)

    def test_section_table_overflow(self):
        data = bytearray(elf_fixture()); struct.pack_into('<I', data, 32, 0xFFFFFFF0)
        with self.assertRaises(ValueError): budget.parse_elf(data)

    def test_section_file_range_overflow(self):
        data = bytearray(elf_fixture()); shoff = struct.unpack_from('<I', data, 32)[0]
        struct.pack_into('<I', data, shoff+2*40+16, 0xFFFFFFF0)
        with self.assertRaises(ValueError): budget.parse_elf(data)

    def test_section_address_overflow(self):
        data = bytearray(elf_fixture()); shoff = struct.unpack_from('<I', data, 32)[0]
        struct.pack_into('<I', data, shoff+3*40+12, 0xFFFFFFF0)
        with self.assertRaises(ValueError): budget.parse_elf(data)

    def test_symbol_table_bad_link_and_entry_size(self):
        for link, size in ((99, 16), (1, 8)):
            data = bytearray(elf_fixture()); shoff = struct.unpack_from('<I', data, 32)[0]
            struct.pack_into('<I', data, shoff+2*40+4, 2)
            struct.pack_into('<I', data, shoff+2*40+24, link)
            struct.pack_into('<I', data, shoff+2*40+36, size)
            with self.subTest(link=link, size=size), self.assertRaises(ValueError): budget.parse_elf(data)

    def test_partition_actual_size(self):
        self.assertEqual(budget.parse_partitions(partition(), 'ota_0')['size_bytes'], 0x1A0000)

    def test_partition_rejects_malformed_and_missing(self):
        for data in (b'', partition()[:31], bytes(32), partition(size=0), partition(size=32, offset=0xFFFFFFF0)):
            with self.subTest(data=data[:4]), self.assertRaises(ValueError): budget.parse_partitions(data, 'ota_0')
        with self.assertRaises(ValueError): budget.parse_partitions(partition(), 'ota_1')
        with self.assertRaises(ValueError): budget.parse_partitions(partition()[:32]*2, 'ota_0')

    def test_budget_and_partition_limits(self):
        limits = {'max_app_bytes': 90, 'min_partition_free_bytes': 10, 'max_static_ram_bytes': 48}
        self.assertEqual(budget.violations(90, 100, 48, limits), [])
        self.assertEqual(len(budget.violations(101, 100, 49, limits)), 4)
        self.assertTrue(budget.violations(0, 100, 0, limits))

    def test_invalid_limits(self):
        for value in (-1, True, '20', 2**32):
            with self.subTest(value=value), self.assertRaises(ValueError):
                budget.validate_limits({'max_app_bytes': value, 'min_partition_free_bytes': 1, 'max_static_ram_bytes': 48})

    def test_budget_requires_object(self):
        for limits in (None, [], 1, 'limits'):
            with self.subTest(limits=limits), self.assertRaises(ValueError): budget.validate_limits(limits)

    def test_symbol_reporting(self):
        symbols = budget.parse_elf(elf_symbols())['largest_symbols']['ram']
        self.assertEqual(symbols, [{'name': 'sample_buffer', 'bytes': 16, 'address': '0x3fc80000'}])

    def test_symbol_boundaries(self):
        for data in (elf_symbols(length=17), elf_symbols(value=0x3FC7FFFF), elf_symbols(string_index=99)):
            with self.subTest(length=len(data)), self.assertRaises(ValueError): budget.parse_elf(data)

    def test_cli_generates_report_and_nonzero_on_overflow(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory); (root/'partition_table').mkdir()
            (root/'project_description.json').write_text(json.dumps({'project_name': 'test'}))
            (root/'test.elf').write_bytes(elf_fixture())
            (root/'partition_table/partition-table.bin').write_bytes(partition(size=100))
            limits = root/'limits.json'; limits.write_text(json.dumps({
                'max_app_bytes': 90, 'min_partition_free_bytes': 10, 'max_static_ram_bytes': 48}))
            output = root/'report.json'
            for app_size, expected in ((90, 0), (91, 1), (101, 1), (0, 1)):
                (root/'test.bin').write_bytes(bytes(app_size))
                with contextlib.redirect_stdout(io.StringIO()):
                    code = budget.main(['--build-dir', directory, '--budget', str(limits), '--output', str(output)])
                self.assertEqual(code, expected)
                report = json.loads(output.read_text())
                self.assertEqual(report['app_bytes'], app_size)
                self.assertEqual(report['passed'], expected == 0)

    def test_cli_missing_artifacts_fails(self):
        with tempfile.TemporaryDirectory() as directory:
            self.assertNotEqual(budget.main(['--build-dir', directory]), 0)


if __name__ == '__main__': unittest.main()
