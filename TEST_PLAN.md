# Test Plan — Timer Digital Relay v4.3.8

> Implements testing requirements from the Industrial-Grade Implementation
> Directive §85-91, §86, §107.

Per brief §107, each test MUST use one of these result strings:
- `PASS` — test was executed and passed
- `FAIL` — test was executed and failed
- `NOT EXECUTED — HARDWARE REQUIRED` — test requires physical hardware not available

**Forbidden**: "probably works", "should work", "implemented", "verified" without evidence.

---

## 1. Unit Tests (brief §85)

### 1.1 Logic Tests (executed via Python)

File: `scripts/test_battery_logic.py`

| Test ID | Description | Result |
|---|---|---|
| UNIT-001 | Imppt = Iinverter - Ibattery (5 acceptance scenarios from brief §62) | PASS |
| UNIT-002 | Cell calculation from cumulative nodes (brief §14) | PASS |
| UNIT-003 | Tap-fault detection (C[n+1] < C[n] - tolerance) (brief §18) | PASS |
| UNIT-004 | Power calculations (signed, charging vs discharging) (brief §21) | PASS |
| UNIT-005 | Power-flow consistency unified formula (brief §22) | PASS |
| UNIT-006 | Energy integration with spike rejection (brief §23) | PASS |
| UNIT-007 | Resistance estimation single-pass min/max algorithm (brief §25-26) | PASS |
| UNIT-008 | SOC coulomb counting with voltage sync (brief §24) | PASS |
| UNIT-009 | INA219 config bit verification (0x3FFB for 32V/±320mV) | PASS |
| UNIT-010 | Resistance staleness check (5-min threshold) | PASS |

### 1.2 Firmware Syntax Check

Command: `g++ -std=c++17 -fsyntax-only -I. -I<stubs> scripts/firmware_syntax_check.cpp`

| Test ID | Description | Result |
|---|---|---|
| FW-SYN-001 | All headers parse cleanly with Arduino stubs | PASS |

### 1.3 PWA Static Checks

| Test ID | Command | Result |
|---|---|---|
| PWA-TSC-001 | `bunx tsc --noEmit` | PASS |
| PWA-LINT-001 | `bun run lint` (eslint) | PASS |
| PWA-BUILD-001 | `bun run build` (next build, 25/25 pages) | PASS |

---

## 2. Integration Tests (brief §85)

### 2.1 PWA ↔ ESP32 REST Round-Trip

| Test ID | Description | Precondition | Result |
|---|---|---|---|
| INT-REST-001 | PWA logs in → receives JWT + CSRF | ESP32 booted, user configured | NOT EXECUTED — HARDWARE REQUIRED |
| INT-REST-002 | PWA polls /api/status every 3s → telemetry updates | Authenticated | NOT EXECUTED — HARDWARE REQUIRED |
| INT-REST-003 | PWA toggles relay → relay changes state → status reflects new state | Authenticated + relay wired | NOT EXECUTED — HARDWARE REQUIRED |
| INT-REST-004 | PWA upserts schedule → schedule active → relay follows schedule | RTC time valid | NOT EXECUTED — HARDWARE REQUIRED |

### 2.2 PWA ↔ ESP32 MQTT Round-Trip

| Test ID | Description | Precondition | Result |
|---|---|---|---|
| INT-MQTT-001 | PWA connects to broker → subscribes to `timer12/<mac>/status` → receives telemetry every 5s | MQTT TLS + ACL configured | NOT EXECUTED — HARDWARE REQUIRED |
| INT-MQTT-002 | PWA publishes relay command → ESP32 ACKs → relay changes state | Authenticated + MQTT configured | NOT EXECUTED — HARDWARE REQUIRED |
| INT-MQTT-003 | PWA disconnects → ESP32 LWT publishes "0" → PWA shows OFFLINE | LWT message configured | NOT EXECUTED — HARDWARE REQUIRED |

### 2.3 ESP32 ↔ GAS Round-Trip

