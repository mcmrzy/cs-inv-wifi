# Grouped battery commands

MQTT and BLE use the same complete configuration object. Every field below is
required exactly once. Extra fields, missing fields, duplicates, strings,
booleans, nonintegers, negative numbers and values outside the integer widths
are rejected before any storage or UART work. The shared native battery policy
also validates relationships between fields. Partial updates are unsupported.

```json
{"type":4,"protocol":5,"material":0,"cells":0,"bulk_dv":540,"float_dv":540,"cutoff_dv":450,"max_charge_a":120,"ac_charge_a":50,"back_utility_dv":480,"back_battery_dv":520,"soc_low":20,"soc_high":80,"soc_cutoff":10,"revision":5}
```

`type` is the **native DSP battery type**: 1 AGM, 2 Flooded, 3 User Pb,
4 managed lithium, 5 User lithium. This differs from the old scalar collector
enum, where 3 is lithium and 4 is User Pb. No scalar type conversion is applied
to this object. A scalar type switch returns `PROFILE_REQUIRED` (-16).

Voltages (`*_dv`) are whole-pack decivolts, currents (`*_a`) are amps, and SOC
fields are integer percentages. All 14 configuration fields are uint16 values;
`revision` is a required uint32 optimistic concurrency token (0..4294967295),
not DSP payload. Read the current configuration and submit its revision with
the complete edited draft. A stale draft returns `REVISION_CONFLICT` (-14).

`protocol=5` is supported only for managed lithium and selects the existing
BBJ7C implementation. Other battery types require protocol 0. Unsupported
protocols return `PROTOCOL_UNSUPPORTED` (-15). `material` (0 LFP, 1 NMC) and
`cells` apply to User lithium; other types require both to be zero. A profile
required by the native policy returns `PROFILE_REQUIRED` (-16).

## MQTT

Send `{"cmd":"get_battery_config","task_id":"read-1"}`. The completed
response includes `config` with all fields and the current `revision`.

For writing, send `{"cmd":"set_battery_config","task_id":"write-1","args":{...}}`
with the complete configuration object directly in `args` (not an array).
Normal command envelopes and `expires_at` remain supported. Retained writes
are rejected. Canonical field order makes reordered objects the same intent;
reusing a task ID for a different intent returns `IDEMPOTENCY_CONFLICT`.

Accepted work is cached before calling the single native grouped writer with
`cloud=true`. Successful enqueue is acknowledged; completion arrives through
the existing `CS_CMD_WRITE_CTRLPARAM`, index 4, size 2 callback and keeps the
original command/task. The callback's type value uses the old collector enum.
Success has `stage:"submitted"`, `saved:true`, `submitted:true`,
`observed:false`, and `desired_revision`. This proves ESP persistence and UART
submission; it does not claim DSP application or physical acceptance.

## BLE

After the existing v2 `open_session` framing/direct-control handshake, send
`action:"get_battery_config", params:{}`. The completed result contains
`actual.config`, including its current revision. Reads omit the redundant
top-level `parameter_revision`; writes retain it in the completion receipt.

For `action:"set_battery_config"`, `params` may be the complete configuration
object directly, or exactly `{"config":{...}}`. Both spellings normalize to
the same idempotency intent. The component calls injected main-layer JSON
executors; it has no dependency on main headers. Validation-only calls must
not mutate storage, UART or command ownership.

COMMAND and RESULT are bounded to 1024 bytes, with the existing 64-frame and
timeout bounds. `open_session` advertises
`actual.battery_config_max_message_bytes:1024`. Full groups can exceed the old
512-byte limit; clients must support the advertised battery message capacity.
The eight cached result bodies remain bounded to 512 bytes and retain the
existing ten-minute replay window. Full read envelopes can exceed 512 bytes;
final write envelopes with the usual App identifiers remain within 512 bytes.

The accepted result and worst-case completion envelope are encoded and cached
before execution. Allocation/preflight failure cannot start a write. A final
success has `status:"saved"`, `actual.param_id:"battery_config"`,
`actual.saved:true`, `actual.submitted:true`, `actual.observed:false`, and
`parameter_revision`. There is no fabricated scalar `value` or applied claim.
Existing operation tokens, disconnect handling and callback timeout behavior
also apply to grouped writes.

## Host verification

With `TEMP` and `TMP` set to the existing project `tmp` directory, run
`python tests/battery_command_test.py`. The test executes the actual MQTT and
BLE dispatchers and shared native policy with host-only storage/UART boundaries;
it performs no device writes. GCC may require execution outside the sandbox.
