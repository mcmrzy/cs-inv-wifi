import 'dart:math';

enum InverterMode { inverting, standby, fault, shutdown, bypass }

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

  ActiveFault({required this.type, required this.endTime, required this.code});
}

class InverterData {
  static const Map<String, FaultType> faultTypes = {
    'overtemp': FaultType(
      name: 'overtemp',
      code: 1,
      description: '过温故障',
      action: '温度飙升',
    ),
    'overvoltage': FaultType(
      name: 'overvoltage',
      code: 2,
      description: '电池过压',
      action: '电压升高',
    ),
    'undervoltage': FaultType(
      name: 'undervoltage',
      code: 3,
      description: '电池欠压',
      action: '电压降低',
    ),
    'overload': FaultType(
      name: 'overload',
      code: 4,
      description: '过载保护',
      action: '负载突增',
    ),
    'short_circuit': FaultType(
      name: 'short_circuit',
      code: 20,
      description: '输出短路',
      action: '功率骤降',
    ),
    'comm_error': FaultType(
      name: 'comm_error',
      code: 21,
      description: '通信故障',
      action: '数据异常',
    ),
    'pv_error': FaultType(
      name: 'pv_error',
      code: 8,
      description: 'PV输入异常',
      action: 'PV功率归零',
    ),
    'cell_imbalance': FaultType(
      name: 'cell_imbalance',
      code: 9,
      description: '电芯压差过大',
      action: '电芯电压差异',
    ),
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

  // 固件版本（可自定义）
  String firmwareArm = 'V1.2.3.20240510';
  String firmwareDsp = 'V1.1.0.20240508';
  String firmwareBms = 'V2.0.1';

  // Config 相关
  int configRev = 1;
  int acEnabled = 1;
  int powerLimit = 6200;
  int chargeLimit = 6200;
  int dischargeLimit = 6200;
  int socLowX10 = 200;
  int socHighX10 = 950;
  int forceCharge = 0;
  int forceDischarge = 0;

  int bmsChargeEnabled = 1;
  int bmsDischargeEnabled = 1;
  int bmsMaxChargeCurrentX10 = 500;
  int bmsMaxDischargeCurrentX10 = 1000;
  int bmsChargeVoltageX10 = 584;
  int bmsDischargeVoltageX10 = 430;

  int parallelMode = 0; // 0=standalone, 1=single phase, 2=three phase
  int? parallelRole; // 0=master, 1=slave
  int? parallelPhase; // 1=L1, 2=L2, 3=L3

  // BMS 动态字段
  int protectStatus = 0;
  int bmsFaultCode = 0;
  double maxChargeCurrent = 60.0;
  double maxDischargeCurrent = 120.0;
  double chargeVoltageRef = 54.6;
  double dischargeCutoffVoltage = 44.0;
  double chargeRequestCurrent = 50.0;
  double chargeRequestVoltage = 54.6;

  // 累计 Ah / 时间
  double totalChargeAh = 0.0;
  double totalDischargeAh = 0.0;
  double totalChargeTime = 0.0;
  double totalDischargeTime = 0.0;

  // 其他
  int systemMode = 0;
  double acVoltageThd = 2.5;
  int tempSensorCount = 16;

  // 远程控制参数
  Map<String, dynamic> remoteControls = {
    'power_limit': 6200,
    'charge_enable': true,
    'discharge_enable': true,
    'max_charge_current': 60.0,
    'max_discharge_current': 120.0,
  };

  bool get parallelEnabled => parallelMode != 0;

  Map<String, dynamic> configSnapshot() => {
    'config_rev': configRev,
    'ac_enabled': acEnabled,
    'power_limit': powerLimit,
    'charge_limit': chargeLimit,
    'discharge_limit': dischargeLimit,
    'soc_low_x10': socLowX10,
    'soc_high_x10': socHighX10,
    'force_charge': forceCharge,
    'force_discharge': forceDischarge,
    'bms_charge_enabled': bmsChargeEnabled,
    'bms_discharge_enabled': bmsDischargeEnabled,
    'bms_max_charge_current_x10': bmsMaxChargeCurrentX10,
    'bms_max_discharge_current_x10': bmsMaxDischargeCurrentX10,
    'bms_charge_voltage_x10': bmsChargeVoltageX10,
    'bms_discharge_voltage_x10': bmsDischargeVoltageX10,
    'parallel_mode': parallelMode,
    'parallel_role': parallelRole,
    'parallel_phase': parallelPhase,
  };

  void restoreConfigSnapshot(Map<String, dynamic> snapshot) {
    int readInt(String key, int fallback) =>
        snapshot[key] is int ? snapshot[key] as int : fallback;

    configRev = readInt('config_rev', configRev);
    acEnabled = readInt('ac_enabled', acEnabled);
    powerLimit = readInt('power_limit', powerLimit);
    chargeLimit = readInt('charge_limit', chargeLimit);
    dischargeLimit = readInt('discharge_limit', dischargeLimit);
    socLowX10 = readInt('soc_low_x10', socLowX10);
    socHighX10 = readInt('soc_high_x10', socHighX10);
    forceCharge = readInt('force_charge', forceCharge);
    forceDischarge = readInt('force_discharge', forceDischarge);
    bmsChargeEnabled = readInt('bms_charge_enabled', bmsChargeEnabled);
    bmsDischargeEnabled = readInt('bms_discharge_enabled', bmsDischargeEnabled);
    bmsMaxChargeCurrentX10 = readInt(
      'bms_max_charge_current_x10',
      bmsMaxChargeCurrentX10,
    );
    bmsMaxDischargeCurrentX10 = readInt(
      'bms_max_discharge_current_x10',
      bmsMaxDischargeCurrentX10,
    );
    bmsChargeVoltageX10 = readInt(
      'bms_charge_voltage_x10',
      bmsChargeVoltageX10,
    );
    bmsDischargeVoltageX10 = readInt(
      'bms_discharge_voltage_x10',
      bmsDischargeVoltageX10,
    );
    parallelMode = readInt('parallel_mode', parallelMode);
    parallelRole = snapshot['parallel_role'] as int?;
    parallelPhase = snapshot['parallel_phase'] as int?;
    markConfigChanged(updateRevision: false);
  }

  void markConfigChanged({bool updateRevision = true}) {
    if (updateRevision) configRev++;
    remoteControls['power_limit'] = powerLimit;
    remoteControls['charge_enable'] = bmsChargeEnabled == 1;
    remoteControls['discharge_enable'] = bmsDischargeEnabled == 1;
    remoteControls['max_charge_current'] = bmsMaxChargeCurrentX10 / 10.0;
    remoteControls['max_discharge_current'] = bmsMaxDischargeCurrentX10 / 10.0;
  }

  // 活动故障
  final Map<String, ActiveFault> _activeFaults = {};

  // 四舍五入辅助函数，返回数字类型（与Python round()一致）
  static double _r1(double v) => double.parse(v.toStringAsFixed(1));
  static double _r2(double v) => double.parse(v.toStringAsFixed(2));
  static double _r3(double v) => double.parse(v.toStringAsFixed(3));

  // ===== 状态枚举映射（与 Python arm_inverter_simulator_gui.py 一致）=====
  static const Map<String, int> workStateMap = {
    'standby': 0,
    'inverting': 1,
    'bypass': 2,
    'shutdown': 3,
    'fault': 4,
  };
  static const Map<String, int> mpptStateMap = {
    'tracking': 0,
    'scanning': 1,
    'standby': 1,
    'fault': 2,
  };
  static const Map<String, int> chargeStateMap = {
    'idle': 0,
    'charging': 1,
    'discharging': 2,
    'fault': 3,
  };

  /// 生成 V1.0.26.713 协议 payload
  /// - heartbeat: ac[8], bat[23], pv[7], sys[11], eng[12], cells[2]
  /// - info: 具名 JSON
  /// - config / parallel / three_phase / status
  Map<String, dynamic> toJson(String topic) {
    switch (topic) {
      case 'heartbeat':
        return _buildHeartbeat();
      case 'info':
        return _buildInfo();
      case 'config':
        return _buildConfig();
      case 'parallel':
        return _buildParallel();
      case 'three_phase':
        return _buildThreePhase();
      default:
        return {};
    }
  }

  Map<String, dynamic> _buildInfo() {
    return {
      'model': 'CS-I10-6k2',
      'manufacturer': '辰烁科技',
      'firmware_arm': firmwareArm,
      'firmware_esp': 'V1.2.0.20260629',
      'firmware_dsp': firmwareDsp,
      'firmware_bms': firmwareBms,
      'device_type': 'off_grid_inverter',
      'rated_power': ratedPower,
      'rated_voltage': ratedVoltage,
      'rated_frequency': ratedFreq,
      'battery_nominal_voltage': 51.2,
      'battery_type': batteryType,
      'cell_count': cellCount,
      'temp_sensor_count': tempSensorCount,
    };
  }

  Map<String, dynamic> _buildHeartbeat() {
    // AC 数组 [8]: voltage, current, active_power, apparent_power, frequency, pf, load_percent, thd_v
    final apparent = _r1(voltage * current);
    final pf = voltage * current > 0
        ? _r2((power / (voltage * current)).clamp(0, 1))
        : 0.0;
    final acArr = [
      _r1(voltage), // 0: ac_voltage
      _r2(current), // 1: ac_current
      _r1(power), // 2: ac_active_power
      apparent, // 3: ac_apparent_power
      _r2(frequency), // 4: ac_frequency
      pf, // 5: ac_power_factor
      _r1(loadPercent), // 6: load_percent
      _r1(acVoltageThd), // 7: ac_voltage_thd
    ];

    // Battery 数组 [23]
    final cellVoltMax = cellVoltages.isEmpty
        ? 0.0
        : cellVoltages.reduce((a, b) => a > b ? a : b);
    final cellVoltMin = cellVoltages.isEmpty
        ? 0.0
        : cellVoltages.reduce((a, b) => a < b ? a : b);
    final tempMax = cellTemps.isEmpty
        ? 0.0
        : cellTemps.reduce((a, b) => a > b ? a : b);
    final tempMin = cellTemps.isEmpty
        ? 0.0
        : cellTemps.reduce((a, b) => a < b ? a : b);
    final batAvgTemp = cellTemps.isEmpty
        ? 25.0
        : cellTemps.reduce((a, b) => a + b) / cellTemps.length;
    final batArr = [
      _r1(soc), // 0: battery_soc
      _r1(soh), // 1: battery_soh
      _r2(batteryVoltage), // 2: battery_voltage
      _r2(batteryCurrent), // 3: battery_current
      _r1(batteryVoltage * batteryCurrent), // 4: battery_power
      _r1(soc / 100.0 * batteryCapacity), // 5: capacity_remain
      batteryCapacity, // 6: capacity_total
      cycleCount, // 7: cycle_cnt
      _r1(tempMax), // 8: battery_temp_max
      _r1(tempMin), // 9: battery_temp_min
      _r3(cellVoltMax), // 10: cell_voltage_max
      _r3(cellVoltMin), // 11: cell_voltage_min
      _r3(cellVoltMax - cellVoltMin), // 12: cell_voltage_diff
      chargeStateMap[chargeState] ?? 0, // 13: battery_state
      protectStatus, // 14: protect_status
      bmsFaultCode, // 15: bms_fault_code
      _r1(maxChargeCurrent), // 16: max_charge_current
      _r1(maxDischargeCurrent), // 17: max_discharge_current
      _r1(chargeVoltageRef), // 18: charge_voltage_ref
      _r1(dischargeCutoffVoltage), // 19: discharge_cutoff_voltage
      _r1(batAvgTemp), // 20: battery_temperature
      (chargeRequestCurrent * 10).toInt(), // 21: charge_request_current_x10
      (chargeRequestVoltage * 10).toInt(), // 22: charge_request_voltage_x10
    ];

    // PV 数组 [7]: pv1_v, pv1_i, pv1_p, pv2_v, pv2_i, pv2_p, mppt_state
    final pvArr = [
      _r1(pv1Voltage), // 0: pv1_voltage
      _r2(pv1Current), // 1: pv1_current
      _r1(pv1Power), // 2: pv1_power
      _r1(pv2Voltage), // 3: pv2_voltage
      _r2(pv2Current), // 4: pv2_current
      _r1(pv2Power), // 5: pv2_power
      mpptStateMap[mpptState] ?? 0, // 6: mppt_state
    ];

    // System 数组 [11]
    final dcBus = _r1(batteryVoltage * 7.4);
    final fanPercent = (fanSpeed / 2400 * 100).clamp(0, 100).toInt();
    final sysArr = [
      workStateMap[mode.name] ?? 0, // 0: work_state
      faultCode, // 1: fault_code
      alarmCode, // 2: alarm_code
      _r1(tempInv), // 3: inverter_temperature
      _r1(tempMos), // 4: mos_temperature
      _r1(tempAmbient), // 5: ambient_temperature
      dcBus, // 6: dc_bus_voltage
      runtimeHours.toInt(), // 7: runtime_hours
      fanPercent, // 8: fan_speed_percent
      _r1(efficiency), // 9: efficiency
      systemMode, // 10: system_mode
    ];

    // Energy 数组 [12]
    final engArr = [
      _r2(dailyPv), // 0: daily_pv_energy
      _r1(totalPv), // 1: total_pv_energy
      _r2(dailyCharge), // 2: daily_charge_energy
      _r1(totalCharge), // 3: total_charge_energy
      _r2(dailyDischarge), // 4: daily_discharge_energy
      _r1(totalDischarge), // 5: total_discharge_energy
      _r2(dailyLoad), // 6: daily_load_energy
      _r1(totalLoad), // 7: total_load_energy
      _r1(totalChargeAh), // 8: total_charge_capacity (Ah)
      _r1(totalDischargeAh), // 9: total_discharge_capacity (Ah)
      totalChargeTime.toInt(), // 10: total_charge_time (s)
      totalDischargeTime.toInt(), // 11: total_discharge_time (s)
    ];

    // Cells 数组 [2]
    final cellsArr = [
      cellVoltages.map((v) => _r3(v)).toList(), // 0: cell voltages
      cellTemps.map((t) => _r1(t)).toList(), // 1: cell temperatures
    ];

    return {
      'ac': acArr,
      'bat': batArr,
      'pv': pvArr,
      'sys': sysArr,
      'eng': engArr,
      'cells': cellsArr,
    };
  }

  /// 协议 §7.3 Config payload
  Map<String, dynamic> _buildConfig() {
    return {
      'rev': configRev,
      'inv': [
        acEnabled,
        powerLimit,
        chargeLimit,
        dischargeLimit,
        socLowX10,
        socHighX10,
        forceCharge,
        forceDischarge,
      ],
      'bms': [
        bmsChargeEnabled,
        bmsDischargeEnabled,
        bmsMaxChargeCurrentX10,
        bmsMaxDischargeCurrentX10,
        bmsChargeVoltageX10,
        bmsDischargeVoltageX10,
      ],
      'parallel': [
        parallelEnabled ? 1 : 0,
        parallelMode,
        parallelRole,
        parallelPhase,
      ],
    };
  }

  /// 协议 §6.3 Parallel payload
  Map<String, dynamic> _buildParallel() {
    return {
      'enabled': parallelEnabled,
      'mode': switch (parallelMode) {
        1 => 'single_phase',
        2 => 'three_phase',
        _ => 'standalone',
      },
      'count': parallelEnabled ? 1 : 0,
      'total_rated_power': parallelEnabled ? ratedPower : 0,
      'total_active_power': parallelEnabled ? _r1(power) : 0,
      'sync_state': parallelEnabled ? 'synced' : 'idle',
      'machines': parallelEnabled
          ? [
              {
                'id': 0,
                'sn': 'H1CNA00135000014',
                'role': parallelRole == 1 ? 'slave' : 'master',
                'phase': switch (parallelPhase) {
                  1 => 'L1',
                  2 => 'L2',
                  3 => 'L3',
                  _ => null,
                },
                'power': _r1(power),
                'state': mode == InverterMode.inverting
                    ? 2
                    : (mode == InverterMode.fault ? 3 : 0),
              },
            ]
          : [],
    };
  }

  /// 协议 §6.4 Three-phase payload（仅三相主机使用，单机返回占位结构）
  Map<String, dynamic> _buildThreePhase() {
    return {
      'voltage': [_r1(voltage), _r1(voltage), _r1(voltage)],
      'current': [_r2(current), _r2(current), _r2(current)],
      'active_power': [_r1(power), _r1(power), _r1(power)],
      'total_active_power': _r1(power * 3),
      'line_voltage': [
        _r1(voltage * 1.732),
        _r1(voltage * 1.732),
        _r1(voltage * 1.732),
      ],
      'frequency': _r2(frequency),
      'voltage_unbalance': 0.0,
      'current_unbalance': 0.0,
    };
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

    return '已注入故障: ${fault.description}，持续$duration秒';
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
    final effectivePowerLimit = min(ratedPower, powerLimit).toDouble();

    // 3. 计算目标AC功率（带小幅度波动）
    final acNoise = 1.0 + random.nextDouble() * 0.1 - 0.05;
    double targetAc = 1800 * acNoise;
    targetAc = targetAc.clamp(0, effectivePowerLimit);

    // 充放电使能控制
    if (bmsChargeEnabled == 0 && targetPv > targetAc) {
      targetAc = targetPv;
    }
    if (bmsDischargeEnabled == 0 && targetPv < targetAc) {
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
      cellVoltages[i] =
          batteryVoltage / cellCount + random.nextDouble() * 0.02 - 0.01;
    }
    for (int i = 0; i < cellTemps.length; i++) {
      // 电芯温度基于逆变器温度，有小幅差异
      cellTemps[i] = tempInv - 5 + random.nextDouble() * 6 - 3;
    }

    // 11. 更新能量统计
    dailyPv += pvTotal * dt / 3600000;
    totalPv += pvTotal * dt / 3600000;
    if (batteryCurrent > 0) {
      dailyCharge += batteryVoltage * batteryCurrent * dt / 3600000;
      totalCharge += batteryVoltage * batteryCurrent * dt / 3600000;
      totalChargeAh += batteryCurrent * dt / 3600;
      totalChargeTime += dt;
    } else {
      dailyDischarge += batteryVoltage * batteryCurrent.abs() * dt / 3600000;
      totalDischarge += batteryVoltage * batteryCurrent.abs() * dt / 3600000;
      totalDischargeAh += batteryCurrent.abs() * dt / 3600;
      totalDischargeTime += dt;
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