| Test ID | Description | Precondition | Result |
|---|---|---|---|
| INT-GAS-001 | ESP32 POSTs logs + status to GAS → HMAC verified → Gemini generates insights → cached | HMAC secret configured, Gemini API key set | NOT EXECUTED — HARDWARE REQUIRED |
| INT-GAS-002 | PWA fetches insights via GET → cache hit | Insights cached | NOT EXECUTED — HARDWARE REQUIRED |
| INT-GAS-003 | Replay attack (same nonce) → 401 "Nonce already used" | Valid HMAC secret | NOT EXECUTED — HARDWARE REQUIRED |

---

## 3. Fault Injection Tests (brief §87)

These require `#define FAULT_INJECTION_ENABLED` compile flag (NOT in production builds).

| Test ID | Description | Fault | Expected | Result |
|---|---|---|---|---|
| FAULT-001 | MQTT broker unreachable | FAIL_MQTT_CONNECT | Circuit breaker opens after 5 fails, retries with backoff | NOT EXECUTED — HARDWARE REQUIRED |
| FAULT-002 | MQTT publish fails | FAIL_MQTT_PUBLISH | ACK queued in NVS journal, retried on reconnect | NOT EXECUTED — HARDWARE REQUIRED |
| FAULT-003 | NVS write fails | FAIL_NVS_WRITE | Transaction aborts, no physical mutation | NOT EXECUTED — HARDWARE REQUIRED |
| FAULT-004 | NVS read fails | FAIL_NVS_READ | Boot fails safe — firmware refuses to start | NOT EXECUTED — HARDWARE REQUIRED |
| FAULT-005 | RTC I²C fails | FAIL_RTC | RTC state → INVALID, scheduler inhibited | NOT EXECUTED — HARDWARE REQUIRED |
| FAULT-006 | PZEM Modbus fails | FAIL_PZEM | PZEM telemetry → UNAVAILABLE status, relay control continues | NOT EXECUTED — HARDWARE REQUIRED |
| FAULT-007 | GAS unreachable | FAIL_GAS | Insights not generated, no impact on relay control | NOT EXECUTED — HARDWARE REQUIRED |
| FAULT-008 | OTA download fails | FAIL_OTA_DOWNLOAD | OTA aborts, no flash write, previous firmware retained | NOT EXECUTED — HARDWARE REQUIRED |
| FAULT-009 | OTA signature invalid | FAIL_OTA_VERIFY | OTA aborts, alarm raised | NOT EXECUTED — HARDWARE REQUIRED |
| FAULT-010 | Force watchdog reset | FORCE_WATCHDOG | Watchdog fires, system reboots, bootCount increments | NOT EXECUTED — HARDWARE REQUIRED |

---

## 4. Power-Loss Tests (brief §86)

24 required power-loss scenarios. All require physical hardware + ability to cut ESP32 power mid-operation.

| Test ID | Description | Result |
|---|---|---|
| PL-001 | Power loss before command received | NOT EXECUTED — HARDWARE REQUIRED |
| PL-002 | Power loss during validation | NOT EXECUTED — HARDWARE REQUIRED |
| PL-003 | Power loss during prepare | NOT EXECUTED — HARDWARE REQUIRED |
| PL-004 | Power loss after physical mutation (relay changed) | NOT EXECUTED — HARDWARE REQUIRED |
| PL-005 | Power loss before commit (journal not durable) | NOT EXECUTED — HARDWARE REQUIRED |
| PL-006 | Power loss after commit (journal durable) | NOT EXECUTED — HARDWARE REQUIRED |
| PL-007 | Power loss before ACK published | NOT EXECUTED — HARDWARE REQUIRED |
| PL-008 | Power loss after ACK published | NOT EXECUTED — HARDWARE REQUIRED |
| PL-009 | Duplicate command after reboot (same requestId) | NOT EXECUTED — HARDWARE REQUIRED |
| PL-010 | requestId collision (same ID, different payload) | NOT EXECUTED — HARDWARE REQUIRED |
| PL-011 | Journal corruption (NVS sector bad) | NOT EXECUTED — HARDWARE REQUIRED |
| PL-012 | CRC corruption (journal entry CRC mismatch) | NOT EXECUTED — HARDWARE REQUIRED |
| PL-013 | Full journal (64-entry ring full) | NOT EXECUTED — HARDWARE REQUIRED |
| PL-014 | Journal rollover (oldest entry evicted) | NOT EXECUTED — HARDWARE REQUIRED |
| PL-015 | NVS failure (erase/write error) | NOT EXECUTED — HARDWARE REQUIRED |
| PL-016 | Config corruption (CRC mismatch on boot) | NOT EXECUTED — HARDWARE REQUIRED |
| PL-017 | OTA interruption (power loss during download) | NOT EXECUTED — HARDWARE REQUIRED |
| PL-018 | OTA power loss (after flash write, before boot) | NOT EXECUTED — HARDWARE REQUIRED |
| PL-019 | OTA invalid signature (forged binary) | NOT EXECUTED — HARDWARE REQUIRED |
| PL-020 | Rollback (new firmware boot fails) | NOT EXECUTED — HARDWARE REQUIRED |
| PL-021 | RTC invalid (battery dead) | NOT EXECUTED — HARDWARE REQUIRED |
| PL-022 | PZEM unavailable (sensor disconnected) | NOT EXECUTED — HARDWARE REQUIRED |
| PL-023 | PIR failure (stuck HIGH) | NOT EXECUTED — HARDWARE REQUIRED |
| PL-024 | WiFi unavailable 24 hours (relay + scheduler continue) | NOT EXECUTED — HARDWARE REQUIRED |

