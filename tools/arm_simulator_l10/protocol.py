# -*- coding: utf-8 -*-
"""
protocol.py — CS-L10-6K2 采集器串口协议 Python 参考实现

与 ARM 端源码 (Collector.c / ComProtocol.c / TEA.c / CRC32.c) 逐字节对齐。
同时供:
  1) arm_simulator_l10 模拟器使用 (PPP 服务端 + TEA CBC + CRC32)
  2) 与 C 实现 (esp32c3_l10_idf/main/cs_protocol.c) 做向量级交叉验证

帧结构 (小端):
  0xAA + CmdLen(u16,4的倍数) + CMD(u8) + Param1(u16) + Param2(u16) + Data[]
  CRC32 存储于帧偏移 CmdLen 处 (帧外 4 字节, 小端)
  发送加密: TempKey CBC 加密 EncryptLen 字节 → 帧尾附 TempKey[2..3] 8B
            → FrameKey CBC 加密 (EncryptLen+8) 字节
  接收解密: FrameKey CBC 解密全帧 → 尾部 8B 重建 TempKey → TempKey CBC 解密全帧
"""

import struct

# ---- 命令码 (Collector.h) ----
CMD_READ_CTRLPARAM   = 0x01
CMD_WRITE_CTRLPARAM  = 0x02
CMD_READ_RUNPARAM    = 0x03
CMD_READ_R_W_DATA    = 0x04
CMD_WRITE_R_W_DATA   = 0x05
CMD_READ_R_DATA      = 0x06
CMD_CTRLCMD          = 0x07
CMD_READ_TEST_RUNPARAM = 0x14

# ---- 帧常量 (ComProtocol.h) ----
HEADER        = 0xAA
FRAME_MAX     = 1088
CMD_MIN_LEN   = 8
PPP_ESC_7E    = 0x7E
PPP_ESC_7D    = 0x7D
PPP_ESC_5E    = 0x5E
PPP_ESC_5D    = 0x5D

# FrameKey (TEA.c 源码公开)
FRAME_KEY = [0x3e23e3fe, 0x4cc197ac, 0x074db01f, 0x42fee1bd]

DELTA = 0x9e3779b9
MASK32 = 0xFFFFFFFF


# ----------------------------------------------------------------------
# CRC32 — 与 CRC32_HW() 一致: 逐 32 位字 (小端读取), MSB-first 逐位,
# poly 0x4C11DB7, init 0xFFFFFFFF, 无反射, 无最终异或
# ----------------------------------------------------------------------
def crc32_words(data: bytes) -> int:
    crc = 0xFFFFFFFF
    for i in range(0, len(data) - len(data) % 4, 4):
        w = struct.unpack_from('<I', data, i)[0]
        crc ^= w
        for _ in range(32):
            if crc & 0x80000000:
                crc = ((crc << 1) ^ 0x4C11DB7) & MASK32
            else:
                crc = (crc << 1) & MASK32
    return crc


# ----------------------------------------------------------------------
# TEA 标准 32 轮 (TEA.c)
# ----------------------------------------------------------------------
def tea_encrypt_block(v0, v1, k):
    s = 0
    for _ in range(32):
        s = (s + DELTA) & MASK32
        v0 = (v0 + ((((v1 << 4) & MASK32) + k[0]) ^ (v1 + s) ^ (((v1 >> 5) + k[1]) & MASK32))) & MASK32
        v1 = (v1 + ((((v0 << 4) & MASK32) + k[2]) ^ (v0 + s) ^ (((v0 >> 5) + k[3]) & MASK32))) & MASK32
    return v0, v1


def tea_decrypt_block(v0, v1, k):
    s = (0xABD47 + 0xC6E479D9) & MASK32      # = 0xC6EF3720 = 32*delta
    delta = (0xA188520 + 0x941EF499) & MASK32  # = 0x9E3779B9
    for _ in range(32):
        v1 = (v1 - ((((v0 << 4) & MASK32) + k[2]) ^ (v0 + s) ^ (((v0 >> 5) + k[3]) & MASK32))) & MASK32
        v0 = (v0 - ((((v1 << 4) & MASK32) + k[0]) ^ (v1 + s) ^ (((v1 >> 5) + k[1]) & MASK32))) & MASK32
        s = (s - delta) & MASK32
    return v0, v1


