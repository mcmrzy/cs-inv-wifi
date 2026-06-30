import 'dart:math';

enum InverterMode {
  inverting,
  standby,
  fault,
  shutdown,
  bypass,
}

class AlarmDefinition {
  final int code;
  final String level;
  final String message;

  const AlarmDefinition({
    required this.code,
    required this.level,
    required this.message,
  });
}

class Alarm {
  final int code;
  final String level;
  final String message;
  final DateTime timestamp;
  bool active;
  DateTime? clearTimestamp;

  Alarm({
    required this.code,
    required this.level,
    required this.message,
    required this.timestamp,
    this.active = true,
    this.clearTimestamp,
  });
}

class FaultType {
  final String name;
  final int code;
  final String description;
  final String action;

  const FaultType({
    required this.name,
    required this.code,
    required this.description,
    required this.action,
  });
}

class ActiveFault {
  final String type;
  final DateTime endTime;
  final int code;

  ActiveFault({
    required this.type,
    required this.endTime,
    required this.code,
  });
}

class InverterData {
  static const Map<String, FaultType> faultTypes = {
    'overtemp': FaultType(name: 'overtemp', code: 1, description: '过温故障', action: '温度飙升'),
    'overvoltage': FaultType(name: 'overvoltage', code: 2, description: '电池过压', action: '电压升高'),
    'undervoltage': FaultType(name: 'undervoltage', code: 3, description: '电池欠压', action: '电压降低'),
    'overload': FaultType(name: 'overload', code: 4, description: '过载保护', action: '负载突增'),
    'short_circuit': FaultType(name: 'short_circuit', code: 20, description: '输出短路', action: '功率骤降'),
    'comm_error': FaultType(name: 'comm_error', code: 21, description: '通信故障', action: '数据异常'),
    'pv_error': FaultType(name: 'pv_error', code: 8, description: 'PV输入异常', action: 'PV功率归零'),
    'cell_imbalance': FaultType(name: 'cell_imbalance', code: 9, description: '电芯压差过大', action: '电芯电压差异'),
  };

  // 状态
  InverterMode mode = InverterMode.inverting;
  int faultCode = 0;
  int alarmCode = 0;

  // 温度
  double tempInv = 42.0;
  double tempMos = 38.0;
  double tempAmbient = 32.0;

  // 电池
  double soc = 75.0;
  double soh = 98.0;
  double batteryVoltage = 52.8;
  double batteryCurrent = 15.3;
  double batteryCapacity = 100.0;
  int cycleCount = 156;
  String chargeState = 'idle';
  List<double> cellVoltages = List.generate(16, (_) => 3.3);
  List<double> cellTemps = List.generate(16, (_) => 34.5);

  // 交流输出
  double voltage = 220.0;
  double current = 8.5;
  double power = 1870.0;
  double frequency = 50.0;
  double loadPercent = 30.0;

  // 光伏输入
  double pv1Voltage = 120.0;
  double pv1Current = 8.0;
  double pv1Power = 960.0;
  double pv1VoltageMax = 120.0;
  double pv1PowerMax = 960.0;
  double pv2Voltage = 118.0;
  double pv2Current = 7.5;
  double pv2Power = 885.0;
  double pv2VoltageMax = 118.0;
  double pv2PowerMax = 885.0;
  String mpptState = 'tracking';

  // 能量统计
  double dailyPv = 15.6;
  double totalPv = 5678.9;
  double dailyCharge = 12.3;
  double totalCharge = 4567.8;
  double dailyDischarge = 8.5;
  double totalDischarge = 3456.7;
  double dailyLoad = 10.2;
  double totalLoad = 5432.1;

  // 运行时间
  double runtimeHours = 1234.5;
  double efficiency = 92.3;
  int fanSpeed = 2400;

  // 额定参数
  int ratedPower = 6200;
  int ratedVoltage = 220;
  double ratedFreq = 50.0;
  String batteryType = 'LiFePO4';
  int cellCount = 16;

  // 远程控制参数
  Map<String, dynamic> remoteControls = {
    'power_limit': 6200,
    'charge_enable': true,
    'discharge_enable': true,
    'max_charge_current': 60.0,
    'max_discharge_current': 120.0,
  };

  // 活动故障
  final Map<String, ActiveFault> _activeFaults = {};

