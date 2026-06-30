import 'package:flutter/material.dart';
import 'package:provider/provider.dart';
import '../providers/simulator_provider.dart';
import '../models/inverter_state.dart';
import '../models/alarm_system.dart';

class SimulatorScreen extends StatefulWidget {
  const SimulatorScreen({super.key});

  @override
  State<SimulatorScreen> createState() => _SimulatorScreenState();
}

class _SimulatorScreenState extends State<SimulatorScreen> {
  int _selectedTab = 0;

  @override
  Widget build(BuildContext context) {
    return Consumer<SimulatorProvider>(
      builder: (context, provider, _) {
        return Scaffold(
          body: Row(
            children: [
              // 左侧导航栏
              _buildNavigationRail(provider),
              // 主内容区
              Expanded(
                child: Column(
                  children: [
                    _buildTopBar(provider),
                    Expanded(
                      child: _selectedTab == 0
                          ? _buildDashboard(provider)
                          : _selectedTab == 1
                              ? _buildAlarmTab(provider)
                              : _buildSettingsTab(provider),
                    ),
                  ],
                ),
              ),
            ],
          ),
        );
      },
    );
  }

  Widget _buildNavigationRail(SimulatorProvider provider) {
    return Container(
      width: 72,
      decoration: BoxDecoration(
        color: const Color(0xFF0A1520),
        border: Border(
          right: BorderSide(color: Colors.white.withOpacity(0.1)),
        ),
      ),
      child: Column(
        children: [
          const SizedBox(height: 16),
          // Logo
          Container(
            width: 48,
            height: 48,
            decoration: BoxDecoration(
              gradient: const LinearGradient(
                colors: [Color(0xFF2C5364), Color(0xFF203A43)],
              ),
              borderRadius: BorderRadius.circular(12),
            ),
            child: const Icon(Icons.electric_bolt, color: Colors.white),
          ),
          const SizedBox(height: 8),
          const Text(
            'ARM\nSimulator',
            textAlign: TextAlign.center,
            style: TextStyle(fontSize: 10, color: Colors.grey),
          ),
          const SizedBox(height: 32),
          // 导航按钮
          _buildNavButton(0, Icons.dashboard, '仪表盘'),
          _buildNavButton(1, Icons.warning, '告警'),
          _buildNavButton(2, Icons.settings, '设置'),
          const Spacer(),
          // 连接状态
          Container(
            margin: const EdgeInsets.all(8),
            padding: const EdgeInsets.all(8),
            decoration: BoxDecoration(
              color: provider.isConnected
                  ? Colors.green.withOpacity(0.2)
                  : Colors.red.withOpacity(0.2),
              borderRadius: BorderRadius.circular(8),
            ),
            child: Column(
              children: [
                Icon(
                  provider.isConnected ? Icons.usb : Icons.usb_off,
                  color: provider.isConnected ? Colors.green : Colors.red,
                  size: 20,
                ),
                const SizedBox(height: 4),
                Text(
                  provider.isConnected ? '已连接' : '离线',
                  style: TextStyle(
                    fontSize: 10,
                    color: provider.isConnected ? Colors.green : Colors.red,
                  ),
                ),
              ],
            ),
          ),
          const SizedBox(height: 16),
        ],
      ),
    );
  }

  Widget _buildNavButton(int index, IconData icon, String label) {
    final isSelected = _selectedTab == index;
    return InkWell(
      onTap: () => setState(() => _selectedTab = index),
      child: Container(
        width: 56,
        padding: const EdgeInsets.symmetric(vertical: 12),
        decoration: BoxDecoration(
          color: isSelected ? Colors.white.withOpacity(0.1) : null,
          borderRadius: BorderRadius.circular(8),
        ),
        child: Column(
          children: [
            Icon(
              icon,
              color: isSelected ? Colors.white : Colors.grey,
              size: 24,
            ),
            const SizedBox(height: 4),
            Text(
              label,
              style: TextStyle(
                fontSize: 10,
                color: isSelected ? Colors.white : Colors.grey,
              ),
            ),
          ],
        ),
      ),
    );
  }