# ---- CBC 链模式 (InitV=0) ----
def encrypt_cbc(buf: bytes, key) -> bytes:
    out = bytearray(buf)
    iv0 = iv1 = 0
    n = len(out) // 8
    for i in range(n):
        v0, v1 = struct.unpack_from('<II', out, i * 8)
        v0 ^= iv0
        v1 ^= iv1
        v0, v1 = tea_encrypt_block(v0, v1, key)
        struct.pack_into('<II', out, i * 8, v0, v1)
        iv0, iv1 = v0, v1
    return bytes(out)


def decrypt_cbc(buf: bytes, key) -> bytes:
    out = bytearray(buf)
    prev0 = prev1 = 0
    n = len(out) // 8
    for i in range(n):
        cur0, cur1 = struct.unpack_from('<II', out, i * 8)
        v0, v1 = tea_decrypt_block(cur0, cur1, key)
        struct.pack_into('<II', out, i * 8, v0 ^ prev0, v1 ^ prev1)
        prev0, prev1 = cur0, cur1
    return bytes(out)


# ----------------------------------------------------------------------
# PPP 封装/解封装 (ComProtocol.c)
# ----------------------------------------------------------------------
def ppp_pack(data: bytes) -> bytes:
    out = bytearray([PPP_ESC_7E])
    for b in data:
        if b == PPP_ESC_7E:
            out += bytes([PPP_ESC_7D, PPP_ESC_5E])
        elif b == PPP_ESC_7D:
            out += bytes([PPP_ESC_7D, PPP_ESC_5D])
        else:
            out.append(b)
    out.append(PPP_ESC_7E)
    return bytes(out)


class PPPReceiver:
    """PPP 接收状态机 — 与 PPPFrameCmdReceive() 逐字节一致"""

    WAIT, RCVING, FINISH = 0, 1, 2

    def __init__(self, max_len=FRAME_MAX):
        self.max_len = max_len
        self.reset()

    def reset(self):
        self.status = self.WAIT
        self.flg7e = 0
        self.length = 0
        self.buf = bytearray()

    def feed(self, b: int) -> bool:
        """喂一个字节, 返回 True 表示一帧接收完成"""
        if self.status == self.WAIT:
            if b == PPP_ESC_7E:
                self.status = self.RCVING
                self.length = 0
                self.flg7e = 0
        elif self.status == self.RCVING:
            if self.length > self.max_len - 1:
                self.status = self.WAIT
                self.length = 0
                self.flg7e = 0
            if b == PPP_ESC_7E:
                if self.flg7e == PPP_ESC_7D:
                    self.status = self.WAIT
                    self.length = 0
                    self.flg7e = 0
                    return False
                if self.length != 0:
                    self.status = self.FINISH
                else:
                    self.status = self.RCVING
                self.flg7e = 0
            elif b == PPP_ESC_5E:
                if self.flg7e == PPP_ESC_7D:
                    self.buf.append(PPP_ESC_7E)
                    self.length += 1
                    self.flg7e = 0
                else:
                    self.buf.append(PPP_ESC_5E)
                    self.length += 1
            elif b == PPP_ESC_5D:
                if self.flg7e == PPP_ESC_7D:
                    self.buf.append(PPP_ESC_7D)
                    self.length += 1
                    self.flg7e = 0
                else:
                    self.buf.append(PPP_ESC_5D)
                    self.length += 1
            elif b == PPP_ESC_7D:
                if self.flg7e == PPP_ESC_7D:
                    self.status = self.WAIT
                    self.length = 0
                    self.flg7e = 0
                    return False
                self.flg7e = PPP_ESC_7D
            else:
                if self.flg7e == PPP_ESC_7D:
                    self.status = self.WAIT
                    self.length = 0
                    self.flg7e = 0
                    return False
                self.buf.append(b)
                self.length += 1
        else:  # FINISH / 其他 → 复位
            self.status = self.WAIT
            self.length = 0
            self.flg7e = 0
        return self.status == self.FINISH