---

## 5. Scheduler Test Matrix (brief §89)

| Test ID | Description | Result |
|---|---|---|
| SCHED-001 | Same-day schedule (on=10:00, off=11:00) | NOT EXECUTED — HARDWARE REQUIRED |
| SCHED-002 | Overnight schedule (on=22:00, off=06:00) | NOT EXECUTED — HARDWARE REQUIRED |
| SCHED-003 | Midnight boundary (on=23:59, off=00:01) | NOT EXECUTED — HARDWARE REQUIRED |
| SCHED-004 | Weekday-specific (Mon-Wed-Fri only) | NOT EXECUTED — HARDWARE REQUIRED |
| SCHED-005 | Overlapping schedules on same channel | NOT EXECUTED — HARDWARE REQUIRED |
| SCHED-006 | Duplicate schedule (same onTime/offTime/dayMask) | NOT EXECUTED — HARDWARE REQUIRED |
| SCHED-007 | Delete active schedule → relay turns OFF | NOT EXECUTED — HARDWARE REQUIRED |
| SCHED-008 | Modify active schedule → relay follows new schedule | NOT EXECUTED — HARDWARE REQUIRED |
| SCHED-009 | Reboot during active schedule → schedule resumes | NOT EXECUTED — HARDWARE REQUIRED |
| SCHED-010 | RTC correction during active schedule → no double-toggle | NOT EXECUTED — HARDWARE REQUIRED |
| SCHED-011 | DST/timezone change → no spurious toggles | NOT EXECUTED — HARDWARE REQUIRED |
| SCHED-012 | Manual override during schedule → manual wins, schedule resumes after | NOT EXECUTED — HARDWARE REQUIRED |
| SCHED-013 | PIR conflict with schedule → priority order respected | NOT EXECUTED — HARDWARE REQUIRED |

---

## 6. Relay Test Matrix (brief §90)

| Test ID | Description | Result |
|---|---|---|
| REL-001 | ON command | NOT EXECUTED — HARDWARE REQUIRED |
| REL-002 | OFF command | NOT EXECUTED — HARDWARE REQUIRED |
| REL-003 | Rapid ON/OFF (anti-chatter test) | NOT EXECUTED — HARDWARE REQUIRED |
| REL-004 | Duplicate command (same requestId) → no double execution | NOT EXECUTED — HARDWARE REQUIRED |
| REL-005 | Command timeout (no ACK) | NOT EXECUTED — HARDWARE REQUIRED |
| REL-006 | Offline command (queued, replayed on reconnect) | NOT EXECUTED — HARDWARE REQUIRED |
| REL-007 | Reboot during pending command | NOT EXECUTED — HARDWARE REQUIRED |
| REL-008 | Power loss during physical mutation | NOT EXECUTED — HARDWARE REQUIRED |
| REL-009 | Interlock violation (mutual exclusion group) | NOT EXECUTED — HARDWARE REQUIRED |
| REL-010 | maxOnTime force-OFF after configured seconds | NOT EXECUTED — HARDWARE REQUIRED |
| REL-011 | minOnTime blocks premature OFF | NOT EXECUTED — HARDWARE REQUIRED |
| REL-012 | minOffTime blocks premature ON | NOT EXECUTED — HARDWARE REQUIRED |

