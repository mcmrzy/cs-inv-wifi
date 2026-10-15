import 'dart:async';
import 'dart:convert';
import 'dart:math';
import 'dart:typed_data';
import 'package:crypto/crypto.dart';
import 'package:flutter/foundation.dart';
import 'package:flutter_libserialport/flutter_libserialport.dart';
import 'package:flutter_secure_storage/flutter_secure_storage.dart';
import 'package:mqtt_client/mqtt_client.dart';
import 'package:shared_preferences/shared_preferences.dart';
import '../models/inverter_state.dart';
import '../models/alarm_system.dart';
import '../protocol/telemetry_protocol.dart';
import '../services/mqtt_service.dart';
import '../services/uart_service.dart';

class SimulatorProvider extends ChangeNotifier {
  final UartService _uartService = UartService();
  InverterData _inverterData = InverterData();
  final AlarmSystem _alarmSystem = AlarmSystem();
  final AlarmConfig _alarmConfig = const AlarmConfig();
  final MqttService _mqttService = MqttService();
  final FlutterSecureStorage _secureStorage;
  final bool useSecureStorage;
  late final Future<void> _settingsFuture;
  StreamSubscription<UartFrame>? _frameSubscription;
  StreamSubscription<String>? _logSubscription;
  bool _disposed = false;

  // 日志列表
  final List<String> _logs = [];
  static const int maxLogCount = 1000;

  // 模拟控制
  bool _autoSimulate = true;
  bool _autoPublish = true;
  double _publishInterval = 180.0; // 3分钟
  Timer? _simTimer;
  Timer? _pubTimer;
  Timer? _parallelTimer;
  Timer? _forceChargeTimer;
  Timer? _forceDischargeTimer;

  // 设备信息
  String _sn = 'H1CNA00135000014';
  bool _firstPubDone = false;
  bool _directMqtt = false;
  bool _mqttConnecting = false;
  String _mqttHost = '127.0.0.1';
  int _mqttPort = 1883;
  bool _mqttTls = false;
  String _mqttUsername = '';
  String _mqttPassword = '';
  bool _settingsLoaded = false;
  int _otaTaskGeneration = 0;
  final Map<String, Map<String, dynamic>> _completedCommands = {};
  static const int _completedCommandLimit = 100;
  static const String _configSnapshotKey = 'inverter.config.snapshot';
  static const String _commandResponsesKey = 'command.completed.responses';
  static const int _trustedEpochFloor = 1577836800; // 2020-01-01 UTC
  String? _activeOtaTaskId;
  String? _activeOtaTarget;
  String? _activeOtaUrl;
  String? _activeOtaSha256;
  int? _activeOtaSize;
  int? _activeOtaSecurityVersion;
  int? _activeOtaTimeout;
  Map<String, dynamic>? _lastOtaStatus;

  // 随机告警控制
  final Map<int, DateTime> _randomAlarmClearTime = {};
  double _alarmProbability = 0.0001;

  // 告警防抖（同一告警码10秒内不重复发送）
  final Map<int, DateTime> _alarmLastSent = {};

  int? _lastPublishedAlarmCode;

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
  bool get isSerialConnected => _serialPort != null && _serialPort!.isOpen;
  bool get isMqttConnected => _mqttService.isConnected;
  bool get isConnected => isSerialConnected || isMqttConnected;
  bool get directMqtt => _directMqtt;
  bool get mqttConnecting => _mqttConnecting;
  String get mqttHost => _mqttHost;
  int get mqttPort => _mqttPort;
  bool get mqttTls => _mqttTls;
  String get mqttUsername => _mqttUsername;
  String get mqttPassword => _mqttPassword;
  bool get settingsLoaded => _settingsLoaded;
  double get alarmProbability => _alarmProbability;
  List<String> get availablePorts => _availablePorts;
  String? get selectedPort => _selectedPort;

  @visibleForTesting
  Map<String, dynamic>? get lastOtaStatus => _lastOtaStatus == null
      ? null
      : Map<String, dynamic>.from(_lastOtaStatus!);

  SimulatorProvider({
    bool discoverSerialPorts = true,
    FlutterSecureStorage? secureStorage,
    this.useSecureStorage = true,
  }) : _secureStorage = secureStorage ?? const FlutterSecureStorage(),
       super() {
    _setupUartListener();
    _startSimulation();
    if (discoverSerialPorts) refreshPorts();
    _settingsFuture = _loadSettings();
  }

  Future<void> initialize() => _settingsFuture;

  Future<void> _loadSettings() async {
    try {
      final prefs = await SharedPreferences.getInstance();
      _directMqtt = prefs.getBool('connection.directMqtt') ?? _directMqtt;
      _sn = prefs.getString('device.sn') ?? _sn;
      _mqttHost = prefs.getString('mqtt.host') ?? _mqttHost;
      _mqttPort = prefs.getInt('mqtt.port') ?? _mqttPort;
      _mqttTls = prefs.getBool('mqtt.tls') ?? _mqttTls;
      _mqttUsername = prefs.getString('mqtt.username') ?? _mqttUsername;
      _selectedPort = prefs.getString('serial.port') ?? _selectedPort;
      if (_selectedPort != null && !_availablePorts.contains(_selectedPort)) {
        _selectedPort = _availablePorts.isNotEmpty ? _availablePorts.first : null;
      }
      _autoSimulate = prefs.getBool('simulation.auto') ?? _autoSimulate;
      _autoPublish = prefs.getBool('publish.auto') ?? _autoPublish;
      _publishInterval =
          prefs.getDouble('publish.interval') ?? _publishInterval;
      _alarmProbability =
          prefs.getDouble('alarm.probability') ?? _alarmProbability;
      _inverterData.firmwareArm =
          prefs.getString('firmware.arm') ?? _inverterData.firmwareArm;
      _inverterData.firmwareDsp =
          prefs.getString('firmware.dsp') ?? _inverterData.firmwareDsp;
      _inverterData.firmwareBms =
          prefs.getString('firmware.bms') ?? _inverterData.firmwareBms;
      final configJson = prefs.getString(_configSnapshotKey);
      if (configJson != null) {
        try {
          final decoded = jsonDecode(configJson);
          if (decoded is Map<String, dynamic>) {
            _inverterData.restoreConfigSnapshot(decoded);
          }
        } catch (_) {
          await prefs.remove(_configSnapshotKey);
        }
      }
      final responsesJson = prefs.getString(_commandResponsesKey);
      if (responsesJson != null) {
        try {
          final decoded = jsonDecode(responsesJson);
          if (decoded is Map) {
            for (final entry in decoded.entries) {
              if (entry.key is String && entry.value is Map) {
                _completedCommands[entry.key as String] =
                    Map<String, dynamic>.from(entry.value as Map);
              }
            }
          }
        } catch (_) {
          await prefs.remove(_commandResponsesKey);
        }
      }
      if (useSecureStorage) {
        try {
          _mqttPassword =
              await _secureStorage.read(key: 'mqtt.password') ?? _mqttPassword;
        } catch (e) {
          _addLog('[WARN] 无法读取安全存储中的 MQTT 密码: $e');
        }
      }
    } finally {
      _settingsLoaded = true;
      _updatePubTimer();
      notifyListeners();
    }
  }