# ----------------------------------------------------------------------
# 帧构建/加密/发送 (Collector.c 发送逻辑)
# ----------------------------------------------------------------------
def frame_build(cmd, param1, param2, data=b'', rand=0):
    """构建明文帧 + CRC (未加密), 返回 (raw, cmdlen)"""
    cmdlen = ((param2 + 8 + 3) >> 2) << 2
    raw = bytearray(cmdlen + 4)
    raw[0] = HEADER
    struct.pack_into('<H', raw, 1, cmdlen)
    raw[3] = cmd
    struct.pack_into('<H', raw, 4, param1)
    struct.pack_into('<H', raw, 6, param2)
    if data:
        raw[8:8 + len(data)] = data
    crc = crc32_words(bytes(raw[:cmdlen]))
    struct.pack_into('<I', raw, cmdlen, crc)
    return bytes(raw), cmdlen


def frame_encrypt_send(raw: bytes, rand: int):
    """加密 + 附加 TempKey (Collector.c ReadRunParam 尾部逻辑), 返回待 PPP 封装的字节"""
    cmdlen = struct.unpack_from('<H', raw, 1)[0]
    enclen = ((cmdlen + 4 + 7) >> 3) << 3
    tmpk = FRAME_KEY[:2] + [(~rand) & MASK32, rand]
    # 第一层: TempKey CBC 加密 enclen 字节 (raw 已含 CRC, 余下补 0)
    buf = bytearray(raw[:cmdlen + 4]) + bytearray(enclen - (cmdlen + 4))
    buf = bytearray(encrypt_cbc(bytes(buf), tmpk))
    # 帧尾附 TempKey[2..3] 8 字节
    buf += struct.pack('<II', tmpk[2], tmpk[3])
    # 第二层: FrameKey CBC 加密全帧
    return encrypt_cbc(bytes(buf), FRAME_KEY)


def send_frame(cmd, param1, param2, data=b'', rand=0):
    """完整发送流程: 构建 → 加密 → PPP 封装, 返回 PPP 字节流"""
    raw, _ = frame_build(cmd, param1, param2, data)
    enc = frame_encrypt_send(raw, rand)
    return ppp_pack(enc)


# ----------------------------------------------------------------------
# 帧接收/解密/解析 (Collector.c 接收逻辑)
# ----------------------------------------------------------------------
def decrypt_rx(buf: bytes) -> bytes:
    """两次解密: FrameKey → 尾部 8B 重建 TempKey → TempKey, 返回明文帧"""
    assert len(buf) % 8 == 0
    layer1 = decrypt_cbc(buf, FRAME_KEY)
    tmpk = FRAME_KEY[:2] + list(struct.unpack('<II', layer1[-8:]))
    layer2 = decrypt_cbc(layer1, tmpk)
    return layer2


def parse_frame(plain: bytes):
    """解析明文帧, 校验 header/CmdLen/CRC; 返回 (cmd, param1, param2, data)
    失败抛 ValueError"""
    if len(plain) < CMD_MIN_LEN + 4:
        raise ValueError('frame too short')
    if plain[0] != HEADER:
        raise ValueError('bad header')
    cmdlen = struct.unpack_from('<H', plain, 1)[0]
    if cmdlen < CMD_MIN_LEN or cmdlen % 4:
        raise ValueError('bad cmdlen')
    if cmdlen + 4 > len(plain):
        raise ValueError('cmdlen beyond frame')
    crc_calc = crc32_words(plain[:cmdlen])
    crc_got = struct.unpack_from('<I', plain, cmdlen)[0]
    if crc_calc != crc_got:
        raise ValueError('crc mismatch')
    cmd = plain[3]
    param1, param2 = struct.unpack_from('<HH', plain, 4)
    data = bytes(plain[8:8 + param2])
    return cmd, param1, param2, data


def recv_frame(ppp_bytes: bytes):
    """PPP 解封装 + 解密 + 解析"""
    rx = PPPReceiver()
    for b in ppp_bytes:
        if rx.feed(b):
            return parse_frame(decrypt_rx(bytes(rx.buf)))
    raise ValueError('no complete frame')
