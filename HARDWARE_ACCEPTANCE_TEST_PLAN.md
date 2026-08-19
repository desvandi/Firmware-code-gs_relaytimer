# HARDWARE_ACCEPTANCE_TEST_PLAN

**Per ChatGPT audit Phase O:** "Jangan mengklaim production-ready sebelum hardware test berikut selesai."

Each test uses format: TEST ID / SETUP / STIMULUS / EXPECTED RESULT / ACTUAL RESULT / EVIDENCE.

Per audit brief §107: "PASS / FAIL / NOT EXECUTED — HARDWARE REQUIRED."

---

## Gate Criteria

**System is NOT production-ready until ALL of the following 12 hardware tests PASS with actual evidence.**

No software-only test can substitute. No "should work" or "probably works". Each test requires:
1. Physical ESP32-WROOM-32 + relay module + sensors
2. Actual stimulus (power cut, network drop, etc.)
3. Observed result captured (Serial log, PWA screenshot, multimeter reading)
4. PASS/FAIL verdict

---

## Required Tests

### HW-001: Power loss during command

| Field | Value |
|---|---|
| **Test ID** | HW-001 |
| **Setup** | ESP32 running, relay OFF, MQTT connected, PWA connected. Operator ready to send ON command + cut power. |
| **Stimulus** | (1) Send ON command via PWA. (2) Cut ESP32 power within 100ms of GPIO write (before journal store). (3) Restore power. |
| **Expected** | On reboot: if journal has entry → replay ACK (no re-execute). If journal empty → command may re-execute (idempotent SET_STATE is safe). Final state: ON or OFF (deterministic based on journal). |
| **Actual** | NOT EXECUTED |
| **Evidence** | NONE — requires physical hardware |
| **Result** | **NOT EXECUTED — HARDWARE REQUIRED** |

### HW-002: Power loss during journal commit

| Field | Value |
|---|---|
| **Test ID** | HW-002 |
| **Setup** | ESP32 running, command received, journal write in progress. |
| **Stimulus** | Cut power during NVS write (after GPIO mutation, before journal commit). |
| **Expected** | On reboot: journal entry may be corrupted (CRC mismatch → rejected). No replay. Physical state = last commanded (GPIO already written). Operator must verify physical state. |
| **Actual** | NOT EXECUTED |
| **Evidence** | NONE |
| **Result** | **NOT EXECUTED — HARDWARE REQUIRED** |

### HW-003: Power loss during configuration write

| Field | Value |
|---|---|
| **Test ID** | HW-003 |
| **Setup** | ESP32 running, config change in progress (atomicWrite to config.json). |
| **Stimulus** | Cut power during atomicWrite (after temp write, before rename). |
| **Expected** | On reboot: config.json intact (atomicWrite uses temp + rename pattern). If temp corrupted, config.json unchanged. Backup (config.bak) used if config.json corrupted. |
| **Actual** | NOT EXECUTED |
| **Evidence** | NONE |
| **Result** | **NOT EXECUTED — HARDWARE REQUIRED** |

### HW-004: Reboot while relay ON

| Field | Value |
|---|---|
| **Test ID** | HW-004 |
| **Setup** | Relay CH1 ON (load energized). |
| **Stimulus** | Reboot ESP32 (via /api/reboot or power cycle). |
| **Expected** | During boot: GPIO set to OFF (boot glitch prevention). After boot: boot policy applies (BOOT_OFF → stays OFF; RESTORE_LAST → ON). No momentary relay chatter. |
| **Actual** | NOT EXECUTED |
| **Evidence** | NONE |
| **Result** | **NOT EXECUTED — HARDWARE REQUIRED** |

### HW-005: Reboot while relay OFF

| Field | Value |
|---|---|
| **Test ID** | HW-005 |
| **Setup** | All relays OFF. |
| **Stimulus** | Reboot ESP32. |
| **Expected** | All relays stay OFF during boot. No momentary energization. Boot policy = BOOT_OFF (default). |
| **Actual** | NOT EXECUTED |
| **Evidence** | NONE |
| **Result** | **NOT EXECUTED — HARDWARE REQUIRED** |

### HW-006: MQTT disconnect (24 hours)

| Field | Value |
|---|---|
| **Test ID** | HW-006 |
| **Setup** | ESP32 running, MQTT connected, scheduler + PIR active. |
| **Stimulus** | Cut MQTT broker (stop Mosquitto service) for 24 hours. |
| **Expected** | ESP32: local automation continues (scheduler, PIR, maxOnTime, interlock). MQTT reconnect attempts with backoff. Health shows mqttReconnectCount incrementing. PWA: shows DEVICE_OFFLINE + STALE timestamp. |
| **Actual** | NOT EXECUTED |
| **Evidence** | NONE |
| **Result** | **NOT EXECUTED — HARDWARE REQUIRED** |

### HW-007: WiFi disconnect (24 hours)

| Field | Value |
|---|---|
| **Test ID** | HW-007 |
| **Setup** | ESP32 running, WiFi connected, MQTT connected. |
| **Stimulus** | Power off WiFi router for 24 hours. |
| **Expected** | ESP32: local automation continues. WiFi reconnect attempts. Health shows wifiReconnectCount. PWA: DEVICE_OFFLINE. |
| **Actual** | NOT EXECUTED |
| **Evidence** | NONE |
| **Result** | **NOT EXECUTED — HARDWARE REQUIRED** |

### HW-008: Watchdog reset

