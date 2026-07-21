import 'dart:async';
import 'dart:convert';

import 'package:mqtt_client/mqtt_client.dart';
import 'package:mqtt_client/mqtt_server_client.dart';

import '../protocol/telemetry_protocol.dart';

class MqttService {
  MqttServerClient? _client;
  StreamSubscription<List<MqttReceivedMessage<MqttMessage>>>? _subscription;

  bool get isConnected =>
      _client?.connectionStatus?.state == MqttConnectionState.connected;

  Future<void> connect({
    required String host,
    required int port,
    required bool useTls,
    required String serialNumber,
    required String username,
    required String password,
    required void Function(String topic, String payload) onMessage,
    required void Function(String message) onLog,
    required void Function(bool connected) onConnectionChanged,
  }) async {
    await disconnect();

    final clientId = 'cs-inv-simulator-$serialNumber';
    final client = MqttServerClient.withPort(host, clientId, port)
      ..secure = useTls
      ..keepAlivePeriod = 30
      ..autoReconnect = true
      ..resubscribeOnAutoReconnect = true
      ..logging(on: false)
      ..onConnected = () {
        onLog('[MQTT] 已连接 $host:$port');
        onConnectionChanged(true);
      }
      ..onDisconnected = () {
        onLog('[MQTT] 连接已断开');
        onConnectionChanged(false);
      }
      ..onAutoReconnect = () {
        onLog('[MQTT] 正在自动重连');
      }
      ..onAutoReconnected = () {
        onLog('[MQTT] 自动重连成功');
        onConnectionChanged(true);
        try {
          publishData(serialNumber, 'status', {
            'online': true,
            'rssi': -45,
          }, retain: true);
          onLog('[MQTT TX] status (reconnected)');
        } catch (e) {
          onLog('[MQTT] 重连在线状态发布失败: $e');
        }
      };

    final statusTopic = 'cs_inv/$serialNumber/status';
    final willPayload = jsonEncode({
      't': DateTime.now().toUtc().millisecondsSinceEpoch ~/ 1000,
      'v': TelemetryProtocol.version,
      'data': {'online': false},
    });
    var connectMessage = MqttConnectMessage()
        .withClientIdentifier(clientId)
        .startClean()
        .withWillTopic(statusTopic)
        .withWillMessage(willPayload)
        .withWillQos(MqttQos.atLeastOnce)
        .withWillRetain();
    if (username.isNotEmpty) {
      connectMessage = connectMessage.authenticateAs(username, password);
    }
    client.connectionMessage = connectMessage;

    _client = client;
    try {
      await client.connect();
    } catch (_) {
      client.disconnect();
      rethrow;
    }
    if (!isConnected) {
      final code = client.connectionStatus?.returnCode;
      client.disconnect();
      throw StateError('MQTT Broker 拒绝连接: $code');
    }

    client.subscribe('cs_inv/$serialNumber/cmd', MqttQos.atLeastOnce);
    client.subscribe('cs_inv/$serialNumber/ota/cmd', MqttQos.atLeastOnce);
    _subscription = client.updates?.listen((messages) {
      for (final received in messages) {
        final message = received.payload as MqttPublishMessage;
        final payload = MqttPublishPayload.bytesToStringAsString(
          message.payload.message,
        );
        onLog('[MQTT RX] ${received.topic}');
        onMessage(received.topic, payload);
      }
    });
  }

  void publishData(
    String serialNumber,
    String subTopic,
    Map<String, dynamic> data, {
    MqttQos qos = MqttQos.atLeastOnce,
    bool retain = false,
  }) {
    final client = _client;
    if (client == null || !isConnected) {
      throw StateError('MQTT 尚未连接');
    }
    final builder = MqttClientPayloadBuilder()
      ..addUTF8String(jsonEncode(TelemetryProtocol.envelope(data)));
    client.publishMessage(
      'cs_inv/$serialNumber/$subTopic',
      qos,
      builder.payload!,
      retain: retain,
    );
  }

  Future<void> disconnect({String? serialNumber}) async {
    final client = _client;
    if (client != null && isConnected && serialNumber != null) {
      try {
        publishData(serialNumber, 'status', {'online': false}, retain: true);
        await Future<void>.delayed(const Duration(milliseconds: 100));
      } catch (_) {}
    }
    await _subscription?.cancel();
    _subscription = null;
    client?.disconnect();
    _client = null;
  }
}
