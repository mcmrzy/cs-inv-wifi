# Host regressions

Use a native C compiler and the same cJSON source as the installed ESP-IDF:

```sh
cmake -S tests/host -B build/ble-host-tests -DCJSON_DIR="$IDF_PATH/components/json/cJSON"
cmake --build build/ble-host-tests
ctest --test-dir build/ble-host-tests --output-on-failure
```

On Windows, run from a Visual Studio x64 developer prompt and use `-G Ninja`
with Ninja on PATH (or set `CMAKE_MAKE_PROGRAM`). Use a Windows path for
`CJSON_DIR`. The tests do not require a BLE radio, NVS, or device credentials.

`ble_control` compiles the real AUTH/COMMAND callbacks and real cJSON. Stubs
replace only platform/transport/executor/crypto boundaries. Assertions cover
read result codes, rejection of the W-to-A alias, nonfinite data, bounded input,
embedded NUL/trailing JSON garbage, invalid timestamps, AUTH types, and log
redaction. The stub refuses oversized copies to keep the vulnerable baseline
from corrupting the test runner; assertions verify rejection *before* copying.

The crypto stubs are not cryptographic implementations. The authentication
setup explicitly exercises the legacy nonce-only flow to reach COMMAND tests;
it does not establish that authentication is secure. Replace this setup when
the versioned mutual-authentication contract lands. No test here proves
per-connection isolation, ARM apply/ACK behavior, or hardware OTA correctness.

`arm_iap_codec` runs the existing codec regression without changing its source.

`ble_pin_logging` compiles the real `ble_ct_pin.c` and captures success, mismatch,
and lockout logs to ensure neither supplied nor expected PINs are emitted.
Only clock, logging, and HMAC boundaries are faked; this is not cryptographic or
hardware validation. `pin_log_redaction` additionally checks source log patterns.

`ble_ota` compiles the real `main/ota/ble_ota.c` adapter with real cJSON. FreeRTOS
queue/task, GATT transport, provisioning handles, and `ota_service` are host stubs
(the authentication gate was removed from the adapter on 2026-09; access is
unauthenticated by product decision). Assertions cover unauthenticated rejection, v2
envelope checks, malformed signature / sha256 / security_version / invalid
target rejection, unsigned-manifest acceptance (optional metadata, 2026-09),
session and transfer binding, continuous offset acceptance, remaining-size
overflow, queue backpressure, query/cancel/status read, and that accepted bytes
are what `ota_service_start_local` consumes. This does not prove flash, radio,
or rollback behavior on hardware.
