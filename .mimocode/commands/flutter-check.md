---
description: "Flutter 模拟器代码检查: analyze + 可选 run (arm_simulator)"
---

# Flutter 模拟器检查命令

对 Flutter ARM 模拟器项目执行静态分析, 可选启动运行测试。

## 用法

用户输入: `$ARGUMENTS` — 格式为 `[--run] [--file <path>]`

- `--run`: 分析通过后启动 `flutter run`
- `--file <path>`: 仅分析指定文件 (默认分析整个 lib/)

## 执行步骤

1. 确认工作目录为 `D:\CS_INV_WIFI\tools\arm_simulator`
2. 静态分析:
   - 若指定 `--file`:
     ```powershell
     flutter analyze <file_path> 2>&1
     ```
   - 否则:
     ```powershell
     flutter analyze 2>&1 | findstr "error"
     ```
   - 超时 60 秒
   - 若有 error, 输出错误详情并停止
3. 若指定 `--run` 且分析通过:
   ```powershell
   flutter run 2>&1 | Select-Object -First 100
   ```
   - 超时 180 秒
4. 输出分析结果摘要 (error 数量、warning 数量)

## 注意事项

- Flutter 项目位于 `tools/arm_simulator/` 子目录
- 仅检查 error 级别问题 (info/warning 不阻断)
- `flutter run` 需要连接设备或模拟器, 无设备时会失败
- 若 `flutter pub get` 失败, 先执行依赖更新再分析
