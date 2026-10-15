from pathlib import Path
import hashlib
import sys

root = Path(__file__).resolve().parents[1]
version = sys.argv[1] if len(sys.argv)>1 else '0.3.0-direct-graphical'
release = root / (sys.argv[2] if len(sys.argv)>2 else 'release/esp32c3_v0.3.0_graphical_20261008')
merged = next(release.glob('*_4MB.bin'))
image = merged.read_bytes()
assert len(image) == 4 * 1024 * 1024
parts = [(0, 'bootloader/bootloader.bin'), (0x8000, 'partition_table/partition-table.bin'),
         (0x10000, 'ota_data_initial.bin'), (0x20000, 'cs_inv_direct_c3.bin')]
end = 0
for offset, filename in parts:
    part = (root / 'build' / filename).read_bytes()
    assert image[end:offset] == b'\xff' * (offset-end), f'gap before {filename}'
    assert image[offset:offset+len(part)] == part, filename
    end = offset + len(part)
assert image[end:] == b'\xff' * (len(image)-end)
app = (root / 'build/cs_inv_direct_c3.bin').read_bytes()
for marker in [version.encode(), b'LCD_GRAPHICAL:', b'LCD_RENDER ms=', b'LCD CS_FIXED_LOW:']:
    assert marker in app, marker
if version == '0.3.1-direct-vivid':
    assert b'LCD ARM_PROFILE:' in app
    assert b'LCD MINIMAL_INIT:' not in app
if version in ('0.3.2-direct-readable', '0.3.3-direct-lcd-test'):
    assert b'LCD MINIMAL_INIT:' in app
    assert b'LCD ARM_PROFILE:' not in app
if version == '0.3.3-direct-lcd-test':
    assert b'LCD_VIEW_TEST inversion=' in app
if version == '0.4.0-direct-bilingual':
    assert b'LCD MINIMAL_INIT:' in app
    assert b'LCD ARM_PROFILE:' not in app
if version in ('0.4.1-boot-splash', '0.4.2-direct-noto', '0.4.3-unified-font'):
    assert b'LCD MINIMAL_INIT:' in app
    assert b'LCD ARM_PROFILE:' not in app
    assert b'LCD_BOOT_SPLASH' in app
    assert b'direct_boot_logo' in app
assert next(release.glob('*_app.bin')).read_bytes() == app
files = sorted(release.glob('*.bin'))
(release/'SHA256SUMS.txt').write_text(''.join(f'{hashlib.sha256(p.read_bytes()).hexdigest()}  {p.name}\n' for p in files), encoding='ascii')
print('PASS: 4194304 bytes, four segments identical, gaps/tail FF, app markers, app copy and SHA256')