  Widget _buildTopBar(SimulatorProvider provider) {
    return Container(
      height: 56,
      padding: const EdgeInsets.symmetric(horizontal: 24),
      decoration: BoxDecoration(
        color: const Color(0xFF0A1520),
        border: Border(
          bottom: BorderSide(color: Colors.white.withOpacity(0.1)),
        ),
      ),
      child: Row(
        children: [
          const Text(
            'CS-I10-6k2 ARM模拟器',
            style: TextStyle(fontSize: 18, fontWeight: FontWeight.bold),
          ),
          const SizedBox(width: 16),
          Container(
            padding: const EdgeInsets.symmetric(horizontal: 8, vertical: 4),
            decoration: BoxDecoration(
              color: _getModeColor(provider.inverterData.mode).withOpacity(0.2),
              borderRadius: BorderRadius.circular(4),
            ),
            child: Text(
              _getModeText(provider.inverterData.mode),
              style: TextStyle(
                color: _getModeColor(provider.inverterData.mode),
                fontSize: 12,
              ),
            ),
          ),
          const Spacer(),
          // SN
          Text(
            'SN: ${provider.sn}',
            style: const TextStyle(color: Colors.grey, fontSize: 12),
          ),
          const SizedBox(width: 16),
          // 运行时间
          Text(
            '运行: ${provider.inverterData.runtimeHours.toStringAsFixed(1)}h',
            style: const TextStyle(color: Colors.grey, fontSize: 12),
          ),
        ],
      ),
    );
  }

  Widget _buildDashboard(SimulatorProvider provider) {
    final data = provider.inverterData;

    return SingleChildScrollView(
      padding: const EdgeInsets.all(16),
      child: Column(
        children: [
          // 顶部指标卡片
          Row(
            children: [
              Expanded(child: _buildMetricCard('功率', '${data.power.toStringAsFixed(0)}W', Icons.power, Colors.amber)),
              const SizedBox(width: 12),
              Expanded(child: _buildMetricCard('SOC', '${data.soc.toStringAsFixed(1)}%', Icons.battery_5_bar, Colors.green)),
              const SizedBox(width: 12),
              Expanded(child: _buildMetricCard('PV', '${(data.pv1Power + data.pv2Power).toStringAsFixed(0)}W', Icons.solar_power, Colors.orange)),
              const SizedBox(width: 12),
              Expanded(child: _buildMetricCard('温度', '${data.tempInv.toStringAsFixed(1)}°C', Icons.thermostat, data.tempInv > 60 ? Colors.red : Colors.blue)),
            ],
          ),
          const SizedBox(height: 16),
          // 中间详细数据
          Row(
            crossAxisAlignment: CrossAxisAlignment.start,
            children: [
              // 左侧 - 交流输出
              Expanded(
                child: _buildDataCard('交流输出', [
                  _buildDataRow('电压', '${data.voltage.toStringAsFixed(1)} V'),
                  _buildDataRow('电流', '${data.current.toStringAsFixed(2)} A'),
                  _buildDataRow('功率', '${data.power.toStringAsFixed(1)} W'),
                  _buildDataRow('频率', '${data.frequency.toStringAsFixed(2)} Hz'),
                  _buildDataRow('负载', '${data.loadPercent.toStringAsFixed(1)} %'),
                  _buildDataRow('功率因数', '${(data.power / (data.voltage * data.current)).clamp(0, 1).toStringAsFixed(2)}'),
                ]),
              ),
              const SizedBox(width: 12),
              // 中间 - 电池
              Expanded(
                child: _buildDataCard('电池系统', [
                  _buildDataRow('SOC', '${data.soc.toStringAsFixed(1)} %'),
                  _buildDataRow('SOH', '${data.soh.toStringAsFixed(1)} %'),
                  _buildDataRow('电压', '${data.batteryVoltage.toStringAsFixed(2)} V'),
                  _buildDataRow('电流', '${data.batteryCurrent.toStringAsFixed(2)} A'),
                  _buildDataRow('循环次数', '${data.cycleCount}'),
                  _buildDataRow('电芯压差', '${(data.cellVoltages.reduce((a, b) => a > b ? a : b) - data.cellVoltages.reduce((a, b) => a < b ? a : b)).toStringAsFixed(3)} V'),
                ]),
              ),
              const SizedBox(width: 12),
              // 右侧 - 光伏
              Expanded(
                child: _buildDataCard('光伏输入', [
                  _buildDataRow('PV1电压', '${data.pv1Voltage.toStringAsFixed(1)} V'),
                  _buildDataRow('PV1电流', '${data.pv1Current.toStringAsFixed(2)} A'),
                  _buildDataRow('PV1功率', '${data.pv1Power.toStringAsFixed(1)} W'),
                  _buildDataRow('PV2电压', '${data.pv2Voltage.toStringAsFixed(1)} V'),
                  _buildDataRow('PV2电流', '${data.pv2Current.toStringAsFixed(2)} A'),
                  _buildDataRow('PV2功率', '${data.pv2Power.toStringAsFixed(1)} W'),
                ]),
              ),
            ],
          ),
          const SizedBox(height: 16),
          // 底部 - 温度和状态
          Row(
            children: [
              Expanded(
                child: _buildDataCard('温度监测', [
                  _buildDataRow('逆变器', '${data.tempInv.toStringAsFixed(1)} °C'),
                  _buildDataRow('MOS管', '${data.tempMos.toStringAsFixed(1)} °C'),
                  _buildDataRow('环境', '${data.tempAmbient.toStringAsFixed(1)} °C'),
                ]),
              ),
              const SizedBox(width: 12),
              Expanded(
                child: _buildDataCard('运行状态', [
                  _buildDataRow('模式', _getModeText(data.mode)),
                  _buildDataRow('故障码', '${data.faultCode}'),
                  _buildDataRow('告警码', '${data.alarmCode}'),
                  _buildDataRow('效率', '${data.efficiency.toStringAsFixed(1)} %'),
                  _buildDataRow('风扇', '${data.fanSpeed} rpm'),
                  _buildDataRow('母线', '${(data.batteryVoltage * 1.2).toStringAsFixed(1)} V'),
                ]),
              ),
              const SizedBox(width: 12),
              Expanded(
                child: _buildDataCard('能量统计', [
                  _buildDataRow('今日PV', '${data.dailyPv.toStringAsFixed(1)} kWh'),
                  _buildDataRow('总PV', '${data.totalPv.toStringAsFixed(0)} kWh'),
                  _buildDataRow('今日充电', '${data.dailyCharge.toStringAsFixed(1)} kWh'),
                  _buildDataRow('今日放电', '${data.dailyDischarge.toStringAsFixed(1)} kWh'),
                  _buildDataRow('今日负载', '${data.dailyLoad.toStringAsFixed(1)} kWh'),
                  _buildDataRow('总负载', '${data.totalLoad.toStringAsFixed(0)} kWh'),
                ]),
              ),
            ],
          ),
        ],
      ),
    );
  }

