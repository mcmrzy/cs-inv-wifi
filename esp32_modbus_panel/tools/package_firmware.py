"""Package the validated full build. Never opens a serial port or flashes a device."""
from pathlib import Path
import hashlib
import json
import shutil
import struct
import re
import subprocess
import sys

root=Path(__file__).resolve().parents[1]
build=root/'build_full'
sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
args=json.loads((build/'flasher_args.json').read_text(encoding='utf-8'))
project=json.loads((build/'project_description.json').read_text(encoding='utf-8'))
assert project['project_name']=='cs_inv_modbus_full'
version=re.search(r'PROJECT_VER "([0-9.]+)-modbus-full"',(root/'CMakeLists.txt').read_text()).group(1)
assert project['project_version']==f'{version}-modbus-full'
release_date='2026-11-12'
out=root/f"dist/{release_date.replace('-','')}-v{version}-full"
assert Path(project['config_file']).resolve()==(root/'sdkconfig.full').resolve()
config=(root/'sdkconfig.full').read_text(encoding='utf-8-sig')
for name,value in [('DIRECT_DSP_TX_PIN',1),('DIRECT_DSP_RX_PIN',0),
                   ('CS_INV_UART_TX_PIN',1),('CS_INV_UART_RX_PIN',0),
                   ('DIRECT_BMS_TX_PIN',21),('DIRECT_BMS_RX_PIN',20)]:
 assert re.search(r'^CONFIG_'+name+'='+str(value)+r'$',config,re.M),name
assert re.search(r'^CONFIG_DIRECT_LCD_BL_PIN=5$',config,re.M)
assert re.search(r'^CONFIG_PANEL_LED_BRIGHTNESS=24$',config,re.M)
assert re.search(r'^CONFIG_MQTT_REPORT_DELETED_MESSAGES=y$',config,re.M)
assert args['extra_esptool_args']['chip']=='esp32c3'
assert args['flash_settings']['flash_size']=='4MB'
assert int(args['app']['offset'],0)==0x20000
app=build/args['app']['file'];app_data=app.read_bytes()
assert app_data[0]==0xe9 and len(app_data)<=0x1a0000
# 0.4.11 adds the trusted CA bundle and panel cloud clients. Keep both slot
# offsets stable for existing units and reserve at least 56 KiB in each slot.
assert 0x1a0000-len(app_data)>=56*1024,'Release requires at least 56 KiB free per OTA slot'
assert app_data[48:80].split(b'\0')[0]==project['project_version'].encode('ascii')
metadata=app_data[288:352]
assert metadata[:6]==b'CSIV\x01\x00' and metadata[6] in (1,2,3) and metadata[7]==0
assert metadata[8:]==b'CS-MB-PANEL'+bytes(45)
partitions={}
part_data=(build/args['partition-table']['file']).read_bytes()
for offset in range(0,len(part_data)-31,32):
 magic,kind,subtype,start,size,label,flags=struct.unpack_from('<HBBII16sI',part_data,offset)
 if magic!=0x50aa:break
 partitions[label.split(b'\0')[0].decode()]={'type':kind,'subtype':subtype,'offset':start,'bytes':size}
assert partitions['ota_0']['offset']==0x20000 and partitions['ota_1']['offset']==0x1c0000
assert partitions['ota_0']['bytes']==partitions['ota_1']['bytes']==0x1a0000
assert partitions['cache']['offset']==0x360000 and partitions['factory']['offset']==0x3f8000
host=(root/'host_test_output.log').read_text(encoding='utf-8-sig')
for marker in ['PASS snapshot:','PASS Modbus stream','PASS control:','PASS UI:', 'PASS display:', 'PASS CHG direction:', 'PASS passive buzzer:',
               'PASS full mapping:','PASS full routes:','PASS local UI cleanup:','PASS hotfix:','PASS UART board:','PASS indicators:','PASS indicator hardware:','PASS display settings:','PASS display runtime:',
               'PASS alarms:','PASS alarm service:','PASS alarm transport:','PASS legacy alarm ownership:','PASS alarm integration:','PASS alarm cloud contract:',
               'PASS user panel:','PASS setting queries:','PASS LVGL native runtime:','PASS LVGL glyph packing:','PASS LVGL alignment:','PASS LVGL boot:','PASS LVGL pool:','PASS font codec:','PASS user journey:','PASS BLE memory admission:',
               'PASS product flow:','PASS actual UART owner:','PASS incremental control:',
               'PASS panel Wi-Fi:','PASS Wi-Fi profiles:','PASS JSON numeric formatting:',
               'PASS LVGL brand splash:','PASS English layout:','PASS connected panel UI:',
               'PASS weather display:','PASS weather:','PASS pairing:','PASS firmware catalog:',
               'PASS panel OTA TLS:',
               'PASS native English vocabulary:','PASS native state:','PASS bus recovery:','PASS BMS recovery:','PASS BMS auto detect:','PASS CSERGY BMS:','PASS bus startup:',
               'PASS preferences:','PASS time transaction:','PASS private protocol rejection and protocol-only MQTT receipt:','PASS BLE product capability:',
               'PASS native panel BLE:','PASS BLE panel telemetry and preferences:',
               'PASS keys: KEY1 ENTER','PASS: BS818A decode','PASS production file commit,','PASS 5000 lossless records,','PASS real V3 offline admission and replay:']:
 assert marker in host,marker
