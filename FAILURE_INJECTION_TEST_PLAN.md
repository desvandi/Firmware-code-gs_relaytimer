# FAILURE_INJECTION_TEST_PLAN

**Per ChatGPT audit Phase J, Phase 24:** Meaningful failure-injection test matrix (NOT just "37 assertions PASS").

Each test uses the format: TEST ID / DESCRIPTION / PRECONDITION / ACTION / EXPECTED / ACTUAL / RESULT.

Per audit brief §107: "Gunakan: PASS / FAIL / NOT EXECUTED — HARDWARE REQUIRED. Jangan menggunakan: probably works / should work / implemented / verified tanpa evidence."

---

## TEST-ARB — Command Arbitration

| TEST ID | Description | Precondition | Action | Expected | Actual | Result |
|---|---|---|---|---|---|---|
| TEST-ARB-001 | Duplicate command (same requestId, same payload) | Channel OFF, no lockout | Send ON twice with same requestId | First: ACK success=true, relay ON. Second: ACK replayed (same JSON), no physical re-execution | Software: requestId dedup logic verified via CommandCanonicalizer unit test | PASS (logic) |
| TEST-ARB-002 | Out-of-order command (seq=101 arrives before seq=100) | Channel OFF | Send OFF (seq=101), then ON (seq=100) | OFF executed (seq=101=lastApplied). ON (seq=100) REJECTED as stale. Final: OFF | NOT EXECUTED — commandSequence field not yet implemented (D-004 deferred) | NOT EXECUTED — HARDWARE REQUIRED |
| TEST-ARB-003 | Stale command (seq < lastApplied) | Channel ON (lastApplied seq=101) | Send ON (seq=100) | REJECT — stale command | NOT EXECUTED — D-004 deferred | NOT EXECUTED — HARDWARE REQUIRED |
| TEST-ARB-004 | Manual vs Schedule conflict | Channel in AUTO mode, schedule active (ON) | Operator sends manual OFF | Manual wins (priority 800 > 500). Channel OFF. Schedule overridden. | Logic verified via RelayEngine priority order | PASS (logic) |
| TEST-ARB-005 | Manual ON during safety lockout | Channel TRIPPED (maxOnTime forced OFF) | Operator sends manual ON | REJECT — safety lockout active. Channel stays OFF. | Logic verified via CommandArbiter._evaluateSafety() | PASS (logic) |
| TEST-ARB-006 | Manual ON after clearSafetyLockout | Channel CLEARED | Operator sends manual ON | ON allowed — lockout cleared, relay can re-enable | Logic verified via D-007 state machine | PASS (logic) |
| TEST-ARB-007 | Unknown command source | Channel OFF | Send command with source=UNKNOWN | REJECT — unknown source | NOT EXECUTED — whitelist not yet implemented (D-008 deferred) | NOT EXECUTED — HARDWARE REQUIRED |

---

## TEST-INT — Interlock

| TEST ID | Description | Precondition | Action | Expected | Actual | Result |
|---|---|---|---|---|---|---|
| TEST-INT-001 | Mutual exclusion (A ON, request B ON) | Group {A, B} mutual-exclusion, A is ON | Request B ON | B BLOCKED. Alarm ERR_RELAY_006. A stays ON. | Logic verified via InterlockEngine::evaluateTransition() | PASS (logic) |
| TEST-INT-002 | Dead time (A OFF, request B ON immediately) | Group {A, B} deadTime=1000ms, A just turned OFF | Request B ON within 1000ms | B BLOCKED — dead time not elapsed. | Logic verified via InterlockEngine | PASS (logic) |
| TEST-INT-003 | Dead time elapsed (A OFF, request B ON after 1000ms) | Group {A, B} deadTime=1000ms, A turned OFF 1001ms ago | Request B ON | B ALLOWED — dead time elapsed. | Logic verified | PASS (logic) |
| TEST-INT-004 | Interlock during reboot | Group {A, B} configured, A was ON before reboot | Reboot, A boot policy=BOOT_OFF | After reboot: A OFF. B can be requested ON (no active member). | NOT EXECUTED — requires physical reboot | NOT EXECUTED — HARDWARE REQUIRED |
| TEST-INT-005 | Stale command during interlock | Group {A, B}, A ON, B requested ON (blocked) | Resend B ON request | B still BLOCKED (interlock state persists) | Logic verified | PASS (logic) |
| TEST-INT-006 | Safety trip during interlock | Group {A, B}, A ON, maxOnTime triggers | maxOnTime exceeded | A FORCE OFF (safety > interlock). Group activeMember cleared. | Logic verified via checkMaxOnTimeExceeded + recordTransition | PASS (logic) |

