"""Verify and package USB diagnostics. Never opens a serial port or flashes."""
from pathlib import Path
import hashlib
import json
import shutil
import struct

root=Path(__file__).resolve().parents[1]
build=root/'build'
out=root/'dist/20261030-v0.1.1-uart-diag'
sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
project=json.loads((build/'project_description.json').read_text(encoding='utf-8'))
args=json.loads((build/'flasher_args.json').read_text(encoding='utf-8'))
assert project['project_name']=='cs_inv_uart_diag'
assert project['project_version']=='0.1.1-uart-diag'
assert project['git_revision']=='v5.5.4'
assert project['target']==args['extra_esptool_args']['chip']=='esp32c3'
assert args['flash_settings']['flash_size']=='4MB' and int(args['app']['offset'],0)==0x20000
config=(build/'config/sdkconfig.h').read_text(encoding='utf-8-sig')
assert '#define CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG 1' in config
assert '#define CONFIG_ESP_CONSOLE_UART_NUM -1' in config
assert '#define CONFIG_ESP_MAIN_TASK_STACK_SIZE 8192' in config
app=build/args['app']['file'];data=app.read_bytes()
assert data[0]==0xe9 and data[48:80].split(b'\0')[0]==b'0.1.1-uart-diag'
assert data[80:112].split(b'\0')[0]==b'cs_inv_uart_diag'
partitions={}
table=(build/args['partition-table']['file']).read_bytes()
for offset in range(0,len(table)-31,32):
    magic,kind,subtype,start,size,label,flags=struct.unpack_from('<HBBII16sI',table,offset)
    if magic!=0x50aa:break
    partitions[label.split(b'\0')[0].decode()]={'offset':start,'bytes':size,'type':kind,'subtype':subtype}
assert partitions['ota_0']['offset']==0x20000 and partitions['ota_1']['offset']==0x1c0000
assert len(data)<=partitions['ota_0']['bytes']==0x1a0000
assert table==(root.parent/'esp32_modbus_panel/build_full/partition_table/partition-table.bin').read_bytes()
host=(root/'host_test_output.log').read_text(encoding='utf-8-sig')
for marker in ['PASS commands:','PASS replies:','PASS runner:','PASS USB queue:','PASS UART flood:']:assert marker in host
assert 'failed' not in host.lower() and 'error:' not in host.lower()
log=(root/'build_output.log').read_text(encoding='utf-8-sig')
assert 'Project build complete.' in log and 'error:' not in log.lower()
compiled=set()
for entry in json.loads((build/'compile_commands.json').read_text(encoding='utf-8')):
    p=Path(entry['file']).resolve()
    assert not any(s in p.as_posix().lower() for s in ['/components/esp_wifi/','/components/bt/']),p
    if p.is_relative_to(root.parent) and not p.is_relative_to(build):
        assert p.stat().st_mtime<=app.stat().st_mtime,f'Rebuild after {p}'
        compiled.add(p)
assert compiled=={root/'main/main.c',root/'main/diag_core.c',root.parent/'esp32_modbus_panel/main/panel_modbus.c'},compiled
out.mkdir(parents=True,exist_ok=True)
image=bytearray(b'\xff')*(4*1024*1024);segments=[]
for address,relative in sorted(args['flash_files'].items(),key=lambda x:int(x[0],0)):
    start=int(address,0);p=(build/relative).resolve();assert p.is_relative_to(build)
    blob=p.read_bytes();end=start+len(blob)
    assert 0<=start<end<=len(image)
    assert not any(start<s['end'] and end>s['start'] for s in segments)
    image[start:end]=blob
    segments.append({'start':start,'end':end,'source':relative,'sha256':sha(p)})
