# Battery configuration implementation plan

User approved the source-backed design in this chat (2026-10-20).

Goal: select and configure battery modes on the LCD or by one grouped remote request, persist a consistent configuration, and submit all required DSP frames without implying an application ACK.

Architecture: a shared bounded battery model validates 48V presets, configuration-word bits and voltages. A single pending writer serializes changes. Keep the existing 92-byte V3 record as a downgrade-readable mirror; additional profile storage is revision/byte-bound to it, and interrupted/mismatched storage must not enable guessed settings. Only existing BBJ protocol 5 is offered for managed lithium. Existing 0x24 is not used for the new transaction because supplied DSP semantics conflict; use the verified 01 packet SOC layout. Preserve the working tree and do not flash, publish or commit.

- [ ] Add failing policy/parameter/storage/runtime tests, then implement the shared model and legacy loading.
- [ ] Implement per-type profiles, complete charge settings, and consistent persistence/failure handling.
- [ ] Extend the UART owner with 0x2F replay and a version-bound completion mask while preserving measurement slots.
- [ ] Add a complete LCD draft/review and specific validation messages.
- [ ] Add grouped BLE/MQTT commands using existing admission, deduplication and completion semantics.
- [ ] Run relevant host tests, full production host suite, ESP-IDF build and resource budget; inspect final patch.

Acceptance: AGM→managed Li→AGM retains profiles; User Li validates material/cells; invalid group has no effect; storage failure retains prior RAM/record; reboot resends complete charge settings; partial UART failure remains saved/unconfirmed; stale 25 cannot confirm the operation; unknown protocols reject; legacy records retain active values; old firmware never receives an oversized record in the legacy key.
