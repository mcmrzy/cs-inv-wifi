"""Check ESP32-C3 build artifacts with Python's standard library only.

RAM is the union of allocated ELF sections in physical SRAM/RTC ranges. The
IRAM alias is translated to DRAM before merging, including linker dummy space.
This is static allocation, not runtime heap/stack headroom. Flash symbol sizes
describe ELF allocations; the actual .bin length governs the OTA budget.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import sys

DEFAULT_LIMITS = Path(__file__).with_name('firmware_budget.json')
UINT32_MAX = 0xFFFFFFFF


def bounded(data, offset, size):
    if offset < 0 or size < 0 or offset > len(data) or size > len(data)-offset:
        raise ValueError('ELF file range is out of bounds')
    return data[offset:offset+size]


def cstring(data, offset):
    if offset >= len(data): raise ValueError('ELF string offset is out of bounds')
    end = data.find(b'\0', offset)
    if end < 0: raise ValueError('ELF string lacks terminator')
    return data[offset:end].decode('utf-8', errors='replace')


def region(address, size):
    end = address+size
    if end > 2**32: raise ValueError('ELF address range overflows 32 bits')
    if 0x3FC7C000 <= address < 0x3FCE0000 and end <= 0x3FCE0000:
        return 'ram', address
    if 0x4037C000 <= address < 0x403E0000 and end <= 0x403E0000:
        return 'ram', address-0x700000
    if 0x50000000 <= address < 0x50002000 and end <= 0x50002000:
        return 'ram', address
    if ((0x3C000000 <= address < 0x3E000000 and end <= 0x3E000000) or
            (0x42000000 <= address < 0x44000000 and end <= 0x44000000)):
        return 'flash', address
    return 'other', address


def union_size(ranges):
    total = 0; previous_end = 0
    for start, end in sorted(ranges):
        total += max(0, end-max(start, previous_end))
        previous_end = max(previous_end, end)
    return total


def parse_elf(data):
    header = bounded(data, 0, 52)
    if header[:7] != b'\x7fELF\x01\x01\x01':
        raise ValueError('expected little-endian ELF32 version 1')
    _, kind, machine, version, _, _, shoff, _, ehsize, _, _, shsize, shnum, shstr = struct.unpack('<16sHHIIIIIHHHHHH', header)
    if kind != 2 or machine != 243 or version != 1 or ehsize != 52:
        raise ValueError('expected executable RISC-V ELF32')
    if shsize != 40 or not shnum or shstr >= shnum:
        raise ValueError('invalid/extended ELF section table')
    table = bounded(data, shoff, shnum*shsize)
    sections = [struct.unpack_from('<10I', table, i*40) for i in range(shnum)]
    names_section = sections[shstr]
    if names_section[1] != 3: raise ValueError('invalid section-name string table')
    names = bounded(data, names_section[4], names_section[5])
    report_sections = []; ranges = []; flash_ranges = []; symbols = {'ram': [], 'flash': []}
    for section in sections:
        name_index, stype, flags, address, offset, size, link, _, _, entsize = section
        name = cstring(names, name_index)
        if stype != 8: bounded(data, offset, size)
        place, physical = region(address, size)
        if flags & 2 and size:
            if place == 'other': raise ValueError('unrecognized allocated ELF memory region: '+name)
            report_sections.append({'name': name, 'bytes': size, 'region': place, 'address': hex(address), 'nobits': stype == 8})
            if place == 'ram': ranges.append((physical, physical+size))
            elif stype != 8: flash_ranges.append((address, address+size))
        if stype != 2: continue
        if entsize != 16 or size % 16 or link >= shnum or sections[link][1] != 3:
            raise ValueError('invalid ELF symbol table')
        strings = bounded(data, sections[link][4], sections[link][5])
        entries = bounded(data, offset, size)
        for at in range(0, size, 16):
            string_index, value, length, info, _, index = struct.unpack_from('<IIIBBH', entries, at)
            if not length or info & 15 not in (1, 2): continue
            symbol_name = cstring(strings, string_index)
            if index == 0 or index >= 0xFF00: continue
            if index >= shnum: raise ValueError('ELF symbol has invalid section index')
            owner = sections[index]
            if not owner[2] & 2: continue
            if value < owner[3] or value+length > owner[3]+owner[5]:
                raise ValueError('ELF symbol exceeds owning section')
            memory, _ = region(value, length)
            if memory in symbols:
                symbols[memory].append({'name': symbol_name, 'bytes': length, 'address': hex(value)})
    for memory in symbols:
        unique = {(s['address'], s['bytes'], s['name']): s for s in symbols[memory]}
        symbols[memory] = sorted(unique.values(), key=lambda s: (-s['bytes'], s['name']))[:20]
    return {'static_ram_bytes': union_size(ranges), 'flash_alloc_bytes': union_size(flash_ranges),
            'sections': report_sections, 'largest_symbols': symbols}


def parse_partitions(data, name):
    if not data or len(data) % 32: raise ValueError('invalid partition table length')
    found = []
    for at in range(0, len(data), 32):
        entry = data[at:at+32]
        if entry == b'\xff'*32: break
        magic = struct.unpack_from('<H', entry)[0]
        if magic == 0xEBEB: continue  # ESP-IDF partition MD5 record
        if magic != 0x50AA: raise ValueError('invalid partition entry magic')
        _, kind, subtype, offset, size, label, _ = struct.unpack('<HBBII16sI', entry)
        if not size or offset+size > 2**32: raise ValueError('partition size/address is invalid')
        label = label.split(b'\0', 1)[0].decode('ascii')
        if label == name:
            if kind != 0: raise ValueError('selected partition is not an app partition')
            found.append({'name': label, 'offset': hex(offset), 'size_bytes': size, 'subtype': subtype})
    if len(found) != 1: raise ValueError('app partition missing or duplicated: '+name)
    return found[0]


def validate_limits(limits):
    keys = ('max_app_bytes', 'min_partition_free_bytes', 'max_static_ram_bytes')
    if not isinstance(limits, dict): raise ValueError('budget must be a JSON object')
    if set(limits) != set(keys): raise ValueError('budget must contain exactly '+', '.join(keys))
    if any(type(limits[k]) is not int or not 0 <= limits[k] <= UINT32_MAX for k in keys):
        raise ValueError('budget values must be unsigned 32-bit integers')
    if not limits['max_app_bytes'] or not limits['max_static_ram_bytes']:
        raise ValueError('maximum budgets must be positive')


def violations(app, partition, ram, limits):
    validate_limits(limits)
    errors = []
    if app <= 0: errors.append('app binary is empty')
    if app > partition: errors.append('app binary exceeds OTA partition')
    if app > limits['max_app_bytes']: errors.append('app binary exceeds max_app_bytes')
    if partition-app < limits['min_partition_free_bytes']: errors.append('OTA partition free bytes below minimum')
    if ram > limits['max_static_ram_bytes']: errors.append('static RAM exceeds max_static_ram_bytes')
    return errors


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir', type=Path, required=True)
    parser.add_argument('--budget', type=Path, default=DEFAULT_LIMITS)
    parser.add_argument('--partition-name', default='ota_0')
    parser.add_argument('--output', type=Path)
    args = parser.parse_args(argv)
    try:
        limits = json.loads(args.budget.read_text(encoding='utf-8'))
        validate_limits(limits)
        description = json.loads((args.build_dir/'project_description.json').read_text(encoding='utf-8'))
        project = description['project_name']
        if not isinstance(project, str) or not project or Path(project).name != project or '/' in project or '\\' in project:
            raise ValueError('invalid project artifact name')
        app = (args.build_dir/(project+'.bin')).read_bytes()
        elf = (args.build_dir/(project+'.elf')).read_bytes()
        partition = parse_partitions((args.build_dir/'partition_table/partition-table.bin').read_bytes(), args.partition_name)
        parsed = parse_elf(elf)
        errors = violations(len(app), partition['size_bytes'], parsed['static_ram_bytes'], limits)
        report = {'project': project, 'app_bytes': len(app), 'app_sha256': hashlib.sha256(app).hexdigest(),
                  'elf_sha256': hashlib.sha256(elf).hexdigest(), 'partition': partition,
                  'partition_free_bytes': partition['size_bytes']-len(app),
                  'partition_free_percent': round((partition['size_bytes']-len(app))*100/partition['size_bytes'], 3),
                  **parsed, 'limits': limits, 'violations': errors, 'passed': not errors,
                  'scope': 'Static ELF allocation and actual app binary only; runtime largest heap and stack watermarks require device measurements.'}
        encoded = json.dumps(report, ensure_ascii=True, indent=2)+'\n'
        if args.output:
            args.output.parent.mkdir(parents=True, exist_ok=True)
            args.output.write_text(encoded, encoding='utf-8')
        print(encoded, end='')
        return 1 if errors else 0
    except (OSError, ValueError, KeyError, TypeError, UnicodeError, struct.error) as error:
        # No input content or configuration values are printed on parse failure.
        print('firmware budget error: '+type(error).__name__, file=sys.stderr)
        return 2


if __name__ == '__main__': sys.exit(main())