  Map<String, dynamic> toJson(String topic) {
    switch (topic) {
      case 'info':
        return {
          'sn': '',
          'model': 'CS-I10-6k2',
          'manufacturer': '辰烁科技',
          'firmware_arm': 'V1.2.3.20240510',
          'firmware_esp': 'V1.2.0.20260629',
          'firmware_dsp': 'V1.1.0.20240508',
          'type': '离网逆变器',
          'rated_power': ratedPower,
          'rated_voltage': ratedVoltage,
          'rated_freq': ratedFreq,
          'battery_voltage': batteryVoltage,
          'battery_type': batteryType,
          'cell_count': cellCount,
        };
      case 'ac':
        return {
          'voltage': voltage.toStringAsFixed(1),
          'current': current.toStringAsFixed(2),
          'power': power.toStringAsFixed(1),
          'apparent': (voltage * current).toStringAsFixed(1),
          'frequency': frequency.toStringAsFixed(2),
          'pf': (power / (voltage * current)).clamp(0, 1).toStringAsFixed(2),
          'load_percent': loadPercent.toStringAsFixed(1),
        };
      case 'battery':
        return {
          'soc': soc.toStringAsFixed(1),
          'soh': soh.toStringAsFixed(1),
          'voltage': batteryVoltage.toStringAsFixed(2),
          'current': batteryCurrent.toStringAsFixed(2),
          'power': (batteryVoltage * batteryCurrent).toStringAsFixed(1),
          'capacity_remain': (soc / 100 * batteryCapacity).toStringAsFixed(1),
          'capacity_total': batteryCapacity,
          'cycle_count': cycleCount,
          'temp_max': cellTemps.reduce((a, b) => a > b ? a : b).toStringAsFixed(1),
          'temp_min': cellTemps.reduce((a, b) => a < b ? a : b).toStringAsFixed(1),
          'cell_volt_max': cellVoltages.reduce((a, b) => a > b ? a : b).toStringAsFixed(3),
          'cell_volt_min': cellVoltages.reduce((a, b) => a < b ? a : b).toStringAsFixed(3),
          'cell_volt_diff': (cellVoltages.reduce((a, b) => a > b ? a : b) -
              cellVoltages.reduce((a, b) => a < b ? a : b)).toStringAsFixed(3),
          'charge_state': chargeState,
          'protect_status': 0,
          'bms_fault_code': 0,
          'max_chg_current': remoteControls['max_charge_current'],
          'max_dischg_current': remoteControls['max_discharge_current'],
          'charge_volt_ref': 54.6,
          'dischg_cut_volt': 44.0,
          'temp_battery': cellTemps.isEmpty ? 25.0 : (cellTemps.reduce((a, b) => a + b) / cellTemps.length).toStringAsFixed(1),
        };
      case 'pv':
        return {
          'pv1_voltage': pv1Voltage.toStringAsFixed(1),
          'pv1_current': pv1Current.toStringAsFixed(2),
          'pv1_power': pv1Power.toStringAsFixed(1),
          'pv1_voltage_max': pv1VoltageMax.toStringAsFixed(1),
          'pv1_power_max': pv1PowerMax.toStringAsFixed(1),
          'pv2_voltage': pv2Voltage.toStringAsFixed(1),
          'pv2_current': pv2Current.toStringAsFixed(2),
          'pv2_power': pv2Power.toStringAsFixed(1),
          'pv2_voltage_max': pv2VoltageMax.toStringAsFixed(1),
          'pv2_power_max': pv2PowerMax.toStringAsFixed(1),
          'pv_power_total': (pv1Power + pv2Power).toStringAsFixed(1),
          'mppt_state': mpptState,
        };
      case 'status':
        return {
          'state': mode.name,
          'fault_code': faultCode,
          'alarm_code': alarmCode,
          'temp_inv': tempInv.toStringAsFixed(1),
          'temp_mos': tempMos.toStringAsFixed(1),
          'temp_ambient': tempAmbient.toStringAsFixed(1),
          'dc_bus_voltage': (batteryVoltage * 1.2).toStringAsFixed(1),
          'efficiency': efficiency.toStringAsFixed(1),
          'runtime_hours': runtimeHours.toStringAsFixed(1),
          'fan_speed': fanSpeed,
        };
      case 'energy':
        return {
          'daily_pv': dailyPv.toStringAsFixed(2),
          'total_pv': totalPv.toStringAsFixed(1),
          'daily_charge': dailyCharge.toStringAsFixed(2),
          'total_charge': totalCharge.toStringAsFixed(1),
          'daily_discharge': dailyDischarge.toStringAsFixed(2),
          'total_discharge': totalDischarge.toStringAsFixed(1),
          'daily_load': dailyLoad.toStringAsFixed(2),
          'total_load': totalLoad.toStringAsFixed(1),
          'runtime_hours': runtimeHours.toStringAsFixed(1),
        };
      case 'cells':
        return {
          'cell_count': cellCount,
          'voltages': cellVoltages.map((v) => double.parse(v.toStringAsFixed(3))).toList(),
          'temps': cellTemps.map((t) => double.parse(t.toStringAsFixed(1))).toList(),
        };
      default:
        return {};
    }
  }

