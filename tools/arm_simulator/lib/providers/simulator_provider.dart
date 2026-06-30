import 'dart:async';
import 'dart:convert';
import 'package:flutter/foundation.dart';
import 'package:flutter_libserialport/flutter_libserialport.dart';
import '../models/inverter_state.dart';
import '../models/alarm_system.dart';
import '../services/uart_service.dart';

class SimulatorProvider extends ChangeNotifier {
  final UartService _uartService = UartService();
  final InverterData _inverterData = InverterData();
  final AlarmSystem _alarmSystem = AlarmSystem();
  final AlarmConfig _alarmConfig = const AlarmConfig();

  // 日志列表
  final List<String> _logs = [];
  static const int maxLogCount = 1000;

  // 模拟控制
  bool _autoSimulate = true;
  bool _autoPublish = true;
  double _publishInterval = 180.0; // 3分钟
  Timer? _simTimer;
  Timer? _pubTimer;

  // 设备信息
  String _sn = 'H1CNA00135000014';
  bool _firstPubDone = false;

  // 随机告警控制
  final Map<int, DateTime> _randomAlarmClearTime = {};
  double _alarmProbability = 0.0001;

  // 串口
  List<String> _availablePorts = [];
  String? _selectedPort;
  SerialPort? _serialPort;

  // Getters
  UartService get uartService => _uartService;
  InverterData get inverterData => _inverterData;
  AlarmSystem get alarmSystem => _alarmSystem;
  List<String> get logs => List.unmodifiable(_logs);
  bool get autoSimulate => _autoSimulate;
  bool get autoPublish => _autoPublish;
  double get publishInterval => _publishInterval;
  String get sn => _sn;
  bool get isConnected => _serialPort != null && _serialPort!.isOpen;
  double get alarmProbability => _alarmProbability;
  List<String> get availablePorts => _availablePorts;
  String? get selectedPort => _selectedPort;

  SimulatorProvider() {
    _setupUartListener();
    _startSimulation();
    refreshPorts();
  }

  /// 刷新可用串口列表
  void refreshPorts() {
    _availablePorts = SerialPort.availablePorts;
    if (_availablePorts.isNotEmpty && _selectedPort == null) {
      _selectedPort = _availablePorts.first;
    }
    _addLog('[SYS] 发现 ${_availablePorts.length} 个串口');
    notifyListeners();
  }

  /// 选择串口
  void setSelectedPort(String? port) {
    _selectedPort = port;
    notifyListeners();
  }

  /// 连接串口
  Future<void> connect() async {
    if (_selectedPort == null) {
      _addLog('[ERR] 请先选择串口');
      return;
    }

    try {
      _serialPort = SerialPort(_selectedPort!);
      if (!_serialPort!.openReadWrite()) {
        _addLog('[ERR] 无法打开串口 $_selectedPort');
        _serialPort = null;
        notifyListeners();
        return;
      }

      // 配置串口参数（必须获取当前配置再修改）
      final config = _serialPort!.config;
      config.baudRate = 115200;
      config.bits = 8;
      config.stopBits = 1;
      config.parity = SerialPortParity.none;
      _serialPort!.config = config;

      // 监听数据
      final reader = SerialPortReader(_serialPort!);
      reader.stream.listen((data) {
        _uartService.processReceivedData(Uint8List.fromList(data));
      });

      // 设置写入回调（逐字节写入，彻底排除USB串口驱动缓冲区问题）
      _uartService.setWriteCallback((Uint8List data) async {
        if (_serialPort != null && _serialPort!.isOpen) {
          try {
            for (int i = 0; i < data.length; i++) {
              final singleByte = Uint8List.fromList([data[i]]);
              _serialPort!.write(singleByte, timeout: 1000);
            }
          } catch (e) {
            _addLog('[ERR] 写入异常: $e');
          }
        }
      });

      _addLog('[SYS] 已连接到 $_selectedPort (115200 8N1)');
      notifyListeners();
    } catch (e) {
      _addLog('[ERR] 连接失败: $e');
      _serialPort = null;
      notifyListeners();
    }
  }

  /// 断开连接
  void disconnect() {
    if (_serialPort != null) {
      _uartService.setWriteCallback(null);
      _serialPort!.close();
      _serialPort = null;
      _addLog('[SYS] 已断开连接');
      notifyListeners();
    }
  }

