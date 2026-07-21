import 'dart:convert';
import 'dart:typed_data';

import 'package:arm_simulator/providers/simulator_provider.dart';
import 'package:arm_simulator/services/uart_service.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:shared_preferences/shared_preferences.dart';

void main() {
  test('UART parser preserves escaped header and escape bytes', () async {
    final service = UartService();
    final received = service.frameStream.first;
    final data = Uint8List.fromList([0xAA, 0x55]);

    service.processReceivedData(_buildFrame(CommandCodes.commandRecv, data));

    final frame = await received;
    expect(frame.command, CommandCodes.commandRecv);
    expect(frame.data, data);
    service.dispose();
  });

  test('standard command updates config once for duplicate task id', () async {
    SharedPreferences.setMockInitialValues({});
    final provider = SimulatorProvider(
      discoverSerialPorts: false,
      useSecureStorage: false,
    );
    await provider.initialize();
    final command = utf8.encode(
      jsonEncode({
        'v': 1,
        'task_id': '019f-command-1',
        't': DateTime.now().toUtc().millisecondsSinceEpoch ~/ 1000,
        'cmd': 'set_power_limit',
        'args': [3000],
      }),
    );
    final frame = _buildFrame(
      CommandCodes.commandRecv,
      Uint8List.fromList(command),
    );

    provider.uartService.processReceivedData(frame);
    await Future<void>.delayed(Duration.zero);
    expect(provider.inverterData.powerLimit, 3000);
    expect(provider.inverterData.configRev, 2);

    provider.uartService.processReceivedData(frame);
    await Future<void>.delayed(Duration.zero);
    expect(provider.inverterData.configRev, 2);
    provider.dispose();
  });

  test(
    'command validation rejects wrong version and expired timestamp',
    () async {
      SharedPreferences.setMockInitialValues({});
      final provider = SimulatorProvider(
        discoverSerialPorts: false,
        useSecureStorage: false,
      );
      await provider.initialize();
      final now = DateTime.now().toUtc().millisecondsSinceEpoch ~/ 1000;

      await provider.handleMqttMessageForTest(
        'cs_inv/test/cmd',
        jsonEncode({
          'v': 2,
          'task_id': 'wrong-version',
          't': now,
          'cmd': 'set_power_limit',
          'args': [1234],
        }),
      );
      expect(provider.inverterData.powerLimit, 6200);

      await provider.handleMqttMessageForTest(
        'cs_inv/test/cmd',
        jsonEncode({
          'v': 1,
          'task_id': 'expired-command',
          't': now - 301,
          'cmd': 'set_power_limit',
          'args': [1234],
        }),
      );
      expect(provider.inverterData.powerLimit, 6200);
      final prefs = await SharedPreferences.getInstance();
      final responses =
          jsonDecode(prefs.getString('command.completed.responses')!)
              as Map<String, dynamic>;
      expect(responses['wrong-version']['stage'], 'completed');
      expect(responses['expired-command']['code'], 'EXPIRED');
      provider.dispose();
    },
  );

  test(
    'config and latest idempotent response survive provider restart',
    () async {
      SharedPreferences.setMockInitialValues({});
      final timestamp = DateTime.now().toUtc().millisecondsSinceEpoch ~/ 1000;
      final first = SimulatorProvider(
        discoverSerialPorts: false,
        useSecureStorage: false,
      );
      await first.initialize();
      await first.handleMqttMessageForTest(
        'cs_inv/test/cmd',
        jsonEncode({
          'v': 1,
          'task_id': 'persistent-command',
          't': timestamp,
          'cmd': 'set_power_limit',
          'args': [3000],
        }),
      );
      expect(first.inverterData.configRev, 2);
      first.dispose();

      final second = SimulatorProvider(
        discoverSerialPorts: false,
        useSecureStorage: false,
      );
      await second.initialize();
      expect(second.inverterData.powerLimit, 3000);
      expect(second.inverterData.configRev, 2);
      await second.handleMqttMessageForTest(
        'cs_inv/test/cmd',
        jsonEncode({
          'v': 1,
          'task_id': 'persistent-command',
          't': timestamp,
          'cmd': 'set_power_limit',
          'args': [1000],
        }),
      );
      expect(second.inverterData.powerLimit, 3000);
      expect(second.inverterData.configRev, 2);
      second.dispose();
    },
  );

  test(
    'OTA V1 start, query and cancel enforce active task correlation',
    () async {
      SharedPreferences.setMockInitialValues({});
      final provider = SimulatorProvider(
        discoverSerialPorts: false,
        useSecureStorage: false,
      );
      await provider.initialize();
      final now = DateTime.now().toUtc().millisecondsSinceEpoch ~/ 1000;
      Map<String, dynamic> ota(String action, String taskId) => {
        'v': 1,
        't': now,
        'task_id': taskId,
        'action': action,
        'target': 'arm',
        'url': 'https://firmware.example.com/arm.bin',
        'version': '1.2.3',
        'size': 4096,
        'sha256': List.filled(64, 'a').join(),
        'security_version': 1,
        'signature': base64Encode(utf8.encode('signature')),
        'timeout': 600,
      };

      final invalid = ota('start', 'ota-invalid')
        ..['url'] = 'http://firmware.example.com/arm.bin';
      await provider.handleMqttMessageForTest(
        'cs_inv/test/ota/cmd',
        jsonEncode(invalid),
      );
      expect(provider.lastOtaStatus?['code'], 'INVALID_ARGS');

      await provider.handleMqttMessageForTest(
        'cs_inv/test/ota/cmd',
        jsonEncode(ota('start', 'ota-active')),
      );
      expect(provider.lastOtaStatus?['state'], 'accepted');
      expect(provider.lastOtaStatus?['message'], contains('未执行真实签名验证'));

      await provider.handleMqttMessageForTest(
        'cs_inv/test/ota/cmd',
        jsonEncode(ota('query', 'ota-query')),
      );
      expect(provider.lastOtaStatus?['task_id'], 'ota-query');
      expect(provider.lastOtaStatus?['active_task_id'], 'ota-active');

      await provider.handleMqttMessageForTest(
        'cs_inv/test/ota/cmd',
        jsonEncode(ota('cancel', 'wrong-task')),
      );
      expect(provider.lastOtaStatus?['code'], 'TASK_MISMATCH');
      await provider.handleMqttMessageForTest(
        'cs_inv/test/ota/cmd',
        jsonEncode(ota('cancel', 'ota-active')),
      );
      expect(provider.lastOtaStatus?['state'], 'cancelled');
      provider.dispose();
    },
  );

  test('factory reset clears persisted simulator settings', () async {
    SharedPreferences.setMockInitialValues({
      'mqtt.host': 'broker.example.com',
      'connection.directMqtt': true,
    });
    final provider = SimulatorProvider(
      discoverSerialPorts: false,
      useSecureStorage: false,
    );
    await provider.initialize();
    expect(provider.mqttHost, 'broker.example.com');

    provider.uartService.processReceivedData(
      _buildFrame(CommandCodes.factoryReset, Uint8List(0)),
    );
    await Future<void>.delayed(const Duration(milliseconds: 20));

    final prefs = await SharedPreferences.getInstance();
    expect(prefs.getKeys(), isEmpty);
    expect(provider.mqttHost, '127.0.0.1');
    expect(provider.directMqtt, isFalse);
    provider.dispose();
  });
}

Uint8List _buildFrame(int command, Uint8List data) {
  final lengthHigh = (data.length >> 8) & 0xFF;
  final lengthLow = data.length & 0xFF;
  var checksum = 0xAA ^ command ^ lengthHigh ^ lengthLow;
  for (final byte in data) {
    checksum ^= byte;
  }

  final bytes = BytesBuilder()..addByte(0xAA);
  for (final byte in [command, lengthHigh, lengthLow, ...data, checksum]) {
    if (byte == 0xAA) {
      bytes.add([0x55, 0x01]);
    } else if (byte == 0x55) {
      bytes.add([0x55, 0x00]);
    } else {
      bytes.addByte(byte);
    }
  }
  return bytes.toBytes();
}
