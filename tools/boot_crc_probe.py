"""测"现场 boot 用大端字序 CRC"假设:
其 0x09 帧的 CRC 槽位 = 0x200068d8。若 BE 字序 CRC 命中, 则 boot 的 CRC 是
硬件风格(字内大端), 而 App 是小端 —— 这正是"App 收我们的帧、boot 不收"的统一解释。
"""
FRAME = bytes.fromhex('aa08000901000000d8680020000000007a602ef7607a289d')
GOT_LE_READ = 0x200068D8          # ESP 按 LE 读 offset 8..11
GOT_BE_READ = 0xD8680020          # 若按 BE 读同 4 字节


def crc_words(data, init=0xFFFFFFFF, order='little'):
    crc = init
    n = len(data) - len(data) % 4
    for i in range(0, n, 4):
        crc ^= int.from_bytes(data[i:i + 4], order)
        for _ in range(32):
            crc = ((crc << 1) ^ 0x04C11DB7) & 0xFFFFFFFF if crc & 0x80000000 else (crc << 1) & 0xFFFFFFFF
    return crc


def crc_bytes(data, init=0xFFFFFFFF):
    """逐字节 MSB-first (等价于 BE 字序打包)"""
    crc = init
    for b in data:
        crc ^= b << 24
        for _ in range(8):
            crc = ((crc << 1) ^ 0x04C11DB7) & 0xFFFFFFFF if crc & 0x80000000 else (crc << 1) & 0xFFFFFFFF
    return crc


targets = {'LE-read(0x200068d8)': GOT_LE_READ, 'BE-read(0xd8680020)': GOT_BE_READ}
print('=== 候选值 ===')
print(f"  frame bytes 0..7 = {FRAME[:8].hex(' ')}")
for name, got in targets.items():
    print(f"  {name}")

for order in ('little', 'big'):
    for length in (4, 8, 12, 16):
        v = crc_words(FRAME[:length], order=order)
        print(f"  crc_words[{order}] first{length}B = 0x{v:08x}")
for length in (4, 8, 12, 16):
    v = crc_bytes(FRAME[:length])
    print(f"  crc_bytes first{length}B = 0x{v:08x}")