  void _setupUartListener() {
    _uartService.frameStream.listen(_handleFrame);
    _uartService.logStream.listen((log) {
      _addLog(log);
    });
  }

  void _startSimulation() {
    // 模拟定时器 (200ms)
    _simTimer = Timer.periodic(const Duration(milliseconds: 200), (_) {
      if (_autoSimulate) {
        _tick(0.2);
      }
    });

    // 发布定时器
    _updatePubTimer();
  }

  void _updatePubTimer() {
    _pubTimer?.cancel();
    if (_autoPublish && isConnected) {
      _pubTimer = Timer.periodic(
        Duration(seconds: _publishInterval.toInt()),
        (_) => _publishAllTopics(),
      );
    }
  }

  /// 模拟tick
  void _tick(double dt) {
    // 更新运行时间
    _inverterData.runtimeHours += dt / 3600.0;

    // 模拟参数波动
    _simulateParameters(dt);

    // 检查告警
    final shouldSendRecovery = _alarmSystem.checkAlarms(_inverterData, _alarmConfig);
    
    // 更新故障码
    _inverterData.faultCode = _alarmSystem.activeAlarms
        .where((code) => AlarmSystem.alarmDefinitions[code]?.level == 'fault')
        .firstOrNull ?? 0;
    
    _inverterData.alarmCode = _alarmSystem.activeAlarms.firstOrNull ?? 0;

    // 发送恢复消息
    if (shouldSendRecovery) {
      _publishTopic('alarm');
    }

    // 随机告警
    _checkRandomAlarms();

    notifyListeners();
  }

  void _simulateParameters(double dt) {
    // 温度波动
    _inverterData.tempInv += (0.1 * (0.5 - _inverterData.tempInv / 100)) * dt;
    _inverterData.tempMos = _inverterData.tempInv - 3 + _randomDouble(-1, 1);
    _inverterData.tempAmbient = 32 + _randomDouble(-2, 2);

    // 电压波动
    _inverterData.voltage = 220 + _randomDouble(-2, 2);
    _inverterData.current = 8.5 + _randomDouble(-0.5, 0.5);
    _inverterData.power = _inverterData.voltage * _inverterData.current * 0.99;
    _inverterData.frequency = 50 + _randomDouble(-0.1, 0.1);
    _inverterData.loadPercent = (_inverterData.power / _inverterData.ratedPower * 100);

    // 电池参数
    _inverterData.batteryVoltage = 52.8 + _randomDouble(-0.5, 0.5);
    _inverterData.batteryCurrent = 15.3 + _randomDouble(-1, 1);

    // 光伏参数
    _inverterData.pv1Voltage = 120 + _randomDouble(-5, 5);
    _inverterData.pv1Current = 8 + _randomDouble(-0.5, 0.5);
    _inverterData.pv1Power = _inverterData.pv1Voltage * _inverterData.pv1Current;
    _inverterData.pv2Voltage = 118 + _randomDouble(-5, 5);
    _inverterData.pv2Current = 7.5 + _randomDouble(-0.5, 0.5);
    _inverterData.pv2Power = _inverterData.pv2Voltage * _inverterData.pv2Current;

    // 电芯电压
    for (int i = 0; i < _inverterData.cellVoltages.length; i++) {
      _inverterData.cellVoltages[i] = 3.3 + _randomDouble(-0.01, 0.01);
    }
    for (int i = 0; i < _inverterData.cellTemps.length; i++) {
      _inverterData.cellTemps[i] = _inverterData.tempInv / _inverterData.cellCount + _randomDouble(-1, 2);
    }
  }

  double _randomDouble(double min, double max) {
    return min + (max - min) * (DateTime.now().microsecondsSinceEpoch % 1000) / 1000;
  }

  void _checkRandomAlarms() {
    // 检查并清除过期的随机告警
    final now = DateTime.now();
    final expired = _randomAlarmClearTime.entries
        .where((e) => now.isAfter(e.value) && _alarmSystem.activeAlarms.contains(e.key))
        .map((e) => e.key)
        .toList();

    for (var code in expired) {
      _alarmSystem.removeAlarm(code);
      _randomAlarmClearTime.remove(code);
    }

    // 随机触发告警
    if (_randomDouble(0, 1) < _alarmProbability) {
      final codes = [8, 9, 10, 11, 12];
      final code = codes[DateTime.now().millisecondsSinceEpoch % codes.length];
      if (!_alarmSystem.activeAlarms.contains(code)) {
        _alarmSystem.addAlarm(code);
        _randomAlarmClearTime[code] = now.add(const Duration(seconds: 30));
      }
    }
  }

