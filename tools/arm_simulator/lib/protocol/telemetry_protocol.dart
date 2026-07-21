import 'dart:convert';

class TelemetryProtocol {
  static const int version = 1;

  static Map<String, dynamic> envelope(
    Map<String, dynamic> data, {
    DateTime? sampledAt,
  }) {
    final timestamp =
        (sampledAt ?? DateTime.now()).toUtc().millisecondsSinceEpoch ~/ 1000;
    return {'t': timestamp, 'v': version, 'data': data};
  }

  static List<int> publishPayload({
    required String serialNumber,
    required String topic,
    required Map<String, dynamic> data,
  }) {
    return utf8.encode(
      jsonEncode({
        'sn': serialNumber,
        'topic': topic,
        // ESP32 adds the common MQTT envelope immediately before publishing.
        'payload': jsonEncode(data),
      }),
    );
  }
}
