import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:provider/provider.dart';
import '../providers/simulator_provider.dart';
import '../models/inverter_state.dart';
import '../models/alarm_system.dart';
import '../services/uart_service.dart';

class SimulatorScreen extends StatefulWidget {
  const SimulatorScreen({super.key});

  @override
  State<SimulatorScreen> createState() => _SimulatorScreenState();
}

class _SimulatorScreenState extends State<SimulatorScreen> {
  int _selectedTab = 0;

  // 颜色常量
  static const Color _bgDark = Color(0xFF0B1220);
  static const Color _bgCard = Color(0xFF151F2E);
  static const Color _border = Color(0xFF253545);
  static const Color _accent = Color(0xFF4FC3F7);

  @override
  Widget build(BuildContext context) {
    return Consumer<SimulatorProvider>(
      builder: (context, provider, _) {
        return Scaffold(
          backgroundColor: _bgDark,
          body: Row(
            children: [
              _buildNavigationRail(provider),
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
      width: 64,
      decoration: BoxDecoration(
        color: const Color(0xFF080E18),
        border: Border(right: BorderSide(color: _border, width: 0.5)),
      ),
      child: Column(
        children: [
          const SizedBox(height: 12),
          Container(
            width: 40,
            height: 40,
            decoration: BoxDecoration(
              gradient: const LinearGradient(
                colors: [Color(0xFF1976D2), Color(0xFF0D47A1)],
              ),
              borderRadius: BorderRadius.circular(10),
            ),
            child: const Icon(Icons.electric_bolt, color: Colors.white, size: 22),
          ),
          const SizedBox(height: 6),
          const Text('ARM Sim', style: TextStyle(fontSize: 9, color: Colors.grey)),
          const SizedBox(height: 24),
          _buildNavButton(0, Icons.dashboard_rounded, '仪表盘'),
          _buildNavButton(1, Icons.warning_rounded, '告警'),
          _buildNavButton(2, Icons.settings_rounded, '设置'),
          const Spacer(),
          Container(
            margin: const EdgeInsets.symmetric(horizontal: 8, vertical: 4),
            padding: const EdgeInsets.symmetric(vertical: 8),
            decoration: BoxDecoration(
              color: provider.isConnected
                  ? Colors.green.withOpacity(0.15)
                  : Colors.red.withOpacity(0.15),
              borderRadius: BorderRadius.circular(8),
            ),
            child: Column(
              children: [
                Icon(
                  provider.isConnected
                      ? (provider.isMqttConnected ? Icons.cloud_done : Icons.usb)
                      : Icons.link_off,
                  color: provider.isConnected ? Colors.greenAccent : Colors.redAccent,
                  size: 18,
                ),
                const SizedBox(height: 2),
                Text(
                  provider.isConnected ? '已连接' : '离线',
                  style: TextStyle(
                    fontSize: 9,
                    color: provider.isConnected ? Colors.greenAccent : Colors.redAccent,
                  ),
                ),
              ],
            ),
          ),
          const SizedBox(height: 12),
        ],
      ),
    );
  }

  Widget _buildNavButton(int index, IconData icon, String label) {
    final isSelected = _selectedTab == index;
    return InkWell(
      onTap: () => setState(() => _selectedTab = index),
      child: Container(
        width: 52,
        padding: const EdgeInsets.symmetric(vertical: 10),
        margin: const EdgeInsets.symmetric(vertical: 2),
        decoration: BoxDecoration(
          color: isSelected ? _accent.withOpacity(0.15) : null,
          borderRadius: BorderRadius.circular(8),
          border: isSelected
              ? Border.all(color: _accent.withOpacity(0.3), width: 0.5)
              : null,
        ),
        child: Column(
          children: [
            Icon(icon, color: isSelected ? _accent : Colors.grey, size: 20),
            const SizedBox(height: 3),
            Text(
              label,
              style: TextStyle(
                fontSize: 9,
                color: isSelected ? _accent : Colors.grey,
                fontWeight: isSelected ? FontWeight.w600 : FontWeight.normal,
              ),
            ),
          ],
        ),
      ),
    );
  }

  Widget _buildTopBar(SimulatorProvider provider) {
    return Container(
      height: 52,
      padding: const EdgeInsets.symmetric(horizontal: 20),
      decoration: BoxDecoration(
        color: const Color(0xFF0D1520),
        border: Border(bottom: BorderSide(color: _border, width: 0.5)),
      ),
      child: Row(
        children: [
          const Text(
            'CS-I10-6k2 ARM模拟器',
            style: TextStyle(fontSize: 16, fontWeight: FontWeight.w600, color: Colors.white),
          ),
          const SizedBox(width: 12),
          Container(
            padding: const EdgeInsets.symmetric(horizontal: 8, vertical: 3),
            decoration: BoxDecoration(
              color: _getModeColor(provider.inverterData.mode).withOpacity(0.15),
              borderRadius: BorderRadius.circular(4),
              border: Border.all(
                color: _getModeColor(provider.inverterData.mode).withOpacity(0.3),
              ),
            ),
            child: Text(
              _getModeText(provider.inverterData.mode),
              style: TextStyle(
                color: _getModeColor(provider.inverterData.mode),
                fontSize: 11,
                fontWeight: FontWeight.w500,
              ),
            ),
          ),
          const Spacer(),
          // SN 显示
          Container(
            padding: const EdgeInsets.symmetric(horizontal: 8, vertical: 3),
            decoration: BoxDecoration(
              color: _bgCard,
              borderRadius: BorderRadius.circular(4),
            ),
            child: Row(
              mainAxisSize: MainAxisSize.min,
              children: [
                const Icon(Icons.badge_outlined, size: 12, color: Colors.grey),
                const SizedBox(width: 4),
                Text(
                  provider.sn,
                  style: const TextStyle(color: Colors.grey, fontSize: 11, fontFamily: 'Consolas'),
                ),
              ],
            ),
          ),
          const SizedBox(width: 12),
          Text(
            '${provider.inverterData.runtimeHours.toStringAsFixed(1)}h',
            style: const TextStyle(color: Colors.grey, fontSize: 11),
          ),
        ],
      ),
    );
  }

  Widget _buildDashboard(SimulatorProvider provider) {
    final data = provider.inverterData;
    final pvTotal = data.pv1Power + data.pv2Power;
    final batPower = data.batteryVoltage * data.batteryCurrent;

    return SingleChildScrollView(
      padding: const EdgeInsets.all(14),
      child: Column(
        children: [
          // 顶部指标卡片
          Row(
            children: [
              Expanded(child: _buildMetricCard(
                'AC功率', '${data.power.toStringAsFixed(0)}W',
                Icons.power_rounded, Colors.amber,
              )),
              const SizedBox(width: 10),
              Expanded(child: _buildMetricCard(
                'SOC', '${data.soc.toStringAsFixed(1)}%',
                Icons.battery_charging_full_rounded,
                data.soc > 50 ? Colors.green : data.soc > 20 ? Colors.orange : Colors.red,
              )),
              const SizedBox(width: 10),
              Expanded(child: _buildMetricCard(
                'PV功率', '${pvTotal.toStringAsFixed(0)}W',
                Icons.solar_power_rounded, Colors.orange,
              )),
              const SizedBox(width: 10),
              Expanded(child: _buildMetricCard(
                '电池功率', '${batPower.toStringAsFixed(0)}W',
                Icons.battery_std_rounded,
                batPower > 0 ? Colors.green : Colors.blue,
              )),
            ],
          ),
          const SizedBox(height: 12),
          Row(
            crossAxisAlignment: CrossAxisAlignment.start,
            children: [
              Expanded(child: _buildDataCard('交流输出', [
                _buildDataRow('电压', '${data.voltage.toStringAsFixed(1)} V'),
                _buildDataRow('电流', '${data.current.toStringAsFixed(2)} A'),
                _buildDataRow('有功功率', '${data.power.toStringAsFixed(1)} W'),
                _buildDataRow('频率', '${data.frequency.toStringAsFixed(2)} Hz'),
                _buildDataRow('负载率', '${data.loadPercent.toStringAsFixed(1)} %'),
                _buildDataRow('功率因数', data.voltage * data.current > 0
                    ? (data.power / (data.voltage * data.current)).clamp(0, 1).toStringAsFixed(3)
                    : '0.000'),
              ])),
              const SizedBox(width: 10),
              Expanded(child: _buildDataCard('电池系统', [
                _buildDataRow('SOC / SOH', '${data.soc.toStringAsFixed(1)}% / ${data.soh.toStringAsFixed(1)}%'),
                _buildDataRow('电压', '${data.batteryVoltage.toStringAsFixed(2)} V'),
                _buildDataRow('电流', '${data.batteryCurrent.toStringAsFixed(2)} A'),
                _buildDataRow('充放电状态', _getChargeStateText(data.chargeState)),
                _buildDataRow('循环次数', '${data.cycleCount}'),
                _buildDataRow('电芯压差', '${(data.cellVoltages.reduce((a, b) => a > b ? a : b) - data.cellVoltages.reduce((a, b) => a < b ? a : b)).toStringAsFixed(3)} V'),
              ])),
              const SizedBox(width: 10),
              Expanded(child: _buildDataCard('光伏输入', [
                _buildDataRow('PV1', '${data.pv1Voltage.toStringAsFixed(1)}V × ${data.pv1Current.toStringAsFixed(2)}A = ${data.pv1Power.toStringAsFixed(0)}W'),
                _buildDataRow('PV2', '${data.pv2Voltage.toStringAsFixed(1)}V × ${data.pv2Current.toStringAsFixed(2)}A = ${data.pv2Power.toStringAsFixed(0)}W'),
                _buildDataRow('MPPT状态', _getMpptStateText(data.mpptState)),
                _buildDataRow('总功率', '${pvTotal.toStringAsFixed(0)} W'),
              ])),
            ],
          ),
          const SizedBox(height: 12),
          Row(
            children: [
              Expanded(child: _buildDataCard('温度监测', [
                _buildDataRow('逆变器', '${data.tempInv.toStringAsFixed(1)} °C',
                  color: data.tempInv > 60 ? Colors.red : null),
                _buildDataRow('MOS管', '${data.tempMos.toStringAsFixed(1)} °C'),
                _buildDataRow('环境', '${data.tempAmbient.toStringAsFixed(1)} °C'),
                _buildDataRow('电芯平均', '${data.cellTemps.isNotEmpty
                    ? (data.cellTemps.reduce((a, b) => a + b) / data.cellTemps.length).toStringAsFixed(1)
                    : "0.0"} °C'),
              ])),
              const SizedBox(width: 10),
              Expanded(child: _buildDataCard('运行状态', [
                _buildDataRow('工作模式', _getModeText(data.mode)),
                _buildDataRow('故障码', '${data.faultCode}',
                  color: data.faultCode != 0 ? Colors.red : null),
                _buildDataRow('告警码', '${data.alarmCode}',
                  color: data.alarmCode != 0 ? Colors.orange : null),
                _buildDataRow('效率', '${data.efficiency.toStringAsFixed(1)} %'),
                _buildDataRow('风扇', '${data.fanSpeed} rpm'),
                _buildDataRow('母线', '${(data.batteryVoltage * 7.4).toStringAsFixed(1)} V'),
              ])),
              const SizedBox(width: 10),
              Expanded(child: _buildDataCard('能量统计', [
                _buildDataRow('今日PV', '${data.dailyPv.toStringAsFixed(2)} kWh'),
                _buildDataRow('累计PV', '${data.totalPv.toStringAsFixed(0)} kWh'),
                _buildDataRow('今日充电', '${data.dailyCharge.toStringAsFixed(2)} kWh'),
                _buildDataRow('今日放电', '${data.dailyDischarge.toStringAsFixed(2)} kWh'),
                _buildDataRow('今日负载', '${data.dailyLoad.toStringAsFixed(2)} kWh'),
                _buildDataRow('累计负载', '${data.totalLoad.toStringAsFixed(0)} kWh'),
              ])),
            ],
          ),
        ],
      ),
    );
  }

  Widget _buildMetricCard(String label, String value, IconData icon, Color color) {
    return Container(
      padding: const EdgeInsets.all(14),
      decoration: BoxDecoration(
        gradient: LinearGradient(
          begin: Alignment.topLeft,
          end: Alignment.bottomRight,
          colors: [color.withOpacity(0.12), color.withOpacity(0.03)],
        ),
        borderRadius: BorderRadius.circular(10),
        border: Border.all(color: color.withOpacity(0.2)),
      ),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Row(
            children: [
              Icon(icon, color: color, size: 18),
              const SizedBox(width: 6),
              Text(label, style: TextStyle(color: Colors.grey[400], fontSize: 11)),
            ],
          ),
          const SizedBox(height: 6),
          Text(
            value,
            style: TextStyle(
              color: color,
              fontSize: 22,
              fontWeight: FontWeight.bold,
            ),
          ),
        ],
      ),
    );
  }

  Widget _buildDataCard(String title, List<Widget> children) {
    return Container(
      padding: const EdgeInsets.all(14),
      decoration: BoxDecoration(
        color: _bgCard,
        borderRadius: BorderRadius.circular(10),
        border: Border.all(color: _border, width: 0.5),
      ),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Text(
            title,
            style: const TextStyle(
              color: Colors.white70,
              fontSize: 13,
              fontWeight: FontWeight.w600,
            ),
          ),
          const SizedBox(height: 8),
          Container(height: 0.5, color: _border),
          const SizedBox(height: 6),
          ...children,
        ],
      ),
    );
  }

