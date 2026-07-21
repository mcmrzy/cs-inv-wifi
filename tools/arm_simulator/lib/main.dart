import 'package:flutter/material.dart';
import 'package:provider/provider.dart';
import 'providers/simulator_provider.dart';
import 'screens/simulator_screen.dart';

Future<void> main() async {
  WidgetsFlutterBinding.ensureInitialized();
  final provider = SimulatorProvider();
  await provider.initialize();
  runApp(
    ChangeNotifierProvider.value(value: provider, child: const SimulatorApp()),
  );
}

class SimulatorApp extends StatelessWidget {
  const SimulatorApp({super.key});

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      title: 'CS-INV ARM Simulator',
      debugShowCheckedModeBanner: false,
      theme: ThemeData(
        useMaterial3: true,
        colorSchemeSeed: const Color(0xFF1976D2),
        brightness: Brightness.dark,
        scaffoldBackgroundColor: const Color(0xFF0B1220),
        inputDecorationTheme: InputDecorationTheme(
          filled: true,
          fillColor: const Color(0xFF151F2E),
          border: OutlineInputBorder(borderRadius: BorderRadius.circular(8)),
          contentPadding: const EdgeInsets.symmetric(horizontal: 12, vertical: 10),
        ),
      ),
      home: const SimulatorScreen(),
    );
  }
}
