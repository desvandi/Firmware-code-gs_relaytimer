# REMAINING_P1_P2_GAPS

**Per ChatGPT audit directive:** "Setelah audit selesai, hanya implementasikan defect yang benar-benar ditemukan. Jangan menambah fitur baru. Jangan menurunkan requirement."

This document lists ALL remaining gaps after v4.3.1 verification audit. Items marked "DEFERRED" are documented in architecture but implementation pending. Items marked "HARDWARE" require physical ESP32 + sensors.

---

## P1 — Production Blockers

### P1-008: Durable telemetry store-and-forward
**Status:** 🟡 SOFTWARE ARCHITECTURE IMPLEMENTED, NVS PERSISTENCE DEFERRED

- ✅ `TelemetrySpool.{h,cpp}` — bounded ring buffer (16 records), sequence dedup, DROP_OLDEST overflow, rate-limited replay (2/sec)
- 🟡 NVS persistence NOT yet implemented — spool is RAM-only, lost on reboot
- 🟡 NOT wired into `MqttClient::publishStatus()` / `loop()` yet
- 🔴 Flash-wear characterization + power-loss validation: HARDWARE REQUIRED

**Remaining action:**
1. Wire `TelemetrySpool::spool()` into `MqttClient::publishStatus()` on publish failure
2. Wire `TelemetrySpool::replay()` into `MqttClient::loop()` on reconnect
3. Implement NVS blob persistence (write spool to NVS every N records, load on boot)
4. Flash-wear testing on actual ESP32 (HARDWARE)

### P1-009: Transactional config A/B recovery
**Status:** 🟡 PARTIAL (atomicWrite exists, true A/B not verified)

- ✅ `ConfigStore` uses `Storage::fs.atomicWrite()` (temp + rename pattern) — atomic at file level
- ✅ Backup file (`config.bak`) exists — loaded if primary corrupted
- 🟡 True A/B with explicit `active`/`inactive`/`commit marker` NOT verified
- 🔴 Power-cut during NVS write: HARDWARE REQUIRED

**Remaining action:**
1. Add explicit `configVersion` field to config JSON (already exists as `Core::CONFIG_VERSION`)
2. Add `commitMarker` (CRC of active config) — verify on boot
3. Implement A/B slot logic: write to inactive slot, validate, swap active pointer
4. Power-loss test on actual ESP32 (HARDWARE)

### P1-012: Ed25519 known-answer test
**Status:** 🟢 HOST KAT PASS, TARGET KAT NOT EXECUTED

- ✅ `scripts/test_ed25519_host_kat.py` — 10 assertions, all PASS
  - Tests: keypair gen, sign/verify correct, REJECT wrong msg/wrong key/tampered sig/tampered firmware
- 🔴 Target ESP32 KAT (same vectors on actual device with mbedtls): HARDWARE REQUIRED

**Remaining action:**
1. Run same test vectors on ESP32 with `mbedtls/ed25519.h` API
2. Verify ESP32 produces same PASS/REJECT results as host
3. If ESP32 rejects valid signature → `MBEDTLS_ED25519_SUPPORTED` build flag needed (see `platformio.ini` comment)

### P1-013: 12-case power-loss test
**Status:** 🔴 NOT EXECUTED — HARDWARE REQUIRED

Per ChatGPT audit Phase O: "test ini harus menggunakan hardware aktual, bukan simulator."

All 24 power-loss scenarios from original brief §86 remain NOT EXECUTED. See `HARDWARE_ACCEPTANCE_TEST_PLAN.md` for the 12 required hardware tests.

**Remaining action:**
1. Owner acquires physical ESP32 + relay + sensors
2. Execute HW-001 through HW-012
3. Capture evidence (Serial log + multimeter + PWA screenshot)
4. Mark each PASS or FAIL
5. If FAIL: root-cause analysis + fix + re-test

### P1-015: MQTT browser credential blast-radius
**Status:** 🟡 DOCUMENTED (architectural), NO AUTH GATEWAY

Per ChatGPT audit: "Jangan menambahkan gateway hanya karena audit meminta 'industrial'. Harus ada architectural justification."

- ✅ Current design: per-device MQTT username/password + broker ACL (blast-radius limited to one device)
- 🟡 PWA still uses `NEXT_PUBLIC_MQTT_USERNAME`/`PASSWORD` (browser bundle)
- ❌ No Auth Gateway for short-lived MQTT credentials

