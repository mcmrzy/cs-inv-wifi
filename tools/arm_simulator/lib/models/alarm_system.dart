import 'inverter_state.dart';

class AlarmSystem {
  static const Map<int, AlarmDefinition> alarmDefinitions = {
    1: AlarmDefinition(code: 1, level: 'fault', message: '逆变器过温保护'),
    2: AlarmDefinition(code: 2, level: 'fault', message: '电池过压保护'),
    3: AlarmDefinition(code: 3, level: 'fault', message: '电池欠压保护'),
    4: AlarmDefinition(code: 4, level: 'fault', message: '输出过载保护'),
    5: AlarmDefinition(code: 5, level: 'fault', message: '直流母线过压'),
    6: AlarmDefinition(code: 6, level: 'warning', message: '逆变器温度过高'),
    7: AlarmDefinition(code: 7, level: 'warning', message: '电池SOC过低'),
    8: AlarmDefinition(code: 8, level: 'warning', message: 'PV输入异常'),
    9: AlarmDefinition(code: 9, level: 'warning', message: '电芯压差过大'),
    10: AlarmDefinition(code: 10, level: 'info', message: '系统启动完成'),
    11: AlarmDefinition(code: 11, level: 'info', message: '进入待机模式'),
    12: AlarmDefinition(code: 12, level: 'info', message: '恢复并网运行'),
  };

  final List<int> _activeAlarms = [];
  final Set<int> _manualAlarms = {};
  final Map<int, DateTime> _alarmCooldown = {};
  final List<Alarm> _alarmHistory = [];
  bool _hadCriticalAlarm = false;

  List<int> get activeAlarms => List.unmodifiable(_activeAlarms);
  List<Alarm> get alarmHistory => List.unmodifiable(_alarmHistory);
  bool get hasCriticalAlarm => _activeAlarms.any((code) {
    final def = alarmDefinitions[code];
    return def != null && (def.level == 'fault' || def.level == 'warning');
  });

  void addAlarm(int code, {bool manual = false}) {
    final now = DateTime.now();
    
    // 冷却期检查
    if (_alarmCooldown.containsKey(code) && now.isBefore(_alarmCooldown[code]!)) {
      return;
    }

    if (!_activeAlarms.contains(code)) {
      _activeAlarms.add(code);
      if (manual) {
        _manualAlarms.add(code);
      }

      final def = alarmDefinitions[code] ?? AlarmDefinition(
        code: code,
        level: 'info',
        message: '未知告警 $code',
      );

      _alarmHistory.add(Alarm(
        code: code,
        level: def.level,
        message: def.message,
        timestamp: now,
      ));
    }
  }

  void removeAlarm(int code) {
    if (_activeAlarms.contains(code)) {
      _activeAlarms.remove(code);
      _manualAlarms.remove(code);
      _alarmCooldown[code] = DateTime.now().add(const Duration(seconds: 10));

      // 更新历史记录
      for (var alarm in _alarmHistory.reversed) {
        if (alarm.code == code && alarm.active) {
          alarm.active = false;
          alarm.clearTimestamp = DateTime.now();
          break;
        }
      }
    }
  }

  void clearAllAlarms() {
    for (var code in List.from(_activeAlarms)) {
      removeAlarm(code);
    }
    _manualAlarms.clear();
  }

  bool isManualAlarm(int code) => _manualAlarms.contains(code);

  /// 检查告警条件，返回是否需要发送恢复消息
  bool checkAlarms(InverterData data, AlarmConfig config) {
    // 过温检测
    if (data.tempInv > config.overtempThreshold) {
      addAlarm(1);
    } else if (data.tempInv < config.overtempThreshold - 5) {
      if (!isManualAlarm(1)) removeAlarm(1);
    }

    // 电池过压检测
    if (data.batteryVoltage > config.overvoltThreshold) {
      addAlarm(2);
    } else if (data.batteryVoltage < config.overvoltThreshold - 2) {
      if (!isManualAlarm(2)) removeAlarm(2);
    }

    // 电池欠压检测
    if (data.batteryVoltage < config.undervoltThreshold) {
      addAlarm(3);
    } else if (data.batteryVoltage > config.undervoltThreshold + 2) {
      if (!isManualAlarm(3)) removeAlarm(3);
    }

    // 过载检测
    if (data.loadPercent > config.overloadThreshold * 100) {
      addAlarm(4);
    } else if (data.loadPercent < config.overloadThreshold * 100 - 5) {
      if (!isManualAlarm(4)) removeAlarm(4);
    }

    // 温度过高警告
    if (data.tempInv > 55 && data.tempInv <= config.overtempThreshold) {
      addAlarm(6);
    } else if (data.tempInv < 50) {
      if (!isManualAlarm(6)) removeAlarm(6);
    }

    // SOC过低警告
    if (data.soc < 25) {
      addAlarm(7);
    } else if (data.soc > 35) {
      if (!isManualAlarm(7)) removeAlarm(7);
    }

    // 检查是否需要发送恢复消息
    final hasCritical = hasCriticalAlarm;
    if (!hasCritical && _hadCriticalAlarm) {
      _hadCriticalAlarm = false;
      return true; // 需要发送恢复消息
    } else if (hasCritical) {
      _hadCriticalAlarm = true;
    }
    return false;
  }

  /// 获取告警数据JSON
  Map<String, dynamic>? getAlarmJson() {
    final criticalAlarms = _activeAlarms.where((code) {
      final def = alarmDefinitions[code];
      return def != null && (def.level == 'fault' || def.level == 'warning');
    }).toList();

    if (criticalAlarms.isNotEmpty) {
      final alarms = criticalAlarms.map((code) {
        final def = alarmDefinitions[code] ?? AlarmDefinition(
          code: code,
          level: 'info',
          message: '未知告警 $code',
        );
        return {
          'code': code,
          'level': def.level,
          'message': def.message,
        };
      }).toList();

      final mainCode = criticalAlarms.first;
      final mainDef = alarmDefinitions[mainCode]!;

      return {
        'code': mainCode,
        'level': mainDef.level,
        'message': mainDef.message,
        'count': criticalAlarms.length,
        'alarms': alarms,
        'timestamp': DateTime.now().millisecondsSinceEpoch ~/ 1000,
      };
    }

    // 无故障/警告时，只在刚恢复时返回恢复状态
    if (_hadCriticalAlarm) {
      _hadCriticalAlarm = false;
      return {
        'code': 0,
        'level': 'normal',
        'message': '故障恢复，系统正常',
        'count': 0,
        'alarms': [],
        'timestamp': DateTime.now().millisecondsSinceEpoch ~/ 1000,
      };
    }
    // 无告警且之前也没有，返回null（不发送）
    return null;
  }
}

class AlarmConfig {
  final double overtempThreshold;
  final double undervoltThreshold;
  final double overvoltThreshold;
  final double overloadThreshold;

  const AlarmConfig({
    this.overtempThreshold = 65.0,
    this.undervoltThreshold = 46.0,
    this.overvoltThreshold = 58.0,
    this.overloadThreshold = 0.95,
  });
}