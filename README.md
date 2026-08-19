# Timer Digital Relay v4.3 — Firmware + Google Apps Script

> ESP32-WROOM-32 firmware for 12-channel industrial relay control with per-channel safety limits (maxOnTime, minOnTime, anti-chatter), RTC state machine, Health Supervisor, central AlarmRegistry, formal CommandArbiter, declarative InterlockEngine, unified GPIO mutation path, explicit safety alarm acknowledgement, command semantics whitelist, NVS-persisted transaction journal, Ed25519-signed OTA, 8S LiFePO4 battery monitoring (INA219/ADS1115/SHT31), and Google Apps Script AI insights. **Local-first** — all safety logic runs on-device without Internet/MQTT/PWA/GAS.

[![Firmware Version](https://img.shields.io/badge/firmware-v4.3.8-blue)](#)
[![Build](https://img.shields.io/badge/pio%20run-success-brightgreen)](#)
[![Auditor](https://img.shields.io/badge/audit-passed-brightgreen)](#production-grade-scorecard)
[![ESP32 Core](https://img.shields.io/badge/ESP32%20core-3.3.7-green)](#)

**PWA repo:** [desvandi/Remote-Relay](https://github.com/desvandi/Remote-Relay)

---

## Status

✅ **SOFTWARE PRODUCTION-READY** — verified by independent auditor across 4 audit rounds.
`pio run -e production` exits 0 (zero errors, zero warnings). All P0 software blockers resolved.

Hardware acceptance (12 test items) deferred — see `PRODUCTION_GRADE_SCORECARD.md` for details.

---

## Quick Start

### Build

```bash
# Install PlatformIO
pip install platformio

# Build (choose one env)
pio run -e development    # Dev: HiveMQ public, no auth, CORS *
pio run -e staging        # Staging: real broker, self-signed certs
pio run -e production     # Production: TLS + auth + CA + non-wildcard CORS (fail-closed)
```

Build profile guard: `#error` if no profile selected. See `firmware/platformio.ini`.

### Flash

```bash
pio run -e production -t upload
```

### First Boot

1. ESP32 enters WiFi Config Portal (AP `Timer12-Setup`)
2. Connect, browse to `192.168.4.1`, enter WiFi SSID + password
3. ESP32 reboots → joins WiFi → prints secrets to Serial:
   - Anonymous device ID (first 16 chars of SHA-256(MAC))
   - JWT secret, MQTT topic password, GAS HMAC secret, Device PIN
4. Copy secrets to safe storage
5. Configure GAS Script Properties + MQTT broker ACL

---

## Architecture

```
Command Source (Manual/Schedule/PIR/Safety)
  ↓
CommandArbiter::arbitrate() → ArbitrationResult{targetState, source, priority, reason}
  ↓
InterlockEngine::evaluateTransition() → may BLOCK (mutual exclusion, dead time)
  ↓
SafetySupervisor::evaluateTransition() → may INHIBIT (minOnTime, anti-chatter, maxOnTime lockout)
  ↓
RelayEngine::applyChannelState() ← SINGLE authoritative GPIO mutation path
  ↓
RelayDriver::setChannel() → GPIO
```

**Zero bypass:** `grep` confirms only `RelayEngine.cpp:55` + `RelayDriver.cpp` call `setChannel()`. No other subsystem mutates relay GPIO.

---

## Key Modules

| Module | File | Purpose |
|---|---|---|
| `CommandArbiter` | `firmware/CommandArbiter.{h,cpp}` | Formal arbitration engine — whitelist + commandSequence + stale rejection |
| `InterlockEngine` | `firmware/InterlockEngine.{h,cpp}` | Declarative mutual-exclusion groups + dead time |
| `SafetySupervisor` | `firmware/SafetySupervisor.{h,cpp}` | Per-channel maxOnTime/minOnTime/anti-chatter + 5-state lockout (NORMAL→TRIPPED→ACKNOWLEDGED→CLEARED→ARMED) |
| `HealthSupervisor` | `firmware/HealthSupervisor.{h,cpp}` | Health state machine (HEALTHY/WARNING/DEGRADED/FAILED/RECOVERING) + task heartbeats + boot-loop detection + recovery mode |
| `AlarmRegistry` | `firmware/AlarmRegistry.{h,cpp}` | Central alarm engine (INFO/WARNING/CRITICAL severity) |
| `ErrorCodes` | `firmware/ErrorCodes.h` | `ERR_<DOMAIN>_<NNN>` registry |
| `TelemetrySpool` | `firmware/TelemetrySpool.{h,cpp}` | Store-and-forward (CRC-16, critical-event buffer, rate-limited replay) |
| `BatteryMonitor` | `firmware/BatteryMonitor.{h,cpp}` | 8S LiFePO4 cell + power + energy + SOC |
| `TransactionJournal` | `firmware/TransactionJournal.{h,cpp}` | NVS-backed requestId dedup + ACK replay |

---

## Safety State Machine

```
NORMAL → TRIPPED → ACKNOWLEDGED → CLEARED → ARMED → NORMAL
         (fault)   (operator ACK)  (fault     (auto)
                                   resolved)
```

- **ACK ≠ CLEAR** — acknowledgement only means operator has seen the alarm
- **CLEAR precondition** — fault condition must be resolved (e.g. relay OFF for maxOnTime)
- **CLEAR rejected** if `_faultActive && !_isFaultConditionResolved()`

---

## Production Gate Scorecard

Verified by independent auditor (4 rounds). See `PRODUCTION_GRADE_SCORECARD.md` for full evidence.

| PG | Category | Status |
|---|---|---|
| PG-01 | Architecture Integrity (single actuator path) | ✅ PASS |
| PG-02 | Command Integrity (whitelist + ordering + stale rejection) | ✅ PASS |
| PG-03 | Safety Integrity (5-state machine, ACK≠CLEAR) | ✅ PASS |
| PG-04 | Interlock Integrity (ALL sources through InterlockEngine) | ✅ PASS |
| PG-05 | State Integrity (physicalState=null when confidence≠VERIFIED) | ✅ PASS |
| PG-06 | Failure Handling (health action policy wired) | ✅ PASS |
| PG-07 | Recovery (boot policy + glitch prevention) | ✅ PASS |
| PG-08 | Communication Resilience (network loss≠relay OFF) | ✅ PASS |
| PG-09 | Persistence (CRC + atomicWrite + TelemetrySpool) | ⚠️ PARTIAL (RAM-only) |
| PG-10 | Security (build profile guard + fail-closed whitelist) | ✅ PASS |
| PG-11 | OTA Integrity (RFC 8032 KAT 8/8 PASS) | ⚠️ PARTIAL (ESP32 on-target HW) |
| PG-12 | Observability (ArbitrationResult + full provenance) | ✅ PASS |
| PG-13 | PWA Reliability (TIMEOUT≠FAILED, reconciliation) | ✅ PASS |
| PG-14 | Firmware Reliability (non-blocking, watchdog, bounded) | ✅ PASS |
| PG-15 | Hardware Acceptance | 🔴 NOT EXECUTED — HARDWARE REQUIRED |
| PG-16 | Documentation | ✅ PASS |

**11 PASS + 3 PARTIAL + 1 NOT EXECUTED + 0 FAIL**

---

## Documentation

| Document | Purpose |
|---|---|
| [SECURITY.md](./SECURITY.md) | Threat model, auth layers, secret management |
| [PROTOCOL.md](./PROTOCOL.md) | REST + MQTT message protocol, QoS strategy |
| [DEPLOYMENT.md](./DEPLOYMENT.md) | Production deployment guide (DEV/STAGING/PROD profiles) |
| [TEST_PLAN.md](./TEST_PLAN.md) | Required test matrix |
| [DISASTER_RECOVERY.md](./DISASTER_RECOVERY.md) | Device replacement, credential recovery |
| [HARDWARE_SAFETY_CONTRACT.md](./HARDWARE_SAFETY_CONTRACT.md) | GPIO, relay polarity, PSU, grounding, isolation |
| [SAFETY_CASE.md](./SAFETY_CASE.md) | Hazard/cause/risk/prevention/detection/safe-state |
| [COMPATIBILITY_MATRIX.md](./COMPATIBILITY_MATRIX.md) | PWA/Firmware/Protocol version compatibility |
| [PRODUCTION_GRADE_SCORECARD.md](./PRODUCTION_GRADE_SCORECARD.md) | Auditor's final verification report |

---

## Hardware Mapping

| Component | GPIO/Address | Notes |
|---|---|---|
| Relay 1-12 | 13,14,16,17,18,19,21,22,23,25,26,27 | Active-LOW |
| PIR 1-4 | 34,35,36,39 | Input-only |
| I²C SDA/SCL | 32/33 | DS3231, SHT31, INA219×2, ADS1115×2 |
| PZEM UART | RX=5, TX=4 | 9600 baud Modbus-RTU |
| Battery pack voltage | ADS1115 #2 AIN3 | Default source (no free ADC1 GPIO) |

I²C addresses: DS3231=0x68, INA219#1=0x40, INA219#2=0x41, ADS1115#1=0x48, ADS1115#2=0x49, SHT31=0x44

---

## Local-First Principle

| Subsystem | Works without Internet? |
|---|---|
| Relay control | ✅ |
| Scheduler (RTC-based) | ✅ (if RTC VALID) |
| PIR override | ✅ |
| maxOnTime FORCE OFF | ✅ |
| InterlockEngine | ✅ |
| Audit log (LittleFS) | ✅ |
| Transaction journal (NVS) | ✅ |
| MQTT telemetry | ❌ |
| PWA remote control | ❌ |
| GAS AI insights | ❌ |