  /// 发布所有主题（逐个发送，每条之间加延迟让ESP32处理）
  Future<void> _publishAllTopics() async {
    if (!_firstPubDone) {
      _firstPubDone = true;
      await _publishTopic('info');
      await Future.delayed(const Duration(milliseconds: 200));
    }

    final topics = ['ac', 'battery', 'pv', 'status', 'energy', 'cells', 'alarm'];
    for (var topic in topics) {
      await _publishTopic(topic);
      await Future.delayed(const Duration(milliseconds: 200));
    }
  }

  /// 发布单个主题
  Future<void> _publishTopic(String topic) async {
    final payload = _inverterData.toJson(topic);
    payload['sn'] = _sn;

    // 告警数据特殊处理
    if (topic == 'alarm') {
      final alarmJson = _alarmSystem.getAlarmJson();
      if (alarmJson == null) return; // 无告警，不发送
      payload.addAll(alarmJson);
    }

    final obj = {
      'sn': _sn,
      'topic': 'data/$topic',
      'payload': jsonEncode(payload),
    };

    final data = utf8.encode(jsonEncode(obj));
    await _uartService.sendFrame(CommandCodes.publish, Uint8List.fromList(data));
    _addLog('[TX] PUBLISH data/$topic');
  }

  /// 处理接收到的帧
  void _handleFrame(UartFrame frame) {
    switch (frame.command) {
      case CommandCodes.heartbeat:
        _uartService.sendAck();
        break;
      case CommandCodes.publish:
        _handlePublish(frame.data);
        break;
      case CommandCodes.commandRecv:
        _handleCommand(frame.data);
        break;
      case CommandCodes.setBroker:
        _uartService.sendAck();
        break;
      case CommandCodes.setSn:
        _handleSetSn(frame.data);
        break;
      case CommandCodes.factoryReset:
        _handleFactoryReset();
        break;
      case CommandCodes.armOtaInfo:
        _handleOtaInfoRequest();
        break;
      case CommandCodes.armOtaStart:
        _handleOtaStart(frame.data);
        break;
      case CommandCodes.armOtaData:
        _handleOtaData(frame.data);
        break;
      case CommandCodes.armOtaEnd:
        _handleOtaEnd();
        break;
      case CommandCodes.ack:
      case CommandCodes.nack:
        break;
      default:
        _uartService.sendNack();
    }
  }

  void _handlePublish(Uint8List data) {
    try {
      final json = jsonDecode(utf8.decode(data));
      _addLog('[RX] PUBLISH: ${json['topic']}');
      _uartService.sendAck();
    } catch (e) {
      _addLog('[ERR] PUBLISH解析失败: $e');
      _uartService.sendNack();
    }
  }

  void _handleCommand(Uint8List data) {
    try {
      final json = jsonDecode(utf8.decode(data));
      final cmd = json['cmd'] as String;
      final params = json['params'] as Map<String, dynamic>? ?? {};
      final taskId = json['task_id'] as String? ?? '';

      _addLog('[RX] CMD: $cmd');

      switch (cmd) {
        case 'set_control':
          _handleSetControl(params, taskId);
          break;
        case 'set_alarm':
          _handleSetAlarm(params, taskId);
          break;
        case 'set_sim_param':
          _handleSetSimParam(params, taskId);
          break;
        case 'reset':
          _handleReset(taskId);
          break;
        default:
          _addLog('[WARN] 未知命令: $cmd');
          _uartService.sendNack();
          _sendCmdResult(taskId, cmd, false, '未知命令: $cmd');
      }
    } catch (e) {
      _addLog('[ERR] 命令解析失败: $e');
      _uartService.sendNack();
    }
  }

  void _handleSetControl(Map<String, dynamic> params, String taskId) {
    _uartService.sendAck();
    _sendCmdResult(taskId, 'set_control', true, '控制参数已设置');
  }

