import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:provider/provider.dart';
import '../providers/multi_device_simulator.dart';

class MultiDeviceScreen extends StatefulWidget {
  const MultiDeviceScreen({super.key});

  @override
  State<MultiDeviceScreen> createState() => _MultiDeviceScreenState();
}

class _MultiDeviceScreenState extends State<MultiDeviceScreen> {
  final _formKey = GlobalKey<FormState>();
  final _mqttHostController = TextEditingController();
  final _mqttPortController = TextEditingController();
  final _mqttUsernameController = TextEditingController();
  final _mqttPasswordController = TextEditingController();
  final _manufacturerController = TextEditingController();
  final _countryController = TextEditingController();
  final _customerController = TextEditingController();
  final _startSequenceController = TextEditingController();
  final _deviceCountController = TextEditingController();
  
  bool _mqttTls = false;
  bool _showPassword = false;

  @override
  void initState() {
    super.initState();
    _loadSettingsAsync();
  }

  Future<void> _loadSettingsAsync() async {
    final provider = context.read<MultiDeviceSimulator>();
    await provider.initialize();
    if (!mounted) return;
    setState(() {
      _mqttHostController.text = provider.mqttHost;
      _mqttPortController.text = provider.mqttPort.toString();
      _mqttUsernameController.text = provider.mqttUsername;
      _mqttPasswordController.text = provider.mqttPassword;
      _manufacturerController.text = provider.manufacturer;
      _countryController.text = provider.country;
      _customerController.text = provider.customer;
      _startSequenceController.text = provider.startSequence.toString();
      _deviceCountController.text = provider.deviceCount.toString();
      _mqttTls = provider.mqttTls;
    });
  }

  @override
  void dispose() {
    _mqttHostController.dispose();
    _mqttPortController.dispose();
    _mqttUsernameController.dispose();
    _mqttPasswordController.dispose();
    _manufacturerController.dispose();
    _countryController.dispose();
    _customerController.dispose();
    _startSequenceController.dispose();
    _deviceCountController.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    return Consumer<MultiDeviceSimulator>(
      builder: (context, provider, _) {
        return Scaffold(
          backgroundColor: const Color(0xFF0B1220),
          appBar: AppBar(
            title: const Text('多设备模拟器'),
            backgroundColor: const Color(0xFF151F2E),
            actions: [
              if (provider.isRunning)
                Chip(
                  label: Text(
                    '${provider.connectedCount}/${provider.deviceCount}',
                    style: const TextStyle(color: Colors.white),
                  ),
                  backgroundColor: provider.allConnected
                      ? Colors.green.withOpacity(0.3)
                      : Colors.orange.withOpacity(0.3),
                ),
              const SizedBox(width: 16),
            ],
          ),
          body: Row(
            children: [
              // 左侧配置面板
              SizedBox(
                width: 350,
                child: _buildConfigPanel(provider),
              ),
              // 右侧日志和设备列表
              Expanded(
                child: _buildRightPanel(provider),
              ),
            ],
          ),
        );
      },
    );
  }

  Widget _buildConfigPanel(MultiDeviceSimulator provider) {
    return Container(
      decoration: BoxDecoration(
        color: const Color(0xFF151F2E),
        border: Border(
          right: BorderSide(color: Colors.grey.withOpacity(0.2)),
        ),
      ),
      child: SingleChildScrollView(
        padding: const EdgeInsets.all(16),
        child: Form(
          key: _formKey,
          child: Column(
            crossAxisAlignment: CrossAxisAlignment.start,
            children: [
              _buildSectionTitle('设备配置'),
              _buildDeviceCountField(provider),
              const SizedBox(height: 16),
              _buildSectionTitle('SN 生成参数'),
              _buildSnConfigFields(),
              const SizedBox(height: 16),
              _buildSectionTitle('MQTT 连接配置'),
              _buildMqttConfigFields(),
              const SizedBox(height: 24),
              _buildActionButtons(provider),
              const SizedBox(height: 16),
              _buildPreviewSection(provider),
            ],
          ),
        ),
      ),
    );
  }

  Widget _buildSectionTitle(String title) {
    return Padding(
      padding: const EdgeInsets.only(bottom: 8),
      child: Text(
        title,
        style: const TextStyle(
          color: Colors.white,
          fontSize: 16,
          fontWeight: FontWeight.bold,
        ),
      ),
    );
  }