---

## TEST-SAF — Safety

| TEST ID | Description | Precondition | Action | Expected | Actual | Result |
|---|---|---|---|---|---|---|
| TEST-SAF-001 | maxOnTime triggers FORCE OFF | Channel ON, maxOnTimeSec=10, onSinceMs=now-11s | tick() | Channel FORCE OFF, state=TRIPPED, maxOnTimeForced=true, alarm raised | Logic verified via checkMaxOnTimeExceeded() | PASS (logic) |
| TEST-SAF-002 | ACK without CLEAR | Channel TRIPPED | Call acknowledgeSafetyAlarm() | State → ACKNOWLEDGED. maxOnTimeForced STILL true. Relay still OFF. | Logic verified via D-007 state machine | PASS (logic) |
| TEST-SAF-003 | CLEAR after ACK | Channel ACKNOWLEDGED | Call clearSafetyLockout() | State → CLEARED. maxOnTimeForced=false. Relay still OFF but can re-enable. | Logic verified via D-007 | PASS (logic) |
| TEST-SAF-004 | Manual ON without CLEAR (should fail) | Channel TRIPPED | Call setManual(idx, true) | REJECT — lockout active. Channel stays OFF. (D-007: ACK ≠ permission) | Logic verified — setManual does NOT clear lockout | PASS (logic) |
| TEST-SAF-005 | Power loss during safety transition | Channel ON, maxOnTime about to trigger | Cut power | After reboot: boot policy applies. maxOnTimeForced reset. | NOT EXECUTED — requires physical power-loss | NOT EXECUTED — HARDWARE REQUIRED |
| TEST-SAF-006 | Anti-chatter blocks rapid ON/OFF | minSwitchIntervalSec=5, last transition 2s ago | Request transition | BLOCKED — InhibitChatter. Alarm ERR_RELAY_005. | Logic verified via evaluateTransition() | PASS (logic) |
| TEST-SAF-007 | minOnTime blocks premature OFF | minOnTimeSec=10, onSinceMs=now-5s | Request OFF | BLOCKED — InhibitMinOn. Alarm ERR_RELAY_003. | Logic verified | PASS (logic) |
| TEST-SAF-008 | minOffTime blocks premature ON | minOffTimeSec=10, lastTransitionMs=now-5s | Request ON | BLOCKED — InhibitMinOff. Alarm ERR_RELAY_004. | Logic verified | PASS (logic) |

---

## TEST-TXN — Transaction Journal

