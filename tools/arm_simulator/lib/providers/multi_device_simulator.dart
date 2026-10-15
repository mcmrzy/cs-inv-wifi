import 'dart:async';
import 'package:flutter/foundation.dart';
import 'package:shared_preferences/shared_preferences.dart';
import '../models/device_simulator.dart';
import '../utils/sn_generator.dart';

/// 多设备模拟器管理类，管理多个设备实例
class MultiDeviceSimulator extends ChangeNotifier {
  final List<DeviceSimulator> _devices = [];
  final List<String> _logs = [];
  static const int maxLogCount = 5000;
  
  // MQTT 连接配置
  String _mqttHost = '127.0.0.1';
  int _mqttPort = 1883;
  bool _mqttTls = false;
  String _mqttUsername = '';
  String _mqttPassword = '';
  
  // 模拟配置
  int _deviceCount = 10;
  bool _isRunning = false;
  bool _disposed = false;
  
  // SN 生成配置
  String _manufacturer = 'H1';
  String _country = 'DE';
  String _customer = 'A001';
  int _startSequence = 1;
  
  // 连接状态
  int _connectedCount = 0;
  int _connectingCount = 0;
  int _failedCount = 0;
  final Set<String> _connectedDevices = {};  // 跟踪已连接的设备SN

  late final Future<void> _settingsFuture;

  MultiDeviceSimulator() {
    _settingsFuture = _loadSettings();
  }

  /// 等待设置加载完成
  Future<void> initialize() => _settingsFuture;

  // Getters
  List<DeviceSimulator> get devices => List.unmodifiable(_devices);
  List<String> get logs => List.unmodifiable(_logs);
  int get deviceCount => _deviceCount;
  bool get isRunning => _isRunning;
  String get mqttHost => _mqttHost;
  int get mqttPort => _mqttPort;
  bool get mqttTls => _mqttTls;
  String get mqttUsername => _mqttUsername;
  String get mqttPassword => _mqttPassword;
  String get manufacturer => _manufacturer;
  String get country => _country;
  String get customer => _customer;
  int get startSequence => _startSequence;
  int get connectedCount => _connectedCount;
  int get connectingCount => _connectingCount;
  int get failedCount => _failedCount;
  bool get allConnected => _connectedCount == _deviceCount && _deviceCount > 0;

  /// 加载设置
  Future<void> _loadSettings() async {
    try {
      final prefs = await SharedPreferences.getInstance();
      _mqttHost = prefs.getString('multi.mqtt.host') ?? _mqttHost;
      _mqttPort = prefs.getInt('multi.mqtt.port') ?? _mqttPort;
      _mqttTls = prefs.getBool('multi.mqtt.tls') ?? _mqttTls;
      _mqttUsername = prefs.getString('multi.mqtt.username') ?? _mqttUsername;
      _mqttPassword = prefs.getString('multi.mqtt.password') ?? _mqttPassword;
      _deviceCount = prefs.getInt('multi.device.count') ?? _deviceCount;
      _manufacturer = prefs.getString('multi.sn.manufacturer') ?? _manufacturer;
      _country = prefs.getString('multi.sn.country') ?? _country;
      _customer = prefs.getString('multi.sn.customer') ?? _customer;
      _startSequence = prefs.getInt('multi.sn.startSeq') ?? _startSequence;
    } catch (_) {}
    notifyListeners();
  }

  /// 保存设置
  Future<void> _saveSettings() async {
    try {
      final prefs = await SharedPreferences.getInstance();
      await prefs.setString('multi.mqtt.host', _mqttHost);
      await prefs.setInt('multi.mqtt.port', _mqttPort);
      await prefs.setBool('multi.mqtt.tls', _mqttTls);
      await prefs.setString('multi.mqtt.username', _mqttUsername);
      await prefs.setString('multi.mqtt.password', _mqttPassword);
      await prefs.setInt('multi.device.count', _deviceCount);
      await prefs.setString('multi.sn.manufacturer', _manufacturer);
      await prefs.setString('multi.sn.country', _country);
      await prefs.setString('multi.sn.customer', _customer);
      await prefs.setInt('multi.sn.startSeq', _startSequence);
    } catch (_) {}
  }

  /// 设置设备数量
  void setDeviceCount(int count) {
    if (count < 0 || count > 100) return;
    _deviceCount = count;
    _saveSettings();
    notifyListeners();
  }

  /// 设置 MQTT 配置
  void setMqttConfig({
    String? host,
    int? port,
    bool? tls,
    String? username,
    String? password,
  }) {
    if (host != null) _mqttHost = host;
    if (port != null) _mqttPort = port;
    if (tls != null) _mqttTls = tls;
    if (username != null) _mqttUsername = username;
    if (password != null) _mqttPassword = password;
    _saveSettings();
    notifyListeners();
  }

  /// 设置 SN 生成参数
  void setSnConfig({
    String? manufacturer,
    String? country,
    String? customer,
    int? startSequence,
  }) {
    if (manufacturer != null) _manufacturer = manufacturer;
    if (country != null) _country = country;
    if (customer != null) _customer = customer;
    if (startSequence != null) _startSequence = startSequence;
    _saveSettings();
    notifyListeners();
  }

  /// 生成 SN 列表
  List<String> generateSns() {
    return SnGenerator.generateBatch(
      count: _deviceCount,
      manufacturer: _manufacturer,
      country: _country,
      customer: _customer,
      startSequence: _startSequence,
    );
  }

