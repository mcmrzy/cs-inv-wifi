# TN readability and compact settings implementation plan

> Execution: implement and verify in this chat. Read-only plan and implementation review delegated under the writing-plans review requirement.

**Goal:** Improve native 320x240 TN readability, keep energy motion across held DSP samples, simplify settings and reduce firmware image size.

**Architecture:** Keep the existing strip renderer, real DSP display cache, protocol indexes and write validation. Change only presentation and navigation. Compress the original indexed house resource losslessly, with per-row offsets for bounded strip decoding and no heap allocation.

**Tech stack:** ESP-IDF C, existing host C fixtures, Python/Pillow resource generator.

User direction: high contrast, thicker energy flow, no stale-home message or age-driven animation stop. User confirmed low-frequency settings should move into Advanced while common menus stay simple.

## Chunk 1: UI and resources

- [x] `tests/home_model_test.c`, `tests/tn_settings_test.c`: observe failing held-sample state and common-menu navigation tests before implementation; preserve all 84 protocol parameters.
- [x] `main/direct/direct_home.c/.h`: allow motion from real retained readings; keep no-sample/zero/OTA/off-home gates. `main/direct/direct_display_events.c` needs no changes: its signature already delegates animation gating to the home model. Preserve independent freshness/alarm/write policies.
- [x] `main/direct/direct_ui.c/.h`, `main/direct/direct_ui_locale.c`, `main/direct/direct_ui_scene.c`: common six-item entry, short functional groups, advanced categories, six rows with inline values, no protocol indexes; high-contrast themes, inverse selection, thicker contrasting cables and directional markers.
- [x] `tools/generate_home_asset.py`, `main/direct/direct_home_asset.c`: lossless row PackBits, clipped decoding; generator verifies all decoded indexes and reviewer independently compares them with the old resource. `tests/home_asset_test.c` compares actual C-rendered pixel hashes with the original uncompressed renderer and preserves full-frame/strip/guard tests.
- [x] `tests/run_home_ui_tests.ps1`, `tests/run_production_host_tests.ps1`: integrate settings regression. Run related host checks and real C renders in Chinese/English and both themes, inspect previews, build Device firmware and compare same-config image sizes.
- [x] Read-only implementation/simplify review. Package an app-only candidate with size/hash evidence. Hardware angle/readability and timing acceptance remain on-device checks.

Common protocol indexes: `0,13,1,10,4,48`. Ordinary functional groups contain at most nine entries. Advanced contains generator, equalization, parallel, socket, raw schedule, battery voltage profiles, complete parameter list, generator information and service tools. `tests/tn_settings_test.c` verifies low-frequency indexes are absent from ordinary groups and present in advanced categories.

Validation commands (from project root): `./tests/run_home_ui_tests.ps1` must exit 0, including six-row navigation, real retained sample motion, fresh-to-held pixel equality and changed phase pixels, alarm/write safeguards, all locale glyphs and full-frame/16-row equality. `python tools/generate_home_asset.py --check` must exit 0 with 25,773 resource bytes and lossless round trip. `./tmp/build_lcd_house_native.ps1` must exit 0. Compare `tmp/tn_before/cs_inv_direct_c3.bin` against `build_lcd_house/cs_inv_direct_c3.bin`, with byte-identical `tmp/tn_before/sdkconfig.lcd_house` and `tmp/sdkconfig.lcd_house`.

Existing BMS/OTA/UI edits are preserved in place; before snapshots are under ignored `tmp/tn_before`. No flashing or publication is included.
