/// 生成模拟 ARM 固件文件用于 OTA 测试
///
/// 用法: dart run generate_test_firmware.dart
///
/// 生成的固件格式:
///   [Magic: "ARM_FW" (5B)] [Version len(1B)] [Version(NB)]
///   [Size(4B,LE)] [Reserved(4B)] [Payload...]
///
/// 输出: test_arm_firmware.bin + 打印 MD5 和大小信息

import 'dart:convert';
import 'dart:io';
import 'dart:typed_data';

void main() {
  const version = 'V2.0.1.20260702';
  const totalSize = 48 * 1024; // 48KB 模拟固件
  const headerMagic = 'ARM_FW';
  const reservedSize = 4;

  // 构建头部
  final versionBytes = utf8.encode(version);
  final headerBuilder = BytesBuilder();

  // Magic (5B)
  headerBuilder.add(utf8.encode(headerMagic));
  // Version len (1B)
  headerBuilder.addByte(versionBytes.length);
  // Version string
  headerBuilder.add(versionBytes);
  // Total payload size placeholder (4B, LE) - will be filled later
  headerBuilder.add([0, 0, 0, 0]);
  // Reserved (4B)
  headerBuilder.add([0x41, 0x52, 0x4D, 0x00]); // "ARM\0"

  final header = headerBuilder.toBytes();
  final payloadSize = totalSize - header.length;

  // 填充 payload size 到头部
  header[header.length - reservedSize - 4 + 0] = payloadSize & 0xFF;
  header[header.length - reservedSize - 4 + 1] = (payloadSize >> 8) & 0xFF;
  header[header.length - reservedSize - 4 + 2] = (payloadSize >> 16) & 0xFF;
  header[header.length - reservedSize - 4 + 3] = (payloadSize >> 24) & 0xFF;

  // 构建 payload: 有规律的数据模式，便于验证
  final payload = Uint8List(payloadSize);
  for (int i = 0; i < payloadSize; i++) {
    // 生成可识别的模式: 递增字节 + 地址哈希
    payload[i] = ((i & 0xFF) ^ ((i >> 8) & 0xFF) ^ 0xA5) & 0xFF;
  }

  // 组合完整固件
  final firmware = BytesBuilder();
  firmware.add(header);
  firmware.add(payload);
  final firmwareBytes = firmware.toBytes();

  // 计算 MD5
  final md5Hash = _md5(firmwareBytes);
  final md5Hex = md5Hash.map((b) => b.toRadixString(16).padLeft(2, '0')).join();

  // 写入文件
  final outPath = 'test_arm_firmware.bin';
  File(outPath).writeAsBytesSync(firmwareBytes);

  print('=== ARM 测试固件生成完成 ===');
  print('文件: $outPath');
  print('大小: ${firmwareBytes.length} 字节 (${firmwareBytes.length / 1024} KB)');
  print('版本: $version');
  print('MD5:  $md5Hex');
  print('');
  print('固件结构:');
  print('  Magic:  ${headerMagic} (${headerMagic.length}B)');
  print('  Header: ${header.length}B');
  print('  Payload: ${payloadSize}B');
  print('');
  print('将此文件用于 MQTT OTA 升级命令的 url 字段指向的文件');
  print('ESP32 下载后会通过 UART 流式发送给模拟器');
}

/// 简单的 MD5 实现（仅供测试脚本使用，非加密用途）
List<int> _md5(List<int> data) {
  // 使用 Dart 标准库的方式：通过 digest 计算
  // 这里用一个简单的实现，因为 dart:crypto 在独立脚本中不可用
  // 实际上我们可以用 process 方式
  return _md5Core(data);
}

/// RFC 1321 MD5 实现
List<int> _md5Core(List<int> input) {
  // 初始化
  int a0 = 0x67452301;
  int b0 = 0xefcdab89;
  int c0 = 0x98badcfe;
  int d0 = 0x10325476;

  // 预处理: 填充
  final msgLen = input.length;
  final bitLen = msgLen * 8;

  // 填充到 56 mod 64 字节
  final padded = List<int>.from(input);
  padded.add(0x80);
  while (padded.length % 64 != 56) {
    padded.add(0);
  }

  // 追加原始长度 (64-bit LE)
  for (int i = 0; i < 8; i++) {
    padded.add((bitLen >> (i * 8)) & 0xFF);
  }

  // 处理每个 512-bit (64-byte) 块
  for (int offset = 0; offset < padded.length; offset += 64) {
    final w = List<int>.filled(16, 0);
    for (int i = 0; i < 16; i++) {
      w[i] = padded[offset + i * 4] |
             (padded[offset + i * 4 + 1] << 8) |
             (padded[offset + i * 4 + 2] << 16) |
             (padded[offset + i * 4 + 3] << 24);
    }

    int aa = a0, bb = b0, cc = c0, dd = d0;

    // Round 1
    for (int i = 0; i < 16; i++) {
      final f = (bb & cc) | (~bb & dd);
      final g = i;
      final temp = dd;
      dd = cc;
      cc = bb;
      bb = _add32(bb, _rotateLeft(_add32(aa, _add32(f, _add32(_t[i], w[g]))), _s1[i]));
      aa = temp;
    }
    // Round 2
    for (int i = 0; i < 16; i++) {
      final f = (dd & bb) | (~dd & cc);
      final g = (5 * i + 1) % 16;
      final temp = dd;
      dd = cc;
      cc = bb;
      bb = _add32(bb, _rotateLeft(_add32(aa, _add32(f, _add32(_t[16 + i], w[g]))), _s2[i]));
      aa = temp;
    }
    // Round 3
    for (int i = 0; i < 16; i++) {
      final f = bb ^ cc ^ dd;
      final g = (3 * i + 5) % 16;
      final temp = dd;
      dd = cc;
      cc = bb;
      bb = _add32(bb, _rotateLeft(_add32(aa, _add32(f, _add32(_t[32 + i], w[g]))), _s3[i]));
      aa = temp;
    }
    // Round 4
    for (int i = 0; i < 16; i++) {
      final f = cc ^ (bb | ~dd);
      final g = (7 * i) % 16;
      final temp = dd;
      dd = cc;
      cc = bb;
      bb = _add32(bb, _rotateLeft(_add32(aa, _add32(f, _add32(_t[48 + i], w[g]))), _s4[i]));
      aa = temp;
    }

    a0 = _add32(a0, aa);
    b0 = _add32(b0, bb);
    c0 = _add32(c0, cc);
    d0 = _add32(d0, dd);
  }

  // 输出 (LE)
  final result = List<int>.filled(16, 0);
  for (int i = 0; i < 4; i++) {
    result[i] = (a0 >> (i * 8)) & 0xFF;
    result[4 + i] = (b0 >> (i * 8)) & 0xFF;
    result[8 + i] = (c0 >> (i * 8)) & 0xFF;
    result[12 + i] = (d0 >> (i * 8)) & 0xFF;
  }
  return result;
}

int _add32(int a, int b) => (a + b) & 0xFFFFFFFF;
int _rotateLeft(int val, int n) => ((val << n) | (val >> (32 - n))) & 0xFFFFFFFF;

const List<int> _s1 = [7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22];
const List<int> _s2 = [5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20];
const List<int> _s3 = [4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23];
const List<int> _s4 = [6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21];

const List<int> _t = [
  0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee,
  0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
  0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be,
  0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
  0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa,
  0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
  0x21e1cde6, 0xc33707d7, 0xf4d50d87, 0x455a14ed,
  0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
  0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c,
  0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
  0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05,
  0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
  0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039,
  0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
  0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1,
  0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391,
];