  /// 开始模拟
  Future<void> startSimulation() async {
    if (_isRunning || _deviceCount <= 0) return;
    
    _isRunning = true;
    _connectedCount = 0;
    _connectingCount = _deviceCount;
    _failedCount = 0;
    _connectedDevices.clear();
    notifyListeners();
    
    _addLog('[SYS] 开始创建 $_deviceCount 个设备模拟器...');
    
    // 生成 SN 列表
    final sns = generateSns();
    
    // 创建设备模拟器
    _devices.clear();
    for (int i = 0; i < _deviceCount; i++) {
      final sn = sns[i];
      final device = DeviceSimulator(
        sn: sn,
        onLog: _handleDeviceLog,
        onConnectionChanged: _handleConnectionChanged,
      );
      _devices.add(device);
    }
    
    // 并行连接所有设备
    _connectAllDevices();
  }

  /// 连接所有设备（分批连接，每批最多5个并发，避免冲击Broker）
  Future<void> _connectAllDevices() async {
    const batchSize = 5;
    
    for (int i = 0; i < _devices.length; i += batchSize) {
      if (!_isRunning) break;  // 如果已停止则中断
      
      final end = (i + batchSize).clamp(0, _devices.length);
      final batch = _devices.sublist(i, end);
      final futures = batch.map((device) => _connectDevice(device)).toList();
      
      await Future.wait(futures, eagerError: false);
      
      // 批次间稍微延迟，避免瞬时并发过高
      if (end < _devices.length && _isRunning) {
        await Future<void>.delayed(const Duration(milliseconds: 200));
      }
    }
    
    _connectingCount = 0;
    _addLog('[SYS] 所有设备连接完成: $_connectedCount/$_deviceCount 成功'
        '${_failedCount > 0 ? ", $_failedCount 失败" : ""}');
    notifyListeners();
  }

  /// 连接单个设备
  Future<void> _connectDevice(DeviceSimulator device) async {
    try {
      await device.connectMqtt(
        host: _mqttHost,
        port: _mqttPort,
        useTls: _mqttTls,
        username: _mqttUsername,
        password: _mqttPassword,
        autoReconnect: true,  // 已连接设备断线后自动重连（初始连接失败由try-catch处理）
      );
    } catch (e) {
      _connectingCount = (_connectingCount - 1).clamp(0, _deviceCount);
      _failedCount++;
      _addLog('[ERR] 设备 ${device.sn} 连接失败: $e');
      notifyListeners();
    }
  }

  /// 停止模拟
  Future<void> stopSimulation() async {
    if (!_isRunning) return;
    
    _addLog('[SYS] 正在停止所有设备模拟...');
    
    // 断开所有设备
    final futures = <Future>[];
    for (final device in _devices) {
      futures.add(device.disconnectMqtt());
    }
    
    await Future.wait(futures, eagerError: false);
    
    // 释放资源
    for (final device in _devices) {
      device.dispose();
    }
    _devices.clear();
    
    _isRunning = false;
    _connectedCount = 0;
    _connectingCount = 0;
    _failedCount = 0;
    _connectedDevices.clear();
    
    _addLog('[SYS] 所有设备模拟已停止');
    notifyListeners();
  }

  /// 处理设备日志
  void _handleDeviceLog(String sn, String message) {
    _addLog('[$sn] $message');
  }

  /// 处理连接状态变化
  void _handleConnectionChanged(String sn, bool connected) {
    if (connected) {
      if (!_connectedDevices.contains(sn)) {
        _connectedDevices.add(sn);
        _connectedCount = _connectedDevices.length;
        _connectingCount = (_connectingCount - 1).clamp(0, _deviceCount);
        _addLog('[SYS] 设备 $sn 已连接 ($_connectedCount/$_deviceCount)');
      }
    } else {
      if (_connectedDevices.contains(sn)) {
        _connectedDevices.remove(sn);
        _connectedCount = _connectedDevices.length;
        _addLog('[SYS] 设备 $sn 已断开 ($_connectedCount/$_deviceCount)');
      }
      // 如果设备从未连接成功，不重复记录断开日志
    }
    
    notifyListeners();
  }

  /// 手动发布所有设备数据
  Future<void> publishAllNow() async {
    if (!_isRunning) return;
    
    _addLog('[SYS] 手动触发所有设备发布...');
    
    final futures = <Future>[];
    for (final device in _devices) {
      if (device.isConnected) {
        futures.add(device.publishNow());
      }
    }
    
    await Future.wait(futures, eagerError: false);
    _addLog('[SYS] 手动发布完成');
  }

  /// 设置所有设备的自动模拟
  void setAllAutoSimulate(bool value) {
    for (final device in _devices) {
      device.setAutoSimulate(value);
    }
    notifyListeners();
  }

  /// 设置所有设备的自动发布
  void setAllAutoPublish(bool value) {
    for (final device in _devices) {
      device.setAutoPublish(value);
    }
    notifyListeners();
  }

  /// 设置所有设备的发布间隔
  void setAllPublishInterval(double seconds) {
    for (final device in _devices) {
      device.setPublishInterval(seconds);
    }
    notifyListeners();
  }

  /// 添加日志
  void _addLog(String message) {
    if (_disposed) return;
    
    final now = DateTime.now();
    final timeStr = '${now.hour.toString().padLeft(2, '0')}:'
        '${now.minute.toString().padLeft(2, '0')}:'
        '${now.second.toString().padLeft(2, '0')}.${now.millisecond.toString().padLeft(3, '0')}';
    
    _logs.insert(0, '[$timeStr] $message');
    
    if (_logs.length > maxLogCount) {
      _logs.removeLast();
    }
    
    notifyListeners();
  }

  /// 清除日志
  void clearLogs() {
    _logs.clear();
    notifyListeners();
  }

  @override
  void dispose() {
    _disposed = true;
    stopSimulation();
    super.dispose();
  }
}