  Widget _buildDataRow(String label, String value, {Color? color}) {
    return Padding(
      padding: const EdgeInsets.symmetric(vertical: 3),
      child: Row(
        mainAxisAlignment: MainAxisAlignment.spaceBetween,
        children: [
          Text(label, style: TextStyle(color: Colors.grey[500], fontSize: 12)),
          Text(
            value,
            style: TextStyle(
              fontWeight: FontWeight.w500,
              fontSize: 12,
              color: color ?? Colors.white,
            ),
          ),
        ],
      ),
    );
  }

  Widget _buildAlarmTab(SimulatorProvider provider) {
    final alarmSystem = provider.alarmSystem;
    return Padding(
      padding: const EdgeInsets.all(14),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          _buildSettingsCard('故障注入', [
            Padding(
              padding: const EdgeInsets.all(14),
              child: Wrap(
                spacing: 6,
                runSpacing: 6,
                children: [
                  for (var entry in [
                    ('母线过压', 1), ('母线欠压', 2), ('输出过压', 3),
                    ('输出欠压', 4), ('过频', 5), ('欠频', 6),
                    ('孤岛保护', 7), ('过温保护', 8), ('PV过压', 9),
                    ('输出过载', 10), ('绝缘阻抗低', 11), ('输出短路', 12),
                  ])
                    _buildFaultButton(entry.$1, entry.$2, provider),
                  const SizedBox(width: 8),
                  ElevatedButton.icon(
                    onPressed: () => provider.clearAllAlarms(),
                    icon: const Icon(Icons.clear_all, size: 16),
                    label: const Text('清除全部', style: TextStyle(fontSize: 12)),
                    style: ElevatedButton.styleFrom(
                      backgroundColor: Colors.blue.withOpacity(0.2),
                      foregroundColor: Colors.blue,
                      side: BorderSide(color: Colors.blue.withOpacity(0.3)),
                    ),
                  ),
                ],
              ),
            ),
          ]),
          const SizedBox(height: 12),
          Expanded(
            child: _buildSettingsCard('活动告警 (${alarmSystem.activeAlarms.length})', [
              Expanded(
                child: alarmSystem.activeAlarms.isEmpty
                    ? const Center(
                        child: Column(
                          mainAxisAlignment: MainAxisAlignment.center,
                          children: [
                            Icon(Icons.check_circle_outline, color: Colors.green, size: 40),
                            SizedBox(height: 12),
                            Text('系统正常，无活动告警', style: TextStyle(color: Colors.grey, fontSize: 13)),
                          ],
                        ),
                      )
                    : ListView.builder(
                        padding: const EdgeInsets.all(8),
                        itemCount: alarmSystem.activeAlarms.length,
                        itemBuilder: (context, index) {
                          final code = alarmSystem.activeAlarms[index];
                          final def = AlarmSystem.alarmDefinitions[code];
                          final isManual = alarmSystem.isManualAlarm(code);
                          final color = _getAlarmColor(def?.level ?? 'info');
                          return Container(
                            margin: const EdgeInsets.only(bottom: 6),
                            padding: const EdgeInsets.all(10),
                            decoration: BoxDecoration(
                              color: color.withOpacity(0.08),
                              borderRadius: BorderRadius.circular(8),
                              border: Border.all(color: color.withOpacity(0.2)),
                            ),
                            child: Row(
                              children: [
                                Icon(_getAlarmIcon(def?.level ?? 'info'), color: color, size: 18),
                                const SizedBox(width: 10),
                                Expanded(
                                  child: Column(
                                    crossAxisAlignment: CrossAxisAlignment.start,
                                    children: [
                                      Text(def?.message ?? '未知告警',
                                        style: TextStyle(color: color, fontWeight: FontWeight.w500, fontSize: 12)),
                                      Text('代码: $code ${isManual ? "• 手动" : "• 自动"}',
                                        style: TextStyle(color: Colors.grey[600], fontSize: 10)),
                                    ],
                                  ),
                                ),
                                IconButton(
                                  icon: const Icon(Icons.close, size: 14),
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
            ]),
          ),
        ],
      ),
    );
  }

  Widget _buildFaultButton(String label, int code, SimulatorProvider provider) {
    return ElevatedButton(
      onPressed: () => provider.triggerAlarm(code),
      style: ElevatedButton.styleFrom(
        backgroundColor: Colors.red.withOpacity(0.12),
        foregroundColor: Colors.redAccent,
        side: BorderSide(color: Colors.red.withOpacity(0.25)),
        padding: const EdgeInsets.symmetric(horizontal: 10, vertical: 6),
        minimumSize: Size.zero,
        tapTargetSize: MaterialTapTargetSize.shrinkWrap,
      ),
      child: Text(label, style: const TextStyle(fontSize: 11)),
    );
  }

  Widget _buildSettingsTab(SimulatorProvider provider) {
    return SingleChildScrollView(
      padding: const EdgeInsets.all(14),
      child: Column(
        children: [
          // === 设备标识（始终可见）===
          _buildSettingsCard('设备标识', [
            Padding(
              padding: const EdgeInsets.all(14),
              child: Row(
                children: [
                  Expanded(
                    child: TextFormField(
                      initialValue: provider.sn,
                      enabled: !provider.isMqttConnected,
                      decoration: InputDecoration(
                        labelText: '设备 SN',
                        hintText: '输入序列号',
                        prefixIcon: const Icon(Icons.badge_outlined, size: 20),
                        border: OutlineInputBorder(borderRadius: BorderRadius.circular(8)),
                        isDense: true,
                      ),
                      onChanged: provider.setSn,
                    ),
                  ),
                  const SizedBox(width: 12),
                  ElevatedButton.icon(
                    onPressed: provider.isConnected
                        ? () => _sendSnViaUart(provider)
                        : null,
                    icon: const Icon(Icons.send, size: 16),
                    label: const Text('发送到ESP32'),
                    style: ElevatedButton.styleFrom(
                      backgroundColor: _accent.withOpacity(0.15),
                      foregroundColor: _accent,
                      side: BorderSide(color: _accent.withOpacity(0.3)),
                    ),
                  ),
                ],
              ),
            ),
          ]),
          const SizedBox(height: 12),
          // === 连接方式 ===
          _buildSettingsCard('连接方式', [
            Padding(
              padding: const EdgeInsets.all(14),
              child: SizedBox(
                width: double.infinity,
                child: SegmentedButton<bool>(
                  segments: const [
                    ButtonSegment(value: false, icon: Icon(Icons.usb), label: Text('串口 ESP32')),
                    ButtonSegment(value: true, icon: Icon(Icons.cloud), label: Text('直连 MQTT')),
                  ],
                  selected: {provider.directMqtt},
                  onSelectionChanged: !provider.settingsLoaded || provider.isConnected || provider.mqttConnecting
                      ? null
                      : (values) => provider.setDirectMqtt(values.first),
                ),
              ),
            ),
          ]),
          const SizedBox(height: 12),
          if (!provider.directMqtt)
            _buildSettingsCard('串口连接', [
              Padding(
                padding: const EdgeInsets.all(14),
                child: Column(
                  children: [
                    Row(
                      children: [
                        Expanded(
                          child: DropdownButtonFormField<String>(
                            decoration: InputDecoration(
                              labelText: '选择串口',
                              border: OutlineInputBorder(borderRadius: BorderRadius.circular(8)),
                              isDense: true,
                            ),
                            value: provider.selectedPort,
                            items: provider.availablePorts
                                .map((port) => DropdownMenuItem(value: port, child: Text(port)))
                                .toList(),
                            onChanged: (v) => provider.setSelectedPort(v),
                          ),
                        ),
                        const SizedBox(width: 12),
                        IconButton(
                          onPressed: () => provider.refreshPorts(),
                          icon: const Icon(Icons.refresh),
                          tooltip: '刷新串口',
                        ),
                      ],
                    ),
                    const SizedBox(height: 12),
                    SizedBox(
                      width: double.infinity,
                      height: 44,
                      child: FilledButton.icon(
                        onPressed: !provider.settingsLoaded
                            ? null
                            : provider.isConnected
                            ? () => provider.disconnect()
                            : () => provider.connect(),
                        icon: Icon(provider.isConnected ? Icons.usb_off : Icons.usb, size: 18),
                        label: Text(provider.isConnected ? '断开连接' : '连接串口'),
                        style: FilledButton.styleFrom(
                          backgroundColor: provider.isConnected ? Colors.red.shade700 : Colors.green.shade700,
                        ),
                      ),
                    ),
                  ],
                ),
              ),
            ]),
          if (provider.directMqtt)
            _buildSettingsCard('MQTT 连接', [
              Padding(
                padding: const EdgeInsets.all(14),
                child: Column(
                  children: [
                    Row(
                      children: [
                        Expanded(
                          flex: 3,
                          child: TextFormField(
                            initialValue: provider.mqttHost,
                            enabled: !provider.isMqttConnected,
                            decoration: InputDecoration(
                              labelText: 'Broker 地址',
                              prefixIcon: const Icon(Icons.dns_outlined, size: 20),
                              border: OutlineInputBorder(borderRadius: BorderRadius.circular(8)),
                              isDense: true,
                            ),
                            onChanged: provider.setMqttHost,
                          ),
                        ),
                        const SizedBox(width: 10),
                        Expanded(
                          child: TextFormField(
                            key: ValueKey(provider.mqttPort),
                            initialValue: provider.mqttPort.toString(),
                            enabled: !provider.isMqttConnected,
                            keyboardType: TextInputType.number,
                            inputFormatters: [FilteringTextInputFormatter.digitsOnly],
                            decoration: InputDecoration(
                              labelText: '端口',
                              border: OutlineInputBorder(borderRadius: BorderRadius.circular(8)),
                              isDense: true,
                            ),
                            onChanged: provider.setMqttPort,
                          ),
                        ),
                      ],
                    ),
                    const SizedBox(height: 10),
                    Row(
                      children: [
                        Expanded(
                          child: TextFormField(
                            initialValue: provider.mqttUsername,
                            enabled: !provider.isMqttConnected,
                            decoration: InputDecoration(
                              labelText: '用户名（可选）',
                              prefixIcon: const Icon(Icons.person_outline, size: 20),
                              border: OutlineInputBorder(borderRadius: BorderRadius.circular(8)),
                              isDense: true,
                            ),
                            onChanged: provider.setMqttUsername,
                          ),
                        ),
                        const SizedBox(width: 10),
                        Expanded(
                          child: TextFormField(
                            initialValue: provider.mqttPassword,
                            enabled: !provider.isMqttConnected,
                            obscureText: true,
                            decoration: InputDecoration(
                              labelText: '密码（可选）',
                              prefixIcon: const Icon(Icons.lock_outline, size: 20),
                              border: OutlineInputBorder(borderRadius: BorderRadius.circular(8)),
                              isDense: true,
                            ),
                            onChanged: provider.setMqttPassword,
                          ),
                        ),
                      ],
                    ),
                    SwitchListTile(
                      contentPadding: EdgeInsets.zero,
                      title: const Text('TLS 加密连接', style: TextStyle(fontSize: 14)),
                      subtitle: Text(provider.mqttTls ? '使用系统可信 CA' : '明文 TCP',
                          style: TextStyle(fontSize: 11, color: Colors.grey[500])),
                      value: provider.mqttTls,
                      onChanged: provider.isMqttConnected ? null : provider.setMqttTls,
                    ),
                    const SizedBox(height: 8),
                    SizedBox(
                      width: double.infinity,
                      height: 44,
                      child: FilledButton.icon(
                        onPressed: !provider.settingsLoaded || provider.mqttConnecting
                            ? null
                            : provider.isMqttConnected
                            ? provider.disconnectMqtt
                            : provider.connectMqtt,
                        icon: provider.mqttConnecting
                            ? const SizedBox(width: 16, height: 16, child: CircularProgressIndicator(strokeWidth: 2))
                            : Icon(provider.isMqttConnected ? Icons.cloud_off : Icons.cloud_queue, size: 18),
                        label: Text(provider.mqttConnecting
                            ? '连接中'
                            : provider.isMqttConnected ? '断开 MQTT' : '连接 MQTT'),
                      ),
                    ),
                  ],
                ),
              ),
            ]),
          const SizedBox(height: 12),
          // === 设备信息 ===
          _buildSettingsCard('固件版本', [
            Padding(
              padding: const EdgeInsets.all(14),
              child: Column(
                children: [
                  _buildVersionField('ARM固件', provider.inverterData.firmwareArm, provider.setFirmwareArm),
                  const SizedBox(height: 10),
                  _buildVersionField('DSP固件', provider.inverterData.firmwareDsp, provider.setFirmwareDsp),
                  const SizedBox(height: 10),
                  _buildVersionField('BMS固件', provider.inverterData.firmwareBms, provider.setFirmwareBms),
                ],
              ),
            ),
          ]),
          const SizedBox(height: 12),
          // === 模拟控制 ===
          _buildSettingsCard('模拟控制', [
            Padding(
              padding: const EdgeInsets.symmetric(horizontal: 14, vertical: 6),
              child: Row(
                mainAxisAlignment: MainAxisAlignment.spaceBetween,
                children: [
                  Column(
                    crossAxisAlignment: CrossAxisAlignment.start,
                    children: [
                      const Text('自动模拟', style: TextStyle(fontSize: 14)),
                      Text('基于能量守恒的物理模型', style: TextStyle(fontSize: 11, color: Colors.grey[500])),
                    ],
                  ),
                  Switch(value: provider.autoSimulate, onChanged: (v) => provider.setAutoSimulate(v)),
                ],
              ),
            ),
            Padding(
              padding: const EdgeInsets.symmetric(horizontal: 14, vertical: 6),
              child: Row(
                mainAxisAlignment: MainAxisAlignment.spaceBetween,
                children: [
                  Column(
                    crossAxisAlignment: CrossAxisAlignment.start,
                    children: [
                      const Text('自动发布', style: TextStyle(fontSize: 14)),
                      Text(provider.directMqtt ? '定时发布到 MQTT' : '定时通过 UART 发送',
                          style: TextStyle(fontSize: 11, color: Colors.grey[500])),
                    ],
                  ),
                  Switch(value: provider.autoPublish, onChanged: (v) => provider.setAutoPublish(v)),
                ],
              ),
            ),
            Padding(
              padding: const EdgeInsets.all(14),
              child: SizedBox(
                width: double.infinity,
                child: OutlinedButton.icon(
                  onPressed: provider.isConnected ? () => _manualPublish(provider) : null,
                  icon: const Icon(Icons.send, size: 16),
                  label: const Text('手动发布一次'),
                  style: OutlinedButton.styleFrom(
                    foregroundColor: _accent,
                    side: BorderSide(color: _accent.withOpacity(0.3)),
                  ),
                ),
              ),
            ),
          ]),
          const SizedBox(height: 12),
          // === 发布间隔 ===
          _buildSettingsCard('发布间隔', [
            Padding(
              padding: const EdgeInsets.all(14),
              child: Column(
                children: [
                  Row(
                    mainAxisAlignment: MainAxisAlignment.center,
                    children: [
                      Text('${provider.publishInterval.toInt()}',
                        style: const TextStyle(fontSize: 24, fontWeight: FontWeight.bold, color: _accent)),
                      const Text(' 秒', style: TextStyle(fontSize: 14, color: Colors.grey)),
                    ],
                  ),
                  Slider(
                    value: provider.publishInterval,
                    min: 5, max: 300, divisions: 59,
                    activeColor: _accent,
                    onChanged: (v) => provider.setPublishInterval(v),
                  ),
                ],
              ),
            ),
          ]),
          const SizedBox(height: 12),
          // === 告警概率 ===
          _buildSettingsCard('随机告警概率', [
            Padding(
              padding: const EdgeInsets.all(14),
              child: Column(
                children: [
                  Text('${(provider.alarmProbability * 100).toStringAsFixed(2)}%',
                    style: const TextStyle(fontSize: 24, fontWeight: FontWeight.bold, color: _accent)),
                  Slider(
                    value: provider.alarmProbability * 1000,
                    min: 0, max: 10, divisions: 100,
                    activeColor: _accent,
                    onChanged: (v) => provider.setAlarmProbability(v / 1000),
                  ),
                ],
              ),
            ),
          ]),
          const SizedBox(height: 12),
          // === 通信日志 ===
          _buildSettingsCard('通信日志', [
            Padding(
              padding: const EdgeInsets.fromLTRB(14, 8, 14, 0),
              child: Row(
                children: [
                  const Spacer(),
                  TextButton.icon(
                    onPressed: () {
                      Clipboard.setData(ClipboardData(text: provider.logs.reversed.join('\n')));
                      ScaffoldMessenger.of(context).showSnackBar(
                        const SnackBar(content: Text('已复制到剪贴板'), duration: Duration(seconds: 1)));
                    },
                    icon: const Icon(Icons.copy, size: 14),
                    label: const Text('复制', style: TextStyle(fontSize: 11)),
                  ),
                  TextButton.icon(
                    onPressed: () => provider.clearLogs(),
                    icon: const Icon(Icons.delete_outline, size: 14),
                    label: const Text('清空', style: TextStyle(fontSize: 11)),
                  ),
                ],
              ),
            ),
            Container(
              height: 280,
              margin: const EdgeInsets.all(14),
              decoration: BoxDecoration(
                color: Colors.black87,
                borderRadius: BorderRadius.circular(8),
                border: Border.all(color: _border, width: 0.5),
              ),
              child: ListView.builder(
                reverse: true,
                padding: const EdgeInsets.all(8),
                itemCount: provider.logs.length,
                itemBuilder: (context, index) {
                  final log = provider.logs[index];
                  return SelectableText(
                    log,
                    style: TextStyle(fontFamily: 'Consolas', fontSize: 11, color: _getLogColor(log)),
                  );
                },
              ),
            ),
          ]),
        ],
      ),
    );
  }

  void _sendSnViaUart(SimulatorProvider provider) {
    if (!provider.isSerialConnected) {
      ScaffoldMessenger.of(context).showSnackBar(
        const SnackBar(content: Text('请先连接串口'), duration: Duration(seconds: 2)));
      return;
    }
    final snBytes = List<int>.from(provider.sn.codeUnits);
    provider.uartService.sendFrame(CommandCodes.setSn, Uint8List.fromList(snBytes));
    ScaffoldMessenger.of(context).showSnackBar(
      SnackBar(content: Text('SN "${provider.sn}" 已发送到 ESP32'), duration: const Duration(seconds: 2)));
  }

  void _manualPublish(SimulatorProvider provider) {
    if (!provider.isConnected) return;
    provider.publishNow();
    ScaffoldMessenger.of(context).showSnackBar(
      const SnackBar(content: Text('已触发发布'), duration: Duration(seconds: 1)));
  }

  Widget _buildSettingsCard(String title, List<Widget> children) {
    return Material(
      color: _bgCard,
      clipBehavior: Clip.antiAlias,
      shape: RoundedRectangleBorder(
        borderRadius: BorderRadius.circular(10),
        side: BorderSide(color: _border, width: 0.5),
      ),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Padding(
            padding: const EdgeInsets.fromLTRB(14, 12, 14, 0),
            child: Text(
              title,
              style: const TextStyle(fontWeight: FontWeight.w600, fontSize: 13, color: Colors.white70),
            ),
          ),
          ...children,
        ],
      ),
    );
  }

  Widget _buildVersionField(String label, String value, ValueChanged<String> onChanged) {
    return Row(
      children: [
        SizedBox(
          width: 70,
          child: Text(label, style: TextStyle(fontSize: 13, color: Colors.grey[500])),
        ),
        Expanded(
          child: TextFormField(
            initialValue: value,
            style: const TextStyle(fontSize: 13),
            decoration: InputDecoration(
              isDense: true,
              contentPadding: const EdgeInsets.symmetric(horizontal: 10, vertical: 8),
              border: OutlineInputBorder(borderRadius: BorderRadius.circular(8)),
            ),
            onChanged: onChanged,
          ),
        ),
      ],
    );
  }

  String _getModeText(InverterMode mode) {
    return switch (mode) {
      InverterMode.inverting => '逆变运行',
      InverterMode.standby => '待机',
      InverterMode.fault => '故障',
      InverterMode.shutdown => '关机',
      InverterMode.bypass => '旁路',
    };
  }

  Color _getModeColor(InverterMode mode) {
    return switch (mode) {
      InverterMode.inverting => Colors.greenAccent,
      InverterMode.standby => Colors.blueAccent,
      InverterMode.fault => Colors.redAccent,
      InverterMode.shutdown => Colors.grey,
      InverterMode.bypass => Colors.orangeAccent,
    };
  }

  String _getChargeStateText(String state) {
    return switch (state) {
      'charging' => '充电中',
      'discharging' => '放电中',
      _ => '空闲',
    };
  }

  String _getMpptStateText(String state) {
    return switch (state) {
      'tracking' => 'MPPT追踪',
      'scanning' => '扫描中',
      'standby' => '待机',
      'fault' => '故障',
      _ => state,
    };
  }

  IconData _getAlarmIcon(String level) {
    return switch (level) {
      'fault' => Icons.error_outline,
      'warning' => Icons.warning_amber_rounded,
      _ => Icons.info_outline,
    };
  }

  Color _getAlarmColor(String level) {
    return switch (level) {
      'fault' => Colors.redAccent,
      'warning' => Colors.orangeAccent,
      _ => Colors.blueAccent,
    };
  }

  Color _getLogColor(String log) {
    if (log.contains('[TX]')) return Colors.greenAccent;
    if (log.contains('[RX]')) return Colors.blueAccent;
    if (log.contains('[ERR]')) return Colors.redAccent;
    if (log.contains('[WARN]')) return Colors.orangeAccent;
    if (log.contains('[ALARM]')) return Colors.yellowAccent;
    if (log.contains('[OTA]')) return Colors.purpleAccent;
    if (log.contains('[SYS]')) return Colors.cyanAccent;
    return Colors.white70;
  }
}
