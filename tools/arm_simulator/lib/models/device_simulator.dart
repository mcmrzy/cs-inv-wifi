import 'dart:async';
import 'dart:math';

import '../models/inverter_state.dart';
import '../services/mqtt_service.dart';

/// 单设备模拟器，负责模拟一台逆变器的数据生成和 MQTT 发布
class DeviceSimulator {
  final String sn;
  final MqttService _mqttService = MqttService();
  InverterData _inverterData = InverterData();
  Timer? _simTimer;
  Timer? _pubTimer;
  bool _autoSimulate = true;
  bool _autoPublish = true;
  double _publishInterval = 180.0; // 3分钟
  bool _disposed = false;
  final void Function(String sn, String message)? onLog;
  final void Function(String sn, bool connected)? onConnectionChanged;

  // 随机数生成器，使用固定种子确保每个设备行为一致但不同
  late final Random _random;

  DeviceSimulator({
    required this.sn,
    this.onLog,
    this.onConnectionChanged,
  }) {
    // 使用SN的哈希作为随机种子，确保每个设备行为不同但可重复
    _random = Random(sn.hashCode);
    _startSimulation();
  }

  bool get isConnected => _mqttService.isConnected;
  bool get autoSimulate => _autoSimulate;
  bool get autoPublish => _autoPublish;
  double get publishInterval => _publishInterval;
  InverterData get inverterData => _inverterData;

  /// 连接到 MQTT 服务器
  Future<void> connectMqtt({
    required String host,
    required int port,
    required bool useTls,
    required String username,
    required String password,
    bool autoReconnect = true,
  }) async {
    if (_disposed) return;
    
    try {
      await _mqttService.connect(
        host: host,
        port: port,
        useTls: useTls,
        serialNumber: sn,
        username: username,
        password: password,
        autoReconnect: autoReconnect,
        onMessage: _handleMqttMessage,
        onLog: (msg) => onLog?.call(sn, msg),
        onConnectionChanged: (connected) {
          onConnectionChanged?.call(sn, connected);
          _updatePubTimer();
        },
      );
      
      // 发送上线状态
      _mqttService.publishData(sn, 'status', {
        'online': true,
        'rssi': -45,
      }, retain: true);
      
      // 发送初始数据
      await _publishStartup();
    } catch (e) {
      onLog?.call(sn, '[ERR] MQTT 连接失败: $e');
      rethrow;
    }
  }

  /// 断开 MQTT 连接
  Future<void> disconnectMqtt() async {
    if (_disposed) return;
    await _mqttService.disconnect(serialNumber: sn);
    _updatePubTimer();
  }

  /// 发布启动数据
  Future<void> _publishStartup() async {
    await _publishTopic('info');
    await _publishTopic('config');
    await _publishTopic('heartbeat');
  }

  /// 模拟 tick
  void _tick(double dt) {
    if (_disposed) return;
    
    // 使用物理模型模拟数据变化
    _inverterData.tick(dt);
    
    // 添加一些随机性，使不同设备行为不同
    _addRandomVariation();
  }

  /// 添加随机变化，使不同设备数据有差异
  void _addRandomVariation() {
    // 随机调整光伏功率 (±10%)
    final pvVariation = 1.0 + (_random.nextDouble() * 0.2 - 0.1);
    _inverterData.pv1Power *= pvVariation;
    _inverterData.pv2Power *= pvVariation;
    
    // 随机调整负载 (±5%)
    final loadVariation = 1.0 + (_random.nextDouble() * 0.1 - 0.05);
    _inverterData.power *= loadVariation;
    
    // 随机调整温度 (±2°C)
    _inverterData.tempInv += (_random.nextDouble() * 4 - 2);
    _inverterData.tempMos += (_random.nextDouble() * 4 - 2);
    
    // 确保值在合理范围内
    _inverterData.pv1Power = _inverterData.pv1Power.clamp(0, 2000);
    _inverterData.pv2Power = _inverterData.pv2Power.clamp(0, 2000);
    _inverterData.power = _inverterData.power.clamp(0, _inverterData.ratedPower.toDouble());
    _inverterData.tempInv = _inverterData.tempInv.clamp(30, 85);
    _inverterData.tempMos = _inverterData.tempMos.clamp(25, 80);
  }

  /// 发布单个主题
  Future<void> _publishTopic(String topic) async {
    if (_disposed || !_mqttService.isConnected) return;
    
    try {
      final payload = _inverterData.toJson(topic);
      if (payload.isEmpty) return;
      
      _mqttService.publishData(sn, topic, payload);
      onLog?.call(sn, '[MQTT TX] $topic');
    } catch (e) {
      onLog?.call(sn, '[ERR] 发布失败: $e');
    }
  }

  /// 发布遥测数据周期
  Future<void> _publishTelemetryCycle() async {
    await _publishTopic('heartbeat');
  }

  /// 处理接收到的 MQTT 消息
  void _handleMqttMessage(String topic, String payload) {
    // 简单记录日志，不处理命令
    onLog?.call(sn, '[MQTT RX] $topic');
  }

  /// 更新发布定时器
  void _updatePubTimer() {
    _pubTimer?.cancel();
    if (_autoPublish && _mqttService.isConnected) {
      _pubTimer = Timer.periodic(
        Duration(seconds: _publishInterval.toInt()),
        (_) => _publishTelemetryCycle(),
      );
    }
  }

  /// 启动模拟
  void _startSimulation() {
    // 模拟定时器 (200ms)
    _simTimer = Timer.periodic(const Duration(milliseconds: 200), (_) {
      if (_autoSimulate) {
        _tick(0.2);
      }
    });
    
    _updatePubTimer();
  }

  /// 设置自动模拟
  void setAutoSimulate(bool value) {
    _autoSimulate = value;
  }

  /// 设置自动发布
  void setAutoPublish(bool value) {
    _autoPublish = value;
    _updatePubTimer();
  }

  /// 设置发布间隔
  void setPublishInterval(double seconds) {
    _publishInterval = seconds;
    _updatePubTimer();
  }

  /// 手动发布一次
  Future<void> publishNow() async {
    if (!_mqttService.isConnected) {
      onLog?.call(sn, '[WARN] 未连接，无法发布');
      return;
    }
    await _publishTelemetryCycle();
    onLog?.call(sn, '[SYS] 手动发布完成');
  }

  /// 释放资源
  void dispose() {
    if (_disposed) return;
    _disposed = true;
    _simTimer?.cancel();
    _pubTimer?.cancel();
    _mqttService.disconnect(serialNumber: sn);
  }
}