  /// 根据SOC计算电池电压 (LiFePO4特性曲线)
  double calcBatteryVoltage(double soc) {
    const vMin = 44.8;
    const vMax = 58.4;
    return vMin + (vMax - vMin) * (soc / 100.0);
  }

  /// 注入故障
  String injectFault(String faultType, {int duration = 30}) {
    if (!faultTypes.containsKey(faultType) && faultType.isNotEmpty) {
      return '未知故障类型: $faultType';
    }

    final fault = faultTypes[faultType];
    if (fault == null) return '未知故障类型';

    _activeFaults[faultType] = ActiveFault(
      type: faultType,
      endTime: DateTime.now().add(Duration(seconds: duration)),
      code: fault.code,
    );

    return '已注入故障: ${fault.description}，持续${duration}秒';
  }

  /// 清除故障
  String clearFault(String faultType) {
    if (_activeFaults.containsKey(faultType)) {
      _activeFaults.remove(faultType);
      return '已清除故障: $faultType';
    }
    return '故障不存在';
  }

  /// 清除所有故障
  String clearAllFaults() {
    _activeFaults.clear();
    return '已清除所有故障';
  }

  /// 应用故障效果
  void applyFaults() {
    final now = DateTime.now();

    // 清除过期故障
    _activeFaults.removeWhere((key, fault) => now.isAfter(fault.endTime));

    // 应用故障效果
    for (var fault in _activeFaults.values) {
      switch (fault.type) {
        case 'overtemp':
          tempInv = min(85, tempInv + 5);
          break;
        case 'overvoltage':
          batteryVoltage = min(65, batteryVoltage + 2);
          break;
        case 'undervoltage':
          batteryVoltage = max(40, batteryVoltage - 2);
          break;
        case 'overload':
          power = min(ratedPower * 1.2, power * 1.1);
          break;
        case 'short_circuit':
          power = max(0, power - 500);
          break;
        case 'pv_error':
          pv1Power = 0;
          pv2Power = 0;
          break;
        case 'cell_imbalance':
          if (cellVoltages.isNotEmpty) {
            final random = Random();
            final maxIdx = random.nextInt(cellVoltages.length);
            final minIdx = random.nextInt(cellVoltages.length);
            cellVoltages[maxIdx] = min(3.65, cellVoltages[maxIdx] + 0.1);
            cellVoltages[minIdx] = max(3.0, cellVoltages[minIdx] - 0.1);
          }
          break;
      }
    }
  }