assert 'FAIL' not in host
assert 'smooth 2s breathing' in host
build_log=(root/'build_full_output.log').read_text(encoding='utf-8-sig')
assert 'Project build complete.' in build_log
compiled=set()
for entry in json.loads((build/'compile_commands.json').read_text(encoding='utf-8')):
 p=Path(entry['file'])
 if not p.is_absolute():p=Path(entry['directory'])/p
 p=p.resolve()
 if p.is_file() and p.is_relative_to(root.parent.resolve()) and not p.is_relative_to(build.resolve()):
  assert p.stat().st_mtime<=app.stat().st_mtime,f'Rebuild after editing {p}'
  compiled.add(p)
assert (root/'full/direct_poll.c').resolve() in compiled
assert (root/'full/direct_ui.c').resolve() in compiled
assert (root.parent/'esp32_direct/main/wifi/wifi_sta.c').resolve() in compiled
assert (root.parent/'esp32_direct/main/ota/ble_ota.c').resolve() in compiled
assert (root/'full/ota_service.c').resolve() in compiled
for relative in ['full/panel_indicators_hw.c','main/panel_indicators.c','full/panel_backlight.c','main/panel_display_settings.c','main/panel_settings_query.c','full/panel_lvgl.c','full/panel_home_art.c','full/bms_bbj_frame.c','main/panel_alarm.c','full/full_alarm_service.c','full/mqtt_handler.c']:
 assert (root/relative).resolve() in compiled,relative
ninja='C:/Users/29538/.espressif/tools/ninja/1.12.1/ninja.exe'
assert subprocess.check_output([ninja,'--version'],encoding='utf-8').strip()=='1.12.1'
dependency_output=subprocess.check_output([ninja,'-C',str(build),'-t','deps'],encoding='utf-8')
dependencies=set()
repo_prefix=str(root.parent.resolve()).replace('\\','/').lower()+'/'
for line in dependency_output.splitlines():
 if line.startswith('    '):
  path_text=line.strip()
  if not path_text.replace('\\','/').lower().startswith(repo_prefix):continue
  p=Path(path_text).resolve()
  if p.is_file() and p.is_relative_to(root.parent.resolve()) and not p.is_relative_to(build.resolve()):
   assert p.stat().st_mtime<=app.stat().st_mtime,f'Rebuild after editing included dependency {p}'
   dependencies.add(p)
assert (root/'full/full_panel_runtime.inc').resolve() in dependencies
assert (root/'full/lv_conf.h').resolve() in dependencies
assert (root/'full/direct_ui_scene.c').resolve() not in compiled
assert (root/'main/panel_fonts.c').resolve() in compiled
assert (root/'main/panel_locale_terms.inc').resolve() in dependencies
assert (root.parent/'esp32_direct/main/direct/direct_gfx_assets.c').resolve() not in (dependencies|compiled)
assert (root.parent/'esp32_direct/main/mqtt/mqtt_handler.c').resolve() in dependencies
if '--check' in sys.argv:
 print(f'PASS package inputs: v{version}, {len(app_data)} app bytes, {0x1a0000-len(app_data)} free bytes, partition/source/host checks')
 sys.exit(0)
image=bytearray(b'\xff')*(4*1024*1024)
segments=[]
for addr,relative in sorted(args['flash_files'].items(),key=lambda item:int(item[0],0)):
 start=int(addr,0);source=(build/relative).resolve();assert source.is_relative_to(build.resolve())
 data=source.read_bytes();end=start+len(data)
 assert 0<=start<end<=len(image)
 assert not any(start<b and end>a for a,b in segments)
 image[start:end]=data
 segments.append((start,end))
