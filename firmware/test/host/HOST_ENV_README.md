# Host Test Environment

**P2-2 F-P0-2 C3-GATE-002-R1/R2 TEST-INFRASTRUCTURE CORRECTION (revised)**

This directory contains host-side regression tests that compile production
firmware source files against minimal host shims under `shims/`.

## Quick Start

```bash
cd firmware/test/host
./setup_host_env.sh        # one-time setup (installs ArduinoJson + compat header)
./run_all_host_tests.sh    # build + run all 6 suites (R2-corrected: exit-code aware)
```

## Expected Results

| Test Suite | Makefile | Expected Result |
|-----------|----------|-----------------|
| TransactionJournalTest | Makefile.tj | 194/194 PASS, exit 0 |
| CommandRoutingTest | Makefile.cr | 133/133 PASS, exit 0 |
| CommandHashEquivalenceTest | Makefile.che | 26/26 PASS, exit 0 |
| WebServerTest | Makefile.ws | 111/111 PASS, exit 0 |
| MqttClientTest | Makefile.mc | 31/31 PASS, exit 0 |
| CommandHashBaseline | Makefile.chb | 14 vectors captured, exit 0 |
| **TOTAL** | | **495 assertions + 14 baseline vectors, all exit 0** |

## Environment Requirements

### 1. ArduinoJson v6.19.1+ (production-compatible)

**C3-GATE-002-R1 CORRECTED ROOT CAUSE (auditor-validated via matrix test):**

ArduinoJson v6.19.0 has a specific bug: `VariantSlot::setKey()` dereferences
a null pointer when `addMember()` returns null due to an exhausted
JsonDocument memory pool. This causes SIGSEGV when `publishStatus()` /
`_publishPirAck()` populate a `DynamicJsonDocument(6144)` with full device
state (12 channels × 4 schedules + PIRs + telemetry) and the pool overflows.

**ArduinoJson v6.19.1 explicitly fixes this bug** per upstream release
notes: *"Fix crash when adding an object member in a too small JsonDocument"*.
All subsequent versions (6.19.2+, 6.20.x, 6.21.x) inherit the fix.

#### Dependency Matrix Evidence (C3-GATE-002-R1)

Tested with identical host shim, fresh state, exit-code-aware runner:

| ArduinoJson Version | MqttClientTest | CommandHashBaseline | Classification |
|--------------------|----------------|---------------------|----------------|
| v6.18.2 | exit 0, 31 PASS | exit 0, 14 vectors | PASS (predates bug) |
| v6.19.0 | **exit 139 (SIGSEGV)** | **exit 139 (SIGSEGV)** | **CRASH** |
| v6.19.1 | exit 0, 31 PASS | exit 0, 14 vectors | **PASS (upstream fix)** |
| v6.19.4 | exit 0, 31 PASS | exit 0, 14 vectors | PASS |
| v6.20.1 | exit 0, 31 PASS | exit 0, 14 vectors | PASS |
| v6.21.6 | exit 0, 31 PASS | exit 0, 14 vectors | PASS |

**Conclusion:** Only v6.19.0 crashes. v6.19.1+ all pass. Auditor's hypothesis
confirmed — the original engineer claim "v6.19+ has a regression" was too
broad. The correct diagnosis is: **v6.19.0 has a specific bug, fixed in
v6.19.1 per upstream release notes**.

#### Production Compatibility

Production `platformio.ini` pins `bblanchon/ArduinoJson@^6.19.0`. Caret
semver (`^6.19.0`) allows any compatible version `>= 6.19.0, < 7.0.0`.
PlatformIO resolves this to the latest 6.x on ESP32 (where the real Arduino
String class is fully compatible with v6.19.1+).

**Host-test environment uses v6.19.1** (the minimum production-compatible
version that fixes the crash). This is **NOT a divergence from production**
— v6.19.1 satisfies `^6.19.0` and is the exact version auditor's R1 finding
identified as the upstream fix.

Previous correction (commit `dedfd67`) used v6.18.2 which predates the
production minimum — that was a silent divergence, now corrected.

### 2. arduinojson_compat.h (test-only, committed)

