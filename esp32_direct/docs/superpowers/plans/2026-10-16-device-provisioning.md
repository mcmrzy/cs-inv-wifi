# Device Provisioning Implementation Plan

> **For agentic workers:** REQUIRED: Use superpowers:subagent-driven-development to implement this plan. Steps use checkbox syntax for tracking.

**Goal:** Extend the ESP32 direct-controller factory workstation with automatic USB identification, persistent per-board identity and network INFO reporting.

**Architecture:** Upgrade the existing dual-slot identity codec with legacy decoding; extend the existing workstation and factory protocol instead of introducing another tool. Keep runtime identity separate from pending stored changes. Preserve the existing guarded flasher.

**Tech Stack:** ESP-IDF C, cJSON, Python/customtkinter, pyserial/esptool, GCC host regression tests.

## Chunk 1: Contracts and persistence

- [ ] Review approved design at `docs/superpowers/specs/2026-10-16-device-provisioning-design.md` for size, compatibility and memory constraints.
- [ ] Add failing C/Python tests for schema 2 roundtrip, legacy decoding, slot selection, interrupted upgrade and sticky lock behavior.
- [ ] Update `main/factory/factory_record.h/.c`, `factory_store.c`, `tools/factory_tool.py` with schema 2 size 736 and extension field names defined by the spec. Canonical encode supports legacy size when extensions are empty; wire schema matches selected size.
- [ ] Run factory record/store/identity/pairing host tests and Python codec tests, including C/Python interoperability. No reset/erase of factory partition on decode failure.

## Chunk 2: USB service and INFO

- [ ] Add failing behavior tests for factory inspect capability/extended records and INFO JSON generated from real identity records, absent identity, pending SN, escaped ASCII and maximum lengths.
- [ ] Update `main/factory/factory_wire.h` payload bound to 4096, `factory_service.c` capability/extended fields/chip/revision/reboot state. Bound static buffers and task stack for new record size.
- [ ] Add focused JSON identity helper under `main/factory/` shared by inspect and INFO if this reduces duplicated field mapping. Update `main/CMakeLists.txt` only for new source registration.
- [ ] Update `main/telemetry/telemetry.c` INFO using cJSON, preserving unknown legacy identity values and actual runtime versions. Trigger INFO after identity generation changes without publishing new SN under an old connection.
- [ ] Run existing DSP info and factory protocol tests plus new JSON behavior tests using bundled cJSON source.

## Chunk 3: Workstation

- [ ] Add failing tests for known USB candidate selection, port removal/reuse, multi-device choice, idle-only probing, stale MAC refusal, schema capability rejection and reboot verification.
- [ ] Add `tools/factory_discovery.py` for pure discovery state; integrate timer and read-only handshake in `factory_gui.py` and `factory_gui_modern.py` without parallel serial ownership.
- [ ] Update `factory_gui_backend.py` for default fields, capability checks, expected MAC binding and revision/runtime verification. Add all schema 2 fields to both GUI forms and localized labels.
- [ ] Retain existing backup/hash/partition protections. Add explicit read-only ROM identification for blank firmware only on user action and bind flashing jobs to inspected MAC where available; no speculative ROM reset during discovery.
- [ ] Update packaging and README/templates/version for new tool and supported firmware; unknown fields and secrets must not leak into reports.

## Chunk 4: Review and delivery

- [ ] Run targeted new tests and existing production/onboarding/flash tool regression suites. Review integration for stack/heap use, no extra serial access while flashing, old GUI fallback and maximum JSON sizes.
- [ ] Build actual ESP-IDF firmware; build/package workstation if dependencies are available. Record hashes and distinguish build evidence from hardware acceptance.
- [ ] Have independent reviewer check spec compliance, then code quality; fix actionable issues and rerun affected checks.
- [ ] Deliver artifact paths, validation results and remaining physical/server acceptance boundaries. Do not alter pre-existing work or push/deploy shared state.
