# Host Test Environment

**P2-2 F-P0-2 C3-GATE-002 TEST-INFRASTRUCTURE CORRECTION**

This directory contains host-side regression tests that compile production
firmware source files against minimal host shims under `shims/`.

## Quick Start

```bash
cd firmware/test/host
./setup_host_env.sh        # one-time setup (installs ArduinoJson + compat header)
./run_all_host_tests.sh    # build + run all 6 suites
```

## Expected Results

| Test Suite | Makefile | Expected Result |
|-----------|----------|-----------------|
| TransactionJournalTest | Makefile.tj | 194/194 PASS |
| CommandRoutingTest | Makefile.cr | 133/133 PASS |
| CommandHashEquivalenceTest | Makefile.che | 26/26 PASS |
| WebServerTest | Makefile.ws | 111/111 PASS |
| MqttClientTest | Makefile.mc | 31/31 PASS |
| CommandHashBaseline | Makefile.chb | 14 vectors captured |
| **TOTAL** | | **495 assertions + 14 baseline vectors** |

## Environment Requirements

### 1. ArduinoJson v6.18.2 (NOT v6.19+)

Production `platformio.ini` pins `bblanchon/ArduinoJson@^6.19.0`. On ESP32,
PlatformIO resolves this to the latest 6.x (v6.21.x) where the real Arduino
String class is fully compatible.

On HOST builds, the shim's `String` class (`shims/Arduino.h`) is a minimal
`std::string` wrapper. ArduinoJson v6.19.0+ has a regression where
`VariantSlot::setKey()` does NOT gracefully handle a null slot returned by
an exhausted memory pool — instead it dereferences the null pointer (SEGV).
v6.18.x handles this case gracefully by returning null `VariantRef`, which
the production code already tolerates.

**Symptom:** MqttClientTest + CommandHashBaseline segfault at
`VariantSlot::setKey()` when `publishStatus()` / `_publishPirAck()` populate
a `DynamicJsonDocument` that overflows its pool.

**Fix:** `setup_host_env.sh` installs ArduinoJson v6.18.2 to
`firmware/.pio/libdeps/development/ArduinoJson/` (gitignored).

### 2. arduinojson_compat.h (auto-generated)

ArduinoJson v6.x references `::StringSumHelper` (a class returned by
Arduino's `String::operator+`) via the `IsString` trait when
`ARDUINOJSON_ENABLE_ARDUINO_STRING=1` (set by `shims/MqttClientDeps.h:186`).
The host shim's `String` class does not provide `StringSumHelper`.
Forward-declaring it as an empty class is sufficient — the trait is never
instantiated because production code never returns `StringSumHelper` (it
uses `operator+=` and explicit `String` constructors exclusively).

**Fix:** `setup_host_env.sh` creates `shims/arduinojson_compat.h` with
the forward declaration. Makefiles auto-include it via `-include` flag.

## Provenance (C3-GATE-002 Disposition)

These two host-environment requirements are **pre-existing** test
infrastructure issues — NOT introduced by C3.

### Root Cause Analysis

1. **`PubSubClient::_connected = true` default** — introduced in commit
   `677d386` (F-P0-1 correction 4, dated 2026-08-15 04:18:30 UTC).
   This is F-P0-1 baseline, well before C3.
2. **ArduinoJson v6.19.0+ regression** — exists in upstream ArduinoJson
   releases. The host shim's String class exposes this regression; the
   real ESP32 Arduino String class does not.

### C3 Non-Involvement Evidence

C3 main commit `4cd5d0b` modified these files (per `git show --stat 4cd5d0b`):
- `firmware/ConfigStore.cpp` + `.h` — added `saveScheduleWithResult(bool)`
- `firmware/RelayHandlers.h` — injected `type='relay'` for hash symmetry
- `firmware/ScheduleHandlers.h` — full journal wrap refactor
- `firmware/test/host/WebServerTest.cpp` — added P9-P14 + F9-F11 tests
- `firmware/test/host/shims/MqttClientDeps.h` — added FileSystem failure
  injection + ConfigStore::saveScheduleWithResult shim + WebServer query
  param support

C3 commit `4cd5d0b` did NOT modify:
- The `PubSubClient` class section (lines 634-676) of `MqttClientDeps.h`
- `firmware/MqttClient.cpp` (production MQTT client)
- Any other test Makefile or shim

Therefore the MqttClientTest + CommandHashBaseline segfault is a
**pre-existing host-test environment defect** unblocked by C3's
introduction of fresh-clone reproducibility (previously the test
environment had been set up ad-hoc and not documented).

### What This Correction Does NOT Change

- **Production source** — unchanged (no `MqttClient.cpp`, no `ConfigStore`,
  no `ScheduleHandlers.h`, no `RestJournalHelper.h` modifications)
- **Canonical hash schema** — unchanged (`Utils::computeCommandHash()`)
- **MQTT semantics** — unchanged
- **C3 implementation** — unchanged
- **F11 failure-path test** — unchanged
- **RestJournalHelper contract** — unchanged
- **`platformio.ini` ArduinoJson pin** — unchanged (still `^6.19.0` for
  ESP32 production builds; only host-test environment uses v6.18.2)

### What This Correction DOES Change

- **Test infrastructure only**:
  - Added `setup_host_env.sh` (one-time environment setup script)
  - Added `run_all_host_tests.sh` (regression runner wrapper)
  - Added `shims/arduinojson_compat.h` (test-only forward declaration)
  - Updated 4 Makefiles (Makefile.ws, Makefile.mc, Makefile.chb,
    Makefile.che) to auto-include the compat header and document the
    environment requirement

## Verification

```bash
cd firmware/test/host
./setup_host_env.sh
./run_all_host_tests.sh
```

Expected output ends with:
```
[ALL GREEN] 495/495 + 14 baseline vectors
```