assert image[0x20000:0x20000+len(app_data)]==app_data

app_name=f'cs_inv_modbus_full_v{version}_app_0x20000.bin'
full_name=f'cs_inv_modbus_full_v{version}_4MB_0x0.bin'
notes_name='更新说明.html'
preview_name='界面预览.png'
preview_source=root/'tmp/ui_readback_review.png'
assert preview_source.is_file(),'Generate the firmware UI preview before packaging'
expected={app_name,full_name,notes_name,preview_name}
expected.add('station')
out.mkdir(parents=True,exist_ok=True)
assert {p.name for p in out.iterdir()}<=expected, f'Unexpected release files in {out}'
shutil.copyfile(app,out/app_name)
(out/full_name).write_bytes(image)
shutil.copyfile(preview_source,out/preview_name)
station=out/'station'
station.mkdir(exist_ok=True)
station_files={
    'bootloader.bin':build/args['bootloader']['file'],
    'partition-table.bin':build/args['partition-table']['file'],
    'ota_data_initial.bin':build/args['flash_files']['0x10000'],
    'app.bin':app,
}
for name,source in station_files.items():
 shutil.copyfile(source,station/name)
manifest={
 'model':'CS-MB-PANEL','chip':'esp32c3',
 'profile':{1:'device',2:'factory',3:'production'}[metadata[6]],
 'version':project['project_version'],
 'factory_partition':{'offset':0x3f8000,'size':0x8000},
 'sha256':{name:sha(station/name) for name in station_files},
}
(station/'flash_manifest.json').write_text(json.dumps(manifest,indent=2)+'\n',encoding='utf-8')

