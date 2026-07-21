import 'inverter_state.dart';

class AlarmSystem {
  static const Map<int, AlarmDefinition> alarmDefinitions = {
    1: AlarmDefinition(code: 1, level: 'fault', message: '母线过压'),
    2: AlarmDefinition(code: 2, level: 'fault', message: '母线欠压'),
    3: AlarmDefinition(code: 3, level: 'fault', message: '输出过压'),
    4: AlarmDefinition(code: 4, level: 'fault', message: '输出欠压'),
    5: AlarmDefinition(code: 5, level: 'fault', message: '过频'),
    6: AlarmDefinition(code: 6, level: 'fault', message: '欠频'),
    7: AlarmDefinition(code: 7, level: 'fault', message: '孤岛保护'),
    8: AlarmDefinition(code: 8, level: 'fault', message: '过温保护'),
    9: AlarmDefinition(code: 9, level: 'fault', message: 'PV过压'),
    10: AlarmDefinition(code: 10, level: 'fault', message: '输出过载'),
    11: AlarmDefinition(code: 11, level: 'fault', message: '绝缘阻抗低'),
    12: AlarmDefinition(code: 12, level: 'fault', message: '输出短路'),
    16: AlarmDefinition(code: 16, level: 'fault', message: '电池反接'),
    17: AlarmDefinition(code: 17, level: 'fault', message: 'BMS通信丢失'),
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
    if (_alarmCooldown.containsKey(code) &&
        now.isBefore(_alarmCooldown[code]!)) {
      return;
    }

    if (!_activeAlarms.contains(code)) {
      _activeAlarms.add(code);
      if (manual) {
        _manualAlarms.add(code);
      }

      final def =
          alarmDefinitions[code] ??
          AlarmDefinition(code: code, level: 'info', message: '未知告警 $code');

      _alarmHistory.add(
        Alarm(
          code: code,
          level: def.level,
          message: def.message,
          timestamp: now,
        ),
      );
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
    // PCS code 8: 过温保护
    if (data.tempInv > config.overtempThreshold) {
      addAlarm(8);
    } else if (data.tempInv < 75) {
      if (!isManualAlarm(8)) removeAlarm(8);
    }

    // PCS code 9: PV 过压
    if (data.pv1Voltage > 150 || data.pv2Voltage > 150) {
      addAlarm(9);
    } else if (data.pv1Voltage < 145 && data.pv2Voltage < 145) {
      if (!isManualAlarm(9)) removeAlarm(9);
    }

    // PCS code 10: 输出过载
    if (data.power > 6200) {
      addAlarm(10);
    } else if (data.power < 6000) {
      if (!isManualAlarm(10)) removeAlarm(10);
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
        final def =
            alarmDefinitions[code] ??
            AlarmDefinition(code: code, level: 'info', message: '未知告警 $code');
        return {'code': code, 'level': def.level, 'message': def.message};
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

    // 无告警，返回null（恢复消息由 checkAlarms 触发后单独处理）
    return null;
  }
}

class AlarmConfig {
  final double overtempThreshold;
  final double undervoltThreshold;
  final double overvoltThreshold;
  final double overloadThreshold;

  const AlarmConfig({
    this.overtempThreshold = 85.0,
    this.undervoltThreshold = 46.0,
    this.overvoltThreshold = 58.0,
    this.overloadThreshold = 0.95,
  });
}