  Widget _buildDeviceCountField(MultiDeviceSimulator provider) {
    return TextFormField(
      controller: _deviceCountController,
      decoration: const InputDecoration(
        labelText: '设备数量 (0-100)',
        hintText: '输入要模拟的设备数量',
        prefixIcon: Icon(Icons.devices),
      ),
      keyboardType: TextInputType.number,
      inputFormatters: [FilteringTextInputFormatter.digitsOnly],
      validator: (value) {
        if (value == null || value.isEmpty) return '请输入设备数量';
        final count = int.tryParse(value);
        if (count == null || count < 0 || count > 100) {
          return '设备数量必须在 0-100 之间';
        }
        return null;
      },
      onChanged: (value) {
        final count = int.tryParse(value) ?? 0;
        provider.setDeviceCount(count);
      },
    );
  }

  Widget _buildSnConfigFields() {
    return Column(
      children: [
        Row(
          children: [
            Expanded(
              child: TextFormField(
                controller: _manufacturerController,
                decoration: const InputDecoration(
                  labelText: '厂商码',
                  hintText: '如 H1',
                ),
                maxLength: 2,
                onChanged: (value) {
                  context.read<MultiDeviceSimulator>().setSnConfig(
                    manufacturer: value.toUpperCase(),
                  );
                },
              ),
            ),
            const SizedBox(width: 8),
            Expanded(
              child: TextFormField(
                controller: _countryController,
                decoration: const InputDecoration(
                  labelText: '国家码',
                  hintText: '如 DE',
                ),
                maxLength: 2,
                onChanged: (value) {
                  context.read<MultiDeviceSimulator>().setSnConfig(
                    country: value.toUpperCase(),
                  );
                },
              ),
            ),
          ],
        ),
        const SizedBox(height: 8),
        Row(
          children: [
            Expanded(
              child: TextFormField(
                controller: _customerController,
                decoration: const InputDecoration(
                  labelText: '客户码',
                  hintText: '如 A001',
                ),
                maxLength: 4,
                onChanged: (value) {
                  context.read<MultiDeviceSimulator>().setSnConfig(
                    customer: value.toUpperCase(),
                  );
                },
              ),
            ),
            const SizedBox(width: 8),
            Expanded(
              child: TextFormField(
                controller: _startSequenceController,
                decoration: const InputDecoration(
                  labelText: '起始序列号',
                  hintText: '如 1',
                ),
                keyboardType: TextInputType.number,
                inputFormatters: [FilteringTextInputFormatter.digitsOnly],
                onChanged: (value) {
                  final seq = int.tryParse(value) ?? 1;
                  context.read<MultiDeviceSimulator>().setSnConfig(
                    startSequence: seq,
                  );
                },
              ),
            ),
          ],
        ),
      ],
    );
  }

  Widget _buildMqttConfigFields() {
    return Column(
      children: [
        TextFormField(
          controller: _mqttHostController,
          decoration: const InputDecoration(
            labelText: 'MQTT Broker 地址',
            hintText: '如 127.0.0.1 或 jiuxiaoyw.online',
            prefixIcon: Icon(Icons.cloud),
          ),
          onChanged: (value) {
            context.read<MultiDeviceSimulator>().setMqttConfig(host: value);
          },
        ),
        const SizedBox(height: 8),
        Row(
          children: [
            Expanded(
              child: TextFormField(
                controller: _mqttPortController,
                decoration: const InputDecoration(
                  labelText: '端口',
                  hintText: '1883 或 8883',
                ),
                keyboardType: TextInputType.number,
                inputFormatters: [FilteringTextInputFormatter.digitsOnly],
                onChanged: (value) {
                  final port = int.tryParse(value) ?? 1883;
                  context.read<MultiDeviceSimulator>().setMqttConfig(port: port);
                },
              ),
            ),
            const SizedBox(width: 16),
            Expanded(
              child: Row(
                children: [
                  const Text('TLS', style: TextStyle(color: Colors.white)),
                  const Spacer(),
                  Switch(
                    value: _mqttTls,
                    onChanged: (value) {
                      setState(() => _mqttTls = value);
                      context.read<MultiDeviceSimulator>().setMqttConfig(tls: value);
                    },
                  ),
                ],
              ),
            ),
          ],
        ),
        const SizedBox(height: 8),
        TextFormField(
          controller: _mqttUsernameController,
          decoration: const InputDecoration(
            labelText: '用户名',
            prefixIcon: Icon(Icons.person),
          ),
          onChanged: (value) {
            context.read<MultiDeviceSimulator>().setMqttConfig(username: value);
          },
        ),
        const SizedBox(height: 8),
        TextFormField(
          controller: _mqttPasswordController,
          decoration: InputDecoration(
            labelText: '密码',
            prefixIcon: const Icon(Icons.lock),
            suffixIcon: IconButton(
              icon: Icon(
                _showPassword ? Icons.visibility : Icons.visibility_off,
              ),
              onPressed: () {
                setState(() => _showPassword = !_showPassword);
              },
            ),
          ),
          obscureText: !_showPassword,
          onChanged: (value) {
            context.read<MultiDeviceSimulator>().setMqttConfig(password: value);
          },
        ),
      ],
    );
  }

