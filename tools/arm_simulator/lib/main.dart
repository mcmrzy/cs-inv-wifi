import 'package:flutter/material.dart';
import 'package:provider/provider.dart';
import 'providers/simulator_provider.dart';
import 'screens/simulator_screen.dart';

void main() {
  runApp(
    ChangeNotifierProvider(
      create: (_) => SimulatorProvider(),
      child: const SimulatorApp(),
    ),
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
        colorSchemeSeed: const Color(0xFF0F2027),
        brightness: Brightness.dark,
        scaffoldBackgroundColor: const Color(0xFF0F1923),
      ),
      home: const SimulatorScreen(),
    );
  }
}