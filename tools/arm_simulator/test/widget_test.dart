import 'package:flutter_test/flutter_test.dart';
import 'package:arm_simulator/main.dart';

void main() {
  testWidgets('App should render', (WidgetTester tester) async {
    await tester.pumpWidget(const SimulatorApp());
    expect(find.text('CS-I10-6k2 ARM模拟器'), findsOneWidget);
  });
}