  Widget _buildActionButtons(MultiDeviceSimulator provider) {
    return Row(
      children: [
        Expanded(
          child: ElevatedButton.icon(
            onPressed: provider.isRunning
                ? () => provider.stopSimulation()
                : () => _startSimulation(provider),
            icon: Icon(provider.isRunning ? Icons.stop : Icons.play_arrow),
            label: Text(provider.isRunning ? '停止模拟' : '开始模拟'),
            style: ElevatedButton.styleFrom(
              backgroundColor: provider.isRunning ? Colors.red : Colors.green,
              foregroundColor: Colors.white,
              padding: const EdgeInsets.symmetric(vertical: 12),
            ),
          ),
        ),
        const SizedBox(width: 8),
        ElevatedButton(
          onPressed: provider.isRunning ? () => provider.publishAllNow() : null,
          style: ElevatedButton.styleFrom(
            backgroundColor: Colors.blue,
            foregroundColor: Colors.white,
            padding: const EdgeInsets.symmetric(vertical: 12, horizontal: 16),
          ),
          child: const Text('立即发布'),
        ),
      ],
    );
  }

  Widget _buildPreviewSection(MultiDeviceSimulator provider) {
    final sns = provider.generateSns();
    return Column(
      crossAxisAlignment: CrossAxisAlignment.start,
      children: [
        Row(
          children: [
            _buildSectionTitle('SN 预览 (前5个)'),
            const Spacer(),
            if (sns.isNotEmpty)
              TextButton.icon(
                onPressed: () => _copySns(sns),
                icon: const Icon(Icons.copy, size: 14),
                label: const Text('复制全部', style: TextStyle(fontSize: 12)),
                style: TextButton.styleFrom(
                  foregroundColor: Colors.grey[300],
                  padding: const EdgeInsets.symmetric(horizontal: 8, vertical: 4),
                ),
              ),
          ],
        ),
        Container(
          padding: const EdgeInsets.all(8),
          decoration: BoxDecoration(
            color: Colors.black.withOpacity(0.3),
            borderRadius: BorderRadius.circular(8),
          ),
          child: Column(
            crossAxisAlignment: CrossAxisAlignment.start,
            children: [
              for (int i = 0; i < (sns.length > 5 ? 5 : sns.length); i++)
                InkWell(
                  onTap: () => _copyToClipboard(sns[i], 'SN ${sns[i]}'),
                  borderRadius: BorderRadius.circular(4),
                  child: Padding(
                    padding: const EdgeInsets.symmetric(vertical: 2, horizontal: 4),
                    child: Row(
                      children: [
                        Text(
                          '${i + 1}. ${sns[i]}',
                          style: const TextStyle(
                            fontFamily: 'monospace',
                            color: Colors.greenAccent,
                          ),
                        ),
                        const SizedBox(width: 6),
                        Icon(Icons.copy, size: 12, color: Colors.grey[600]),
                      ],
                    ),
                  ),
                ),
              if (sns.length > 5)
                Text(
                  '... 共 ${sns.length} 个',
                  style: TextStyle(color: Colors.grey[400]),
                ),
            ],
          ),
        ),
      ],
    );
  }

  Widget _buildRightPanel(MultiDeviceSimulator provider) {
    return Column(
      children: [
        // 设备状态概览
        if (provider.isRunning) _buildDeviceStatusOverview(provider),
        // 日志区域
        Expanded(child: _buildLogSection(provider)),
      ],
    );
  }