  Widget _buildMetricCard(String label, String value, IconData icon, Color color) {
    return Container(
      padding: const EdgeInsets.all(16),
      decoration: BoxDecoration(
        gradient: LinearGradient(
          begin: Alignment.topLeft,
          end: Alignment.bottomRight,
          colors: [
            color.withOpacity(0.2),
            color.withOpacity(0.05),
          ],
        ),
        borderRadius: BorderRadius.circular(12),
        border: Border.all(color: color.withOpacity(0.3)),
      ),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Row(
            children: [
              Icon(icon, color: color, size: 20),
              const SizedBox(width: 8),
              Text(
                label,
                style: TextStyle(color: color.withOpacity(0.8), fontSize: 12),
              ),
            ],
          ),
          const SizedBox(height: 8),
          Text(
            value,
            style: TextStyle(
              color: color,
              fontSize: 24,
              fontWeight: FontWeight.bold,
            ),
          ),
        ],
      ),
    );
  }

  Widget _buildDataCard(String title, List<Widget> children) {
    return Container(
      padding: const EdgeInsets.all(16),
      decoration: BoxDecoration(
        color: const Color(0xFF1A2A3A),
        borderRadius: BorderRadius.circular(12),
        border: Border.all(color: Colors.white.withOpacity(0.1)),
      ),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Text(
            title,
            style: const TextStyle(
              color: Colors.white70,
              fontSize: 14,
              fontWeight: FontWeight.w500,
            ),
          ),
          const Divider(height: 16),
          ...children,
        ],
      ),
    );
  }

  Widget _buildDataRow(String label, String value) {
    return Padding(
      padding: const EdgeInsets.symmetric(vertical: 4),
      child: Row(
        mainAxisAlignment: MainAxisAlignment.spaceBetween,
        children: [
          Text(label, style: const TextStyle(color: Colors.grey, fontSize: 13)),
          Text(value, style: const TextStyle(fontWeight: FontWeight.w500, fontSize: 13)),
        ],
      ),
    );
  }

  Widget _buildAlarmTab(SimulatorProvider provider) {
    final alarmSystem = provider.alarmSystem;

    return Padding(
      padding: const EdgeInsets.all(16),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          // 告警触发按钮
          Container(
            padding: const EdgeInsets.all(16),
            decoration: BoxDecoration(
              color: const Color(0xFF1A2A3A),
              borderRadius: BorderRadius.circular(12),
              border: Border.all(color: Colors.white.withOpacity(0.1)),
            ),
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.start,
              children: [
                const Text('故障注入', style: TextStyle(fontWeight: FontWeight.bold)),
                const SizedBox(height: 12),
                Wrap(
                  spacing: 8,
                  runSpacing: 8,
                  children: [
                    _buildFaultButton('过温', 1, provider),
                    _buildFaultButton('过压', 2, provider),
                    _buildFaultButton('欠压', 3, provider),
                    _buildFaultButton('过载', 4, provider),
                    _buildFaultButton('母线过压', 5, provider),
                    _buildFaultButton('温度高', 6, provider, isWarning: true),
                    _buildFaultButton('SOC低', 7, provider, isWarning: true),
                    _buildFaultButton('PV异常', 8, provider, isWarning: true),
                    _buildFaultButton('压差大', 9, provider, isWarning: true),
                    const SizedBox(width: 16),
                    ElevatedButton.icon(
                      onPressed: () => provider.clearAllAlarms(),
                      icon: const Icon(Icons.clear_all),
                      label: const Text('清除全部'),
                      style: ElevatedButton.styleFrom(
                        backgroundColor: Colors.blue,
                      ),
                    ),
                  ],
                ),
              ],
            ),
          ),
          const SizedBox(height: 16),
          // 活动告警列表
          Expanded(
            child: Container(
              padding: const EdgeInsets.all(16),
              decoration: BoxDecoration(
                color: const Color(0xFF1A2A3A),
                borderRadius: BorderRadius.circular(12),
                border: Border.all(color: Colors.white.withOpacity(0.1)),
              ),
              child: Column(
                crossAxisAlignment: CrossAxisAlignment.start,
                children: [
                  Row(
                    children: [
                      const Text('活动告警', style: TextStyle(fontWeight: FontWeight.bold)),
                      const SizedBox(width: 8),
                      Container(
                        padding: const EdgeInsets.symmetric(horizontal: 8, vertical: 2),
                        decoration: BoxDecoration(
                          color: alarmSystem.activeAlarms.isNotEmpty
                              ? Colors.orange.withOpacity(0.2)
                              : Colors.green.withOpacity(0.2),
                          borderRadius: BorderRadius.circular(4),
                        ),
                        child: Text(
                          '${alarmSystem.activeAlarms.length}',
                          style: TextStyle(
                            color: alarmSystem.activeAlarms.isNotEmpty
                                ? Colors.orange
                                : Colors.green,
                            fontSize: 12,
                          ),
                        ),
                      ),
                    ],
                  ),
                  const Divider(),
                  Expanded(
                    child: alarmSystem.activeAlarms.isEmpty
                        ? const Center(
                            child: Column(
                              mainAxisAlignment: MainAxisAlignment.center,
                              children: [
                                Icon(Icons.check_circle, color: Colors.green, size: 48),
                                SizedBox(height: 16),
                                Text('系统正常，无活动告警', style: TextStyle(color: Colors.grey)),
                              ],
                            ),
                          )
                        : ListView.builder(
                            itemCount: alarmSystem.activeAlarms.length,
                            itemBuilder: (context, index) {
                              final code = alarmSystem.activeAlarms[index];
                              final def = AlarmSystem.alarmDefinitions[code];
                              final isManual = alarmSystem.isManualAlarm(code);
                              final color = _getAlarmColor(def?.level ?? 'info');

                              return Container(
                                margin: const EdgeInsets.only(bottom: 8),
                                padding: const EdgeInsets.all(12),
                                decoration: BoxDecoration(
                                  color: color.withOpacity(0.1),
                                  borderRadius: BorderRadius.circular(8),
                                  border: Border.all(color: color.withOpacity(0.3)),
                                ),
                                child: Row(
                                  children: [
                                    Icon(_getAlarmIcon(def?.level ?? 'info'), color: color, size: 20),
                                    const SizedBox(width: 12),
                                    Expanded(
                                      child: Column(
                                        crossAxisAlignment: CrossAxisAlignment.start,
                                        children: [
                                          Text(
                                            def?.message ?? '未知告警',
                                            style: TextStyle(color: color, fontWeight: FontWeight.w500),
                                          ),
                                          Text(
                                            '代码: $code ${isManual ? "• 手动触发" : "• 自动触发"}',
                                            style: const TextStyle(color: Colors.grey, fontSize: 11),
                                          ),
                                        ],
                                      ),
                                    ),
                                    IconButton(
                                      icon: const Icon(Icons.close, size: 16),
                                      onPressed: () {
                                        alarmSystem.removeAlarm(code);
                                        provider.updateUI();
                                      },
                                    ),
                                  ],
                                ),
                              );
                            },
                          ),
                  ),
                ],
              ),
            ),
          ),
        ],
      ),
    );
  }

  Widget _buildFaultButton(String label, int code, SimulatorProvider provider, {bool isWarning = false}) {
    final color = isWarning ? Colors.orange : Colors.red;
    return ElevatedButton(
      onPressed: () => provider.triggerAlarm(code),
      style: ElevatedButton.styleFrom(
        backgroundColor: color.withOpacity(0.2),
        foregroundColor: color,
        side: BorderSide(color: color.withOpacity(0.5)),
      ),
      child: Text(label),
    );
  }

  Widget _buildSettingsTab(SimulatorProvider provider) {
    return SingleChildScrollView(
      padding: const EdgeInsets.all(16),
      child: Column(
        children: [
          // 串口连接
          _buildSettingsCard('串口连接', [
            Padding(
              padding: const EdgeInsets.all(16),
              child: Column(
                children: [
                  Row(
                    children: [
                      Expanded(
                        child: DropdownButtonFormField<String>(
                          decoration: const InputDecoration(
                            labelText: '选择串口',
                            border: OutlineInputBorder(),
                          ),
                          value: provider.selectedPort,
                          items: provider.availablePorts
                              .map((port) => DropdownMenuItem(
                                    value: port,
                                    child: Text(port),
                                  ))
                              .toList(),
                          onChanged: (v) => provider.setSelectedPort(v),
                        ),
                      ),
                      const SizedBox(width: 16),
                      ElevatedButton.icon(
                        onPressed: () => provider.refreshPorts(),
                        icon: const Icon(Icons.refresh),
                        label: const Text('刷新'),
                      ),
                    ],
                  ),
                  const SizedBox(height: 16),
                  SizedBox(
                    width: double.infinity,
                    child: ElevatedButton.icon(
                      onPressed: provider.isConnected
                          ? () => provider.disconnect()
                          : () => provider.connect(),
                      icon: Icon(provider.isConnected ? Icons.usb_off : Icons.usb),
                      label: Text(provider.isConnected ? '断开连接' : '连接串口'),
                      style: ElevatedButton.styleFrom(
                        backgroundColor: provider.isConnected ? Colors.red : Colors.green,
                        padding: const EdgeInsets.symmetric(vertical: 12),
                      ),
                    ),
                  ),
                ],
              ),
            ),
          ]),
          const SizedBox(height: 16),
          _buildSettingsCard('模拟控制', [
            Padding(
              padding: const EdgeInsets.symmetric(horizontal: 16, vertical: 8),
              child: Row(
                mainAxisAlignment: MainAxisAlignment.spaceBetween,
                children: [
                  Column(
                    crossAxisAlignment: CrossAxisAlignment.start,
                    children: [
                      const Text('自动模拟', style: TextStyle(fontSize: 16)),
                      Text(
                        '自动更新逆变器参数',
                        style: TextStyle(fontSize: 12, color: Colors.grey[400]),
                      ),
                    ],
                  ),
                  Switch(
                    value: provider.autoSimulate,
                    onChanged: (v) => provider.setAutoSimulate(v),
                  ),
                ],
              ),
            ),
            Padding(
              padding: const EdgeInsets.symmetric(horizontal: 16, vertical: 8),
              child: Row(
                mainAxisAlignment: MainAxisAlignment.spaceBetween,
                children: [
                  Column(
                    crossAxisAlignment: CrossAxisAlignment.start,
                    children: [
                      const Text('自动发布', style: TextStyle(fontSize: 16)),
                      Text(
                        '定时通过UART发送数据',
                        style: TextStyle(fontSize: 12, color: Colors.grey[400]),
                      ),
                    ],
                  ),
                  Switch(
                    value: provider.autoPublish,
                    onChanged: (v) => provider.setAutoPublish(v),
                  ),
                ],
              ),
            ),
          ]),
          const SizedBox(height: 16),
          _buildSettingsCard('发布间隔', [
            Padding(
              padding: const EdgeInsets.all(16),
              child: Column(
                children: [
                  Text(
                    '${provider.publishInterval.toInt()} 秒',
                    style: const TextStyle(fontSize: 24, fontWeight: FontWeight.bold),
                  ),
                  Slider(
                    value: provider.publishInterval,
                    min: 5,
                    max: 300,
                    divisions: 59,
                    onChanged: (v) => provider.setPublishInterval(v),
                  ),
                  const Row(
                    mainAxisAlignment: MainAxisAlignment.spaceBetween,
                    children: [
                      Text('5秒', style: TextStyle(color: Colors.grey, fontSize: 12)),
                      Text('300秒', style: TextStyle(color: Colors.grey, fontSize: 12)),
                    ],
                  ),
                ],
              ),
            ),
          ]),
          const SizedBox(height: 16),
          _buildSettingsCard('告警概率', [
            Padding(
              padding: const EdgeInsets.all(16),
              child: Column(
                children: [
                  Text(
                    '${(provider.alarmProbability * 100).toStringAsFixed(2)}%',
                    style: const TextStyle(fontSize: 24, fontWeight: FontWeight.bold),
                  ),
                  Slider(
                    value: provider.alarmProbability * 1000,
                    min: 0,
                    max: 10,
                    divisions: 100,
                    onChanged: (v) => provider.setAlarmProbability(v / 1000),
                  ),
                ],
              ),
            ),
          ]),
          const SizedBox(height: 16),
          _buildSettingsCard('通信日志', [
            Container(
              height: 300,
              margin: const EdgeInsets.all(16),
              decoration: BoxDecoration(
                color: Colors.black,
                borderRadius: BorderRadius.circular(8),
              ),
              child: ListView.builder(
                reverse: true,
                padding: const EdgeInsets.all(8),
                itemCount: provider.logs.length,
                itemBuilder: (context, index) {
                  final log = provider.logs[index];
                  return Text(
                    log,
                    style: TextStyle(
                      fontFamily: 'Consolas',
                      fontSize: 12,
                      color: _getLogColor(log),
                    ),
                  );
                },
              ),
            ),
          ]),
        ],
      ),
    );
  }

  Widget _buildSettingsCard(String title, List<Widget> children) {
    return Container(
      decoration: BoxDecoration(
        color: const Color(0xFF1A2A3A),
        borderRadius: BorderRadius.circular(12),
        border: Border.all(color: Colors.white.withOpacity(0.1)),
      ),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Padding(
            padding: const EdgeInsets.fromLTRB(16, 16, 16, 0),
            child: Text(title, style: const TextStyle(fontWeight: FontWeight.bold)),
          ),
          ...children,
        ],
      ),
    );
  }

  String _getModeText(InverterMode mode) {
    switch (mode) {
      case InverterMode.inverting:
        return '逆变运行';
      case InverterMode.standby:
        return '待机';
      case InverterMode.fault:
        return '故障';
      case InverterMode.shutdown:
        return '关机';
      case InverterMode.bypass:
        return '旁路';
    }
  }

  Color _getModeColor(InverterMode mode) {
    switch (mode) {
      case InverterMode.inverting:
        return Colors.green;
      case InverterMode.standby:
        return Colors.blue;
      case InverterMode.fault:
        return Colors.red;
      case InverterMode.shutdown:
        return Colors.grey;
      case InverterMode.bypass:
        return Colors.orange;
    }
  }

  IconData _getAlarmIcon(String level) {
    switch (level) {
      case 'fault':
        return Icons.error;
      case 'warning':
        return Icons.warning;
      case 'info':
        return Icons.info;
      default:
        return Icons.help;
    }
  }

  Color _getAlarmColor(String level) {
    switch (level) {
      case 'fault':
        return Colors.red;
      case 'warning':
        return Colors.orange;
      case 'info':
        return Colors.blue;
      default:
        return Colors.grey;
    }
  }

  Color _getLogColor(String log) {
    if (log.contains('[TX]')) return Colors.green;
    if (log.contains('[RX]')) return Colors.blue;
    if (log.contains('[ERR]')) return Colors.red;
    if (log.contains('[WARN]')) return Colors.orange;
    if (log.contains('[ALARM]')) return Colors.yellow;
    return Colors.white70;
  }
}