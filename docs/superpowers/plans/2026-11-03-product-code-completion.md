# Product code completion

## Scope and acceptance

Continue the CS-MB-PANEL sales-readiness work as one integrated implementation. User explicitly authorizes completing all code-addressable gaps, deleting old code/resources to create firmware headroom, and reviewing UI/settings/error details. Do not create incremental release archives. Preserve existing external protocol meanings, persistence compatibility and dual OTA layout. Physical/electrical/SKU acceptance remains evidence to collect on devices rather than a software completion claim.

Route: refactor + fix-bug, Full depth. Parallel workers have exclusive file ownership because legacy UI, native settings and bus/network recovery are separable; root integrates runtime, build, tests and acceptance record.

## File ownership and tasks

- [x] Legacy UI worker: full/direct_ui.c, direct_ui_scene.c, direct_ui_locale.c and board-only helpers/tests. Delete unused 84-param/old battery/DSP pages and remove old scene dependency. Keep actual local UI flows, house home and native routes. Replace obsolete fixture tests with actual reachable product flows.
- [x] Native worker: main/panel_ui.c/.h, panel_catalog.c/.h, new panel_locale.c/.h and native behavior/render tests. Complete Chinese/English support, read-only/restricted/error explanations, command recovery/context handling, edit/review/results, numeric/enumeration presentation.
- [x] Runtime worker: main/panel_bus.c, panel_control.c/.h, full/direct_poll.c, app_main.c, ota_service.c and targeted tests. Audit disconnected/quiet/network/OTA/BMS lifecycle and owner timing; implement diagnosed fixes with fault tests.
- [x] Root: full/full_panel_runtime.inc, component.cmake, build/test runner, fonts and resource budget. Remove unused UI snapshot/config/legacy adapters, correct cross-module integration and make NVS/settings operations transactional. Check first-key wake, held keys, alarm/OTA takeover and restore.
- [x] Review actual ELF/map sizes against baseline 1,691,424-byte App / 12,512-byte free OTA slot; remove linked legacy resources and protect a useful budget in build validation.
- [x] Run meaningful host behavior, context/timeout/fault, full/strip rendering, language/width/glyph tests and final ESP-IDF build. Reconcile all remaining items in product-readiness record with evidence and exact physical limits.

## Baseline

- Current source version 0.3.1-modbus-full. App SHA256 dd3c974809d0137ee51e87f4a47084dc1f92e4810bee6f5d0805c6aef37a7e18.
- Source tree includes pre-existing untracked projects and unrelated modified files. Preserve them. No commit, push, device writes or archive packaging during implementation.
- Previous tests over-rendered unreachable old pages and could not prove product completeness. New tests must follow current reachable navigation and add failure/recovery coverage.
- Actual private protocol is CVTE V1.27 spreadsheet identified by existing packaging script; WF_UserSet completion markers are not configuration values. Preserve uncertain readback limits.

## Completion evidence

- Integrated host suite and ESP-IDF build passed. Native UI covers 870 views, board-local UI 228 reachable scenes, and the alarm contract accepts 134 generated frames. Driver, storage, alarm/wake, UI context, and bus-quiet fault fixtures passed.
- App version 0.4.0-modbus-full, 1,575,984 bytes, 127,952 bytes free per unchanged OTA slot, 115,440 bytes smaller than baseline. SHA-256: 888bcb02a9e4cbd629cdf4ec0c47e96c93a446fef66f16e8fcbe409c055da3bd.
- A single local build and an example-data preview are the handoff. No archive was generated or device flashed. See `esp32_modbus_panel/docs/product-readiness-20261103.md` for SKU and hardware gates and explicit protocol limitations.