ArduinoJson v6.x references `::StringSumHelper` (a class returned by
Arduino's `String::operator+`) via the `IsString` trait when
`ARDUINOJSON_ENABLE_ARDUINO_STRING=1` (set by `shims/MqttClientDeps.h:186`).
The host shim's `String` class does not provide `StringSumHelper`.
Forward-declaring it as an empty class is sufficient — the trait is never
instantiated because production code never returns `StringSumHelper` (it
uses `operator+=` and explicit `String` constructors exclusively).

**File:** `shims/arduinojson_compat.h` (committed; auto-verified by
`setup_host_env.sh` if missing).

## R2 Correction: Exit-Code-Aware Runner

**C3-GATE-002-R2 AUDITOR FINDING:**

Previous `run_all_host_tests.sh` used `out=$(./bin 2>&1 || true)` which
**discarded exit code**. This meant SIGSEGV (exit 139) with empty output
was misclassified as PASS because `fail_count == 0`. The runner could emit
`[ALL GREEN]` even when a binary crashed — exactly the failure mode that
caused the original C3-GATE-002 blocker.

### R2 Fix

Corrected runner now:
1. **Captures exit code separately** from output (no `|| true`)
2. **Treats non-zero exit as FAILURE** (including signal terminations)
3. **Detects SIGSEGV** (exit 139 = 128+11) and other signals explicitly
4. **Requires zero [FAIL] AND non-zero [PASS] count** for suite PASS
5. **Does NOT emit [ALL GREEN]** if ANY suite fails or crashes

### R2 Acceptance Criteria Validation

Validated via `r2_failure_injection_test.cpp` (test-only, not committed)
in 4 modes against runner logic:

| Mode | Binary Behavior | Runner Verdict | Correct? |
|------|----------------|----------------|----------|
| `pass` | exit 0, 1 [PASS], 0 [FAIL] | PASS | ✓ |
| `exit1` | exit 1, 1 [PASS], 1 [FAIL] | **FAIL** (non-zero exit) | ✓ |
| `segfault` | exit 139 (SIGSEGV), partial output | **FAIL** (CRASH detected) | ✓ |
| `failonly` | exit 0, 1 [PASS], 1 [FAIL] | **FAIL** ([FAIL] count) | ✓ |

**End-to-end validation:** Runner tested against v6.19.0 (known crash
version) — correctly reported `FAIL — CRASH(exit=139,signal=SIGSEGV)` for
MqttClientTest + CommandHashBaseline, and exit code 1 for the runner
itself (not `[ALL GREEN]`).

## Provenance (C3-GATE-002 Disposition)

The crash trigger (`PubSubClient::_connected = true` default in
`shims/MqttClientDeps.h`) is **pre-existing** test infrastructure —
introduced in commit `677d386` (F-P0-1 correction 4, 2026-08-15 04:18:30 UTC),
well before C3.

C3 main commit `4cd5d0b` modified:
- `firmware/ConfigStore.cpp` + `.h` — added `saveScheduleWithResult(bool)`
- `firmware/RelayHandlers.h` — injected `type='relay'` for hash symmetry
- `firmware/ScheduleHandlers.h` — full journal wrap refactor
- `firmware/test/host/WebServerTest.cpp` — added P9-P14 + F9-F11 tests
- `firmware/test/host/shims/MqttClientDeps.h` — added FileSystem failure
  injection + ConfigStore::saveScheduleWithResult shim + WebServer query
  param support (NOT the PubSubClient class section)

C3 did NOT modify:
- The `PubSubClient` class section (lines 634-676) of `MqttClientDeps.h`
- `firmware/MqttClient.cpp` (production MQTT client)
- Any test Makefile (C3-GATE-002-R1/R2 commits modify Makefiles, not C3)

Therefore the MqttClientTest + CommandHashBaseline segfault is a
**pre-existing host-test environment defect** unblocked by C3's
introduction of fresh-clone reproducibility.

## What This Correction Does NOT Change

- **Production source** — unchanged (no `MqttClient.cpp`, no `ConfigStore`,
  no `ScheduleHandlers.h`, no `RestJournalHelper.h` modifications)
- **Canonical hash schema** — unchanged (`Utils::computeCommandHash()`)
- **MQTT semantics** — unchanged
- **C3 implementation** — unchanged
- **F11 failure-path test** — unchanged
- **RestJournalHelper contract** — unchanged
- **`platformio.ini` ArduinoJson pin** — unchanged (still `^6.19.0` for
  ESP32 production builds; host-test environment uses v6.19.1 which
  satisfies `^6.19.0` — no divergence)

## What This Correction DOES Change (vs commit `dedfd67`)

- **`setup_host_env.sh`**: changed ArduinoJson version from v6.18.2 → v6.19.1
  (production-compatible; eliminates host/production divergence)
- **`HOST_ENV_README.md`** (this file): corrected root cause analysis with
  dependency matrix evidence; documented R2 fix
- **`run_all_host_tests.sh`**: R2 fix — exit-code-aware runner that detects
  SIGSEGV / non-zero exit / [FAIL] assertions; no longer emits false green

## Verification

```bash
cd firmware/test/host
./setup_host_env.sh
./run_all_host_tests.sh
echo "Runner exit: $?"
```

Expected: runner exit 0, output ends with `[ALL GREEN] 495/495 + 14 baseline
vectors (every binary exit == 0)`.

To verify R2 detection works (optional, requires v6.19.0 install):
```bash
# Temporarily install v6.19.0 (crash version)
rm -rf ../../.pio/libdeps/development/ArduinoJson
git clone --depth 1 --branch v6.19.0 \
  https://github.com/bblanchon/ArduinoJson.git \
  ../../.pio/libdeps/development/ArduinoJson
./run_all_host_tests.sh
echo "Runner exit: $?"
# Expected: runner exit 1, MqttClientTest + CommandHashBaseline marked
# FAIL — CRASH(exit=139,signal=SIGSEGV), output ends with [FAILED]

# Restore v6.19.1
./setup_host_env.sh
./run_all_host_tests.sh
echo "Runner exit: $?"
# Expected: runner exit 0, [ALL GREEN]
```