  Future<void> _saveSetting(String key, Object value) async {
    final prefs = await SharedPreferences.getInstance();
    switch (value) {
      case bool value:
        await prefs.setBool(key, value);
      case int value:
        await prefs.setInt(key, value);
      case double value:
        await prefs.setDouble(key, value);
      case String value:
        await prefs.setString(key, value);
    }
  }

  /// 刷新可用串口列表
  void refreshPorts() {
    _availablePorts = SerialPort.availablePorts;
    if (_selectedPort != null && !_availablePorts.contains(_selectedPort)) {
      _selectedPort = null;
    }
    if (_availablePorts.isNotEmpty && _selectedPort == null) {
      _selectedPort = _availablePorts.first;
    }
    _addLog('[SYS] 发现 ${_availablePorts.length} 个串口');
    notifyListeners();
  }

  /// 选择串口
  void setSelectedPort(String? port) {
    _selectedPort = port;
    if (port != null) _saveSetting('serial.port', port);
    notifyListeners();
  }

  void setDirectMqtt(bool value) {
    if (!_settingsLoaded || isConnected || _mqttConnecting) return;
    _directMqtt = value;
    _saveSetting('connection.directMqtt', value);
    notifyListeners();
  }

  void setSn(String value) {
    final normalized = value.trim();
    if (RegExp(r'^[A-Za-z0-9_-]{1,50}$').hasMatch(normalized)) {
      _sn = normalized;
      _saveSetting('device.sn', normalized);
      notifyListeners();
    }
  }

  void setMqttHost(String value) {
    _mqttHost = value.trim();
    _saveSetting('mqtt.host', _mqttHost);
  }

  void setMqttPort(String value) {
    final parsed = int.tryParse(value);
    if (parsed != null && parsed > 0 && parsed <= 65535) {
      _mqttPort = parsed;
      _saveSetting('mqtt.port', parsed);
    }
  }

  void setMqttTls(bool value) {
    _mqttTls = value;
    if (_mqttPort == 1883 && value) _mqttPort = 8883;
    if (_mqttPort == 8883 && !value) _mqttPort = 1883;
    _saveSetting('mqtt.tls', value);
    _saveSetting('mqtt.port', _mqttPort);
    notifyListeners();
  }

  void setMqttUsername(String value) {
    _mqttUsername = value;
    _saveSetting('mqtt.username', value);
  }

  void setMqttPassword(String value) {
    _mqttPassword = value;
    unawaited(_saveMqttPassword(value));
  }

  Future<void> _saveMqttPassword(String value) async {
    if (!useSecureStorage) return;
    try {
      if (value.isEmpty) {
        await _secureStorage.delete(key: 'mqtt.password');
      } else {
        await _secureStorage.write(key: 'mqtt.password', value: value);
      }
    } catch (e) {
      _addLog('[WARN] MQTT 密码安全存储失败: $e');
    }
  }

  void setFirmwareArm(String value) {
    _inverterData.firmwareArm = value;
    _saveSetting('firmware.arm', value);
    notifyListeners();
  }

  void setFirmwareDsp(String value) {
    _inverterData.firmwareDsp = value;
    _saveSetting('firmware.dsp', value);
    notifyListeners();
  }

  void setFirmwareBms(String value) {
    _inverterData.firmwareBms = value;
    _saveSetting('firmware.bms', value);
    notifyListeners();
  }

  Future<void> connectMqtt() async {
    await initialize();
    if (_mqttConnecting || isMqttConnected) return;
    if (!_directMqtt || isSerialConnected) {
      _addLog('[ERR] 当前连接方式不是直连 MQTT');
      return;
    }
    if (_mqttHost.isEmpty) {
      _addLog('[ERR] MQTT Broker 地址不能为空');
      return;
    }
    _mqttConnecting = true;
    notifyListeners();
    try {
      await _mqttService.connect(
        host: _mqttHost,
        port: _mqttPort,
        useTls: _mqttTls,
        serialNumber: _sn,
        username: _mqttUsername,
        password: _mqttPassword,
        onMessage: _handleMqttMessage,
        onLog: _addLog,
        onConnectionChanged: (_) {
          _updatePubTimer();
          notifyListeners();
        },
      );
      _firstPubDone = false;
      _updatePubTimer();
      _mqttService.publishData(_sn, 'status', {
        'online': true,
        'rssi': -45,
      }, retain: true);
      await _publishStartup();
    } catch (e) {
      _addLog('[ERR] MQTT 连接失败: $e');
      await _mqttService.disconnect();
    } finally {
      _mqttConnecting = false;
      notifyListeners();
    }
  }

  Future<void> disconnectMqtt() async {
    await _mqttService.disconnect(serialNumber: _sn);
    _updatePubTimer();
    notifyListeners();
  }

