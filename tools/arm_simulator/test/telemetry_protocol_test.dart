import 'dart:convert';

import 'package:arm_simulator/models/inverter_state.dart';
import 'package:arm_simulator/models/alarm_system.dart';
import 'package:arm_simulator/protocol/telemetry_protocol.dart';
import 'package:flutter_test/flutter_test.dart';

void main() {
  group('telemetry V1', () {
    test('heartbeat uses the frozen array sizes and contains no SN', () {
      final data = InverterData().toJson('heartbeat');

      expect(
        data.keys,
        unorderedEquals(['ac', 'bat', 'pv', 'sys', 'eng', 'cells']),
      );
      expect(data['ac'], hasLength(8));
      expect(data['bat'], hasLength(23));
      expect(data['pv'], hasLength(7));
      expect(data['sys'], hasLength(11));
      expect(data['eng'], hasLength(12));
      expect(data['cells'], hasLength(2));
      expect(data, isNot(contains('sn')));
    });

    test('all uplinks use the common envelope', () {
      final sampledAt = DateTime.fromMillisecondsSinceEpoch(
        1783676930000,
        isUtc: true,
      );
      final envelope = TelemetryProtocol.envelope({
        'online': true,
      }, sampledAt: sampledAt);

      expect(envelope, {
        't': 1783676930,
        'v': 1,
        'data': {'online': true},
      });
    });

    test('UART wrapper leaves envelope creation to ESP32', () {
      final bytes = TelemetryProtocol.publishPayload(
        serialNumber: 'H1CNA00135000014',
        topic: 'heartbeat',
        data: {'ac': <dynamic>[]},
      );
      final wrapper = jsonDecode(utf8.decode(bytes)) as Map<String, dynamic>;
      final payload =
          jsonDecode(wrapper['payload'] as String) as Map<String, dynamic>;

      expect(wrapper['topic'], 'heartbeat');
      expect(wrapper['sn'], 'H1CNA00135000014');
      expect(payload.keys, ['ac']);
      expect(payload, isNot(contains('sn')));
      expect(bytes.length, lessThanOrEqualTo(1024));
    });

    test('info uses protocol field names', () {
      final info = InverterData().toJson('info');

      expect(info['device_type'], 'off_grid_inverter');
      expect(info['rated_frequency'], 50.0);
      expect(info['battery_nominal_voltage'], 51.2);
      expect(info, isNot(contains('parallel_mode')));
      expect(info, isNot(contains('sn')));
    });

    test('config uses the final V1 field order', () {
      final config = InverterData().toJson('config');

      expect(config['rev'], 1);
      expect(config['inv'], [1, 6200, 6200, 6200, 200, 950, 0, 0]);
      expect(config['bms'], [1, 1, 500, 1000, 584, 430]);
      expect(config['parallel'], [0, 0, null, null]);
    });

    test('PCS alarm codes match the published dictionary', () {
      expect(AlarmSystem.alarmDefinitions[1]?.message, '母线过压');
      expect(AlarmSystem.alarmDefinitions[8]?.message, '过温保护');
      expect(AlarmSystem.alarmDefinitions[10]?.message, '输出过载');
      expect(AlarmSystem.alarmDefinitions[17]?.message, 'BMS通信丢失');
    });
  });
}