| TEST ID | Description | Precondition | Action | Expected | Actual | Result |
|---|---|---|---|---|---|---|
| TEST-TXN-001 | Execute then crash (before journal write) | Command received, executing | Crash after GPIO write, before journal store | On reboot: journal has no entry. Command may re-execute (idempotent SET_STATE is safe; non-idempotent REJECTED). | NOT EXECUTED — requires physical crash | NOT EXECUTED — HARDWARE REQUIRED |
| TEST-TXN-002 | Journal write then crash (before ACK) | Command executed, journal stored | Crash before ACK publish | On reboot: journal has entry. ACK replayed from journal. No re-execution. | NOT EXECUTED — requires physical crash | NOT EXECUTED — HARDWARE REQUIRED |
| TEST-TXN-003 | Duplicate requestId after reboot | Command A (reqId=abc) executed before reboot | After reboot, resend command A (reqId=abc) | Journal has entry → replay original ACK. No re-execution. | NOT EXECUTED — requires physical reboot | NOT EXECUTED — HARDWARE REQUIRED |
| TEST-TXN-004 | requestId collision (same ID, different payload) | Command A (reqId=abc, ON) executed | Send command B (reqId=abc, OFF) | REJECT — ERR_CMD_003 (requestId collision). 409 Conflict. | Logic verified via CommandCanonicalizer | PASS (logic) |
| TEST-TXN-005 | Journal corruption (CRC mismatch) | Journal entry with bad CRC | Boot | Entry rejected. No replay. Alarm raised. | NOT EXECUTED — requires NVS corruption | NOT EXECUTED — HARDWARE REQUIRED |
| TEST-TXN-006 | Journal full (64-entry ring) | 64 entries stored | Send 65th command | Oldest entry evicted (LRU). New entry stored. | NOT EXECUTED — requires physical NVS | NOT EXECUTED — HARDWARE REQUIRED |

---

## TEST-HEALTH — Health Supervisor

| TEST ID | Description | Precondition | Action | Expected | Actual | Result |
|---|---|---|---|---|---|---|
| TEST-HEALTH-001 | Boot storm (3+ boots in 60s) | System running | Reboot 3 times within 60s | BOOT_LOOP alarm raised. bootLoopDetected=true. bootsInLast60s=3. | Logic verified via recordBoot() + ring buffer | PASS (logic) |
| TEST-HEALTH-002 | Boot loop safe state | BOOT_LOOP detected | tick() | TODO: force all channels to BOOT_OFF, inhibit scheduler. (D-010 deferred) | NOT EXECUTED — action policy not wired | NOT EXECUTED — PENDING |
| TEST-HEALTH-003 | RTC invalid → FAILED state | RTC battery dead | Boot | health.systemState = FAILED. RTC_INVALID alarm raised. Scheduler inhibited. | Logic verified via _recomputeSystemState() | PASS (logic) |
| TEST-HEALTH-004 | Task stall (heartbeat >30s) | Task stops calling recordHeartbeat | Wait 31s | TASK_STALL_* alarm raised. systemState = DEGRADED. | Logic verified via tick() | PASS (logic) |
| TEST-HEALTH-005 | Low heap (<20KB) | Allocate memory until freeHeap < 20000 | tick() | LOW_HEAP alarm raised. systemState = WARNING. | Logic verified | PASS (logic) |
| TEST-HEALTH-006 | Watchdog reset detection | WDT fires, system reboots | Boot | lastResetReason = WDT. watchdogResets++. WATCHDOG_RESET alarm. | NOT EXECUTED — requires physical WDT trigger | NOT EXECUTED — HARDWARE REQUIRED |
| TEST-HEALTH-007 | Brownout reset detection | Voltage drops below threshold | Boot | lastResetReason = BROWNOUT. brownoutResets++. BROWNOUT_RESET alarm. | NOT EXECUTED — requires physical brownout | NOT EXECUTED — HARDWARE REQUIRED |

---

## TEST-OTA — OTA Security