  Widget _buildDeviceStatusOverview(MultiDeviceSimulator provider) {
    return Container(
      padding: const EdgeInsets.all(16),
      decoration: BoxDecoration(
        color: const Color(0xFF151F2E),
        border: Border(
          bottom: BorderSide(color: Colors.grey.withOpacity(0.2)),
        ),
      ),
      child: Row(
        children: [
          _buildStatusCard(
            '总设备',
            provider.deviceCount.toString(),
            Icons.devices,
            Colors.blue,
          ),
          const SizedBox(width: 16),
          _buildStatusCard(
            '已连接',
            provider.connectedCount.toString(),
            Icons.cloud_done,
            Colors.green,
          ),
          const SizedBox(width: 16),
          _buildStatusCard(
            '连接中',
            provider.connectingCount.toString(),
            Icons.cloud_upload,
            Colors.orange,
          ),
          const SizedBox(width: 16),
          _buildStatusCard(
            '失败',
            provider.failedCount.toString(),
            Icons.cloud_off,
            Colors.red,
          ),
        ],
      ),
    );
  }

  Widget _buildStatusCard(String title, String value, IconData icon, Color color) {
    return Expanded(
      child: Card(
        color: color.withOpacity(0.1),
        child: Padding(
          padding: const EdgeInsets.all(12),
          child: Column(
            children: [
              Icon(icon, color: color, size: 24),
              const SizedBox(height: 4),
              Text(
                value,
                style: TextStyle(
                  color: color,
                  fontSize: 24,
                  fontWeight: FontWeight.bold,
                ),
              ),
              Text(
                title,
                style: TextStyle(color: Colors.grey[400], fontSize: 12),
              ),
            ],
          ),
        ),
      ),
    );
  }

  Widget _buildLogSection(MultiDeviceSimulator provider) {
    return Container(
      margin: const EdgeInsets.all(16),
      decoration: BoxDecoration(
        color: Colors.black.withOpacity(0.3),
        borderRadius: BorderRadius.circular(8),
        border: Border.all(color: Colors.grey.withOpacity(0.2)),
      ),
      child: Column(
        children: [
          Container(
            padding: const EdgeInsets.symmetric(horizontal: 12, vertical: 8),
            decoration: BoxDecoration(
              color: Colors.grey.withOpacity(0.1),
              borderRadius: const BorderRadius.vertical(top: Radius.circular(8)),
            ),
            child: Row(
              children: [
                const Icon(Icons.terminal, size: 16, color: Colors.grey),
                const SizedBox(width: 8),
                const Text(
                  '运行日志',
                  style: TextStyle(color: Colors.white, fontWeight: FontWeight.bold),
                ),
                const Spacer(),
                Text(
                  '${provider.logs.length} 条',
                  style: TextStyle(color: Colors.grey[400], fontSize: 12),
                ),
                const SizedBox(width: 8),
                IconButton(
                  icon: const Icon(Icons.copy, size: 16),
                  onPressed: () => _copyLogs(provider),
                  tooltip: '复制日志',
                ),
                IconButton(
                  icon: const Icon(Icons.clear_all, size: 16),
                  onPressed: () => provider.clearLogs(),
                  tooltip: '清除日志',
                ),
              ],
            ),
          ),
          Expanded(
            child: ListView.builder(
              padding: const EdgeInsets.all(8),
              itemCount: provider.logs.length,
              itemBuilder: (context, index) {
                final log = provider.logs[index];
                return Padding(
                  padding: const EdgeInsets.symmetric(vertical: 1),
                  child: Text(
                    log,
                    style: TextStyle(
                      fontFamily: 'monospace',
                      fontSize: 12,
                      color: _getLogColor(log),
                    ),
                  ),
                );
              },
            ),
          ),
        ],
      ),
    );
  }

  Color _getLogColor(String log) {
    if (log.contains('[ERR]')) return Colors.redAccent;
    if (log.contains('[WARN]')) return Colors.orangeAccent;
    if (log.contains('[MQTT TX]')) return Colors.greenAccent;
    if (log.contains('[MQTT RX]')) return Colors.blueAccent;
    if (log.contains('[SYS]')) return Colors.yellowAccent;
    return Colors.grey[300]!;
  }

  void _startSimulation(MultiDeviceSimulator provider) {
    if (_formKey.currentState!.validate()) {
      provider.startSimulation();
    }
  }

  void _copyToClipboard(String text, String label) {
    Clipboard.setData(ClipboardData(text: text));
    ScaffoldMessenger.of(context).showSnackBar(
      SnackBar(
        content: Text('已复制 $label'),
        duration: const Duration(seconds: 1),
        behavior: SnackBarBehavior.floating,
      ),
    );
  }

  void _copySns(List<String> sns) {
    final text = sns.join('\n');
    _copyToClipboard(text, '${sns.length} 个 SN');
  }

  void _copyLogs(MultiDeviceSimulator provider) {
    final text = provider.logs.join('\n');
    _copyToClipboard(text, '${provider.logs.length} 条日志');
  }
}