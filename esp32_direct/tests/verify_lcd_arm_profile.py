"""Compare migrated analog settings against the original supplied ARM source."""
import re
from pathlib import Path

root = Path(__file__).resolve().parents[1]
arm = Path('C:/Users/29538/Documents/xwechat_files/wxid_6n4stl4q5dzq22_9518/msg/file/2026-09/App(10)/App/source/GUI/halLcd.c').read_text(encoding='gb18030', errors='replace')
arm = arm.split('void  hal_Lcd_init(void)')[1].split('void hal_Lcd_wakeup')[0]
arm = re.sub(r'//[^\n]*', '', arm)
original = {}
register = None
for operation, literal in re.findall(r'Write(Comm|Data)\s*\(\s*(0x[0-9a-fA-F]+)\s*\)', arm):
    value = int(literal, 16)
    if operation == 'Comm':
        register = value
        original[register] = []
    elif register is not None:
        original[register].append(value)
port = (root/'main/direct/direct_panel.c').read_text()
for register in [0xb7, 0xbb, 0xc0, 0xc2, 0xc3, 0xc4, 0xc6, 0xd0, 0xd6]:
    match = re.search(r'command\(0x%02x\);((?:\s*data8\(0x[0-9a-f]+\);)+)' % register, port)
    assert match, hex(register)
    values = [int(v,16) for v in re.findall(r'data8\((0x[0-9a-f]+)\)',match[1])]
    assert values == original[register], (register, values, original[register])
for name,register in [('gamma_pos',0xe0),('gamma_neg',0xe1),('porch',0xb2)]:
    match = re.search(r'%s\[\]\s*=\s*\{([^}]+)\}' % name, port)
    values = [int(v,16) for v in re.findall(r'0x[0-9a-f]+',match[1])]
    assert values == original[register], name
print('PASS: optional legacy profile matches original ARM analog power, VCOM, both gamma tables and porch. This does not validate optical quality or select the active profile.')
