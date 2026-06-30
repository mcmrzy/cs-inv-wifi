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

enum RxState { waitHeader, waitData }

class UartService {
  // 协议常量 (与ESP32一致)
  static const int frameHeader = 0xAA;
  static const int frameEscape = 0x01;
  static const int escHeader = 0x01;   // 0xAA -> 0x01 0x01
  static const int escEscape = 0x00;   // 0x01 -> 0x01 0x00
  static const int maxDataLen = 1024;

  // 数据流控制器
  final _frameController = StreamController<UartFrame>.broadcast();
  final _logController = StreamController<String>.broadcast();
  
  Stream<UartFrame> get frameStream => _frameController.stream;
  Stream<String> get logStream => _logController.stream;

  // 接收状态机
  RxState _rxState = RxState.waitHeader;
  int _rxCmd = 0;
  int _rxDataLen = 0;
  int _rxDataIdx = 0;
  List<int> _rxDataBuf = [];
  int _rxXor = 0;
  bool _rxEscNext = false;

  // 串口写入回调
  Function(Uint8List)? _writeCallback;

  /// 设置串口写入回调
  void setWriteCallback(Function(Uint8List)? callback) {
    _writeCallback = callback;
  }

  /// 计算XOR校验 (与ESP32一致: 包括header)
  int _calcXor(int cmd, int lenHigh, int lenLow, Uint8List data) {
    int x = frameHeader ^ cmd ^ lenHigh ^ lenLow;
    for (var byte in data) {
      x ^= byte;
    }
    return x & 0xFF;
  }

  /// 写入转义字节
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

  /// 构建帧 (带转义)
  Uint8List _buildFrame(int command, Uint8List data) {
    final len = data.length;
    final lenHigh = (len >> 8) & 0xFF;
    final lenLow = len & 0xFF;
    final xor = _calcXor(command, lenHigh, lenLow, data);
    
    final buffer = BytesBuilder();
    
    // Header (不转义)
    buffer.addByte(frameHeader);
    // Command (转义)
    _writeEscaped(buffer, command);
    // Length high (转义)
    _writeEscaped(buffer, lenHigh);
    // Length low (转义)
    _writeEscaped(buffer, lenLow);
    // Data (转义)
    for (var byte in data) {
      _writeEscaped(buffer, byte);
    }
    // XOR (转义)
    _writeEscaped(buffer, xor);

    return buffer.toBytes();
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

  /// 处理接收到的数据 (带转义解码)
  void processReceivedData(Uint8List data) {
    for (var byte in data) {
      _processByte(byte);
    }
  }

  /// 处理单个字节
  void _processByte(int b) {
    // 转义解码 (在非等待帧头状态下)
    if (_rxState != RxState.waitHeader && !_rxEscNext && b == frameEscape) {
      _rxEscNext = true;
      return;
    }
    
    if (_rxEscNext) {
      _rxEscNext = false;
      if (b == escHeader) {
        b = frameHeader;
      } else if (b == escEscape) {
        b = frameEscape;
      }
    }

    switch (_rxState) {
      case RxState.waitHeader:
        if (b == frameHeader) {
          _rxState = RxState.waitData;
          _rxEscNext = false;
        }
        break;

      case RxState.waitData:
        // 第1个字节: 命令码
        if (_rxCmd == 0 && _rxDataLen == 0 && _rxDataIdx == 0) {
          _rxCmd = b;
        }
        // 第2个字节: 长度高字节
        else if (_rxDataLen == 0 && _rxDataIdx == 0) {
          _rxDataLen = b << 8;
        }
        // 第3个字节: 长度低字节
        else if (_rxDataIdx == 0) {
          _rxDataLen |= b;
          if (_rxDataLen > maxDataLen) {
            _resetRxState();
            _log('[WARN] 数据长度超限: $_rxDataLen');
          }
        }
        // 数据字节
        else if (_rxDataIdx < _rxDataLen) {
          _rxDataBuf.add(b);
          _rxDataIdx++;
        }
        // XOR校验字节
        else {
          _rxXor = b;
          _validateFrame();
        }
        break;
    }
  }

  /// 验证帧
  void _validateFrame() {
    final data = Uint8List.fromList(_rxDataBuf);
    final expectedXor = _calcXor(_rxCmd, (_rxDataLen >> 8) & 0xFF, _rxDataLen & 0xFF, data);
    
    if (_rxXor == expectedXor) {
      _frameController.add(UartFrame(command: _rxCmd, data: data));
      _log('RX: CMD=0x${_rxCmd.toRadixString(16).padLeft(2, '0')} LEN=$_rxDataLen');
    } else {
      _log('[WARN] XOR校验失败: 期望=0x${expectedXor.toRadixString(16)} 收到=0x${_rxXor.toRadixString(16)}');
    }
    
    _resetRxState();
  }

  /// 重置接收状态
  void _resetRxState() {
    _rxState = RxState.waitHeader;
    _rxCmd = 0;
    _rxDataLen = 0;
    _rxDataIdx = 0;
    _rxDataBuf = [];
    _rxXor = 0;
    _rxEscNext = false;
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