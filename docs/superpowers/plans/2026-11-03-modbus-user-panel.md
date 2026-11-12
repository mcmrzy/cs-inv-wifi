# Modbus user panel implementation plan

**Goal:** Make the CS-MB-PANEL LCD menus, measurements and controls follow the inverter's CVTE V1.27 protocol.

**Architecture:** Keep the existing graphical home and local controller pages; route inverter functions directly into the single native Modbus UI/catalog. Local preferences and inverter writes remain separate. Preserve bus ownership, operation receipts, freshness and two-stage confirmation.

**Tech Stack:** ESP-IDF 5.5.4, ESP32-C3, C11 host tests, ST7789 320x240.

## Evidence and scope

- Actual protocol: `C:/Users/29538/Documents/CVTE-INV-Modbus地址表（外发）V1.27.xlsx`, referenced by the existing firmware packaging script. The older Csergy PDF is a different protocol.
- The pasted log shows inverter ONLINE / INV_DATA READY and repeated BLE low-memory failure. The missing normal key logs after action 99 belong to an independent native UI loop, and do not prove a six-second stall.
- The existing main menus expose 84 legacy parameter indices while the native Modbus catalog has 46 actual settings. Old schedule/socket/lithium-cell group editors must not be reachable through user navigation.
- WF_UserSet 410x reads are completion markers, not current settings. Only explicit measured/setpoint mappings may provide current values; ambiguous targets never produce confirmed-success receipts.
- No physical device flashing, writes or deployment are included in local validation.

## Tasks

- [x] Add failing behavior tests for direct menu destinations, back navigation, unknown/stale drafts, generator measurements, readable settings and units.
- [x] Route six main entries into actual Modbus operation pages or controller-local preferences; remove legacy menu paths and synchronous refresh at entry.
- [x] Improve native setting selection/review, actual-value labels, protocol enum labels and fresh readback mappings; add generator/output/system monitoring.
- [x] Fix BLE low-memory admission without lowering memory thresholds; explicit phone requests may release MQTT to acquire memory.
- [x] Enforce text bounds/glyph coverage, render full/strip fixtures, exercise actual navigation and command receipts.
- [x] Run all panel host checks and ESP-IDF build; inspect image/partition budget and package a versioned App and full image with SHA256, previews and hardware acceptance notes.

## Acceptance

WHEN a user opens inverter settings THEN only actual V1.27 parameters are shown. WHEN current values are absent/stale THEN the draft starts unselected and cannot be submitted until a direction key selects it. WHEN confirmed twice THEN exactly one command is queued, and only an independent actual readback may report confirmed. WHEN a native function is cancelled THEN BACK returns directly to its entry point, preserving menu selection. WHEN cloud is online and BLE has insufficient memory THEN no repeated failed initialization occurs; a phone setup request still enters the memory recovery path.

## Validation notes

- RED/GREEN: unknown output-voltage draft initially seeded minimum; fixed to require a direction key. Native actual-loop SPI failure initially failed to redraw; fixed to force full redraw after transmission errors. BLE admission reproduced the pasted memory figures before the fix.
- All panel host checks pass, including 129 native views with complete glyphs, text width bounds and full/strip equality; 452 peripheral scene renders; extracted actual LCD/key loop, BLE ownership and alarm contract tests.
- ESP-IDF 5.5.4 final build passes, product CS-MB-PANEL, version 0.3.0-modbus-full, unchanged dual OTA partition layout. No serial port was opened.
- Package: `esp32_modbus_panel/dist/CS-MB-PANEL_v0.3.0_full.zip`, SHA256 `90b24a1dbb6382bdea0b1b78ac1553b20715f7669f125b56ef18d76b6ccec370`. App is 1,687,424 bytes with 16,512 bytes free in each app slot; full image is 4,194,304 bytes. Package tooling verified actual compiled dependencies and 238 source/config hashes.
- Final review corrected FFF2 dual-output timeout semantics, redirected BMS shortcuts to inverter data, protected 4KL raw enum ambiguity and moved charging target reads into the regular polling round.
- Native Modbus pages use Chinese and follow light/dark themes; peripheral pages retain their existing language setting. Physical display, BLE memory recovery, inverter write/readback and OTA acceptance remain unverified.