  /// 基于能量守恒的物理模拟
  void tick(double dt) {
    final random = Random();

    // 1. 计算目标PV功率（带小幅度波动）
    final pvNoise = 1.0 + random.nextDouble() * 0.1 - 0.05;
    double targetPv = 1500 * pvNoise;
    targetPv = targetPv.clamp(0, 2000);

    // 2. 应用远程控制限制
    final effectivePowerLimit = min(ratedPower, remoteControls['power_limit'] as int).toDouble();

    // 3. 计算目标AC功率（带小幅度波动）
    final acNoise = 1.0 + random.nextDouble() * 0.1 - 0.05;
    double targetAc = 1800 * acNoise;
    targetAc = targetAc.clamp(0, effectivePowerLimit);

    // 充放电使能控制
    if (!(remoteControls['charge_enable'] as bool) && targetPv > targetAc) {
      targetAc = targetPv;
    }
    if (!(remoteControls['discharge_enable'] as bool) && targetPv < targetAc) {
      targetAc = targetPv;
    }

    // 4. 能量守恒计算电池功率
    double batteryPower = targetPv - targetAc;

    // 考虑逆变效率
    if (batteryPower > 0) {
      batteryPower = batteryPower * 0.92;
    } else {
      batteryPower = batteryPower / 0.92;
    }

    // 5. 计算电池电压和电流
    batteryVoltage = calcBatteryVoltage(soc);
    if (batteryVoltage > 0) {
      batteryCurrent = batteryPower / batteryVoltage;
    } else {
      batteryCurrent = 0.0;
    }
    batteryCurrent = batteryCurrent.clamp(-60, 60);

    // 6. 更新SOC
    final socDelta = -(batteryCurrent * dt / 3600.0) / batteryCapacity * 100;
    soc = (soc + socDelta).clamp(20, 100);

    // 充放电状态
    if (batteryCurrent > 1.0) {
      chargeState = 'charging';
    } else if (batteryCurrent < -1.0) {
      chargeState = 'discharging';
    } else {
      chargeState = 'idle';
    }

    // 7. 计算AC输出
    power = min(targetAc, ratedPower.toDouble());
    voltage = 220.0 + random.nextDouble() * 2 - 1;
    voltage = voltage.clamp(210, 230);
    current = voltage > 0 ? power / voltage : 0;
    loadPercent = power / ratedPower * 100;
    frequency = 50.0 + random.nextDouble() * 0.1 - 0.05;
    frequency = frequency.clamp(49.5, 50.5);

    // 8. 计算PV参数（双路MPPT）
    const pv1Ratio = 0.53;
    double pv1PowerCalc = targetPv * pv1Ratio;
    if (pv1PowerCalc > 0) {
      pv1Voltage = 70.0 + (pv1PowerCalc / 1060.0) * 50.0;
      pv1Voltage += random.nextDouble() * 4 - 2;
      pv1Voltage = pv1Voltage.clamp(60, 150);
      pv1Current = pv1PowerCalc / pv1Voltage;
      if (pv1Voltage > pv1VoltageMax) pv1VoltageMax = pv1Voltage;
      if (pv1PowerCalc > pv1PowerMax) pv1PowerMax = pv1PowerCalc;
    } else {
      pv1Voltage = 0;
      pv1Current = 0;
    }
    pv1Power = pv1PowerCalc;

    double pv2PowerCalc = targetPv * (1 - pv1Ratio);
    if (pv2PowerCalc > 0) {
      pv2Voltage = 70.0 + (pv2PowerCalc / 940.0) * 50.0;
      pv2Voltage += random.nextDouble() * 4 - 2;
      pv2Voltage = pv2Voltage.clamp(60, 150);
      pv2Current = pv2PowerCalc / pv2Voltage;
      if (pv2Voltage > pv2VoltageMax) pv2VoltageMax = pv2Voltage;
      if (pv2PowerCalc > pv2PowerMax) pv2PowerMax = pv2PowerCalc;
    } else {
      pv2Voltage = 0;
      pv2Current = 0;
    }
    pv2Power = pv2PowerCalc;

    // MPPT状态
    final pvTotal = pv1Power + pv2Power;
    if (pvTotal > 100) {
      mpptState = 'tracking';
    } else if (pvTotal > 10) {
      mpptState = 'scanning';
    } else {
      mpptState = 'standby';
    }

    // 9. 更新温度
    tempInv = 42 + power / 1000 * 5 + random.nextDouble() * 2 - 1;
    tempMos = tempInv - 3 + random.nextDouble() * 2 - 1;
    tempAmbient = 32 + random.nextDouble() * 4 - 2;

    // 10. 更新电芯数据
    for (int i = 0; i < cellVoltages.length; i++) {
      cellVoltages[i] = batteryVoltage / cellCount + random.nextDouble() * 0.02 - 0.01;
    }
    for (int i = 0; i < cellTemps.length; i++) {
      cellTemps[i] = tempInv / cellCount + random.nextDouble() * 3 - 1.5;
    }

    // 11. 更新能量统计
    dailyPv += pvTotal * dt / 3600000;
    totalPv += pvTotal * dt / 3600000;
    if (batteryCurrent > 0) {
      dailyCharge += batteryVoltage * batteryCurrent * dt / 3600000;
      totalCharge += batteryVoltage * batteryCurrent * dt / 3600000;
    } else {
      dailyDischarge += batteryVoltage * batteryCurrent.abs() * dt / 3600000;
      totalDischarge += batteryVoltage * batteryCurrent.abs() * dt / 3600000;
    }
    dailyLoad += power * dt / 3600000;
    totalLoad += power * dt / 3600000;

    // 12. 运行时间
    runtimeHours += dt / 3600;

    // 13. 风扇转速
    if (tempInv > 50) {
      fanSpeed = 2400 + ((tempInv - 50) * 100).toInt();
    } else {
      fanSpeed = 1200;
    }

    // 14. 效率计算
    if (power > 0 && pvTotal > 0) {
      efficiency = (power / pvTotal * 100).clamp(85, 98);
    }

    // 15. 应用故障效果
    applyFaults();
  }
}