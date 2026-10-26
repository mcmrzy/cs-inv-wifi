"""Explicit ROM discovery for a selected port. Importing never accesses USB.

ROM discovery resets the selected device into download mode, so it is reserved
for an operator action and is never part of automatic read-only enumeration.
"""

def canonical_mac(value):
 try:
  raw=bytes.fromhex(value.replace(':','').replace('-',''))
 except (AttributeError,ValueError) as exc:raise RuntimeError('invalid expected MAC') from exc
 if len(raw)!=6 or not any(raw) or raw[0]&1:raise RuntimeError('invalid expected MAC')
 return ':'.join(f'{byte:02X}' for byte in raw)

def open_rom(port):
 if not port:raise ValueError('select an actual serial port')
 from esptool.targets.esp32c3 import ESP32C3ROM
 loader=ESP32C3ROM(port,115200)
 try:
  loader.connect()
  return loader
 except BaseException:
  loader._port.close()
  raise

def read_identity(loader,expected_mac=None,require_4mb=False):
 if loader.CHIP_NAME!='ESP32-C3':raise RuntimeError('connected chip is not ESP32-C3')
 mac=canonical_mac(':'.join(f'{byte:02X}' for byte in loader.read_mac()))
 if expected_mac is not None and mac!=canonical_mac(expected_mac):
  raise RuntimeError('connected MAC differs from selected device; identify again before flashing')
 loader.flash_spi_attach(0)
 exponent=(loader.flash_id()>>16)&0xff
 if not 18<=exponent<=30:raise RuntimeError('cannot identify SPI Flash capacity')
 flash_bytes=1<<exponent
 if require_4mb and flash_bytes!=0x400000:raise RuntimeError('connected Flash must be exactly 4MB')
 return dict(result='OK',chip='esp32c3',mac=mac,flash_bytes=flash_bytes,identification_mode='rom',
             next_step='已进入 ROM 下载模式。可选择适配的固件烧录；重新运行设备需断电重启。板卡信息需通过工单配置。')

def identify(port):
 loader=open_rom(port)
 try:return read_identity(loader)
 finally:loader._port.close()
