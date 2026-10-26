# Direct BMS upload implementation plan

**Goal:** Upload the direct BBJ BMS snapshot using the existing L10 heartbeat v3 `bms_summary` layout 0 (100 little-endian bytes plus `age_ms`).

**Architecture:** Validate the complete BBJ 0x01 reply before producing a summary. Store it with a 64-bit monotonic receive timestamp under the existing UART snapshot mutex. Copy the snapshot before publishing; retain existing DSP measurement freshness requirements. Never refresh BMS age from DSP traffic or failed BMS replies.

**Tech Stack:** C11, ESP-IDF 5.5.4, existing MQTT envelope and host C/Python harnesses.

- [x] Test existing heartbeat behavior, showing that a valid BMS snapshot is currently missing from MQTT.
- [x] Add and test complete BBJ-to-layout-0 mapping: signed current, raw SOC/SOH, capacity, cells/balancing, typed temperatures, cycle count, mode/status, cumulative capacity and charge requests. Reject CRC, address/function/length errors and truncated tails. Leave incompatible L10 alarm/protection/fault fields reserved zero; never reinterpret unrelated BBJ bits as those flags.
- [x] Capture and publish the independent BMS snapshot, omitting it before first valid receipt and preserving its age after loss. Saturate ages above UINT32_MAX.
- [x] Regress actual UART task, reconnect/query/periodic telemetry, worst-case payload size, stale DSP rejection and existing configuration locking.
- [x] Build and package an ESP application candidate, documenting field limitations and physical/MQTT acceptance still outstanding.

Reference: `esp32c3_l10_idf/main/telemetry/telemetry.c`, service `battery_summary.go`, and supplied BMS `PC_485_protocol.C` / `drv_pc_protocol.h`. BMS temperature types come from the encoded high byte (two cell sensors, MOS, PCB in the supplied firmware), rather than the older ARM parser's positional assumptions. Unrepresented fault/alarm bits, energy/time/fan fields and unavailable sensors require a separate protocol extension; this change retains the service's existing contract.