**Threat model:**
- For small fleet (<5 devices): current design acceptable if each device has unique credential + TLS + ACL
- For large fleet (>5 devices): Auth Gateway recommended (PWA → Auth Gateway → short-lived MQTT credential → broker)

**Remaining action:**
1. Document threat model in `SECURITY.md` (already done)
2. If fleet grows: deploy Auth Gateway (separate service, not in this repo)
3. Rotate PWA MQTT credential periodically (operational procedure)

### P1-017: Secure Boot + Flash Encryption provisioning
**Status:** 🟡 DOCUMENTED (procedure), NOT PROVISIONED

Per ChatGPT audit: "Saya sangat tidak menyarankan langsung mengaktifkannya pada ESP32 produksi. Harus ada: DISPOSABLE DEVICE → provision → secure boot → flash encryption → OTA → rollback → factory reset behavior → recovery."

- ✅ `DEPLOYMENT.md` documents Secure Boot + Flash Encryption provisioning procedure
- 🔴 NOT provisioned on any actual device
- 🔴 Provisioning on disposable ESP32: HARDWARE REQUIRED

**Remaining action:**
1. Acquire disposable ESP32 for provisioning testing
2. Execute provisioning procedure on disposable device
3. Verify Secure Boot rejects unsigned firmware
4. Verify Flash Encryption prevents NVS reads via JTAG
5. Verify OTA rollback still works with Secure Boot active
6. Only then: provision production devices

### P1-018: PWA timeout reconciliation
**Status:** 🟡 ARCHITECTURE DEFINED, IMPLEMENTATION PENDING

- ✅ `CommandExecutionState` type has `TIMEOUT` state (distinct from `FAILED`)
- 🟡 Reconciliation logic (TIMEOUT → reconnect → fetch state → resolve) NOT implemented in PWA

**Architecture (documented in `CONTROL_SEMANTICS_MATRIX.md` §6):**
```
TIMEOUT → MQTT reconnect → fetch /api/status →
  if reportedState == desiredState → RESOLVED (CONFIRMED_ON/OFF)
  else → STATE_DRIFT (operator must investigate)
```

**Remaining action:**
1. Implement reconciliation function in `useApi.ts`
2. Trigger on MQTT reconnect event
3. Update pending command state based on current device state

### P1-019: Command sequence / stale command protection
**Status:** 🟡 ARCHITECTURE DEFINED, IMPLEMENTATION DEFERRED (requires protocol v5)

- ✅ `requestId` dedup exists (TransactionJournal)
- 🟡 `commandSequence` field NOT in `CommandRequest`
- 🟡 Stale command detection NOT in `CommandArbiter::processCommand()`

**Architecture (documented in `CONTROL_SEMANTICS_MATRIX.md` §5):**
```
Command A: ON (seq=100) → executed, lastApplied=100
Command B: OFF (seq=101) → executed, lastApplied=101
Command A arrives late (seq=100 < lastApplied=101) → REJECT as STALE
```

**Why deferred:** Adding `commandSequence` is a breaking protocol change (requires PWA + firmware to both send/check the field). Scheduled for protocol v5 (v4.4.0 release).

**Remaining action:**
1. Add `commandSequence` field to `CommandRequest` struct
2. Add per-channel `lastAppliedSequence` to `Channel` struct
3. `CommandArbiter::processCommand()` rejects `seq < lastAppliedSequence`
4. Update PWA `api.ts` to send `commandSequence` per command
5. Update TransactionJournal to store `commandSequence`

---

## P2 — Recommended (Not Blocking)

### P2-001: PWA UI rendering of CommandExecutionState
**Status:** 🟡 TYPES DEFINED, UI NOT UPDATED

- ✅ `CommandExecutionState` type exists (8 states)
- 🟡 Relay card still shows simple ON/OFF — doesn't render TIMEOUT/UNKNOWN distinctly

**Remaining action:** Update `RelayCard` component to show `CommandExecutionState` badge.

### P2-002: PWA UI rendering of StateConfidence
**Status:** 🟡 TYPES DEFINED, UI NOT UPDATED

- ✅ `StateConfidence` type exists (SOFTWARE_ONLY/VERIFIED/UNKNOWN/FAULT)
- 🟡 Relay card doesn't show confidence badge

**Remaining action:** Update `RelayCard` to show "SOFTWARE_ONLY" badge (honest disclosure — no aux contact feedback).