---

## 7. PWA Test Matrix (brief §91)

| Test ID | Description | Result |
|---|---|---|
| PWA-001 | Command → ACK → state confirmed | NOT EXECUTED — HARDWARE REQUIRED |
| PWA-002 | Command timeout (no ACK in 5s) → UI shows TIMEOUT | NOT EXECUTED — HARDWARE REQUIRED |
| PWA-003 | Retry with backoff + jitter | NOT EXECUTED — HARDWARE REQUIRED |
| PWA-004 | Duplicate ACK handled (no double UI update) | NOT EXECUTED — HARDWARE REQUIRED |
| PWA-005 | Stale state detected + shown | NOT EXECUTED — HARDWARE REQUIRED |
| PWA-006 | State drift (desired != reported) → STATE_DRIFT alarm | NOT EXECUTED — HARDWARE REQUIRED |
| PWA-007 | Offline → STALE indicator + timestamp | NOT EXECUTED — HARDWARE REQUIRED |
| PWA-008 | Reconnect → state reconciliation | NOT EXECUTED — HARDWARE REQUIRED |
| PWA-009 | Unauthorized command → 403 | NOT EXECUTED — HARDWARE REQUIRED |
| PWA-010 | Expired session → 401 → redirect to login | NOT EXECUTED — HARDWARE REQUIRED |
| PWA-011 | Multiple tabs (state sync via localStorage events) | NOT EXECUTED — HARDWARE REQUIRED |
| PWA-012 | Simultaneous users (deterministic ordering) | NOT EXECUTED — HARDWARE REQUIRED |

---

## 8. OTA Tests (brief §85)

| Test ID | Description | Result |
|---|---|---|
| OTA-001 | Valid signed binary → installed → boot marked healthy | NOT EXECUTED — HARDWARE REQUIRED |
| OTA-002 | Invalid signature → rejected | NOT EXECUTED — HARDWARE REQUIRED |
| OTA-003 | Downgrade attempt → rejected (anti-downgrade) | NOT EXECUTED — HARDWARE REQUIRED |
| OTA-004 | Oversized binary (>2 MB) → rejected | NOT EXECUTED — HARDWARE REQUIRED |
| OTA-005 | URL not in allowlist → rejected | NOT EXECUTED — HARDWARE REQUIRED |
| OTA-006 | Power loss during OTA → previous firmware retained | NOT EXECUTED — HARDWARE REQUIRED |
| OTA-007 | New firmware boot fails 3x → rollback to previous | NOT EXECUTED — HARDWARE REQUIRED |
| OTA-008 | OTA via MQTT (signed) → installed | NOT EXECUTED — HARDWARE REQUIRED |
| OTA-009 | OTA via REST (multipart upload) → installed | NOT EXECUTED — HARDWARE REQUIRED |

---

## 9. Acceptance Criteria (brief §106)

System is production-ready ONLY when ALL of these are false:

- [ ] P0 issue exists
- [ ] P1 issue exists
- [ ] Cross-device authorization is possible
- [ ] Production credential is shared without isolation
- [ ] Duplicate command causes unintended physical mutation
- [ ] RTC failure can run schedule incorrectly
- [ ] Network failure stops safety logic
- [ ] Invalid telemetry becomes 0 without status
- [ ] OTA signature not verified
- [ ] Configuration corruption not recoverable
- [ ] Watchdog not working
- [ ] State drift not detected
- [ ] Interlock not deterministic
- [ ] maxOnTime not working
- [ ] Secrets in repository
- [ ] Build not reproducible
- [ ] Test claimed PASS without evidence

All items in v4.3.8 are either implemented and software-verified (PASS) or
marked NOT EXECUTED — HARDWARE REQUIRED where physical hardware is needed.