  void _handleSetAlarm(Map<String, dynamic> params, String taskId) {
    final code = params['code'] as int? ?? 0;
    if (code > 0) {
      _alarmSystem.addAlarm(code, manual: true);
      _addLog('[ALARM] 远程触发告警: $code');
      _uartService.sendAck();
      _sendCmdResult(taskId, 'set_alarm', true, '已触发告警 $code');
    } else {
      _alarmSystem.clearAllAlarms();
      _addLog('[ALARM] 远程清除所有告警');
      _uartService.sendAck();
      _sendCmdResult(taskId, 'set_alarm', true, '已清除所有告警');
    }
  }

  void _handleSetSimParam(Map<String, dynamic> params, String taskId) {
    _uartService.sendAck();
    _sendCmdResult(taskId, 'set_sim_param', true, '模拟参数已设置');
  }

  void _handleReset(String taskId) {
    _inverterData.soc = 75.0;
    _alarmSystem.clearAllAlarms();
    _inverterData.mode = InverterMode.inverting;
    _addLog('[SYS] 远程重置逆变器');
    _uartService.sendAck();
    _sendCmdResult(taskId, 'reset', true, '逆变器已重置');
  }

  void _handleSetSn(Uint8List data) {
    try {
      final sn = utf8.decode(data);
      _sn = sn;
      _addLog('[SYS] SN设置为: $sn');
      _uartService.sendAck();
    } catch (e) {
      _uartService.sendNack();
    }
  }

  void _handleFactoryReset() {
    _addLog('[SYS] 恢复出厂设置');
    _uartService.sendAck();
  }

  void _handleOtaInfoRequest() {
    final info = {
      'version': 'V1.2.3.20240510',
      'size': 0,
      'md5': '',
    };
    final data = utf8.encode(jsonEncode(info));
    _uartService.sendFrame(CommandCodes.armOtaInfo, Uint8List.fromList(data));
    _addLog('[OTA] 发送固件信息');
  }

  void _handleOtaStart(Uint8List data) {
    _addLog('[OTA] 收到OTA开始命令');
    _uartService.sendFrame(CommandCodes.armOtaAck, Uint8List(0));
  }

  void _handleOtaData(Uint8List data) {
    _uartService.sendFrame(CommandCodes.armOtaAck, Uint8List(0));
  }

  void _handleOtaEnd() {
    _addLog('[OTA] OTA传输结束');
    _uartService.sendFrame(CommandCodes.armOtaAck, Uint8List(0));
  }

  void _sendCmdResult(String taskId, String cmd, bool success, String message) {
    final result = {
      'task_id': taskId,
      'cmd': cmd,
      'success': success,
      'message': message,
      'timestamp': DateTime.now().millisecondsSinceEpoch ~/ 1000,
    };
    final data = utf8.encode(jsonEncode(result));
    _uartService.sendFrame(CommandCodes.commandSend, Uint8List.fromList(data));
    _addLog('[TX] CMD_RESULT: $cmd ${success ? "成功" : "失败"}');
  }

  /// 手动触发告警
  void triggerAlarm(int code) {
    _alarmSystem.addAlarm(code, manual: true);
    _addLog('[ALARM] 手动触发告警: $code');
    notifyListeners();
  }

  /// 清除所有告警
  void clearAllAlarms() {
    _alarmSystem.clearAllAlarms();
    _addLog('[ALARM] 清除所有告警');
    notifyListeners();
  }

  /// 更新UI
  void updateUI() {
    notifyListeners();
  }

  /// 设置模拟参数
  void setAutoSimulate(bool value) {
    _autoSimulate = value;
    notifyListeners();
  }

  void setAutoPublish(bool value) {
    _autoPublish = value;
    _updatePubTimer();
    notifyListeners();
  }

  void setPublishInterval(double seconds) {
    _publishInterval = seconds;
    _updatePubTimer();
    notifyListeners();
  }

  void setAlarmProbability(double value) {
    _alarmProbability = value;
    notifyListeners();
  }

  void clearLogs() {
    _logs.clear();
    notifyListeners();
  }

  void _addLog(String message) {
    final now = DateTime.now();
    final timeStr = '${now.hour.toString().padLeft(2, '0')}:'
        '${now.minute.toString().padLeft(2, '0')}:'
        '${now.second.toString().padLeft(2, '0')}';
    _logs.insert(0, '[$timeStr] $message');
    if (_logs.length > maxLogCount) {
      _logs.removeLast();
    }
    notifyListeners();
  }

  @override
  void dispose() {
    _simTimer?.cancel();
    _pubTimer?.cancel();
    _uartService.dispose();
    super.dispose();
  }
}