### P2-003: PWA MQTT password in memory
**Status:** 🟡 DOCUMENTED LIMITATION

Per ChatGPT audit §14: "password tetap berada di state.password selama session."

- ✅ `mqttTransaction.ts` does NOT store password (good)
- 🟡 `mqtt.ts` stores password in state for session duration

**Remaining action:** Refactor `mqtt.ts` to only use password during `mqtt.connect()` call, not store in state.

### P2-004: HealthSupervisor action policy wiring
**Status:** 🟡 STATE COMPUTED, ACTIONS NOT WIRED

- ✅ `HealthState` computed (HEALTHY/WARNING/DEGRADED/FAILED/RECOVERING)
- 🟡 Actions per state NOT wired (FAILED → force BOOT_OFF + inhibit scheduler)

**Remaining action:** Wire `_recomputeSystemState()` result into `RelayEngine` + `Scheduler` to actually disable subsystems on FAILED.

### P2-005: Boot loop safe state action
**Status:** 🟡 DETECTION CORRECT, ACTION NOT WIRED

- ✅ `BOOT_LOOP` alarm raised when 3+ boots in 60s
- 🟡 Safe state (force BOOT_OFF, inhibit scheduler) NOT implemented

**Remaining action:** On `bootLoopDetected=true`, enter safe state.

### P2-006: Command whitelist (D-008)
**Status:** 🟡 BLACKLIST (current), WHITELIST (target)

- ✅ Current: `CommandSemantics::NonIdempotentAction` rejected (blacklist — functionally safe)
- 🟡 Target: `SupportedCommandType` enum + explicit whitelist

**Remaining action:** Add whitelist for v4.3.2.

### P2-007: REST endpoints for safety ACK/CLEAR
**Status:** 🟡 API DEFINED, ENDPOINTS NOT ADDED

- ✅ `SafetySupervisor::acknowledgeSafetyAlarm()` + `clearSafetyLockout()` exist
- 🟡 No REST endpoint (`/api/safety/acknowledge`, `/api/safety/clear`) for PWA to call

**Remaining action:** Add `SafetyHandlers.h` with REST endpoints.

### P2-008: CI/CD pipeline
**Status:** 🟡 DOCUMENTED, NOT IMPLEMENTED

- ✅ `DEPLOYMENT.md` documents CI/CD with secret scanning
- 🟡 No `.github/workflows/` YAML files in repo

**Remaining action:** Add GitHub Actions workflow files for firmware compile + PWA build + secret scan.

---

## Summary

| ID | Item | Severity | Status |
|---|---|---|---|
| P1-008 | Telemetry store-and-forward | P1 | 🟡 Architecture done, NVS persistence + wiring pending |
| P1-009 | Config A/B recovery | P1 | 🟡 atomicWrite exists, true A/B verification pending |
| P1-012 | Ed25519 KAT | P1 | 🟢 Host PASS, 🔴 Target NOT EXECUTED |
| P1-013 | 12-case power-loss test | P1 | 🔴 NOT EXECUTED — HARDWARE |
| P1-015 | MQTT browser credential | P1 | 🟡 Per-device ACL (acceptable for small fleet), Auth Gateway for large fleet |
| P1-017 | Secure Boot provisioning | P1 | 🟡 Procedure documented, 🔴 NOT PROVISIONED |
| P1-018 | PWA timeout reconciliation | P1 | 🟡 Architecture defined, implementation pending |
| P1-019 | Command sequence | P1 | 🟡 Architecture defined, deferred to protocol v5 |
| P2-001 | PWA CommandExecutionState UI | P2 | 🟡 Types defined, UI not updated |
| P2-002 | PWA StateConfidence UI | P2 | 🟡 Types defined, UI not updated |
| P2-003 | PWA MQTT password in memory | P2 | 🟡 Documented limitation |
| P2-004 | HealthSupervisor action wiring | P2 | 🟡 State computed, actions not wired |
| P2-005 | Boot loop safe state | P2 | 🟡 Detection correct, action not wired |
| P2-006 | Command whitelist | P2 | 🟡 Blacklist (current), whitelist (target) |
| P2-007 | REST endpoints for safety ACK/CLEAR | P2 | 🟡 API defined, endpoints not added |
| P2-008 | CI/CD pipeline | P2 | 🟡 Documented, not implemented |

**Production readiness gate:** ALL P1 items marked ✅ PASS (with evidence) + all 12 hardware tests PASS.
