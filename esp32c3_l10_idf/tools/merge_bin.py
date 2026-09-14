#!/usr/bin/env python3
"""合并 bootloader + partition-table + ota_data + app 为4MB flash 镜像"""
import os, sys

BUILD = os.path.join(os.path.dirname(__file__), '..', 'build')
FLASH_SIZE = 4 * 1024 * 1024

OFFSETS = {
    'bootloader': 0x1000,
    'partitions': 0x8000,
    'ota_data':   0x10000,
    'app':        0x20000,
}
SOURCES = {
    'bootloader': os.path.join(BUILD, 'bootloader', 'bootloader.bin'),
    'partitions': os.path.join(BUILD, 'partition_table', 'partition-table.bin'),
    'ota_data':   os.path.join(BUILD, 'ota_data_initial.bin'),
    'app':        os.path.join(BUILD, 'cs_inv_l10_c3.bin'),
}

def main():
    out_dir = sys.argv[1] if len(sys.argv) > 1 else '.'
    ver = sys.argv[2] if len(sys.argv) > 2 else 'dev'

    out = bytearray(b'\xff' * FLASH_SIZE)
    for name, offset in OFFSETS.items():
        with open(SOURCES[name], 'rb') as f:
            data = f.read()
        out[offset:offset + len(data)] = data
        print(f'  {name}: 0x{offset:05X} ({len(data):,} bytes)')

    merged = os.path.join(out_dir, f'cs_inv_l10_v{ver}_merged.bin')
    with open(merged, 'wb') as f:
        f.write(out)
    print(f'  merged -> {os.path.basename(merged)} ({FLASH_SIZE:,} bytes)')

    app_out = os.path.join(out_dir, f'cs_inv_l10_v{ver}.bin')
    with open(SOURCES['app'], 'rb') as src, open(app_out, 'wb') as dst:
        dst.write(src.read())
    print(f'  app    -> {os.path.basename(app_out)}')

if __name__ == '__main__':
    main()
