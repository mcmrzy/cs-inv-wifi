"""Package the LED-only diagnostic. Does not open serial ports or flash devices."""
from pathlib import Path
import hashlib
import json
import shutil
import struct
import subprocess

root = Path(__file__).resolve().parents[1]
build = root / 'build'
out = root / 'dist/20261103-led1-only'
sha = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
project = json.loads((build / 'project_description.json').read_text(encoding='utf-8'))
assert project['project_name'] == 'cs_inv_led1_diag'
assert project['project_version'] == '0.1.0-led1-diag'
args = json.loads((build / 'flasher_args.json').read_text(encoding='utf-8'))
app = build / args['app']['file']
assert int(args['app']['offset'], 0) == 0x20000
assert app.read_bytes()[48:80].split(b'\0')[0] == b'0.1.0-led1-diag'
parts = {}
raw = (build / args['partition-table']['file']).read_bytes()
for offset in range(0, len(raw) - 31, 32):
    magic, kind, subtype, start, size, label, flags = struct.unpack_from('<HBBII16sI', raw, offset)
    if magic != 0x50aa:
        break
    parts[label.split(b'\0')[0].decode()] = (start, size)
assert parts['ota_0'] == (0x20000, 0x1a0000)
assert parts['ota_1'] == (0x1c0000, 0x1a0000)
assert parts['nvs'] == (0x9000, 0x6000)
assert parts['cache'] == (0x360000, 0x98000)
assert parts['factory'] == (0x3f8000, 0x8000)
assert len(app.read_bytes()) <= parts['ota_0'][1]
nm = Path('C:/Users/29538/.espressif/tools/riscv32-esp-elf/esp-14.2.0_20260121/riscv32-esp-elf/bin/riscv32-esp-elf-nm.exe')
symbols = subprocess.check_output([nm, '--defined-only', build / 'cs_inv_led1_diag.elf'], encoding='utf-8')
names = {line.split()[-1] for line in symbols.splitlines() if line.split()}
for forbidden in ['esp_wifi_init', 'esp_wifi_start', 'nimble_port_init', 'esp_bt_controller_init',
                  'esp_mqtt_client_init', 'nvs_flash_init', 'nvs_set_blob', 'panel_task',
                  'direct_panel_init', 'direct_keys_init', 'arm_poll_init', 'uart_driver_install']:
    assert forbidden not in names, forbidden
assert 'app_main' in names and 'rmt_transmit' in names
subprocess.run([root / 'tmp/waveform_test.exe'], check=True)
assert 'Project build complete.' in (root / 'build_output.log').read_text(encoding='utf-8-sig')
out.mkdir(parents=True, exist_ok=True)
files = {
    'bootloader.bin': build / args['bootloader']['file'],
    'partition-table.bin': build / args['partition-table']['file'],
    'ota_data_initial.bin': build / args['otadata']['file'],
    'led1_only_red_app_0x20000.bin': app,
    '使用说明.md': root / 'README.md',
    'idf_build.txt': root / 'build_output.log',
}
for name, source in files.items():
    shutil.copyfile(source, out / name)
(out / '烧录测试.ps1').write_text(r'''param(
    [string]$Port='COM15',
    [string]$IdfPython='C:/Users/29538/.espressif/python_env/idf5.5_py3.13_env/Scripts/python.exe'
)
$ErrorActionPreference='Stop'
Push-Location $PSScriptRoot
try {
    & $IdfPython -m esptool --chip esp32c3 --port $Port --baud 460800 --before default_reset --after hard_reset write_flash --flash_mode dio --flash_size 4MB --flash_freq 80m 0x0 bootloader.bin 0x8000 partition-table.bin 0x10000 ota_data_initial.bin 0x20000 led1_only_red_app_0x20000.bin
    if($LASTEXITCODE -ne 0){throw "Flash failed: $LASTEXITCODE"}
} finally {Pop-Location}
''', encoding='utf-8-sig')
artifacts = {p.name: {'bytes': p.stat().st_size, 'sha256': sha(p)} for p in sorted(out.iterdir())
             if p.is_file() and p.name not in ('manifest.json', 'SHA256.txt')}
manifest = {
    'version': project['project_version'], 'date': '2026-11-03', 'target': 'esp32c3',
    'gpio': 10, 'wire_order': 'GRB', 'physical_leds': 3, 'led1_rgb': [64, 0, 0],
    'other_leds_rgb': [0, 0, 0], 'refresh_ms': 1000,
    'rmt_resolution_hz': 20000000, 'rmt_memory_words': 96, 'symbols': 73,
    'low_latch_us': 300, 'partitions': parts, 'app_offset': 0x20000,
    'elf_isolation_verified': True, 'waveform_decode_passed': True,
    'hardware_validated': False, 'device_flashed': False,
    'flash_preserves_data_partitions': True,
    'warning': 'USB diagnostic replaces the normal application. Restore normal firmware after testing. Do not erase the whole flash.',
    'files': artifacts,
    'source_sha256': {str(p.relative_to(root)): sha(p) for p in sorted(root.rglob('*'))
                      if p.is_file() and p.suffix in ('.c', '.h', '.py', '.ps1', '.csv')
                      and p.relative_to(root).parts[0] in ('main', 'tests', 'tools')},
}
(out / 'manifest.json').write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
(out / 'SHA256.txt').write_text(''.join(f'{v["sha256"]}  {name}\n' for name, v in artifacts.items()), encoding='utf-8')
archive = Path(shutil.make_archive(str(out.parent / 'LED1_ONLY_RED_20261103'), 'zip', out.parent, out.name))
archive.with_suffix('.zip.sha256').write_text(f'{sha(archive)}  {archive.name}\n', encoding='ascii')
print(f'PASS LED1 package: {app.stat().st_size} byte App, waveform decode and ELF isolation verified')
print(f'App: {out / "led1_only_red_app_0x20000.bin"}')
print(f'ZIP: {archive} SHA256={sha(archive)}')
