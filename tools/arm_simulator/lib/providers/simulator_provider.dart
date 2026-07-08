import 'dart:async';
import 'dart:convert';
import 'dart:typed_data';
import 'package:crypto/crypto.dart';
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

  // 告警防抖（同一告警码10秒内不重复发送）
  final Map<int, DateTime> _alarmLastSent = {};

  // 告警发送后跳过下一次 data/status，避免云端重复记录
  bool _skipNextStatus = false;

  // 恢复消息已发送标志（避免重复发送）
  bool _recoverySent = false;

  // ACK等待机制
  Completer<void>? _ackCompleter;

  // ARM OTA Bootloader 模拟状态
  static const int _packetSize = 480; // 与 ESP32 ARM_OTA_PACKET_SIZE 一致
  String _otaState = 'idle'; // idle, ready, receiving, verifying, done, error
  int _otaExpectedSize = 0;
  int _otaReceivedBytes = 0;
  int _otaTotalPackets = 0;
  int _otaNextPacketIndex = 0; // 期望的下一个包序号
  List<int> _otaFirmwareBuffer = [];
  List<int> _otaExpectedMd5 = [];
  String _otaVersion = '';

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
      _firstPubDone = false;  // 重连后重新发送 info
      _updatePubTimer();  // 启动发布定时器
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
      _updatePubTimer();  // 停止发布定时器
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

    // 发送恢复消息（直接构造，不依赖 getAlarmJson）
    if (shouldSendRecovery && !_recoverySent) {
      _recoverySent = true;
      _publishRecovery();
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

  /// 发布所有主题（逐个发送，每条等待ACK后再发下一条）
  Future<void> _publishAllTopics() async {
    if (!_firstPubDone) {
      _firstPubDone = true;
      await _publishTopic('info');
    }

    final topics = ['ac', 'battery', 'pv', 'status', 'energy', 'cells'];
    for (var topic in topics) {
      // 告警发送后跳过 status，避免云端重复记录
      if (topic == 'status' && _skipNextStatus) {
        _skipNextStatus = false;
        continue;
      }
      await _publishTopic(topic);
    }
  }

  /// 发布单个主题，发送后等待ESP32回复ACK
  Future<void> _publishTopic(String topic) async {
    final payload = _inverterData.toJson(topic);
    payload['sn'] = _sn;

    // 告警数据特殊处理
    if (topic == 'alarm') {
      final alarmJson = _alarmSystem.getAlarmJson();
      if (alarmJson == null) return;  // 无告警数据，跳过
      payload.addAll(alarmJson);
    }

    final subTopic = topic == 'info' ? 'info' : 'data/$topic';

    // 安全检查：确保 payload 不为空
    if (payload.isEmpty) {
      _addLog('[WARN] 跳过空 payload: $topic');
      return;
    }

    final obj = {
      'sn': _sn,
      'topic': subTopic,
      'payload': jsonEncode(payload),
    };

    final data = utf8.encode(jsonEncode(obj));

    // 创建ACK等待器
    _ackCompleter = Completer<void>();

    await _uartService.sendFrame(CommandCodes.publish, Uint8List.fromList(data));
    _addLog('[TX] PUBLISH data/$topic');

    // 等待ACK，超时500ms后继续
    try {
      await _ackCompleter!.future.timeout(const Duration(milliseconds: 500));
    } on TimeoutException {
      _addLog('[WARN] ACK timeout for data/$topic');
    }
    _ackCompleter = null;
  }

  /// 发布告警恢复消息（checkAlarms 检测到状态转换时调用）
  Future<void> _publishRecovery() async {
    final payload = {
      'sn': _sn,
      'code': 0,
      'level': 'normal',
      'message': '故障恢复，系统正常',
      'count': 0,
      'alarms': [],
      'timestamp': DateTime.now().millisecondsSinceEpoch ~/ 1000,
    };

    final obj = {
      'sn': _sn,
      'topic': 'data/alarm',
      'payload': jsonEncode(payload),
    };

    final data = utf8.encode(jsonEncode(obj));

    _ackCompleter = Completer<void>();

    await _uartService.sendFrame(CommandCodes.publish, Uint8List.fromList(data));
    _addLog('[TX] PUBLISH data/alarm (recovery)');

    try {
      await _ackCompleter!.future.timeout(const Duration(milliseconds: 500));
    } on TimeoutException {
      _addLog('[WARN] ACK timeout for data/alarm (recovery)');
    }
    _ackCompleter = null;
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
        _handleOtaEnd(frame.data);
        break;
      case CommandCodes.ack:
      case CommandCodes.nack:
        // 完成等待ACK的Completer
        if (_ackCompleter != null && !_ackCompleter!.isCompleted) {
          _ackCompleter!.complete();
        }
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
      final cmd = (json['cmd'] ?? json['command'] ?? '') as String;
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
        case 'get_params':
          _handleGetParams(taskId);
          break;
        case 'set_params':
          _handleSetParams(params, taskId);
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
      // 防抖：同一告警码10秒内不重复发送
      final now = DateTime.now();
      if (_alarmLastSent.containsKey(code) &&
          now.difference(_alarmLastSent[code]!).inSeconds < 10) {
        _addLog('[ALARM] 告警 $code 在防抖期内，跳过');
        _uartService.sendAck();
        _sendCmdResult(taskId, 'set_alarm', true, '告警 $code 在防抖期内');
        return;
      }

      _alarmSystem.addAlarm(code, manual: true);
      _syncAlarmCodes();
      _alarmLastSent[code] = now;
      _skipNextStatus = true;
      _recoverySent = false;  // 新告警，重置恢复标志
      _addLog('[ALARM] 远程触发告警: $code');
      _uartService.sendAck();
      _sendCmdResult(taskId, 'set_alarm', true, '已触发告警 $code');
      _publishTopic('alarm');
    } else {
      _alarmSystem.clearAllAlarms();
      _syncAlarmCodes();
      _addLog('[ALARM] 远程清除所有告警');
      _uartService.sendAck();
      _sendCmdResult(taskId, 'set_alarm', true, '已清除所有告警');
      // 恢复消息由 _tick 中 checkAlarms 统一发送
    }
  }

  void _handleSetSimParam(Map<String, dynamic> params, String taskId) {
    _uartService.sendAck();
    _sendCmdResult(taskId, 'set_sim_param', true, '模拟参数已设置');
  }

  void _handleReset(String taskId) {
    _inverterData.soc = 75.0;
    _alarmSystem.clearAllAlarms();
    _syncAlarmCodes();
    _inverterData.mode = InverterMode.inverting;
    _addLog('[SYS] 远程重置逆变器');
    _uartService.sendAck();
    _sendCmdResult(taskId, 'reset', true, '逆变器已重置');
    // 恢复消息由 _tick 中 checkAlarms 统一发送
  }

  void _handleGetParams(String taskId) {
    // 返回当前设备参数
    final data = {
      'soc': _inverterData.soc,
      'soh': _inverterData.soh,
      'voltage': _inverterData.voltage,
      'current': _inverterData.current,
      'power': _inverterData.power,
      'frequency': _inverterData.frequency,
      'load_percent': _inverterData.loadPercent,
      'temp_inv': _inverterData.tempInv,
      'temp_mos': _inverterData.tempMos,
      'battery_voltage': _inverterData.batteryVoltage,
      'battery_current': _inverterData.batteryCurrent,
      'pv1_power': _inverterData.pv1Power,
      'pv2_power': _inverterData.pv2Power,
      'runtime_hours': _inverterData.runtimeHours,
      'state': _inverterData.mode.name,
      'fault_code': _inverterData.faultCode,
      'alarm_code': _inverterData.alarmCode,
    };
    _addLog('[RX] get_params');
    _uartService.sendAck();
    _sendCmdResult(taskId, 'get_params', true, '参数已获取', data);
  }

  void _handleSetParams(Map<String, dynamic> params, String taskId) {
    // 应用远程控制参数
    _addLog('[RX] set_params: $params');
    
    // 处理充电控制等参数
    if (params.containsKey('charge_control')) {
      _inverterData.remoteControls['charge_enable'] = params['charge_control'];
    }
    if (params.containsKey('discharge_control')) {
      _inverterData.remoteControls['discharge_enable'] = params['discharge_control'];
    }
    if (params.containsKey('power_limit')) {
      _inverterData.remoteControls['power_limit'] = params['power_limit'];
    }
    if (params.containsKey('max_charge_current')) {
      _inverterData.remoteControls['max_charge_current'] = params['max_charge_current'];
    }
    if (params.containsKey('max_discharge_current')) {
      _inverterData.remoteControls['max_discharge_current'] = params['max_discharge_current'];
    }
    
    _uartService.sendAck();
    _sendCmdResult(taskId, 'set_params', true, '参数已设置');
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
    // 二进制格式: [version_len(1B)] [version_string(N)] [fw_size(4B,LE)] [md5(16B)]
    final version = 'V1.2.3.20240510';
    final versionBytes = utf8.encode(version);
    final buf = BytesBuilder();
    buf.addByte(versionBytes.length);
    buf.add(versionBytes);
    // fw_size: 4 bytes LE (当前固件大小，若无则为0)
    final fwSize = _otaFirmwareBuffer.isNotEmpty ? _otaFirmwareBuffer.length : 0;
    for (int i = 0; i < 4; i++) {
      buf.addByte((fwSize >> (i * 8)) & 0xFF);
    }
    // md5: 16 bytes
    if (_otaFirmwareBuffer.isNotEmpty) {
      buf.add(md5.convert(_otaFirmwareBuffer).bytes);
    } else {
      buf.add(List.filled(16, 0));
    }
    _uartService.sendFrame(CommandCodes.armOtaInfo, buf.toBytes());
    _addLog('[OTA] 发送固件信息 (二进制): version=$version, size=$fwSize');
  }

  /// 处理 OTA START 命令
  /// DATA: [total_size(4B,LE)] [md5(16B)] [version_len(1B)] [version_string]
  void _handleOtaStart(Uint8List data) {
    if (data.length < 4 + 16 + 1) {
      _addLog('[OTA] START 数据长度不足: ${data.length}');
      _sendOtaNack(0);
      return;
    }

    // 解析 total_size (4B, LE)
    _otaExpectedSize = data[0] | (data[1] << 8) | (data[2] << 16) | (data[3] << 24);

    // 解析 md5 (16B)
    _otaExpectedMd5 = data.sublist(4, 20).toList();

    // 解析 version
    final versionLen = data[20];
    if (data.length >= 21 + versionLen) {
      _otaVersion = utf8.decode(data.sublist(21, 21 + versionLen));
    } else {
      _otaVersion = 'unknown';
    }

    // 初始化接收状态
    _otaState = 'ready';
    _otaReceivedBytes = 0;
    _otaNextPacketIndex = 0;

    _otaFirmwareBuffer = [];
    _otaTotalPackets = (_otaExpectedSize + _packetSize - 1) ~/ _packetSize;

    _addLog('[OTA] START: size=$_otaExpectedSize, packets=$_otaTotalPackets, '
        'md5=${_otaExpectedMd5.map((b) => b.toRadixString(16).padLeft(2, '0')).join()}, '
        'version=$_otaVersion');

    // 回复 ACK（表示 ARM 已进入 IAP 模式就绪）
    _uartService.sendFrame(CommandCodes.armOtaAck, Uint8List(0));
    _addLog('[OTA] ARM IAP 模式就绪，发送 ACK');
  }

  /// 处理 OTA DATA 命令
  /// DATA: [packet_index(2B,LE)] [firmware_data(up to 480B)]
  void _handleOtaData(Uint8List data) {
    if (_otaState != 'ready' && _otaState != 'receiving') {
      _addLog('[OTA] DATA 在非法状态 $_otaState 收到');
      _sendOtaNack(0);
      return;
    }

    if (data.length < 2) {
      _addLog('[OTA] DATA 数据长度不足: ${data.length}');
      _sendOtaNack(_otaNextPacketIndex);
      return;
    }

    // 解析 packet_index (2B, LE)
    final packetIndex = data[0] | (data[1] << 8);
    final firmwareData = data.sublist(2);

    // 检查包序号
    if (packetIndex != _otaNextPacketIndex) {
      _addLog('[OTA] DATA 包序号不匹配: 期望=$_otaNextPacketIndex, 收到=$packetIndex');
      _sendOtaNack(_otaNextPacketIndex);
      return;
    }

    // 累积固件数据
    _otaFirmwareBuffer.addAll(firmwareData);
    _otaReceivedBytes += firmwareData.length;
    _otaNextPacketIndex++;
    _otaState = 'receiving';


    // 进度
    final progress = _otaExpectedSize > 0
        ? (_otaReceivedBytes * 100 ~/ _otaExpectedSize).clamp(0, 100)
        : 0;

    // 每10包或首尾包打印日志
    if (packetIndex % 10 == 0 || packetIndex == _otaTotalPackets - 1) {
      _addLog('[OTA] DATA[$packetIndex/$_otaTotalPackets] '
          '${firmwareData.length}B, 累计 $_otaReceivedBytes/$_otaExpectedSize ($progress%)');
    }

    // 回复 ACK
    _uartService.sendFrame(CommandCodes.armOtaAck, Uint8List(0));
  }

  /// 处理 OTA END 命令
  /// DATA: [total_packets(2B,LE)] [total_size(4B,LE)] [md5(16B)]
  void _handleOtaEnd(Uint8List data) {
    // END 帧数据通过 _handleFrame 传入时已经解码，但当前 _handleOtaEnd 没有参数
    // 我们用内部状态来验证
    if (_otaState != 'receiving' && _otaState != 'ready') {
      _addLog('[OTA] END 在非法状态 $_otaState 收到');
      _sendOtaNack(0);
      return;
    }

    _otaState = 'verifying';

    // 解析 END 帧数据
    int endPackets = 0, endSize = 0;
    List<int> endMd5 = [];
    if (data.length >= 2 + 4 + 16) {
      endPackets = data[0] | (data[1] << 8);
      endSize = data[2] | (data[3] << 8) | (data[4] << 16) | (data[5] << 24);
      endMd5 = data.sublist(6, 22).toList();
      _addLog('[OTA] END 帧: packets=$endPackets, size=$endSize');
    }

    _addLog('[OTA] 开始校验...');

    // 校验1: 数据大小 (使用 END 帧中的 size 或 START 帧中的)
    final expectedSize = endSize > 0 ? endSize : _otaExpectedSize;
    final sizeOk = _otaReceivedBytes == expectedSize;
    _addLog('[OTA] 大小校验: ${sizeOk ? "通过" : "失败"} '
        '(期望=$expectedSize, 实际=$_otaReceivedBytes)');

    // 校验2: 包数量 (使用 END 帧中的 packets 或计算值)
    final expectedPackets = endPackets > 0 ? endPackets : _otaTotalPackets;
    final packetsOk = _otaNextPacketIndex == expectedPackets;
    _addLog('[OTA] 包数校验: ${packetsOk ? "通过" : "失败"} '
        '(期望=$expectedPackets, 实际=$_otaNextPacketIndex)');

    // 校验3: MD5 (优先用 END 帧中的 md5，其次用 START 帧中的)
    final expectedMd5 = endMd5.isNotEmpty && endMd5.any((b) => b != 0)
        ? endMd5
        : _otaExpectedMd5;
    bool md5Ok = true;
    if (expectedMd5.isNotEmpty && expectedMd5.any((b) => b != 0)) {
      final actualMd5 = md5.convert(_otaFirmwareBuffer).bytes;
      for (int i = 0; i < 16; i++) {
        if (actualMd5[i] != expectedMd5[i]) {
          md5Ok = false;
          break;
        }
      }
      _addLog('[OTA] MD5校验: ${md5Ok ? "通过" : "失败"}');
    } else {
      _addLog('[OTA] MD5校验: 跳过 (ESP32未提供MD5)');
    }

    if (sizeOk && packetsOk && md5Ok) {
      _otaState = 'done';
      _addLog('[OTA] ✅ 固件校验通过! OTA 完成');
      _addLog('[OTA] 固件版本: $_otaVersion, 大小: $_otaReceivedBytes 字节, '
          '包数: $_otaNextPacketIndex');
      // 回复 ACK（表示校验成功）
      _uartService.sendFrame(CommandCodes.armOtaAck, Uint8List(0));
    } else {
      _otaState = 'error';
      _addLog('[OTA] ❌ 固件校验失败!');
      // 回复 NACK
      _sendOtaNack(0);
    }

    // 重置状态以便下次 OTA
    _resetOtaState();
  }

  void _sendOtaNack(int packetIndex) {
    // NACK DATA: [packet_index(2B, LE)]
    final buf = Uint8List(2);
    buf[0] = packetIndex & 0xFF;
    buf[1] = (packetIndex >> 8) & 0xFF;
    _uartService.sendFrame(CommandCodes.armOtaNack, buf);
    _addLog('[OTA] 发送 NACK: packet=$packetIndex');
  }

  void _resetOtaState() {
    // 延迟重置，让日志先显示
    Future.delayed(const Duration(seconds: 1), () {
      _otaState = 'idle';
      _otaFirmwareBuffer = [];
      _otaExpectedMd5 = [];
      _otaExpectedSize = 0;
      _otaReceivedBytes = 0;
      _otaTotalPackets = 0;
      _otaNextPacketIndex = 0;
    });
  }

  void _sendCmdResult(String taskId, String cmd, bool success, String message, [Map<String, dynamic>? data]) {
    final result = {
      'task_id': taskId,
      'cmd': cmd,
      'success': success,
      'message': message,
      'timestamp': DateTime.now().millisecondsSinceEpoch ~/ 1000,
    };
    if (data != null) {
      result['data'] = data;
    }
    final encoded = utf8.encode(jsonEncode(result));
    _uartService.sendFrame(CommandCodes.commandSend, Uint8List.fromList(encoded));
    _addLog('[TX] CMD_RESULT: $cmd ${success ? "成功" : "失败"}');
  }

  /// 手动触发告警
  void triggerAlarm(int code) {
    // 防抖：同一告警码10秒内不重复发送
    final now = DateTime.now();
    if (_alarmLastSent.containsKey(code) &&
        now.difference(_alarmLastSent[code]!).inSeconds < 10) {
      _addLog('[ALARM] 告警 $code 在防抖期内，跳过');
      return;
    }

    _alarmSystem.addAlarm(code, manual: true);
    _syncAlarmCodes();
    _alarmLastSent[code] = now;
    _skipNextStatus = true;
    _recoverySent = false;  // 新告警，重置恢复标志
    _addLog('[ALARM] 手动触发告警: $code');
    _publishTopic('alarm');
    notifyListeners();
  }

  /// 清除所有告警
  void clearAllAlarms() {
    _alarmSystem.clearAllAlarms();
    _alarmLastSent.clear();
    _syncAlarmCodes();
    _addLog('[ALARM] 清除所有告警');
    // 恢复消息由 _tick 中 checkAlarms 检测到状态转换后统一发送
    notifyListeners();
  }

  /// 同步告警码到 InverterData（确保 data/status 与 alarm 一致）
  void _syncAlarmCodes() {
    _inverterData.faultCode = _alarmSystem.activeAlarms
        .where((code) => AlarmSystem.alarmDefinitions[code]?.level == 'fault')
        .firstOrNull ?? 0;
    _inverterData.alarmCode = _alarmSystem.activeAlarms.firstOrNull ?? 0;
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