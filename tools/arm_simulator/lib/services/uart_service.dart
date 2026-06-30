import 'dart:async';
import 'dart:typed_data';

// 命令码定义
class CommandCodes {
  static const int setBroker = 0x01;
  static const int publish = 0x02;
  static const int commandRecv = 0x03;
  static const int commandSend = 0x04;
  static const int factoryReset = 0x05;
  static const int heartbeat = 0x06;
  static const int setSn = 0x08;
  static const int setKey = 0x09;
  static const int setApSsid = 0x0A;
  static const int ack = 0xFE;
  static const int nack = 0xFF;
  static const int armOtaStart = 0x10;
  static const int armOtaData = 0x11;
  static const int armOtaEnd = 0x12;
  static const int armOtaAck = 0x13;
  static const int armOtaNack = 0x14;
  static const int armOtaInfo = 0x15;
}

class UartFrame {
  final int command;
  final Uint8List data;

  UartFrame({required this.command, required this.data});
}

class UartService {
  static const int header = 0xAA;
  static const int maxDataLen = 1024;

  // 数据流控制器
  final _frameController = StreamController<UartFrame>.broadcast();
  final _logController = StreamController<String>.broadcast();
  
  Stream<UartFrame> get frameStream => _frameController.stream;
  Stream<String> get logStream => _logController.stream;

  // 接收缓冲区
  final List<int> _rxBuffer = [];

  // 串口写入回调
  Function(Uint8List)? _writeCallback;

  /// 设置串口写入回调
  void setWriteCallback(Function(Uint8List)? callback) {
    _writeCallback = callback;
  }

  /// 发送帧
  Future<bool> sendFrame(int command, Uint8List data) async {
    if (_writeCallback == null) {
      _log('[WARN] 未设置串口写入回调');
      return false;
    }

    try {
      final frame = _buildFrame(command, data);
      _writeCallback!(frame);
      _log('TX: CMD=0x${command.toRadixString(16).padLeft(2, '0')} LEN=${data.length}');
      return true;
    } catch (e) {
      _log('[ERR] 发送失败: $e');
      return false;
    }
  }

  /// 发送ACK
  Future<bool> sendAck() async {
    return sendFrame(CommandCodes.ack, Uint8List(0));
  }

  /// 发送NACK
  Future<bool> sendNack() async {
    return sendFrame(CommandCodes.nack, Uint8List(0));
  }

  /// 构建帧
  Uint8List _buildFrame(int command, Uint8List data) {
    final len = data.length;
    final buffer = BytesBuilder();
    
    // Header
    buffer.addByte(header);
    // Command
    buffer.addByte(command);
    // Length (big-endian)
    buffer.addByte((len >> 8) & 0xFF);
    buffer.addByte(len & 0xFF);
    // Data
    buffer.add(data);
    // XOR checksum
    int xor = command ^ ((len >> 8) & 0xFF) ^ (len & 0xFF);
    for (var byte in data) {
      xor ^= byte;
    }
    buffer.addByte(xor);

    return buffer.toBytes();
  }

  /// 处理接收到的数据
  void processReceivedData(Uint8List data) {
    for (var byte in data) {
      _rxBuffer.add(byte);
      _tryParseFrame();
    }
  }

  /// 尝试解析帧
  void _tryParseFrame() {
    while (_rxBuffer.isNotEmpty) {
      // 查找帧头
      if (_rxBuffer[0] != header) {
        _rxBuffer.removeAt(0);
        continue;
      }

      // 检查最小长度 (header + cmd + len_h + len_l + xor = 5 bytes minimum)
      if (_rxBuffer.length < 5) return;

      // 解析长度 (byte 2 and 3)
      final lenHigh = _rxBuffer[2];
      final lenLow = _rxBuffer[3];
      final dataLen = (lenHigh << 8) | lenLow;

      // 检查数据长度是否合理
      if (dataLen > maxDataLen) {
        _rxBuffer.removeAt(0);
        continue;
      }

      // 检查完整帧 (header + cmd + len_h + len_l + data + xor)
      final frameLen = 4 + dataLen + 1;
      if (_rxBuffer.length < frameLen) return;

      // 提取帧
      final command = _rxBuffer[1];
      final frameData = Uint8List.fromList(_rxBuffer.sublist(4, 4 + dataLen));
      final xor = _rxBuffer[4 + dataLen];

      // 验证XOR
      int expectedXor = command ^ lenHigh ^ lenLow;
      for (var byte in frameData) {
        expectedXor ^= byte;
      }

      if (xor == expectedXor) {
        _frameController.add(UartFrame(command: command, data: frameData));
        _log('RX: CMD=0x${command.toRadixString(16).padLeft(2, '0')} LEN=$dataLen');
      } else {
        _log('[WARN] XOR校验失败');
      }

      // 移除已处理的帧
      _rxBuffer.removeRange(0, frameLen);
    }
  }

  void _log(String message) {
    final now = DateTime.now();
    final timeStr = '${now.hour.toString().padLeft(2, '0')}:'
        '${now.minute.toString().padLeft(2, '0')}:'
        '${now.second.toString().padLeft(2, '0')}';
    _logController.add('[$timeStr] $message');
  }

  void dispose() {
    _frameController.close();
    _logController.close();
  }
}