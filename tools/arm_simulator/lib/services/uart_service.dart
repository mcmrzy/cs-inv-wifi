import 'dart:async';
import 'dart:typed_data';

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
  static const int frameHeader = 0xAA;
  static const int frameEscape = 0x55;
  static const int escHeader = 0x01;   // 0xAA → 0x55 0x01
  static const int escEscape = 0x00;   // 0x55 → 0x55 0x00
  static const int maxDataLen = 1024;

  final _frameController = StreamController<UartFrame>.broadcast();
  final _logController = StreamController<String>.broadcast();
  
  Stream<UartFrame> get frameStream => _frameController.stream;
  Stream<String> get logStream => _logController.stream;

  // 接收缓冲区（直接存储原始字节，最后统一解析）
  final List<int> _rxBuffer = [];
  bool _rxEscNext = false;

  FutureOr<void> Function(Uint8List)? _writeCallback;

  void setWriteCallback(FutureOr<void> Function(Uint8List)? callback) {
    _writeCallback = callback;
  }

  int _calcXor(int cmd, int lenHigh, int lenLow, List<int> data) {
    int x = frameHeader ^ cmd ^ lenHigh ^ lenLow;
    for (var byte in data) {
      x ^= byte;
    }
    return x & 0xFF;
  }

  void _writeEscaped(BytesBuilder buffer, int b) {
    if (b == frameHeader) {
      buffer.addByte(frameEscape);
      buffer.addByte(escHeader);
    } else if (b == frameEscape) {
      buffer.addByte(frameEscape);
      buffer.addByte(escEscape);
    } else {
      buffer.addByte(b);
    }
  }

  Uint8List _buildFrame(int command, Uint8List data) {
    final len = data.length;
    final lenHigh = (len >> 8) & 0xFF;
    final lenLow = len & 0xFF;
    final xor = _calcXor(command, lenHigh, lenLow, data.toList());
    
    final buffer = BytesBuilder();
    buffer.addByte(frameHeader);
    _writeEscaped(buffer, command);
    _writeEscaped(buffer, lenHigh);
    _writeEscaped(buffer, lenLow);
    for (var byte in data) {
      _writeEscaped(buffer, byte);
    }
    _writeEscaped(buffer, xor);
    return buffer.toBytes();
  }

  Future<bool> sendFrame(int command, Uint8List data) async {
    if (_writeCallback == null) {
      _log('[WARN] 未设置串口写入回调');
      return false;
    }
    try {
      final frame = _buildFrame(command, data);
      await _writeCallback!(frame);
      final hexStr = frame.map((b) => b.toRadixString(16).padLeft(2, '0')).join(' ');
      _log('TX: CMD=0x${command.toRadixString(16).padLeft(2, '0')} LEN=${data.length} [$hexStr]');
      return true;
    } catch (e) {
      _log('[ERR] 发送失败: $e');
      return false;
    }
  }

  Future<bool> sendAck() => sendFrame(CommandCodes.ack, Uint8List(0));
  Future<bool> sendNack() => sendFrame(CommandCodes.nack, Uint8List(0));

  void processReceivedData(Uint8List data) {
    final hexStr = data.map((b) => b.toRadixString(16).padLeft(2, '0')).join(' ');
    _log('RX: [$hexStr]');
    for (var byte in data) {
      _processByte(byte);
    }
  }

  void _processByte(int b) {
    // 转义处理
    if (_rxEscNext) {
      _rxEscNext = false;
      if (b == escHeader) b = frameHeader;
      else if (b == escEscape) b = frameEscape;
    } else if (b == frameEscape) {
      _rxEscNext = true;
      return;
    }

    // 查找帧头
    if (b == frameHeader) {
      _rxBuffer.clear();
      return;
    }

    // 收集数据
    _rxBuffer.add(b);

    // 检查是否收到完整帧: cmd(1) + len_h(1) + len_l(1) + data(N) + xor(1) = N+4
    if (_rxBuffer.length < 4) return;

    final cmd = _rxBuffer[0];
    final lenHigh = _rxBuffer[1];
    final lenLow = _rxBuffer[2];
    final dataLen = (lenHigh << 8) | lenLow;

    // 数据长度检查
    if (dataLen > maxDataLen) {
      _log('[WARN] 数据长度超限: $dataLen');
      _rxBuffer.clear();
      return;
    }

    // 检查是否收齐
    if (_rxBuffer.length < dataLen + 4) return;

    // 提取数据和XOR
    final dataList = _rxBuffer.sublist(3, 3 + dataLen);
    final receivedXor = _rxBuffer[3 + dataLen];

    // 验证XOR
    final expectedXor = _calcXor(cmd, lenHigh, lenLow, dataList);
    
    if (receivedXor == expectedXor) {
      _frameController.add(UartFrame(command: cmd, data: Uint8List.fromList(dataList)));
      _log('RX: CMD=0x${cmd.toRadixString(16).padLeft(2, '0')} LEN=$dataLen');
    } else {
      _log('[WARN] XOR校验失败: 期望=0x${expectedXor.toRadixString(16)} 收到=0x${receivedXor.toRadixString(16)}');
    }

    _rxBuffer.clear();
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