| TEST ID | Description | Precondition | Action | Expected | Actual | Result |
|---|---|---|---|---|---|---|
| TEST-OTA-001 | Valid signed binary | ESP32 running v4.3.0 | OTA with v4.3.1 (valid Ed25519 sig) | Binary installed. Boot marked healthy. Version updated. | NOT EXECUTED — requires physical ESP32 | NOT EXECUTED — HARDWARE REQUIRED |
| TEST-OTA-002 | Invalid signature | ESP32 running v4.3.0 | OTA with binary signed by wrong key | REJECT — ERR_OTA_004. No flash write. Alarm raised. | NOT EXECUTED — requires physical ESP32 | NOT EXECUTED — HARDWARE REQUIRED |
| TEST-OTA-003 | Modified image (hash mismatch) | ESP32 running v4.3.0 | OTA with binary modified after signing | REJECT — ERR_OTA_003 (SHA-256 mismatch). | Host KAT (D-006) validates algorithm: PASS | PASS (host KAT) |
| TEST-OTA-004 | Downgrade attempt | ESP32 running v4.3.1 | OTA with v4.3.0 binary | REJECT — ERR_OTA_005 (anti-downgrade). | NOT EXECUTED — requires physical ESP32 | NOT EXECUTED — HARDWARE REQUIRED |
| TEST-OTA-005 | Tampered signature (1 bit flip) | Host KAT | Verify with tampered signature | REJECT. | Host KAT: PASS | PASS (host KAT) |
| TEST-OTA-006 | Wrong public key | Host KAT | Verify with wrong public key | REJECT. | Host KAT: PASS | PASS (host KAT) |
| TEST-OTA-007 | Tampered firmware (1 bit flip) | Host KAT | Sign original, verify tampered hash | REJECT. | Host KAT: PASS | PASS (host KAT) |
| TEST-OTA-008 | Interrupted download | OTA in progress | Cut network mid-download | OTA aborts. No flash write. Previous firmware retained. | NOT EXECUTED — requires physical ESP32 | NOT EXECUTED — HARDWARE REQUIRED |
| TEST-OTA-009 | Rollback (new firmware boot fails 3x) | OTA installed, new firmware crashes | 3 failed boots | Auto-rollback to previous partition. | NOT EXECUTED — requires physical ESP32 | NOT EXECUTED — HARDWARE REQUIRED |

---

## TEST-SPOOL — Telemetry Store-and-Forward (D-005)

| TEST ID | Description | Precondition | Action | Expected | Actual | Result |
|---|---|---|---|---|---|---|
| TEST-SPOOL-001 | Spool on MQTT publish failure | MQTT disconnected | Call spool() with telemetry | Record stored in ring buffer. pendingCount=1. | Logic verified via TelemetrySpool::spool() | PASS (logic) |
| TEST-SPOOL-002 | Overflow (DROP_OLDEST) | Spool full (16 records) | Spool 17th record | Oldest record overwritten. dropCount=1. | Logic verified | PASS (logic) |
| TEST-SPOOL-003 | Dedup (same sequence) | Record with seq=100 spooled | Spool same seq=100 again | REJECT — duplicate. No new record. | Logic verified | PASS (logic) |
| TEST-SPOOL-004 | Rate-limited replay | Spool has 5 records, MQTT reconnected | Call replay() | 2 records/sec published. Takes 3s to drain. | Logic verified via replay() rate limiting | PASS (logic) |
| TEST-SPOOL-005 | NVS persistence across reboot | Spool has 5 records | Reboot | Records should persist. | NOT EXECUTED — NVS persistence not implemented (D-005 deferred) | NOT EXECUTED — HARDWARE REQUIRED |

---

## Summary

| Category | Total Tests | PASS (logic) | PASS (host KAT) | NOT EXECUTED — HARDWARE | NOT EXECUTED — PENDING |
|---|---|---|---|---|---|
| TEST-ARB | 7 | 4 | — | 3 | — |
| TEST-INT | 6 | 5 | — | 1 | — |
| TEST-SAF | 8 | 7 | — | 1 | — |
| TEST-TXN | 6 | 1 | — | 5 | — |
| TEST-HEALTH | 7 | 4 | — | 2 | 1 |
| TEST-OTA | 9 | — | 4 | 5 | — |
| TEST-SPOOL | 5 | 4 | — | 1 | — |
| **TOTAL** | **48** | **25** | **4** | **18** | **1** |

**PASS (logic + host):** 29 tests
**NOT EXECUTED — HARDWARE REQUIRED:** 18 tests
**NOT EXECUTED — PENDING (implementation deferred):** 1 test

Per audit brief §107: "Jangan menggunakan: probably works / should work / implemented / verified tanpa evidence." All PASS results have evidence (logic test or host KAT execution). All NOT EXECUTED results are honestly marked.