  /// 连接串口
  Future<void> connect() async {
    await initialize();
    if (_directMqtt || _mqttConnecting || isMqttConnected) {
      _addLog('[ERR] 当前连接方式不是串口 ESP32');
      return;
    }
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
      _firstPubDone = false; // 重连后重新发送 info
      _updatePubTimer(); // 启动发布定时器
      await _publishStartup();
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
      _updatePubTimer(); // 停止发布定时器
      _addLog('[SYS] 已断开连接');
      notifyListeners();
    }
  }

  void _setupUartListener() {
    _frameSubscription = _uartService.frameStream.listen(_handleFrame);
    _logSubscription = _uartService.logStream.listen((log) {
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
    _parallelTimer?.cancel();
    if (_autoPublish && isConnected) {
      _pubTimer = Timer.periodic(
        Duration(seconds: _publishInterval.toInt()),
        (_) => _publishTelemetryCycle(),
      );
      _parallelTimer = Timer.periodic(
        const Duration(seconds: 300),
        (_) => _publishParallelTopologyIfRequired(),
      );
    }
  }

  /// 模拟tick
  void _tick(double dt) {
    // 使用基于能量守恒的物理模型
    _inverterData.tick(dt);

    // 检查告警
    final shouldSendRecovery = _alarmSystem.checkAlarms(
      _inverterData,
      _alarmConfig,
    );

    // 更新故障码
    _inverterData.faultCode =
        _alarmSystem.activeAlarms
            .where(
              (code) => AlarmSystem.alarmDefinitions[code]?.level == 'fault',
            )
            .firstOrNull ??
        0;

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

  // _simulateParameters 已替换为 InverterData.tick() 物理模型

  void _checkRandomAlarms() {
    // 检查并清除过期的随机告警
    final now = DateTime.now();
    final expired = _randomAlarmClearTime.entries
        .where(
          (e) =>
              now.isAfter(e.value) && _alarmSystem.activeAlarms.contains(e.key),
        )
        .map((e) => e.key)
        .toList();

    for (var code in expired) {
      _alarmSystem.removeAlarm(code);
      _randomAlarmClearTime.remove(code);
    }

    // 随机触发告警
    if (Random().nextDouble() < _alarmProbability) {
      final codes = [8, 9, 10, 11, 12];
      final code = codes[Random().nextInt(codes.length)];
      if (!_alarmSystem.activeAlarms.contains(code)) {
        _alarmSystem.addAlarm(code);
        _randomAlarmClearTime[code] = now.add(const Duration(seconds: 30));
      }
    }
  }

  Future<void> _publishStartup() async {
    if (!_firstPubDone) {
      _firstPubDone = true;
      await _publishTopic('info');
      await _publishTopic('config');
    }
    await _publishTelemetry();
    await _publishParallelTopologyIfRequired();
    await _publishThreePhaseIfRequired();
  }

  Future<void> _publishTelemetry() => _publishTopic('heartbeat');

  Future<void> _publishTelemetryCycle() async {
    await _publishTelemetry();
    await _publishThreePhaseIfRequired();
  }

  Future<void> _publishParallelTopologyIfRequired() async {
    if (!_inverterData.parallelEnabled || _inverterData.parallelRole != 0) {
      return;
    }
    await _publishTopic('parallel');
  }

  Future<void> _publishThreePhaseIfRequired() async {
    if (_inverterData.parallelMode == 2 && _inverterData.parallelRole == 0) {
      await _publishTopic('three_phase');
    }
  }

  /// 发布单个主题，发送后等待ESP32回复ACK
  Future<void> _publishTopic(String topic) async {
    if (_disposed) return;
    Map<String, dynamic> payload = _inverterData.toJson(topic);

    if (topic == 'parallel' && payload['machines'] is List) {
      final machines = payload['machines'] as List<dynamic>;
      if (machines.isNotEmpty && machines.first is Map<String, dynamic>) {
        (machines.first as Map<String, dynamic>)['sn'] = _sn;
      }
    }

    if (topic == 'alarm') {
      final code = _alarmSystem.activeAlarms.firstOrNull;
      if (code == null) return;
      final definition = AlarmSystem.alarmDefinitions[code];
      _lastPublishedAlarmCode = code;
      payload = {
        'source': 0,
        'code': code,
        'level': definition?.level == 'fault' ? 2 : 1,
        'state': 1,
      };
    }

    if (payload.isEmpty) {
      _addLog('[WARN] 跳过空 payload: $topic');
      return;
    }

    final data = TelemetryProtocol.publishPayload(
      serialNumber: _sn,
      topic: topic,
      data: payload,
    );

    if (isMqttConnected) {
      try {
        _mqttService.publishData(
          _sn,
          topic,
          payload,
          qos: topic == 'three_phase'
              ? MqttQos.atMostOnce
              : MqttQos.atLeastOnce,
        );
        _addLog('[MQTT TX] $topic');
      } catch (e) {
        _addLog('[ERR] MQTT 发布失败: $e');
      }
      return;
    }

    // 创建ACK等待器
    _ackCompleter = Completer<void>();

    await _uartService.sendFrame(
      CommandCodes.publish,
      Uint8List.fromList(data),
    );
    _addLog('[TX] PUBLISH $topic');

    // 等待ACK，超时500ms后继续
    try {
      await _ackCompleter!.future.timeout(const Duration(milliseconds: 500));
    } on TimeoutException {
      _addLog('[WARN] ACK timeout for $topic');
    }
    _ackCompleter = null;
  }

  /// 发布告警恢复消息（checkAlarms 检测到状态转换时调用）
  Future<void> _publishRecovery() async {
    final code = _lastPublishedAlarmCode;
    if (code == null) return;
    final payload = {
      'source': 0,
      'code': code,
      'level': AlarmSystem.alarmDefinitions[code]?.level == 'fault' ? 2 : 1,
      'state': 0,
    };
    if (isMqttConnected) {
      try {
        _mqttService.publishData(_sn, 'alarm', payload);
        _addLog('[MQTT TX] alarm (recovery)');
      } catch (e) {
        _addLog('[ERR] 告警恢复发布失败: $e');
      }
      return;
    }
    final data = TelemetryProtocol.publishPayload(
      serialNumber: _sn,
      topic: 'alarm',
      data: payload,
    );

    _ackCompleter = Completer<void>();

    await _uartService.sendFrame(
      CommandCodes.publish,
      Uint8List.fromList(data),
    );
    _addLog('[TX] PUBLISH alarm (recovery)');

    try {
      await _ackCompleter!.future.timeout(const Duration(milliseconds: 500));
    } on TimeoutException {
      _addLog('[WARN] ACK timeout for alarm (recovery)');
    }
    _ackCompleter = null;
  }

  /// 手动触发一次发布
  Future<void> publishNow() async {
    if (!isConnected) {
      _addLog('[WARN] 未连接，无法发布');
      return;
    }
    await _publishTelemetryCycle();
    _addLog('[SYS] 手动发布完成');
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
        unawaited(_handleCommand(frame.data));
        break;
      case CommandCodes.setBroker:
        _uartService.sendAck();
        break;
      case CommandCodes.setSn:
        _handleSetSn(frame.data);
        break;
      case CommandCodes.factoryReset:
        unawaited(_handleFactoryReset());
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

  void _handleMqttMessage(String topic, String payload) {
    if (topic.endsWith('/ota/cmd')) {
      unawaited(_handleMqttOtaCommand(payload));
      return;
    }
    unawaited(_handleCommand(Uint8List.fromList(utf8.encode(payload))));
  }

  @visibleForTesting
  Future<void> handleMqttMessageForTest(String topic, String payload) async {
    if (topic.endsWith('/ota/cmd')) {
      await _handleMqttOtaCommand(payload);
    } else {
      await _handleCommand(Uint8List.fromList(utf8.encode(payload)));
    }
  }

  Future<void> _handleMqttOtaCommand(String payload) async {
    try {
      final decoded = jsonDecode(payload);
      if (decoded is! Map<String, dynamic>) {
        throw const FormatException('OTA payload 必须是 JSON object');
      }
      final taskId = decoded['task_id'] is String
          ? decoded['task_id'] as String
          : '';
      final action = decoded['action'] is String
          ? decoded['action'] as String
          : '';
      final target = decoded['target'] is String
          ? decoded['target'] as String
          : '';
      final version = decoded['version'] is String
          ? decoded['version'] as String
          : '';
      final timestamp = decoded['t'];
      if (decoded['v'] != TelemetryProtocol.version ||
          timestamp is! int ||
          timestamp <= 0 ||
          !_validTaskId(taskId) ||
          !{'start', 'cancel', 'query'}.contains(action) ||
          _isExpired(timestamp)) {
        _publishOtaStatus(
          taskId,
          target,
          'failed',
          0,
          version: version,
          code: 'INVALID_ARGS',
          message: 'v=1、正整数 t、task_id 和合法 action 必填',
        );
        return;
      }
      if (action == 'cancel') {
        if (_activeOtaTaskId == null || taskId != _activeOtaTaskId) {
          _publishOtaStatus(
            taskId,
            target,
            'failed',
            0,
            version: version,
            code: 'TASK_MISMATCH',
            message: 'cancel task_id 必须匹配活动 OTA 任务',
          );
          return;
        }
        _otaTaskGeneration++;
        _otaState = 'cancelled';
        _publishOtaStatus(
          taskId,
          _activeOtaTarget!,
          'cancelled',
          _otaProgress,
          version: _otaVersion,
        );
        _clearActiveOtaTask();
        return;
      }
      if (action == 'query') {
        if (_activeOtaTaskId == null) {
          _publishOtaStatus(
            taskId,
            target,
            'failed',
            0,
            version: version,
            code: 'NOT_FOUND',
            message: '没有活动 OTA 任务',
          );
          return;
        }
        _publishOtaStatus(
          taskId,
          _activeOtaTarget!,
          _otaState,
          _otaProgress,
          version: _otaVersion,
          activeTaskId: _activeOtaTaskId,
        );
        return;
      }

      final url = decoded['url'];
      final size = decoded['size'];
      final sha256Value = decoded['sha256'];
      final securityVersion = decoded['security_version'];
      final signature = decoded['signature'];
      final timeout = decoded['timeout'];
      final uri = url is String ? Uri.tryParse(url) : null;
      if (!{'arm', 'esp'}.contains(target) ||
          version.isEmpty ||
          uri == null ||
          uri.scheme != 'https' ||
          uri.host.isEmpty ||
          size is! int ||
          size <= 0 ||
          sha256Value is! String ||
          !RegExp(r'^[0-9a-fA-F]{64}$').hasMatch(sha256Value) ||
          securityVersion is! int ||
          securityVersion < 0 ||
          signature is! String ||
          !_isValidBase64(signature) ||
          timeout is! int ||
          timeout <= 0 ||
          timeout > 86400) {
        _publishOtaStatus(
          taskId,
          target,
          'failed',
          0,
          version: version,
          code: 'INVALID_ARGS',
          message: 'OTA V1 start 字段无效',
        );
        return;
      }
      if (_activeOtaTaskId != null) {
        _publishOtaStatus(
          taskId,
          target,
          'failed',
          0,
          version: version,
          code: 'BUSY',
          message: '已有活动 OTA 任务',
        );
        return;
      }

      final generation = ++_otaTaskGeneration;
      _activeOtaTaskId = taskId;
      _activeOtaTarget = target;
      _activeOtaUrl = url;
      _activeOtaSize = size;
      _activeOtaSha256 = sha256Value.toLowerCase();
      _activeOtaSecurityVersion = securityVersion;
      _activeOtaTimeout = timeout;
      _otaVersion = version;
      _otaState = 'accepted';
      _publishOtaStatus(
        taskId,
        target,
        'accepted',
        0,
        version: version,
        message: '仅完成格式与任务关联校验；模拟器未执行真实签名验证',
      );
      Future<void>.delayed(const Duration(milliseconds: 150), () {
        if (_disposed || generation != _otaTaskGeneration) return;
        _otaState = 'installing';
        _publishOtaStatus(taskId, target, 'installing', 80, version: version);
      });
      Future<void>.delayed(const Duration(milliseconds: 300), () {
        if (_disposed || generation != _otaTaskGeneration) return;
        _otaState = 'succeeded';
        _publishOtaStatus(taskId, target, 'succeeded', 100, version: version);
        _clearActiveOtaTask();
      });
    } catch (e) {
      _addLog('[ERR] OTA 命令解析失败: $e');
    }
  }

  int get _otaProgress => _otaExpectedSize > 0
      ? (_otaReceivedBytes * 100 ~/ _otaExpectedSize).clamp(0, 100)
      : switch (_otaState) {
          'installing' => 80,
          'succeeded' => 100,
          _ => 0,
        };

  bool _isValidBase64(String value) {
    if (value.isEmpty) return false;
    try {
      base64Decode(value);
      return true;
    } on FormatException {
      return false;
    }
  }

  void _clearActiveOtaTask() {
    _activeOtaTaskId = null;
    _activeOtaTarget = null;
    _activeOtaUrl = null;
    _activeOtaSize = null;
    _activeOtaSha256 = null;
    _activeOtaSecurityVersion = null;
    _activeOtaTimeout = null;
  }

  void _publishOtaStatus(
    String taskId,
    String target,
    String state,
    int progress, {
    required String version,
    String code = 'OK',
    String message = '',
    String? activeTaskId,
  }) {
    final status = <String, dynamic>{
      'task_id': taskId,
      'target': target,
      'state': state,
      'progress': progress,
      'version': version,
      'code': code,
      'message': message,
      if (activeTaskId != null) 'active_task_id': activeTaskId,
      if (activeTaskId != null) 'active_url': _activeOtaUrl,
      if (activeTaskId != null) 'active_size': _activeOtaSize,
      if (activeTaskId != null) 'active_sha256': _activeOtaSha256,
      if (activeTaskId != null)
        'active_security_version': _activeOtaSecurityVersion,
      if (activeTaskId != null) 'active_timeout': _activeOtaTimeout,
    };
    _lastOtaStatus = Map<String, dynamic>.from(status);
    if (!isMqttConnected) return;
    try {
      _mqttService.publishData(_sn, 'ota/status', status);
      _addLog('[MQTT TX] ota/status: $state $progress%');
    } catch (e) {
      _addLog('[ERR] OTA 状态发布失败: $e');
    }
  }

  Future<void> _handleCommand(Uint8List data) async {
    try {
      final decoded = jsonDecode(utf8.decode(data));
      if (decoded is! Map<String, dynamic>) {
        throw const FormatException('命令必须是 JSON object');
      }
      final json = decoded;
      final cmd = json['cmd'] is String ? json['cmd'] as String : '';
      final params = json['params'] is Map
          ? (json['params'] as Map).cast<String, dynamic>()
          : <String, dynamic>{};
      final args = json['args'] is List
          ? List<dynamic>.from(json['args'] as List)
          : <dynamic>[];
      final taskId = json['task_id'] is String ? json['task_id'] as String : '';
      final timestamp = json['t'];

      if (json['v'] != TelemetryProtocol.version ||
          timestamp is! int ||
          timestamp <= 0 ||
          !_validTaskId(taskId) ||
          cmd.isEmpty ||
          _isExpired(timestamp)) {
        _sendCommandTransportNack();
        await _sendCmdResult(
          taskId,
          cmd,
          false,
          'v=1、正整数 t、cmd 和 task_id 必填，且命令不得过期',
          null,
          _isExpired(timestamp is int ? timestamp : 0)
              ? 'EXPIRED'
              : 'INVALID_ARGS',
        );
        return;
      }

      final previous = _completedCommands[taskId];
      if (previous != null) {
        _sendCommandTransportAck();
        _sendCommandPayload(previous);
        return;
      }

      _addLog('[RX] CMD: $cmd');
      if (isMqttConnected) {
        _sendMqttCommandStage(
          taskId: taskId,
          cmd: cmd,
          stage: 'acknowledged',
          success: true,
          code: 'OK',
        );
      }

      switch (cmd) {
        case 'ac_on':
          if (_alarmSystem.activeAlarms.isNotEmpty) {
            await _completeCommand(
              taskId,
              cmd,
              success: false,
              code: 'INTERLOCKED',
              message: '存在活动故障或告警',
            );
            break;
          }
          _inverterData.acEnabled = 1;
          _inverterData.mode = InverterMode.inverting;
          await _completeConfigCommand(taskId, cmd);
          break;
        case 'ac_off':
          _inverterData.acEnabled = 0;
          _inverterData.mode = InverterMode.shutdown;
          await _completeConfigCommand(taskId, cmd);
          break;
        case 'restart':
          await _handleReset(taskId, command: cmd);
          break;
        case 'set_power_limit':
          await _setRangedConfig(
            taskId,
            cmd,
            args,
            0,
            _inverterData.ratedPower,
            (value) => _inverterData.powerLimit = value,
          );
          break;
        case 'set_charge_limit':
          await _setRangedConfig(
            taskId,
            cmd,
            args,
            0,
            _inverterData.ratedPower,
            (value) => _inverterData.chargeLimit = value,
          );
          break;
        case 'set_discharge_limit':
          await _setRangedConfig(
            taskId,
            cmd,
            args,
            0,
            _inverterData.ratedPower,
            (value) => _inverterData.dischargeLimit = value,
          );
          break;
        case 'set_soc_window':
          final low = _intArg(args, 0);
          final high = _intArg(args, 1);
          if (low == null || high == null) {
            await _invalidArgs(taskId, cmd);
          } else if (low < 0 || high > 1000 || low >= high) {
            await _outOfRange(taskId, cmd);
          } else {
            _inverterData.socLowX10 = low;
            _inverterData.socHighX10 = high;
            await _completeConfigCommand(taskId, cmd);
          }
          break;
        case 'force_charge':
        case 'force_discharge':
          await _handleForceMode(taskId, cmd, args);
          break;
        case 'bms_set_enable':
          final charge = _intArg(args, 0);
          final discharge = _intArg(args, 1);
          if (charge == null || discharge == null) {
            await _invalidArgs(taskId, cmd);
          } else if (![0, 1].contains(charge) || ![0, 1].contains(discharge)) {
            await _outOfRange(taskId, cmd);
          } else {
            _inverterData.bmsChargeEnabled = charge;
            _inverterData.bmsDischargeEnabled = discharge;
            await _completeConfigCommand(taskId, cmd);
          }
          break;
        case 'bms_set_charge_current':
          await _setRangedConfig(
            taskId,
            cmd,
            args,
            0,
            (_inverterData.maxChargeCurrent * 10).round(),
            (value) => _inverterData.bmsMaxChargeCurrentX10 = value,
          );
          break;
        case 'bms_set_discharge_current':
          await _setRangedConfig(
            taskId,
            cmd,
            args,
            0,
            (_inverterData.maxDischargeCurrent * 10).round(),
            (value) => _inverterData.bmsMaxDischargeCurrentX10 = value,
          );
          break;
        case 'bms_set_charge_voltage':
          await _setRangedConfig(
            taskId,
            cmd,
            args,
            544,
            584,
            (value) => _inverterData.bmsChargeVoltageX10 = value,
          );
          break;
        case 'bms_set_discharge_voltage':
          await _setRangedConfig(
            taskId,
            cmd,
            args,
            400,
            480,
            (value) => _inverterData.bmsDischargeVoltageX10 = value,
          );
          break;
        case 'parallel_enable':
          final mode = _intArg(args, 0);
          if (mode == null) {
            await _invalidArgs(taskId, cmd);
          } else if (mode != 1 && mode != 2) {
            await _outOfRange(taskId, cmd);
          } else {
            _inverterData.parallelMode = mode;
            _inverterData.parallelRole ??= 0;
            await _completeConfigCommand(taskId, cmd, publishParallel: true);
          }
          break;
        case 'parallel_set_role':
          final role = _intArg(args, 0);
          final machineId = _intArg(args, 1);
          if (role == null || machineId == null) {
            await _invalidArgs(taskId, cmd);
          } else if (![0, 1].contains(role) || machineId < 0 || machineId > 7) {
            await _outOfRange(taskId, cmd);
          } else if (!_inverterData.parallelEnabled) {
            await _completeCommand(
              taskId,
              cmd,
              success: false,
              code: 'INTERLOCKED',
              message: '尚未启用并机',
            );
          } else {
            _inverterData.parallelRole = role;
            await _completeConfigCommand(taskId, cmd, publishParallel: true);
          }
          break;
        case 'parallel_set_phase':
          final phase = _intArg(args, 0);
          if (phase == null) {
            await _invalidArgs(taskId, cmd);
          } else if (phase < 1 || phase > 3) {
            await _outOfRange(taskId, cmd);
          } else if (_inverterData.parallelMode != 2) {
            await _completeCommand(
              taskId,
              cmd,
              success: false,
              code: 'INTERLOCKED',
              message: '仅三相模式允许设置相位',
            );
          } else {
            _inverterData.parallelPhase = phase;
            await _completeConfigCommand(taskId, cmd, publishParallel: true);
          }
          break;
        case 'parallel_disable':
          _inverterData.parallelMode = 0;
          _inverterData.parallelRole = null;
          _inverterData.parallelPhase = null;
          await _completeConfigCommand(taskId, cmd, publishParallel: true);
          break;
        case 'parallel_sync_start':
        case 'parallel_sync_stop':
          if (!_inverterData.parallelEnabled ||
              _inverterData.parallelRole != 0) {
            await _completeCommand(
              taskId,
              cmd,
              success: false,
              code: 'INTERLOCKED',
              message: '仅并机主机允许同步操作',
            );
          } else {
            await _completeCommand(taskId, cmd);
            unawaited(_publishTopic('parallel'));
          }
          break;
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
          await _handleReset(taskId);
          break;
        case 'get_params':
          _handleGetParams(taskId);
          break;
        case 'set_params':
          await _handleSetParams(params, taskId);
          break;
        case 'query_telemetry':
          _sendCommandTransportAck();
          await _sendCmdResult(taskId, cmd, true, '');
          _publishTopic('heartbeat');
          break;
        case 'query_info':
          _sendCommandTransportAck();
          await _sendCmdResult(taskId, cmd, true, '');
          _publishTopic('info');
          break;
        case 'query_config':
          _sendCommandTransportAck();
          await _sendCmdResult(taskId, cmd, true, '');
          _publishTopic('config');
          break;
        default:
          _addLog('[WARN] 未知命令: $cmd');
          await _completeCommand(
            taskId,
            cmd,
            success: false,
            code: 'NOT_SUPPORTED',
            message: '未知命令: $cmd',
          );
      }
    } catch (e) {
      _addLog('[ERR] 命令解析失败: $e');
      _sendCommandTransportNack();
    }
  }

  int? _intArg(List<dynamic> args, int index) {
    if (index >= args.length) return null;
    final value = args[index];
    if (value is int) return value;
    if (value is num && value.isFinite && value == value.roundToDouble()) {
      return value.toInt();
    }
    return null;
  }

  bool _validTaskId(String value) =>
      RegExp(r'^[A-Za-z0-9._:-]{1,128}$').hasMatch(value);

  bool _isExpired(int timestamp) {
    if (timestamp <= 0) return false;
    final now = DateTime.now().toUtc().millisecondsSinceEpoch ~/ 1000;
    return now >= _trustedEpochFloor && now - timestamp > 300;
  }

  Future<void> _saveInverterConfig() async {
    final prefs = await SharedPreferences.getInstance();
    final saved = await prefs.setString(
      _configSnapshotKey,
      jsonEncode(_inverterData.configSnapshot()),
    );
    if (!saved) throw StateError('保存逆变器配置快照失败');
  }

  Future<void> _saveCompletedCommands() async {
    final prefs = await SharedPreferences.getInstance();
    final saved = await prefs.setString(
      _commandResponsesKey,
      jsonEncode(_completedCommands),
    );
    if (!saved) throw StateError('保存命令幂等响应失败');
  }

  Future<void> _setRangedConfig(
    String taskId,
    String cmd,
    List<dynamic> args,
    int min,
    int max,
    ValueChanged<int> apply,
  ) async {
    final value = _intArg(args, 0);
    if (value == null) {
      await _invalidArgs(taskId, cmd);
    } else if (value < min || value > max) {
      await _outOfRange(taskId, cmd);
    } else {
      apply(value);
      await _completeConfigCommand(taskId, cmd);
    }
  }

  Future<void> _handleForceMode(
    String taskId,
    String cmd,
    List<dynamic> args,
  ) async {
    final enable = _intArg(args, 0);
    final timeout = _intArg(args, 1);
    if (enable == null || timeout == null) {
      await _invalidArgs(taskId, cmd);
      return;
    }
    if (![0, 1].contains(enable) || timeout < 60 || timeout > 86400) {
      await _outOfRange(taskId, cmd);
      return;
    }
    final charging = cmd == 'force_charge';
    if (enable == 1 &&
        (charging
            ? _inverterData.forceDischarge == 1
            : _inverterData.forceCharge == 1)) {
      await _completeCommand(
        taskId,
        cmd,
        success: false,
        code: 'INTERLOCKED',
        message: '强制充电和强制放电互斥',
      );
      return;
    }
    final timer = Timer(
      Duration(seconds: timeout),
      () => unawaited(_expireForcedMode(charging)),
    );
    if (charging) {
      _forceChargeTimer?.cancel();
      _forceChargeTimer = enable == 1 ? timer : null;
      _inverterData.forceCharge = enable;
    } else {
      _forceDischargeTimer?.cancel();
      _forceDischargeTimer = enable == 1 ? timer : null;
      _inverterData.forceDischarge = enable;
    }
    if (enable == 0) timer.cancel();
    await _completeConfigCommand(taskId, cmd);
  }

  Future<void> _expireForcedMode(bool charging) async {
    if (charging) {
      _inverterData.forceCharge = 0;
      _forceChargeTimer = null;
    } else {
      _inverterData.forceDischarge = 0;
      _forceDischargeTimer = null;
    }
    _inverterData.markConfigChanged();
    await _saveInverterConfig();
    await _publishTopic('config');
    notifyListeners();
  }

  Future<void> _invalidArgs(String taskId, String cmd) async {
    await _completeCommand(
      taskId,
      cmd,
      success: false,
      code: 'INVALID_ARGS',
      message: '参数数量或类型错误',
    );
  }

  Future<void> _outOfRange(String taskId, String cmd) async {
    await _completeCommand(
      taskId,
      cmd,
      success: false,
      code: 'OUT_OF_RANGE',
      message: '参数超出允许范围',
    );
  }

  Future<void> _completeConfigCommand(
    String taskId,
    String cmd, {
    bool publishParallel = false,
  }) async {
    _inverterData.markConfigChanged();
    await _saveInverterConfig();
    await _completeCommand(taskId, cmd);
    unawaited(_publishTopic('config'));
    if (publishParallel) unawaited(_publishTopic('parallel'));
    notifyListeners();
  }

  Future<void> _completeCommand(
    String taskId,
    String cmd, {
    bool success = true,
    String code = 'OK',
    String message = '',
  }) async {
    _sendCommandTransportAck();
    await _sendCmdResult(taskId, cmd, success, message, null, code);
  }

  void _handleSetControl(Map<String, dynamic> params, String taskId) {
    _sendCommandTransportAck();
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
        _sendCommandTransportAck();
        _sendCmdResult(taskId, 'set_alarm', true, '告警 $code 在防抖期内');
        return;
      }

      _alarmSystem.addAlarm(code, manual: true);
      _syncAlarmCodes();
      _alarmLastSent[code] = now;
      _recoverySent = false; // 新告警，重置恢复标志
      _addLog('[ALARM] 远程触发告警: $code');
      _sendCommandTransportAck();
      _sendCmdResult(taskId, 'set_alarm', true, '已触发告警 $code');
      _publishTopic('alarm');
    } else {
      _alarmSystem.clearAllAlarms();
      _syncAlarmCodes();
      _addLog('[ALARM] 远程清除所有告警');
      _sendCommandTransportAck();
      _sendCmdResult(taskId, 'set_alarm', true, '已清除所有告警');
      // 恢复消息由 _tick 中 checkAlarms 统一发送
    }
  }

  void _handleSetSimParam(Map<String, dynamic> params, String taskId) {
    _sendCommandTransportAck();
    _sendCmdResult(taskId, 'set_sim_param', true, '模拟参数已设置');
  }

  Future<void> _handleReset(String taskId, {String command = 'reset'}) async {
    _inverterData.soc = 75.0;
    _alarmSystem.clearAllAlarms();
    _syncAlarmCodes();
    _inverterData.mode = InverterMode.inverting;
    _addLog('[SYS] 远程重置逆变器');
    _sendCommandTransportAck();
    await _sendCmdResult(taskId, command, true, '逆变器已重置');
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
    _sendCommandTransportAck();
    _sendCmdResult(taskId, 'get_params', true, '参数已获取', data);
  }

  Future<void> _handleSetParams(
    Map<String, dynamic> params,
    String taskId,
  ) async {
    // 应用远程控制参数
    _addLog('[RX] set_params: $params');

    // 处理充电控制等参数
    if (params.containsKey('charge_control')) {
      _inverterData.bmsChargeEnabled = params['charge_control'] == true ? 1 : 0;
    }
    if (params.containsKey('discharge_control')) {
      _inverterData.bmsDischargeEnabled = params['discharge_control'] == true
          ? 1
          : 0;
    }
    if (params.containsKey('power_limit')) {
      final value = params['power_limit'];
      if (value is num) _inverterData.powerLimit = value.toInt();
    }
    if (params.containsKey('max_charge_current')) {
      final value = params['max_charge_current'];
      if (value is num) {
        _inverterData.bmsMaxChargeCurrentX10 = (value * 10).round();
      }
    }
    if (params.containsKey('max_discharge_current')) {
      final value = params['max_discharge_current'];
      if (value is num) {
        _inverterData.bmsMaxDischargeCurrentX10 = (value * 10).round();
      }
    }

    _inverterData.markConfigChanged();
    await _saveInverterConfig();
    _sendCommandTransportAck();
    await _sendCmdResult(taskId, 'set_params', true, '参数已设置');
    unawaited(_publishTopic('config'));
  }

  void _handleSetSn(Uint8List data) {
    try {
      final sn = utf8.decode(data);
      final normalized = sn.trim();
      if (!RegExp(r'^[A-Za-z0-9_-]{1,50}$').hasMatch(normalized)) {
        _uartService.sendNack();
        return;
      }
      _sn = normalized;
      unawaited(_saveSetting('device.sn', normalized));
      _addLog('[SYS] SN设置为: $normalized');
      _uartService.sendAck();
    } catch (e) {
      _uartService.sendNack();
    }
  }

  Future<void> _handleFactoryReset() async {
    try {
      final prefs = await SharedPreferences.getInstance();
      final prefsCleared = await prefs.clear();
      if (!prefsCleared) throw StateError('清除 SharedPreferences 失败');
      if (useSecureStorage) {
        await _secureStorage.delete(key: 'mqtt.password');
      }
      _forceChargeTimer?.cancel();
      _forceChargeTimer = null;
      _forceDischargeTimer?.cancel();
      _forceDischargeTimer = null;
      _parallelTimer?.cancel();
      _parallelTimer = null;
      _completedCommands.clear();
      _otaTaskGeneration++;
      _clearActiveOtaTask();
      _resetOtaState();
      _directMqtt = false;
      _sn = 'H1CNA00135000014';
      _mqttHost = '127.0.0.1';
      _mqttPort = 1883;
      _mqttTls = false;
      _mqttUsername = '';
      _mqttPassword = '';
      _selectedPort = null;
      _autoSimulate = true;
      _autoPublish = true;
      _publishInterval = 180.0;
      _alarmProbability = 0.0001;
      _inverterData = InverterData();
      _alarmSystem.clearAllAlarms();
      _alarmLastSent.clear();
      _randomAlarmClearTime.clear();
      _lastPublishedAlarmCode = null;
      _recoverySent = false;
      _updatePubTimer();
      _addLog('[SYS] 已恢复出厂设置并清除持久化配置');
      await _uartService.sendAck();
      notifyListeners();
    } catch (e) {
      _addLog('[ERR] 恢复出厂设置部分失败，未确认完成: $e');
      await _uartService.sendNack();
    }
  }

  void _handleOtaInfoRequest() {
    // 二进制格式: [version_len(1B)] [version_string(N)] [fw_size(4B,LE)] [md5(16B)]
    final version = 'V1.2.3.20240510';
    final versionBytes = utf8.encode(version);
    final buf = BytesBuilder();
    buf.addByte(versionBytes.length);
    buf.add(versionBytes);
    // fw_size: 4 bytes LE (当前固件大小，若无则为0)
    final fwSize = _otaFirmwareBuffer.isNotEmpty
        ? _otaFirmwareBuffer.length
        : 0;
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
    _otaExpectedSize =
        data[0] | (data[1] << 8) | (data[2] << 16) | (data[3] << 24);

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

    _addLog(
      '[OTA] START: size=$_otaExpectedSize, packets=$_otaTotalPackets, '
      'md5=${_otaExpectedMd5.map((b) => b.toRadixString(16).padLeft(2, '0')).join()}, '
      'version=$_otaVersion',
    );

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
      _addLog(
        '[OTA] DATA[$packetIndex/$_otaTotalPackets] '
        '${firmwareData.length}B, 累计 $_otaReceivedBytes/$_otaExpectedSize ($progress%)',
      );
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
    _addLog(
      '[OTA] 大小校验: ${sizeOk ? "通过" : "失败"} '
      '(期望=$expectedSize, 实际=$_otaReceivedBytes)',
    );

    // 校验2: 包数量 (使用 END 帧中的 packets 或计算值)
    final expectedPackets = endPackets > 0 ? endPackets : _otaTotalPackets;
    final packetsOk = _otaNextPacketIndex == expectedPackets;
    _addLog(
      '[OTA] 包数校验: ${packetsOk ? "通过" : "失败"} '
      '(期望=$expectedPackets, 实际=$_otaNextPacketIndex)',
    );

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
      _addLog(
        '[OTA] 固件版本: $_otaVersion, 大小: $_otaReceivedBytes 字节, '
        '包数: $_otaNextPacketIndex',
      );
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
      if (_disposed) return;
      _otaState = 'idle';
      _otaFirmwareBuffer = [];
      _otaExpectedMd5 = [];
      _otaExpectedSize = 0;
      _otaReceivedBytes = 0;
      _otaTotalPackets = 0;
      _otaNextPacketIndex = 0;
    });
  }

  void _sendCommandTransportAck() {
    if (!isMqttConnected) _uartService.sendAck();
  }

  void _sendCommandTransportNack() {
    if (!isMqttConnected) _uartService.sendNack();
  }

  Future<void> _sendCmdResult(
    String taskId,
    String cmd,
    bool success,
    String message, [
    Map<String, dynamic>? data,
    String? responseCode,
  ]) async {
    final result = {
      'task_id': taskId,
      'cmd': cmd,
      'stage': 'completed',
      'success': success,
      'code': responseCode ?? (success ? 'OK' : 'EXEC_FAILED'),
      'message': message,
      'result': data == null ? <dynamic>[] : [data],
    };
    if (taskId.isNotEmpty) {
      _completedCommands[taskId] = Map<String, dynamic>.from(result);
      if (_completedCommands.length > _completedCommandLimit) {
        _completedCommands.remove(_completedCommands.keys.first);
      }
      await _saveCompletedCommands();
    }
    _sendCommandPayload(result);
  }

  void _sendCommandPayload(Map<String, dynamic> result) {
    final cmd = result['cmd'] as String? ?? '';
    if (isMqttConnected) {
      try {
        _mqttService.publishData(_sn, 'cmd/response', result);
        _addLog('[MQTT TX] cmd/response: $cmd');
      } catch (e) {
        _addLog('[ERR] 命令回复发布失败: $e');
      }
      return;
    }
    final encoded = utf8.encode(jsonEncode(result));
    _uartService.sendFrame(
      CommandCodes.commandSend,
      Uint8List.fromList(encoded),
    );
    _addLog('[TX] CMD_RESULT: $cmd ${result['success'] == true ? "成功" : "失败"}');
  }

  void _sendMqttCommandStage({
    required String taskId,
    required String cmd,
    required String stage,
    required bool success,
    required String code,
    String message = '',
    List<dynamic> result = const [],
  }) {
    try {
      _mqttService.publishData(_sn, 'cmd/response', {
        'task_id': taskId,
        'cmd': cmd,
        'stage': stage,
        'success': success,
        'code': code,
        'message': message,
        'result': result,
      });
      _addLog('[MQTT TX] cmd/response: $cmd $stage');
    } catch (e) {
      _addLog('[ERR] 命令阶段发布失败: $e');
    }
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
    _recoverySent = false; // 新告警，重置恢复标志
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
    _inverterData.faultCode =
        _alarmSystem.activeAlarms
            .where(
              (code) => AlarmSystem.alarmDefinitions[code]?.level == 'fault',
            )
            .firstOrNull ??
        0;
    _inverterData.alarmCode = _alarmSystem.activeAlarms.firstOrNull ?? 0;
  }

  /// 更新UI
  void updateUI() {
    notifyListeners();
  }

  /// 设置模拟参数
  void setAutoSimulate(bool value) {
    _autoSimulate = value;
    _saveSetting('simulation.auto', value);
    notifyListeners();
  }

  void setAutoPublish(bool value) {
    _autoPublish = value;
    _saveSetting('publish.auto', value);
    _updatePubTimer();
    notifyListeners();
  }

  void setPublishInterval(double seconds) {
    _publishInterval = seconds;
    _saveSetting('publish.interval', seconds);
    _updatePubTimer();
    notifyListeners();
  }

  void setAlarmProbability(double value) {
    _alarmProbability = value;
    _saveSetting('alarm.probability', value);
    notifyListeners();
  }

  void clearLogs() {
    _logs.clear();
    notifyListeners();
  }

  void _addLog(String message) {
    if (_disposed) return;
    final now = DateTime.now();
    final timeStr =
        '${now.hour.toString().padLeft(2, '0')}:'
        '${now.minute.toString().padLeft(2, '0')}:'
        '${now.second.toString().padLeft(2, '0')}';
    _logs.insert(0, '[$timeStr] $message');
    if (_logs.length > maxLogCount) {
      _logs.removeLast();
    }
    notifyListeners();
  }

  @override
  void notifyListeners() {
    if (!_disposed) super.notifyListeners();
  }

  @override
  void dispose() {
    _disposed = true;
    _simTimer?.cancel();
    _pubTimer?.cancel();
    _parallelTimer?.cancel();
    _forceChargeTimer?.cancel();
    _forceDischargeTimer?.cancel();
    unawaited(_frameSubscription?.cancel());
    unawaited(_logSubscription?.cancel());
    _mqttService.disconnect();
    _uartService.dispose();
    super.dispose();
  }
}
