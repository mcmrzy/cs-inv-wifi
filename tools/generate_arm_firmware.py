#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
ARM模拟固件生成工具
==================
用于生成GD32F303 ARM芯片的测试固件文件，支持OTA升级测试。

功能：
- 生成不同大小的测试固件
- 生成包含错误的固件用于测试错误检测
- 支持自定义版本号
- 生成固件信息文件(.json)

用法:
  python generate_arm_firmware.py                    # 生成默认固件
  python generate_arm_firmware.py --size 64          # 生成64KB固件
  python generate_arm_firmware.py --error corrupt    # 生成损坏的固件
  python generate_arm_firmware.py --error invalid_header  # 生成无效头部的固件
  python generate_arm_firmware.py --error version_downgrade  # 生成低版本固件
"""

import os
import sys
import json
import struct
import hashlib
import argparse
import random
from datetime import datetime


# ==================== GD32F303 固件格式定义 ====================

# 固件头部结构 (参考GD32F303 IAP)
FIRMWARE_HEADER_MAGIC = b'GD32'           # 4字节魔数
FIRMWARE_HEADER_VERSION = 1               # 头部版本
FIRMWARE_MAX_SIZE = 256 * 1024            # 最大固件大小 256KB
FIRMWARE_MIN_SIZE = 4 * 1024              # 最小固件大小 4KB

# 默认配置
DEFAULT_VERSION = "V1.3.0.20260702"
DEFAULT_SIZE_KB = 64                      # 默认64KB


class ArmFirmwareGenerator:
    """ARM固件生成器"""
    
    def __init__(self, version: str = DEFAULT_VERSION, size_kb: int = DEFAULT_SIZE_KB):
        self.version = version
        self.size_kb = size_kb
        self.size_bytes = size_kb * 1024
        
    def generate_normal_firmware(self) -> bytes:
        """生成正常的测试固件"""
        print(f"[INFO] 生成正常固件: {self.version}, {self.size_kb}KB")
        
        # 构建固件数据
        firmware = bytearray()
        
        # 1. 固件头部 (32字节)
        header = self._build_header()
        firmware.extend(header)
        
        # 2. 中断向量表 (模拟)
        vector_table = self._generate_vector_table()
        firmware.extend(vector_table)
        
        # 3. 代码段 (模拟)
        code_size = self.size_bytes - len(firmware) - 32  # 预留尾部
        code_section = self._generate_code_section(code_size)
        firmware.extend(code_section)
        
        # 4. 固件尾部 (包含校验信息)
        footer = self._build_footer(firmware)
        firmware.extend(footer)
        
        # 确保大小正确
        if len(firmware) < self.size_bytes:
            firmware.extend(b'\xFF' * (self.size_bytes - len(firmware)))
        elif len(firmware) > self.size_bytes:
            firmware = firmware[:self.size_bytes]
            
        return bytes(firmware)
    
    def generate_corrupt_firmware(self) -> bytes:
        """生成损坏的固件（用于测试校验失败）"""
        print(f"[INFO] 生成损坏固件: {self.version}, {self.size_kb}KB")
        
        # 先生成正常固件
        firmware = bytearray(self.generate_normal_firmware())
        
        # 随机损坏一些字节
        corrupt_positions = random.sample(range(32, len(firmware) - 32), 
                                         min(10, len(firmware) // 1000))
        for pos in corrupt_positions:
            firmware[pos] = random.randint(0, 255)
            
        return bytes(firmware)
    
    def generate_invalid_header_firmware(self) -> bytes:
        """生成无效头部的固件"""
        print(f"[INFO] 生成无效头部固件: {self.version}, {self.size_kb}KB")
        
        firmware = bytearray(self.generate_normal_firmware())
        
        # 破坏魔数
        firmware[0:4] = b'BAD!'
        
        # 破坏CRC
        firmware[28:32] = b'\x00\x00\x00\x00'
        
        return bytes(firmware)
    
    def generate_version_downgrade_firmware(self) -> bytes:
        """生成低版本固件（用于测试版本降级检测）"""
        print(f"[INFO] 生成低版本固件: V0.9.0, {self.size_kb}KB")
        
        # 保存原始版本
        original_version = self.version
        self.version = "V0.9.0.20230101"
        
        firmware = self.generate_normal_firmware()
        
        # 恢复原始版本
        self.version = original_version
        
        return firmware
    
    def generate_oversize_firmware(self) -> bytes:
        """生成超大固件（用于测试大小限制）"""
        print(f"[INFO] 生成超大固件: 300KB")
        
        original_size = self.size_kb
        self.size_kb = 300
        self.size_bytes = 300 * 1024
        
        firmware = self.generate_normal_firmware()
        
        self.size_kb = original_size
        self.size_bytes = original_size * 1024
        
        return firmware
    
    def _build_header(self) -> bytearray:
        """构建固件头部"""
        header = bytearray(32)
        
        # 魔数 (4字节)
        header[0:4] = FIRMWARE_HEADER_MAGIC
        
        # 头部版本 (1字节)
        header[4] = FIRMWARE_HEADER_VERSION
        
        # 固件标志 (1字节)
        header[5] = 0x01  # 正常固件
        
        # 保留 (2字节)
        header[6:8] = b'\x00\x00'
        
        # 固件大小 (4字节, little-endian)
        struct.pack_into('<I', header, 8, self.size_bytes)
        
        # 版本字符串长度 (1字节)
        version_bytes = self.version.encode('ascii')
        header[12] = len(version_bytes)
        
        # 版本字符串 (最多15字节)
        header[13:13 + min(len(version_bytes), 15)] = version_bytes[:15]
        
        # 头部CRC (4字节, 计算前28字节)
        header_crc = self._calculate_crc32(header[0:28])
        struct.pack_into('<I', header, 28, header_crc)
        
        return header
    
    def _generate_vector_table(self) -> bytearray:
        """生成模拟的中断向量表"""
        # Cortex-M4向量表，通常以栈指针和复位向量开始
        vector_table = bytearray(256)
        
        # 初始栈指针 (RAM顶部)
        struct.pack_into('<I', vector_table, 0, 0x20010000)
        
        # 复位向量
        struct.pack_into('<I', vector_table, 4, 0x08000000 + 256)
        
        # 其他中断向量 (模拟)
        for i in range(2, 64):
            addr = 0x08000000 + 256 + (i - 2) * 4
            struct.pack_into('<I', vector_table, i * 4, addr | 1)  # Thumb模式
            
        return vector_table
    
    def _generate_code_section(self, size: int) -> bytearray:
        """生成模拟的代码段"""
        code = bytearray(size)
        
        # 填充一些看似合理的ARM Thumb-2指令
        # 这些不是真实的指令，但格式正确
        patterns = [
            b'\x00\xBF',  # NOP
            b'\x70\x47',  # BX LR
            b'\x00\xF0',  # BL指令前缀
            b'\x10\xB5',  # PUSH {R4, LR}
            b'\x10\xBD',  # POP {R4, PC}
            b'\x00\x20',  # MOVS R0, #0
            b'\x01\x20',  # MOVS R0, #1
        ]
        
        # 在开头放置一些指令
        pos = 0
        while pos < min(1024, size):
            pattern = random.choice(patterns)
            code[pos:pos + len(pattern)] = pattern
            pos += len(pattern)
            
        # 其余部分用0xFF填充（Flash擦除状态）
        for i in range(1024, size):
            code[i] = 0xFF
            
        return code
    
    def _build_footer(self, firmware_data: bytearray) -> bytearray:
        """构建固件尾部"""
        footer = bytearray(32)
        
        # 尾部魔数
        footer[0:4] = b'FOOT'
        
        # 固件CRC32
        fw_crc = self._calculate_crc32(firmware_data)
        struct.pack_into('<I', footer, 4, fw_crc)
        
        # 固件MD5 (16字节)
        fw_md5 = hashlib.md5(firmware_data).digest()
        footer[8:24] = fw_md5
        
        # 构建时间戳
        timestamp = int(datetime.now().timestamp())
        struct.pack_into('<I', footer, 24, timestamp)
        
        # 尾部校验
        footer_crc = self._calculate_crc32(footer[0:28])
        struct.pack_into('<I', footer, 28, footer_crc)
        
        return footer
    
    def _calculate_crc32(self, data: bytes) -> int:
        """计算CRC32"""
        import binascii
        return binascii.crc32(data) & 0xFFFFFFFF
    
    def get_firmware_info(self, firmware: bytes) -> dict:
        """获取固件信息"""
        fw_size = len(firmware)
        fw_md5 = hashlib.md5(firmware).hexdigest()
        fw_sha256 = hashlib.sha256(firmware).hexdigest()
        
        # 解析头部
        magic = firmware[0:4]
        header_version = firmware[4]
        flags = firmware[5]
        declared_size = struct.unpack_from('<I', firmware, 8)[0]
        version_len = firmware[12]
        version = firmware[13:13 + version_len].decode('ascii', errors='replace')
        
        header_crc = struct.unpack_from('<I', firmware, 28)[0]
        header_crc_calc = self._calculate_crc32(firmware[0:28])
        
        return {
            'version': version,
            'size_bytes': fw_size,
            'size_kb': fw_size // 1024,
            'md5': fw_md5,
            'sha256': fw_sha256,
            'header': {
                'magic': magic.hex(),
                'magic_valid': magic == FIRMWARE_HEADER_MAGIC,
                'header_version': header_version,
                'flags': flags,
                'declared_size': declared_size,
                'size_match': declared_size == fw_size,
                'crc_valid': header_crc == header_crc_calc
            },
            'generated_at': datetime.now().isoformat(),
            'description': f'GD32F303 ARM固件 {version}'
        }


def main():
    parser = argparse.ArgumentParser(
        description='GD32F303 ARM固件生成工具',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
示例:
  %(prog)s                           # 生成默认64KB正常固件
  %(prog)s --size 128                # 生成128KB固件
  %(prog)s --version V2.0.0          # 指定版本号
  %(prog)s --error corrupt           # 生成损坏固件
  %(prog)s --error invalid_header    # 生成无效头部固件
  %(prog)s --error version_downgrade # 生成低版本固件
  %(prog)s --error oversize          # 生成超大固件
  %(prog)s --all                     # 生成所有类型的测试固件
        """
    )
    
    parser.add_argument('--size', type=int, default=DEFAULT_SIZE_KB,
                        help=f'固件大小(KB)，默认{DEFAULT_SIZE_KB}KB')
    parser.add_argument('--version', type=str, default=DEFAULT_VERSION,
                        help=f'固件版本号，默认{DEFAULT_VERSION}')
    parser.add_argument('--error', type=str, 
                        choices=['corrupt', 'invalid_header', 'version_downgrade', 'oversize'],
                        help='生成包含错误的固件')
    parser.add_argument('--output', type=str, default=None,
                        help='输出目录，默认为当前目录')
    parser.add_argument('--all', action='store_true',
                        help='生成所有类型的测试固件')
    parser.add_argument('--info', action='store_true',
                        help='显示固件信息')
    
    args = parser.parse_args()
    
    # 输出目录
    output_dir = args.output or os.path.dirname(os.path.abspath(__file__))
    os.makedirs(output_dir, exist_ok=True)
    
    generator = ArmFirmwareGenerator(args.version, args.size)
    
    if args.all:
        # 生成所有类型的固件
        print("=" * 60)
        print("生成所有测试固件")
        print("=" * 60)
        
        # 正常固件
        firmware = generator.generate_normal_firmware()
        save_firmware(firmware, os.path.join(output_dir, 'arm_firmware.bin'), generator)
        
        # 损坏固件
        firmware = generator.generate_corrupt_firmware()
        save_firmware(firmware, os.path.join(output_dir, 'arm_firmware_corrupt.bin'), generator)
        
        # 无效头部固件
        firmware = generator.generate_invalid_header_firmware()
        save_firmware(firmware, os.path.join(output_dir, 'arm_firmware_bad_header.bin'), generator)
        
        # 低版本固件
        firmware = generator.generate_version_downgrade_firmware()
        save_firmware(firmware, os.path.join(output_dir, 'arm_firmware_old_version.bin'), generator)
        
        # 超大固件
        firmware = generator.generate_oversize_firmware()
        save_firmware(firmware, os.path.join(output_dir, 'arm_firmware_oversize.bin'), generator)
        
        print("\n" + "=" * 60)
        print("所有固件生成完成!")
        print("=" * 60)
        
    else:
        # 根据参数生成固件
        if args.error == 'corrupt':
            firmware = generator.generate_corrupt_firmware()
            filename = 'arm_firmware_corrupt.bin'
        elif args.error == 'invalid_header':
            firmware = generator.generate_invalid_header_firmware()
            filename = 'arm_firmware_bad_header.bin'
        elif args.error == 'version_downgrade':
            firmware = generator.generate_version_downgrade_firmware()
            filename = 'arm_firmware_old_version.bin'
        elif args.error == 'oversize':
            firmware = generator.generate_oversize_firmware()
            filename = 'arm_firmware_oversize.bin'
        else:
            firmware = generator.generate_normal_firmware()
            filename = 'arm_firmware.bin'
            
        save_firmware(firmware, os.path.join(output_dir, filename), generator)


def save_firmware(firmware: bytes, filepath: str, generator: ArmFirmwareGenerator):
    """保存固件和信息文件"""
    # 保存固件文件
    with open(filepath, 'wb') as f:
        f.write(firmware)
    
    # 保存固件信息
    info = generator.get_firmware_info(firmware)
    info['filename'] = os.path.basename(filepath)
    info['filepath'] = filepath
    
    info_path = filepath.replace('.bin', '_info.json')
    with open(info_path, 'w', encoding='utf-8') as f:
        json.dump(info, f, indent=2, ensure_ascii=False)
    
    print(f"[OK] 固件已保存: {filepath}")
    print(f"[OK] 信息已保存: {info_path}")
    print(f"     版本: {info['version']}")
    print(f"     大小: {info['size_kb']}KB ({info['size_bytes']} bytes)")
    print(f"     MD5: {info['md5']}")
    print(f"     头部有效: {info['header']['magic_valid']}")
    print(f"     CRC有效: {info['header']['crc_valid']}")
    print()


if __name__ == '__main__':
    main()
