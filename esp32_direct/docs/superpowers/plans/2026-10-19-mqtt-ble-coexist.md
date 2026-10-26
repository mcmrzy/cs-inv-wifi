# MQTT/BLE coexistence and OTA memory implementation plan

> **For agentic workers:** Use subagent-driven-development with independent review; preserve existing dirty files. Do not commit, push, flash, or deploy in this task.

**Goal:** Keep normal MQTT and a discoverable/connectable BLE peripheral resident together, and prevent the observed remote-DSP-download heap exhaustion and cache null write.

**Architecture:** User approved normal coexistence with temporary idle-only BLE suspension for remote OTA. Recover RAM by moving optional Wi-Fi speed-optimized code from IRAM into flash, not by reducing TLS record capacity, task stacks, BLE MTU or security. Admit BLE only with adequate internal contiguous heap; MQTT reconnect may temporarily pause idle BLE, never an active phone/local OTA. OTA controls network-backfill admission before allocating its full worker and second TLS connection.

**Tech Stack:** ESP-IDF 5.5.4, ESP32-C3, NimBLE, MQTT/TLS, C host regression harnesses.

## Evidence and scope

- Field 0.6.38 ELF prefix `f3b164b1d`: connected at 4065ms, free=57920/max=40960; 35s TLS TX allocation 2389 failed with max=2304 while remote DSP image was downloading. No BLE startup in this excerpt.
- Matching ELF resolves RA `0x420262ba` to `offline_cache_add:446`. A previously failed full batch can successfully flush/free on the next add, which then dereferences the cleared pointer.
- Do not change DSP/BMS protocol, partition layout, SN/Wi-Fi, TLS IN=16384/OUT=2048, MQTT stack=8192, OTA worker=12288, or BLE MTU=512.

## Session plan and ownership

| Session | Role | Goal | Owned paths | Dependency / done signal |
|---|---|---|---|---|
| Main | implementer/integrator | OTA network peak and coexist state, configuration, release | app_main, mqtt_handler, ota_service, defaults, related tests/docs | root evidence; reviewed build + regression |
| S2 Rawls | researcher/implementer | conservative BLE budget and idle-only OTA suspend | ble_prov.c/.h, ble_ota_memory_lifecycle_test.py | none; source-backed report and RED/GREEN |
| S3 Lagrange | implementer | full-batch retry null-pointer fix | offline_cache.c, offline_cache_memory_test.py | none; RED then GREEN |
| S4 reviewer | reviewer | requirements then lifecycle/quality review | none | implementation; no unresolved P1/P2 |

## Chunk 1: deterministic crash

- [ ] Extract real cache functions into host regression; fail on full-batch retry after earlier persistence failure.
- [ ] Flush a pre-existing full batch before allocating/reusing the next batch; handle allocation/persistence failure without null/OOB writes.
- [ ] Verify retry, allocation-failure, normal batch flush and data-retention cases.

## Chunk 2: OTA memory peak

- [ ] Test all remote targets pause new cache workers and drain the in-flight PUBACK before allocating worker/HTTP TLS; MQTT control stays alive and DSP polling continues during download.
- [ ] Implement reversible cache-backfill admission and network quiet for all remote targets; restore on cancellation/failure/success.
- [ ] Add WARN heap checkpoints at download setup/stream stages for field acceptance.

## Chunk 3: coexistence

- [ ] Test saved-Wi-Fi MQTT success starts BLE without releasing MQTT; active peer is never forced down; no-Wi-Fi provisioning, reconnect, failed BLE init and OTA exclusion remain safe.
- [ ] Disable optional Wi-Fi IRAM/RX speed placement in dedicated build and defaults; compare linker heap start against 0.6.38. Keep pool/MTU/stack limits unchanged initially.
- [ ] Replace exclusive default policy with guarded coexistence; keep low-memory exclusive local fallback and idle-only reconnect windows.
- [ ] Build candidate 0.6.39, independently review and run targeted regressions. Package application-only binary and hash.

## Merge gate / verification

- Independent write sets; main reviews S3 delta before integration. Reviewer checks approved behavior before quality/lifecycle review. No inferred hardware success from host tests.
- IDF `idf.py -B build_mqtt_coexist_20261019 build`; `git diff --check`; run cache/coexist/MQTT/TLS/BLE/OTA regressions with IDF Python.
- Verify embedded version, image checksum, SHA256, unchanged partitions and TLS settings. Report built/tested separately from device flashing, real BLE+MQTT coexistence, OTA install and version readback.

## Abort / fallback

- If linker savings or field headroom cannot support coexistence, do not remove the heap guard or claim success. Retain safe MQTT/local fallback and report actual evidence.
- Current task does not authorize changing production broker, credentials, hardware power or DSP firmware.

## Final implementation and merge result

User clarified that other work may time-share, but DSP normal communications take priority. Network quiet now leaves ordinary DSP polling active (including ESP download); the existing UART upgrade owner still governs actual target IAP. DSP task remains priority 6, download worker 5. No DSP protocol or task source was changed this round.

S2/S3 disjoint changes were reviewed and integrated. Independent review found owner-restoration and explicit-local-fallback gaps; real C regressions reproduced these before fixes, including the combined OTA-owner + exclusive-local sequence. Final review reported no remaining P1/P2. Simplification review removed the obsolete exclusive-cloud-mode test rather than retaining contradictory expectations; no broad scheduler rewrite was added.

Final candidate build: `build_mqtt_coexist_20261019`, version `0.6.39-mqtt-ble-coexist`, app 1,618,544 bytes, free OTA capacity 85,392 bytes. Final linked heap start `0x3fcaad20` versus old `0x3fcaf720`: +18,944B internal heap. This is a linker comparison, not a claim that largest contiguous runtime heap grows by that amount.

Final 13 directed regression groups passed (BLE 16 tests; offline cache 6), plus DSP TX race and real direct-OTA flow host checks. Image checksum and validation hash are valid; partition binary unchanged. Application SHA-256: `cc7a9a331d4566ca73fc6043b044e33c5dc7e9fcc0a31b09f3708c4b0de1bb05`. No flashing, hardware/production actions, Git commit/push or memory writes. Release notes state the pending physical coexistence, DSP gap and complete OTA/version-readback acceptance separately.
