import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:provider/provider.dart';
import 'package:shared_preferences/shared_preferences.dart';
import 'package:arm_simulator/main.dart';
import 'package:arm_simulator/providers/simulator_provider.dart';

void main() {
  testWidgets('App should render', (WidgetTester tester) async {
    tester.view.physicalSize = const Size(1280, 800);
    tester.view.devicePixelRatio = 1;
    addTearDown(tester.view.resetPhysicalSize);
    addTearDown(tester.view.resetDevicePixelRatio);
    SharedPreferences.setMockInitialValues({'connection.directMqtt': true});
    final provider = SimulatorProvider(
      discoverSerialPorts: false,
      useSecureStorage: false,
    );
    await provider.initialize();
    await tester.pumpWidget(
      ChangeNotifierProvider(
        create: (_) => provider,
        child: const SimulatorApp(),
      ),
    );
    await tester.pump();
    expect(find.text('CS-I10-6k2 ARM模拟器'), findsOneWidget);

    await tester.tap(find.text('设置'));
    await tester.pump(const Duration(milliseconds: 50));
    expect(find.text('TLS 加密连接'), findsOneWidget);
    expect(tester.takeException(), isNull);
  });
}