| Field | Value |
|---|---|
| **Test ID** | HW-008 |
| **Setup** | ESP32 running normally. |
| **Stimulus** | Trigger infinite loop in main loop (comment out esp_task_wdt_reset() temporarily). |
| **Expected** | Watchdog fires after 10s. System reboots. lastResetReason = WDT. watchdogResets++. WATCHDOG_RESET alarm on next boot. |
| **Actual** | NOT EXECUTED |
| **Evidence** | NONE |
| **Result** | **NOT EXECUTED — HARDWARE REQUIRED** |

### HW-009: Boot loop

| Field | Value |
|---|---|
| **Test ID** | HW-009 |
| **Setup** | ESP32 with firmware that crashes on boot (e.g., NULL deref in setup()). |
| **Stimulus** | Flash crashing firmware. Let it reboot 3+ times. |
| **Expected** | bootsInLast60s ≥ 3. BOOT_LOOP alarm raised. bootLoopDetected=true. (TODO D-010: safe state action — force BOOT_OFF + inhibit scheduler.) |
| **Actual** | NOT EXECUTED |
| **Evidence** | NONE |
| **Result** | **NOT EXECUTED — HARDWARE REQUIRED** |

### HW-010: OTA interruption

| Field | Value |
|---|---|
| **Test ID** | HW-010 |
| **Setup** | ESP32 running v4.3.0. OTA in progress (downloading v4.3.1). |
| **Stimulus** | Cut power mid-download. |
| **Expected** | OTA aborts. No flash write. Previous firmware (v4.3.0) retained. System boots normally. |
| **Actual** | NOT EXECUTED |
| **Evidence** | NONE |
| **Result** | **NOT EXECUTED — HARDWARE REQUIRED** |

### HW-011: Safety trip (maxOnTime)

| Field | Value |
|---|---|
| **Test ID** | HW-011 |
| **Setup** | CH1 = heater, maxOnTimeSec=10. Relay ON. Load connected (small test load). |
| **Stimulus** | Wait 11 seconds. |
| **Expected** | At t=10s: maxOnTime triggers. Relay FORCE OFF. State = TRIPPED. Alarm ERR_RELAY_002. maxOnTimeForced=true. |
| **Actual** | NOT EXECUTED |
| **Evidence** | NONE |
| **Result** | **NOT EXECUTED — HARDWARE REQUIRED** |

### HW-012: Interlock transition

| Field | Value |
|---|---|
| **Test ID** | HW-012 |
| **Setup** | CH1=FORWARD, CH2=REVERSE in interlock group. deadTime=1000ms. CH1 ON. |
| **Stimulus** | (1) Request CH2 ON → BLOCKED (mutual exclusion). (2) Turn CH1 OFF. (3) Immediately request CH2 ON → BLOCKED (dead time). (4) Wait 1001ms. (5) Request CH2 ON → ALLOWED. |
| **Expected** | Step 1: CH2 stays OFF, alarm. Step 3: CH2 stays OFF, alarm. Step 5: CH2 ON. |
| **Actual** | NOT EXECUTED |
| **Evidence** | NONE |
| **Result** | **NOT EXECUTED — HARDWARE REQUIRED** |

---

## Summary

| Test ID | Description | Result |
|---|---|---|
| HW-001 | Power loss during command | NOT EXECUTED — HARDWARE REQUIRED |
| HW-002 | Power loss during journal commit | NOT EXECUTED — HARDWARE REQUIRED |
| HW-003 | Power loss during config write | NOT EXECUTED — HARDWARE REQUIRED |
| HW-004 | Reboot while relay ON | NOT EXECUTED — HARDWARE REQUIRED |
| HW-005 | Reboot while relay OFF | NOT EXECUTED — HARDWARE REQUIRED |
| HW-006 | MQTT disconnect 24h | NOT EXECUTED — HARDWARE REQUIRED |
| HW-007 | WiFi disconnect 24h | NOT EXECUTED — HARDWARE REQUIRED |
| HW-008 | Watchdog reset | NOT EXECUTED — HARDWARE REQUIRED |
| HW-009 | Boot loop | NOT EXECUTED — HARDWARE REQUIRED |
| HW-010 | OTA interruption | NOT EXECUTED — HARDWARE REQUIRED |
| HW-011 | Safety trip (maxOnTime) | NOT EXECUTED — HARDWARE REQUIRED |
| HW-012 | Interlock transition | NOT EXECUTED — HARDWARE REQUIRED |

**All 12 hardware acceptance tests: NOT EXECUTED — HARDWARE REQUIRED.**

Per ChatGPT audit Phase O: "Jangan mengklaim production-ready sebelum hardware test berikut selesai." These 12 tests are the FINAL GATE to production readiness. They cannot be substituted by software tests.

---

## Required Equipment

To execute these tests, the owner needs:
1. ESP32-WROOM-32 dev board (production-matching hardware)
2. 12-channel relay module (active-LOW, 5V)
3. DS3231 RTC module + CR2032 battery
4. PZEM-004T v3.0 power meter
5. 4× HC-SR501 PIR sensors
6. 8S LiFePO4 battery pack (for battery monitoring tests)
7. INA219 ×2, ADS1115 ×2, SHT31 sensors
8. WiFi router (isolated test network)
9. MQTT broker (Mosquitto on VPS or local)
10. Power-cut switch (relay or mechanical, to cut ESP32 power mid-operation)
11. Multimeter (to verify physical relay state)
12. Oscilloscope (optional, to verify no relay chatter during boot)
13. USB serial monitor (to capture boot logs + crash dumps)

---

## Test Execution Procedure

For each test:
1. Set up precondition (verify with multimeter where applicable)
2. Apply stimulus (cut power / drop network / send command)
3. Observe result (Serial log + PWA + multimeter)
4. Capture evidence (screenshot + Serial log + photo of multimeter)
5. Record PASS or FAIL
6. If FAIL: investigate root cause, fix, re-test

**No test may be marked PASS without captured evidence.**