notes=f'''<!doctype html>
<html lang="zh-CN">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>CS-MB-PANEL {version} 更新说明</title>
<style>
body {{font: 16px/1.65 system-ui, sans-serif; color:#172633; background:#f3f6fa; margin:0}}
main {{max-width:52rem; margin:2rem auto; padding:1.5rem; background:white}}
h1,h2 {{line-height:1.25}}
a {{color:#1458a3}}
table {{width:100%; border-collapse:collapse}}
th,td {{text-align:left; padding:.5rem; border-bottom:1px solid #dde4ec}}
code {{overflow-wrap:anywhere}}
</style>
</head>
<body><main>
<h1>CS-MB-PANEL {version} 更新说明</h1>
<p><time datetime="{release_date}">{release_date}</time> · ESP32-C3 · 试产验证版</p>
<p><a href="{preview_name}">查看固件渲染的界面预览</a></p>
<h2>本次更新</h2>
<ul>
<li>设置页直接显示五项参数和当前值，3KU 输出频率和电压限制按运行模式说明。</li>
<li>天气刷新保留已连接的 MQTT，资源不足时保留上次预报。中文界面识别长沙，温度使用单个摄氏度字形。</li>
<li>电池已连接且零功率时 CHG 蓝色常亮，实际放电蓝色呼吸、充电绿色呼吸。灯与测量共用方向判定，3KU 零电流小功率和首页瓦数保留实测值。</li>
<li>重新整理分类图标、参数编辑卡片、数值基线和中英文对齐，减少重复装饰图。</li>
<li>修复 SPIFFS 不支持 access 导致新缓存无法读取的问题，使用 stat 检查，保留原文件继续补传。</li>
<li>修复 CHG 电流归零时误凭充电状态亮绿灯。3KU 按同一采集批次的有符号电池功率补充方向，充电标志单独存在时不再亮绿色呼吸，诊断日志增加电池功率原始值。</li>
<li>蓝牙低内存启动无法接纳时立即恢复云连接，自动重试退避 5 分钟，重新进入手机连接页面可重试。蓝牙调度等待不再反复关闭 MQTT 重连窗口。蓝牙内存门槛保持不变。</li>
<li>修复联网后缓存补传任务栈溢出重启。压缩缓冲按需分配，补传交由 MQTT 网络任务发送，补传任务保留 6 KB 栈及低余量诊断。</li>
<li>运行快照离线保存支持原始 V3 格式与 1023 字节载荷。新批次使用无损压缩，恢复网络后按原采集时间续传，等待 MQTT PUBACK 后移除。</li>
<li>缓存每 5 条或约 10 秒落盘，使用临时文件提交。保留旧缓存读取能力，挂载失败不会直接清空已有分区，未校时记录不会当作过期数据删除。</li>
<li>突然断电仍可能丢失最后一批内存数据。PUBACK 不代表业务数据库入库，云端需配套部署历史快照处理更新。</li>
<li>蓝牙参数名称、枚举值、操作限制和运行参数按 App 语言返回。配套 App 增加参数搜索、当前值与目标值对照、运行数据卡片和告警入口。</li>
<li>面板设置列表支持上下逐项选择和左右整页翻动。开关机菜单可选择停用远程开关机，发送前仍需确认。</li>
<li>3KU 新增云告警发生、恢复和断网补报。云服务需同步更新码表。</li>
<li>手机可通过 BLE 读取运行数据、机型参数、BMS 单体、告警及诊断，调整逆变器设置和开关机。需配套支持面板协议的 App。</li>
<li>蓝牙快照每秒更新。参数沿用面板机型规则，读回一致和设备受理采用不同回执。</li>
<li>手机可修改显示、声音、时区、电池协议和逆变器串口配置，也可搜索逆变器地址。</li>
<li>逆变器设置直接打开全部参数分类，运行参数以整屏图示和数值卡片呈现。</li>
<li>保存后自动读取设备值、替代参数或协议应答，区分实际生效、设备受理、拒绝和超时。</li>
<li>显示设置中的 LED 亮度按百分比调整。无源蜂鸣器固定鸣叫 70 ms，支持关闭、低、中、高。</li>
<li>统一 BBJ 和 CSERGY 电池页。无有效 BMS 数据时不显示无效读数，断联后缓存标为上次数据。</li>
<li>CSERGY 电池可在自定义电池和待机条件下带入充电电压、电流上限和关机电压，确认后逐项保存。也可手动设置电池类型和参数。</li>
<li>2KU、3KU、6KU、12KU 共用一份固件。请在「维护诊断 → 逆变器通信设置」核对铭牌机型。</li>
</ul>
<h2>本地配网操作</h2>
<p>菜单 → 辅助设置 → Wi-Fi 配网。上下选择网络，确认进入密码输入。方向键选字符，确认输入。向上可进入分类，底部可退格、连接或取消。列表中右键重新扫描。</p>
<p>密码输入支持 8–63 个字符或 64 位十六进制密钥。扫描列表显示最多八个不同名称的可用网络；支持 2.4 GHz，不显示隐藏网络。保存失败会单独提示。</p>
<p>天气页 → 右键进入城市 → 右键搜索 → 键盘输入拼音或英文 → 搜索 → 上下选择结果 → 确认保存。城市页按左键恢复 IP 定位。新预报需要互联网访问。默认预报来自 <a href="https://open-meteo.com/">Open-Meteo</a>，地名数据来自 <a href="https://www.geonames.org/">GeoNames</a>。公共 API 用于工程验证，商业使用需配置获授权的服务。</p>
<p>云端固件列表使用设备通信服务。生产端仍需发布对应服务才能列出固件。</p>
<h2>固件文件</h2>
<table>
<thead><tr><th>文件</th><th>使用方式</th></tr></thead>
<tbody>
<tr><td><a href="{app_name}">应用固件</a></td><td>用于面板 ESP OTA。串口手动写入时先核对实际启动槽，OTA0 起始地址为 <code>0x20000</code>。</td></tr>
<tr><td><a href="{full_name}">4 MB 完整固件</a></td><td>用于首次安装或恢复，写入 <code>0x0</code>。整片写入会覆盖身份、NVS、配置和缓存。</td></tr>
<tr><td><a href="station/flash_manifest.json">工站镜像清单</a></td><td>配合 flash_production.py 的 CS-MB-PANEL 模式，先做离线校验。常规有线更新使用分段镜像。</td></tr>
</tbody></table>
<h2>验证与校验</h2>
<p>主机回归与 ESP32-C3 编译通过，覆盖明暗主题、中英文排版、实时与历史数据切换、参数编辑和回读。四款机型的实板验收待完成。每个应用槽剩余 {0x1a0000-len(app_data):,} 字节，发布最低余量为 56 KiB。</p>
<ul>
<li>应用 SHA-256 <code>{sha(out/app_name)}</code></li>
<li>完整镜像 SHA-256 <code>{sha(out/full_name)}</code></li>
</ul>
</main></body>
</html>
'''
(out/notes_name).write_text(notes,encoding='utf-8')
assert {p.name for p in out.iterdir()}==expected
assert (out/full_name).stat().st_size==4*1024*1024
print(f'PASS release v{version}: {len(app_data)} app bytes, {0x1a0000-len(app_data)} OTA bytes free')
print(out)
for name in sorted(expected):
 print(name,(out/name).stat().st_size)