app_name='cs_inv_uart_diag_v0.1.1_app_0x20000.bin'
full_name='cs_inv_uart_diag_v0.1.1_4MB_0x0.bin'
shutil.copyfile(app,out/app_name);(out/full_name).write_bytes(image)
assert image[0x20000:0x20000+len(data)]==data
files=[app_name,full_name]
for source,target in [('README.md','USB串口测试说明.md'),('console.ps1','console.ps1'),
                      ('host_test_output.log','host_validation.txt'),('build_output.log','idf_build.txt'),
                      ('build/cs_inv_uart_diag.elf','cs_inv_uart_diag.elf')]:
    shutil.copyfile(root/source,out/target);files.append(target)
(out/'先看这里.txt').write_text(
    'ESP32-C3 USB TTL诊断固件 0.1.1\n'
    '完整4MB镜像写0x0，会清空原面板配置和身份。App镜像初始槽为0x20000，先核对当前启动槽。\n'
    'USB COM口115200；输入help回车。逆变器UART默认TX0 RX1、9600 8N1、地址1。\n'
    '旧direct源码DSP波特率115200：先baud 115200，再legacy；swap后再legacy。\n'
    'Modbus：timeout 200 / verbose 0 / hunt 1 16；完整地址范围hunt 1 247耗时较长，stop停止。\n'
    '找到后status、read 0042 1、poll 0042 1 1000、stats；截图完整TX/RX/RESULT。\n'
    '屏幕/网络/BMS均不启动，配置只在RAM；未自动连接、烧录或验证实际逆变器。\n',encoding='utf-8')
files.append('先看这里.txt')
sources={p for folder in ['main','tests','tools'] for p in (root/folder).rglob('*') if p.is_file() and '__pycache__' not in str(p)}
sources.update(root/p for p in ['CMakeLists.txt','partitions.csv','sdkconfig.defaults','sdkconfig','build.ps1','console.ps1','README.md'])
sources.update(root.parent/p for p in ['esp32_modbus_panel/main/panel_modbus.c','esp32_modbus_panel/main/panel_modbus.h',
    'esp32_direct/main/direct/direct_protocol.c','esp32_direct/main/direct/direct_protocol.h'])
manifest={'version':'0.1.1-uart-diag','date':'2026-10-30','product':'CS-MB-UART-DIAG','idf':project['git_revision'],
    'chip':'esp32c3','app_offset':0x20000,'app_bytes':len(data),'partitions':partitions,
    'usb':'native USB Serial/JTAG GPIO18/19','default_uart':{'port':0,'tx':0,'rx':1,'baud':9600,'format':'8N1','address':1},
    'search_read_only':True,'startup_transmissions':False,'nvs_writes':False,'efuse_writes':False,
    'usb_command_queue_depth':8,'stop_clears_pending_commands':True,'probe_logs_actual_config':True,
    'continuous_rx_yields':True,'previous_wdt_root_cause_confirmed':False,
    'hardware_validated':False,'device_flashed':False,
    'warning':'Full 4MB image at 0 clears persistent configuration and identity. Verify active slot before App-only USB flash.',
    'host_tests':['command boundaries','protocol CRC/matching/echo/legacy compatibility','actual runner with deterministic UART/USB mocks'],
    'compiled_project_files':sorted(str(p.relative_to(root.parent)).replace('\\','/') for p in compiled),
    'source_sha256':{str(p.relative_to(root.parent)).replace('\\','/'):sha(p) for p in sorted(sources)},
    'segments':segments,'files':{name:{'bytes':(out/name).stat().st_size,'sha256':sha(out/name)} for name in files}}
(out/'manifest.json').write_text(json.dumps(manifest,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
archive=Path(shutil.make_archive(str(out.parent/'CS-MB-UART-DIAG_v0.1.1'),'zip',root_dir=out.parent,base_dir=out.name))
archive.with_suffix('.zip.sha256').write_text(f'{sha(archive)}  {archive.name}\n',encoding='ascii')
print(f'PASS package: {len(compiled)} project sources, native USB, app metadata, matching dual OTA layout, host checks')
for name in [app_name,full_name]:print(f'{name}: {manifest["files"][name]}')
print(f'{archive.name}: {archive.stat().st_size} bytes SHA256 {sha(archive)}')
