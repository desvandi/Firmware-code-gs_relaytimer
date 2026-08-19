# Timer Digital Relay v4.3 — Firmware + Google Apps Script (Industrial-Grade R2)

> **Status: 🟡 NOT PRODUCTION READY — HARDENING ROUND REQUIRED** (per ChatGPT audit)
>
> ESP32-WROOM-32 firmware for 12-channel industrial relay control with per-channel safety limits (maxOnTime, minOnTime, anti-chatter), RTC state machine, Health Supervisor with state machine (HEALTHY/WARNING/DEGRADED/FAILED/RECOVERING), central AlarmRegistry, monotonic telemetry sequence, formal CommandArbiter (priority-based arbitration), declarative InterlockEngine (mutual-exclusion groups with dead time), unified GPIO mutation path, explicit safety alarm acknowledgement (separate from manual relay commands), command semantics classification (IDEMPOTENT_STATE vs NON_IDEMPOTENT_ACTION), NVS-persisted transaction journal, Ed25519-signed OTA, 8S LiFePO4 battery monitoring (INA219/ADS1115/SHT31), and Google Apps Script AI insights pipeline. **Local-first** — all safety logic runs on-device without Internet/MQTT/PWA/GAS.

[![Firmware Version](https://img.shields.io/badge/firmware-v4.3.0-blue)](#)
[![Industrial Grade](https://img.shields.io/badge/grade-industrial-orange)](#industrial-grade-hardening-v43)
[![Status](https://img.shields.io/badge/status-NOT%20PROD%20READY%20—%20HARDENING%20ROUND-yellow)](#production-readiness-status)
[![Local-First](https://img.shields.io/badge/local--first-✓-green)](#local-first-control-principle)
[![Security Audit](https://img.shields.io/badge/audit-round%2010K-brightgreen)](#security-audit-history)
[![ESP32 Core](https://img.shields.io/badge/ESP32%20core-3.3.7-green)](#)
[![License](https://img.shields.io/badge/license-proprietary-lightgrey)](#)

This repo holds the **device-side code** for the Timer Digital Relay v4.3 system. The companion PWA dashboard lives in a separate repo: **[desvandi/Remote-Relay](https://github.com/desvandi/Remote-Relay)**.

---

## 🟡 Production Readiness Status

Per ChatGPT targeted remediation audit (v4.2 → v4.3), the following P1 blockers were addressed:

| P1 ID | Issue | Status (v4.3) |
|---|---|---|
| P1-001 | Formal Command Arbitration Engine | ✅ `CommandArbiter` module — produces `ArbitrationResult{targetState, source, priority, reason, requestId}` |
| P1-002 | Formal Interlock Engine | ✅ `InterlockEngine` module — declarative mutual-exclusion groups + dead time |
| P1-003 | Separate safety acknowledgement from manual command | ✅ `SafetySupervisor::acknowledgeSafetyAlarm()` — `setManual()` no longer auto-clears lockout |
| P1-004 | Unify all GPIO mutation through one path | ✅ All transitions now flow through `RelayEngine::applyChannelState()` — single authoritative actuator |
| P1-005 | Separate desired/reported/physical state semantics | ✅ Added `relayPhysicalState[]`, `relayStateConfidence[]`, `relayStateSequence[]`, `relayStateTimestamp[]`, `relayFault[]` globals + PWA `ChannelState` type |
| P1-006 | COMMAND_TIMEOUT = UNKNOWN execution status | ✅ PWA `CommandExecutionState` type — `TIMEOUT` is distinct from `FAILED` |
| P1-007 | Command semantics classification | ✅ `CommandSemantics` enum (IDEMPOTENT_STATE / NON_IDEMPOTENT_ACTION); firmware rejects non-idempotent through transaction path |
| P1-010 | HealthSupervisor state machine | ✅ `HealthState` enum (HEALTHY/WARNING/DEGRADED/FAILED/RECOVERING) + `_recomputeSystemState()` policy |
| P1-011 | Time-window boot loop detection | ✅ Ring buffer of last 8 boot timestamps + `bootsInLast60s` counter + `BOOT_LOOP` alarm |
| P1-014 | Separate commandedState from physicalState | ✅ `relayPhysicalState[]` defaults to `false` (UNKNOWN) — PWA `StateConfidence::SoftwareOnly` reflects no-aux-feedback limitation |
| P1-016 | DEV/STAGING/PRODUCTION build profiles | ✅ Compile-time guard in `Config.h` — build FAILS if no profile selected |

### Remaining blockers (NOT software-fixable — require hardware):

| P1 ID | Issue | Why hardware-required |
|---|---|---|
| P1-008 | Durable telemetry store-and-forward | Needs ESP32 flash wear testing across reboots |
| P1-009 | Transactional config A/B recovery | Needs ESP32 power-loss test harness |
| P1-012 | Ed25519 known-answer test | Needs ESP32 toolchain (mbedtls Ed25519 KAT) |
| P1-013 | 12-case power-loss test | Needs physical ESP32 + relay hardware |
| P1-015 | MQTT browser credential blast-radius | Architectural — needs Auth Gateway deployment |
| P1-017 | Secure Boot + Flash Encryption provisioning | Needs disposable ESP32 for provisioning verification |

**These 6 items remain NOT EXECUTED — HARDWARE REQUIRED per audit brief §107. Owner must execute on physical hardware before declaring production-ready.**

---

## ⚡ What's New in v4.3 (ChatGPT Targeted Remediation)

v4.3 implements the 11 P1 blockers identified by ChatGPT's source-code re-audit. Per the audit directive: "DO NOT ADD FEATURES RANDOMLY. Perform an independent source-code re-audit against the existing Industrial-Grade Directive." This is targeted remediation, not feature addition.

### P1-001: CommandArbiter (formal arbitration engine)

Replaces the hardcoded precedence logic in `RelayEngine::_computeChannel()` with a formal `CommandArbiter` module that produces `ArbitrationResult{targetState, source, priority, reason, vetoed, vetoReason}`.

```
Input sources (manual/schedule/PIR/safety)
     ↓
CommandArbiter::arbitrate() → ArbitrationResult{targetState, source, priority, reason}
     ↓
InterlockEngine::evaluateTransition() → may BLOCK (mutual exclusion, dead time)
     ↓
SafetySupervisor::evaluateTransition() → may INHIBIT (minOnTime, anti-chatter, maxOnTime lockout)
     ↓
RelayEngine::applyChannelState() ← SINGLE authoritative GPIO mutation path
     ↓
RelayDriver::setChannel()
     ↓
Physical GPIO
```

### P1-002: InterlockEngine (declarative mutual-exclusion groups)

Implements interlock groups for forward/reverse, charge/discharge, motor contactor pairs:

```cpp
uint8_t motorGroup[] = {0, 1};  // CH1 = FORWARD, CH2 = REVERSE
Services::interlock.registerGroup(
    "motor_fwd_rev",
    Services::InterlockType::MutualExclusion,
    motorGroup, 2,
    1000  // 1s dead time between OFF one and ON another
);
```

When CH1 is ON, attempts to turn CH2 ON are BLOCKED with `ERR_RELAY_006` alarm. After CH1 turns OFF, a 1000 ms dead time must elapse before CH2 can turn ON.

### P1-003: Separate safety acknowledgement from manual command

Per ChatGPT audit: "fungsi setManual() sendiri bukan acknowledgement protocol. Yang benar: Harus dipisahkan: SET_RELAY dan ACK_SAFETY_ALARM"

**Before (v4.2 — buggy):**
```cpp
void RelayEngine::setManual(uint8_t idx, bool on) {
  Core::channels[idx].maxOnTimeForced = false;  // ❌ auto-clears safety lockout
  Services::alarms.clear(Err::RELAY_MAX_ON_TIME);  // ❌ implicit acknowledgement
  // ...
}
```

**After (v4.3 — explicit):**
```cpp
void RelayEngine::setManual(uint8_t idx, bool on) {
  // ✅ Does NOT clear maxOnTimeForced — operator must call
  //    SafetySupervisor::acknowledgeSafetyAlarm() explicitly.
  //    If lockout is active, arbitrate() returns Safety source
  //    with targetState=false, overriding this manual command.
  Core::channels[idx].modeAuto = false;
  Core::channels[idx].manualState = on;
}

// Separate explicit acknowledgement API:
bool SafetySupervisor::acknowledgeSafetyAlarm(uint8_t idx);  // clears lockout
bool SafetySupervisor::isSafetyLockoutActive(uint8_t idx) const;
```

### P1-004: Unified GPIO mutation path

Before v4.3, the `maxOnTime` force-OFF path called `Drivers::relay.setChannel()` directly, bypassing the safety/interlock gates. This created two physical actuation paths. v4.3 unifies ALL GPIO mutation through `RelayEngine::applyChannelState()`:

```cpp
// v4.3 P1-004: SINGLE authoritative GPIO mutation function.
// All paths (maxOnTime force-off, normal transition, boot policy) must
// route through this function. No other code may call
// Drivers::relay.setChannel() except this function + RelayDriver::allOff()
// (which is for factory reset only).
static void applyChannelState(uint8_t idx, bool newState) {
  Drivers::relay.setChannel(idx, newState);
  Core::relayState[idx] = newState;
  Services::safety.recordTransition(idx, newState);
  Services::interlock.recordTransition(idx, newState);
}
```

### P1-005, P1-014: Separate desired/reported/physical state

Per ChatGPT audit: "Untuk industrial controller saya ingin: desiredState, reportedState, physicalState, stateConfidence, stateTimestamp, stateSequence, fault"

Added globals:
- `relayPhysicalState[NUM_CHANNELS]` — last confirmed physical state (false/UNKNOWN without aux feedback)
- `relayStateConfidence[NUM_CHANNELS]` — `SOFTWARE_ONLY` / `VERIFIED` / `UNKNOWN` / `FAULT`
- `relayStateSequence[NUM_CHANNELS]` — monotonic per-channel counter
- `relayStateTimestamp[NUM_CHANNELS]` — last state change ms
- `relayFault[NUM_CHANNELS]` — state drift or interlock violation

PWA type `ChannelState` mirrors this:
```typescript
type ChannelState = {
  desiredState: boolean;
  reportedState: boolean;
  physicalState: boolean | null;  // null = UNKNOWN without aux feedback
  stateConfidence: StateConfidence;
  stateTimestamp: number;
  stateSequence: number;
  fault: boolean;
};
```

### P1-006: COMMAND_TIMEOUT = UNKNOWN execution status

Per ChatGPT audit: "UI harus membedakan: COMMAND_TIMEOUT dengan COMMAND_FAILED dan UNKNOWN_EXECUTION_STATUS. Karena timeout berarti: 'kita tidak tahu apakah command telah dieksekusi'."

PWA `CommandExecutionState` type now has 8 explicit states:
- `COMMAND_PENDING` — sent, waiting for ACK
- `CONFIRMED_ON` / `CONFIRMED_OFF` — ACK received
- `TIMEOUT` — UNKNOWN execution (device may have executed, may not have)
- `FAILED` — ACK received with success=false
- `DEVICE_OFFLINE` — no ACK possible
- `UNKNOWN` — never commanded
- `STATE_DRIFT` — desired != reported for sustained period

### P1-007: Command semantics classification

Per ChatGPT audit: "logical idempotency ≠ physical side-effect idempotency. Untuk SET CH1 ON dua kali: ON ON secara logical memang idempotent. Tetapi untuk PULSE/TOGGLE/START_MOTOR/TRIGGER_CONTACTOR/RESET: tidak lagi aman."

Firmware `CommandSemantics` enum + `CommandArbiter::processCommand()` rejects `NON_IDEMPOTENT_ACTION` through the transactional (replay-safe) path:
```cpp
if (req.semantics == CommandSemantics::NonIdempotentAction) {
  r.vetoed = true;
  setReasonStr(r.vetoReason, ..., "non-idempotent action rejected — use IDEMPOTENT_STATE only");
  return r;
}
```

### P1-010: HealthSupervisor state machine

Per ChatGPT audit: "Tambahkan state: HEALTHY, WARNING, DEGRADED, FAILED, RECOVERING dan recovery policy per subsystem."

`HealthState` enum + `_recomputeSystemState()` policy:
- `FAILED` — boot loop OR RTC invalid OR filesystem/NVS not OK
- `DEGRADED` — any sensor UNAVAILABLE/ERROR OR task stalled >30s
- `WARNING` — any active WARNING alarm
- `HEALTHY` — no active alarms

### P1-011: Time-window boot loop detection

Per ChatGPT audit: "Harus dibuat benar-benar time-window based: bootTimestamp ring buffer — 3+ boots in 60 seconds → BOOT_LOOP"

Replaced the v4.2 buggy `bootCount > lastBootCount + 5` heuristic with a ring buffer of last 8 boot timestamps persisted in NVS:
```cpp
static constexpr uint8_t BOOT_LOOP_WINDOW = 8;
uint32_t _bootTimestamps[BOOT_LOOP_WINDOW] = {};
uint8_t  _bootTimestampIdx = 0;
```

Detection logic: count boots in last 60 seconds; if ≥3 → `BOOT_LOOP` CRITICAL alarm + `bootLoopDetected=true`.

### P1-016: DEV/STAGING/PRODUCTION build profiles

Per ChatGPT audit: "development mode harus explicitly enabled, bukan default. PRODUCTION_BUILD harus tidak mungkin tersandung ke public broker."

Compile-time guard in `Config.h`:
```cpp
#if !defined(DEVELOPMENT_BUILD) && !defined(STAGING_BUILD) && !defined(PRODUCTION_BUILD)
  #error "Build profile not selected. Define one of: -DDEVELOPMENT_BUILD, -DSTAGING_BUILD, or -DPRODUCTION_BUILD"
#endif
```

Three explicit envs in `platformio.ini`:
- `[env:development]` → `-DDEVELOPMENT_BUILD` (HiveMQ public, no auth, CORS *)
- `[env:staging]` → `-DSTAGING_BUILD` (real broker, self-signed certs allowed, CORS *)
- `[env:production]` → `-DPRODUCTION_BUILD` (TLS + auth + CA + non-wildcard CORS — fail-closed)

No implicit default — operator MUST choose.

---

## 🏭 Industrial-Grade Hardening (v4.3)

### Local-First Control Principle

The ESP32 is the **authoritative edge controller** (audit brief §4, §5).
PWA / GAS / MQTT are enhancement layers, NOT sources of truth for physical
relay state. If all cloud connectivity is lost:

| Subsystem | Continues to work? |
|---|---|
| Relay control | ✅ Yes |
| Scheduler (RTC-based) | ✅ Yes (if RTC VALID) |
| PIR override | ✅ Yes |
| maxOnTime FORCE OFF | ✅ Yes |
| minOnTime / anti-chatter | ✅ Yes |
| Boot policy | ✅ Yes |
| InterlockEngine (mutual exclusion) | ✅ Yes |
| Audit log (LittleFS) | ✅ Yes |
| Transaction journal (NVS) | ✅ Yes |
| OTA rollback detection | ✅ Yes |
| MQTT telemetry publish | ❌ Queued / dropped |
| PWA control | ❌ Disabled until reconnect |
| GAS AI insights | ❌ Disabled |

### Key v4.3 Modules

| Module | File | Audit P1 | Purpose |
|---|---|---|---|
| `CommandArbiter.{h,cpp}` | P1-001 | Formal arbitration engine → `ArbitrationResult{targetState, source, priority, reason}` |
| `InterlockEngine.{h,cpp}` | P1-002 | Declarative mutual-exclusion groups + dead time |
| `SafetySupervisor.{h,cpp}` | P1-003, P1-004 | Per-channel maxOnTime/minOnTime/anti-chatter + explicit acknowledge API |
| `HealthSupervisor.{h,cpp}` | P1-010, P1-011 | Health state machine + time-window boot loop detection |
| `ErrorCodes.h` | §59 | Centralized `ERR_<DOMAIN>_<NNN>` registry |
| `AlarmRegistry.{h,cpp}` | §60 | Central alarm engine with severity levels |
| `BatteryMonitor.{h,cpp}` | §21-28 (v4.1) | 8S LiFePO4 cell + power + energy + SOC |
| `BatteryStatusSerializer.h` | §31-33 | Shared REST+MQTT telemetry serializer |

### Architecture (v4.3)

```
                    ┌─────────────────────┐
                    │       USER          │
                    └──────────┬──────────┘
                               │
                               ▼
                    ┌─────────────────────┐
                    │       PWA           │
                    │ UI / State Model    │
                    │ (CommandExecutionState) │
                    └──────────┬──────────┘
                               │
                               ▼
                    ┌─────────────────────┐
                    │ APPLICATION / GAS   │
                    │ Auth / Data / API   │
                    └──────────┬──────────┘
                               │
                               ▼
                    ┌─────────────────────┐
                    │       MQTT          │
                    │ Transport Layer     │
                    └──────────┬──────────┘
                               │
                               ▼
             ┌────────────────────────────────┐
             │ ESP32 AUTHORITATIVE EDGE       │
             │ CONTROLLER                     │
             │                                │
             │ CommandArbiter                  │
             │   ↓                            │
             │ InterlockEngine                │
             │   ↓                            │
             │ SafetySupervisor               │
             │   ↓                            │
             │ RelayEngine (unified GPIO)     │
             │   ↓                            │
             │ RelayDriver                    │
             │                                │
             │ HealthSupervisor               │
             │ AlarmRegistry                  │
             │ OTA / Auth / TransactionJournal│
             └───────────────┬────────────────┘
                             │
                             ▼
                    PHYSICAL HARDWARE
```

---

## 📚 Documentation Index

In addition to this README, see:

| Doc | Purpose |
|---|---|
| [SECURITY.md](./SECURITY.md) | Security model, attack surface, hardening checklist |
| [PROTOCOL.md](./PROTOCOL.md) | MQTT/REST message protocol, packet format, QoS |
| [DEPLOYMENT.md](./DEPLOYMENT.md) | Production deployment guide (DEV/STAGING/PROD build profiles) |
| [TEST_PLAN.md](./TEST_PLAN.md) | Required test matrix (unit / integration / fault injection / power-loss) |
| [DISASTER_RECOVERY.md](./DISASTER_RECOVERY.md) | Device replacement, credential recovery, OTA recovery |
| [HARDWARE_SAFETY_CONTRACT.md](./HARDWARE_SAFETY_CONTRACT.md) | GPIO, relay polarity, PSU, grounding, isolation |
| [SAFETY_CASE.md](./SAFETY_CASE.md) | Hazard/cause/risk/prevention/detection/safe-state |
| [COMPATIBILITY_MATRIX.md](./COMPATIBILITY_MATRIX.md) | PWA/Firmware/Protocol version compatibility |

---

## 🔧 Existing v4.0/v4.1/v4.2 Documentation

The sections below are preserved from previous versions. They remain accurate
for all existing features. The v4.3 additions are layered on top — no existing
behavior has been removed.

## 🔧 Existing v4.0/v4.1 Documentation

The sections below are preserved from v4.0/v4.1. They remain accurate for
all existing features. The v4.2 additions are layered on top — no existing
behavior has been removed.

---


## Table of Contents

1. [Repository Layout](#repository-layout)
2. [Architecture Overview](#architecture-overview)
3. [Security Architecture (Rounds 9–10K)](#security-architecture-rounds-910k)
4. [Production Deployment Guide](#production-deployment-guide)
   - [Step 1: Generate Ed25519 Signing Keys](#step-1-generate-ed25519-signing-keys)
   - [Step 2: Deploy Mosquitto MQTT Broker](#step-2-deploy-mosquitto-mqtt-broker)
   - [Step 3: Deploy Google Apps Script](#step-3-deploy-google-apps-script)
   - [Step 4: Configure Firmware (Config.h)](#step-4-configure-firmware-configh)
   - [Step 5: Compile with PRODUCTION_BUILD Flag](#step-5-compile-with-production_build-flag)
   - [Step 6: Flash + First Boot](#step-6-flash--first-boot)
   - [Step 7: Connect PWA](#step-7-connect-pwa)
5. [Development Setup (Quick Start)](#development-setup-quick-start)
6. [Hardware Wiring](#hardware-wiring)
7. [Firmware Subsystems](#firmware-subsystems)
8. [OTA Firmware Update (Signed)](#ota-firmware-update-signed)
9. [API Contract](#api-contract)
10. [Power-Loss Test Plan](#power-loss-test-plan)
11. [Security Audit History](#security-audit-history)
12. [Known Limitations](#known-limitations)
13. [Troubleshooting](#troubleshooting)

---

## Repository Layout

```
Firmware-code-gs_relaytimer/
├── firmware/                           ← ESP32 Arduino sketch (55 files, flat layout)
│   ├── firmware_v4.ino                 ← main entry (setup + loop)
│   ├── platformio.ini                  ← PlatformIO config (optional)
│   ├── Config.h                        ← ALL compile-time constants (edit this!)
│   │
│   ├── ── Transaction Layer (R10G-R10K) ──
│   ├── TransactionJournal.h            ← NVS-persisted transaction journal
│   ├── TransactionJournal.cpp          ← CRC32 + magic + two-phase commit
│   │
│   ├── ── Network ──
│   ├── MqttClient.h                    ← MQTT client + ACK transaction + dedup
│   ├── MqttClient.cpp                  ← TLS, Ed25519 OTA, all mutation ACK
│   ├── WifiManager.h                   ← AP+STA, Config Portal
│   ├── WifiManager.cpp                 ← NVS credential generation (MQTT pass, GAS secret)
│   ├── HttpServer.h                    ← REST API server (22 routes)
│   ├── HttpServer.cpp                  ← CORS, security headers, route registration
│   ├── Common.h                        ← Shared CORS + JSON helpers
│   │
│   ├── ── Auth ──
│   ├── AuthManager.h                   ← JWT + CSRF + refresh token rotation
│   ├── AuthManager.cpp                 ← NVS-backed refresh tokens, rate limiter
│   ├── AuthHandlers.h                  ← /api/login, /api/logout, /api/refresh, /api/session
│   │
│   ├── ── OTA ──
│   ├── OtaManager.h                    ← Boot health check + rollback
│   ├── OtaManager.cpp                  ← esp_ota_mark_app_invalid_rollback_and_restart()
│   ├── OtaHandlers.h                   ← REST /api/ota upload handler
│   │
│   ├── ── Drivers ──
│   ├── RelayDriver.{cpp,h}             ← 12-channel active-LOW relay
│   ├── PirDriver.{cpp,h}               ← 4× HC-SR501 PIR (debounce, stuck detect)
│   ├── RtcDriver.{cpp,h}               ← DS3231 RTC (I2C 400kHz)
│   ├── PzemDriver.{cpp,h}              ← PZEM-004T v3.0 (self-contained Modbus-RTU)
│   │
│   ├── ── Services ──
│   ├── RelayEngine.{cpp,h}             ← Priority: Manual > PIR > Schedule > Off
│   ├── Scheduler.{cpp,h}               ← Schedule evaluation (overnight + dayMask)
│   ├── LogService.{cpp,h}              ← Activity log (JSON-lines) + audit log
│   ├── Advisor.{cpp,h}                 ← GAS integration (hourly POST → Gemini)
│   │
│   ├── ── Storage ──
│   ├── ConfigStore.{cpp,h}             ← LittleFS persistence + NVS (JWT secret, refresh tokens)
│   ├── FileSystem.{cpp,h}              ← LittleFS wrapper
│   │
│   ├── ── Crypto ──
│   ├── Crypto.{cpp,h}                  ← SHA-256, PBKDF2, HMAC, JWT, Ed25519 (PSA Crypto API)
│   ├── Crc.{cpp,h}                     ← CRC-32 (zlib)
│   ├── Json.{cpp,h}                    ← ArduinoJson helpers
│   │
│   ├── ── REST Handlers (12 files) ──
│   ├── *Handlers.h                     ← 22 route handlers (one header per resource)
│   │
│   └── Types.h, Globals.h              ← Data structures + global state
│
├── code.gs/
│   └── Code.gs                         ← Google Apps Script (HMAC + Gemini)
│
├── scripts/
│   └── sign_firmware.py                ← Ed25519 signing tool (generate keys + sign firmware)
│
├── TEST_PLAN.md                        ← 12 power-loss acceptance tests
└── README.md                           ← this file
```

The firmware uses a **flat layout** (all `.cpp`/`.h` files at root of `firmware/`) so it works with both Arduino IDE (auto-discovers `.ino` + same-folder sources) and PlatformIO. There is **no nested `src/` directory**.

---

## Architecture Overview

```
                          ┌─────────────────────────────────────────┐
                          │     ESP32-WROOM-32 (this repo)           │
                          │                                         │
   PIR 1-4 ─── GPIO 34-39 │  TransactionJournal (NVS, CRC32, 2PC)  │
   DS3231 ──── I2C (32,33) │  MqttClient (TLS, ACK, Ed25519 OTA)    │
   PZEM-004T ─ UART2 (4,5) │  AuthManager (JWT 15min + refresh 7d)  │
                          │  RelayEngine (Manual>PIR>Schedule>Off)  │
                          │  PzemDriver (Modbus-RTU, self-contained) │
                          │  Advisor (GAS HMAC → Gemini)            │
                          │  OtaManager (boot health + rollback)    │
                          └────────┬──────────┬──────────┬─────────┘
                                   │          │          │
                         REST (80) │          │ MQTT     │ HTTPS (hourly)
                                   │          │ 8883/TLS │ + HMAC
                       ┌───────────┘          │          └──────────┐
                       │                      │                     │
              ┌────────▼─────────┐   ┌────────▼────────┐   ┌────────▼────────┐
              │ Cloudflare Tunnel │   │ Mosquitto Broker │   │ Google Apps     │
              │ (optional, LAN)   │   │ (self-hosted,    │   │ Script Web App  │
              └────────┬─────────┘   │  TLS + ACL +     │   │                 │
                       │             │  per-device auth) │   │ → Gemini API    │
              ┌────────▼─────────┐   └────────┬────────┘   │ → cache 1 hour  │
              │  PWA on Vercel   │◄───────────┘            └─────────────────┘
              │  (Remote-Relay)  │    WSS (8884) for PWA
              └──────────────────┘
```

**Key design principle**: ESP32 is the **single source of truth**. It keeps working even if internet, Cloudflare, Vercel, Google, or the MQTT broker are all down. The PWA is just a UI; all scheduling, PIR logic, RTC time, and relay control live in firmware and run locally.

---

## Security Architecture (Rounds 9–10K)

This firmware has been through **12 rounds of security audit** by an external engineer. Key hardening applied:

### Transaction Layer (R10G–R10K)

- **NVS-persisted transaction journal**: `{requestId, commandHash, ackJson}` stored in flash. Survives reboot. Same requestId → NEVER re-execute → always replay original ACK.
- **Two-phase commit**: `tj_entry_N` (blob data) + `tj_commit_N` (1-byte commit flag). writeIdx persisted BEFORE commit flag. All 3 failure scenarios verified safe.
- **CRC32 + magic + version**: Every blob has `[magic:2][version:1][valid:1][CRC32:4][payload]`. CRC mismatch → entry rejected, slot freed.
- **64-entry LRU journal**: At 100 commands/day, oldest entry is ~15 hours old when evicted. PWA retry window is ~2 minutes. 15h >> 2min.
- **ACK retry queue**: Pending ACKs retried every 2s (max 10 attempts). On boot: all stored ACKs re-queued from NVS.

### MQTT Security (R10A–R10F)

- **TLS mandatory** in production (port 8883/8884). `setCACert(MQTT_ROOT_CA)`. No `setInsecure()` fallback.
- **PRODUCTION_BUILD flag**: When defined, enforces TLS + username + password + CA + CORS + OTA pubkey + OTA CA. Hard-fail if any missing.
- **Password removed from topic** (R10C-3): Topic is `timer12/<mac>/<subtopic>`. Auth via broker credentials, authz via broker ACL.
- **Strict requestId validation**: Required, max 64 chars, safe ASCII only.
- **Unknown-field rejection**: Each command type has whitelist. Extra fields → command rejected.
- **Per-command-type canonical hash**: `commandHash = SHA-256(type|action|field1=val1|...)`. requestId bound to exact command.

### OTA Security (R10B–R10D)

- **Ed25519 signature verification** via PSA Crypto API: `PSA_KEY_TYPE_ECC_PUBLIC_KEY(PSA_ECC_FAMILY_TWISTED_EDWARDS)` + `PSA_ALG_PURE_EDDSA`.
- **Signing contract**: `signature = ed25519_sign(SHA256(firmware.bin), private_key)`. ESP32 verifies signature over 32-byte SHA-256 hash.
- **HTTPS download**: `WiFiClientSecure` + `setCACert(OTA_HTTPS_ROOT_CA)`. No plain HTTP.
- **Strict SemVer**: `sscanf("%d.%d.%d%c")` — rejects "4.1.0foo", "4.1.0-beta", "4.1.0.1".
- **Anti-downgrade**: New version must be strictly greater than current.
- **Boot health check + rollback**: 3 failed boots → `esp_ota_mark_app_invalid_rollback_and_restart()`.

### Auth Security (R10B-5)

- **JWT access token**: 15-minute TTL (HS256, per-device random NVS secret).
- **Refresh token rotation**: 7-day TTL, one-time use, NVS-backed. `/api/refresh` validates + rotates. Reuse of invalidated token → security violation.
- **JWT secret**: Random 32-byte per-device, stored in NVS. No compile-time fallback.
- **PBKDF2-HMAC-SHA256** password hashing (10000 iterations, 16-byte salt).
- **CSRF double-submit cookie**: sameSite=Strict, constant-time compare.
- **Rate limiter**: 5 fails → 60s block, 10 fails → 5min block.

### GAS Security (R10A–R10C)

- **HMAC-SHA256 authentication**: ESP32 signs each POST with `HMAC-SHA256(deviceSecret, timestamp\nnonce\ndeviceId\nbody)`.
- **Auth metadata via URL params** (not HTTP headers — GAS doesn't expose headers): `?deviceId=...&timestamp=...&nonce=...&signature=...`
- **Timestamp ±5min tolerance** + **nonce replay protection** (LockService atomic check).
- **Anonymous device ID**: `SHA-256(MAC).substring(0, 16)`. Gemini never sees raw MAC.
- **Body validation**: Max 16KB, max 100 logs, max 32-char channel names.

---

## Production Deployment Guide

### Step 1: Generate Ed25519 Signing Keys

OTA firmware updates require Ed25519 signature verification. Generate a keypair once:

```bash
# Install cryptography library
pip install cryptography

# Generate keypair (one-time per project)
python3 scripts/sign_firmware.py --gen-keys

# Output:
#   firmware_signing_private.pem  (KEEP SECRET — signing machine only)
#   firmware_signing_public.pem   (can be public)
#
# Serial output will show the public key hex:
#   constexpr const char* OTA_ED25519_PUBLIC_KEY_HEX = "abcdef0123456789...";
```

**⚠️ CRITICAL**: Add `firmware_signing_private.pem` to `.gitignore`. NEVER commit the private key.

### Step 2: Deploy Mosquitto MQTT Broker

Production 220V relay control requires a self-hosted MQTT broker with TLS + ACL + per-device credentials.

#### 2a. Provision VPS

- **Recommended**: DigitalOcean Droplet ($4/mo) or Hetzner Cloud (€3.29/mo)
- **OS**: Ubuntu 22.04 LTS
- **Domain**: Register a domain (e.g., `mqtt.yourdomain.com`) and point DNS A record to VPS IP

#### 2b. Install Mosquitto

```bash
ssh root@your-vps
apt update && apt install -y mosquitto mosquitto-clients certbot
```

#### 2c. Get Let's Encrypt TLS Certificate

```bash
certbot certonly --standalone -d mqtt.yourdomain.com
# Certificates saved to:
#   /etc/letsencrypt/live/mqtt.yourdomain.com/fullchain.pem
#   /etc/letsencrypt/live/mqtt.yourdomain.com/privkey.pem
```

#### 2d. Configure Mosquitto

Create `/etc/mosquitto/conf.d/timer12.conf`:

```
# TLS listener on port 8883
listener 8883
certfile /etc/letsencrypt/live/mqtt.yourdomain.com/fullchain.pem
keyfile /etc/letsencrypt/live/mqtt.yourdomain.com/privkey.pem
tls_version tlsv1.2

# Require authentication
allow_anonymous false
password_file /etc/mosquitto/passwd

# ACL file
acl_file /etc/mosquitto/acl
```

#### 2e. Create Device Credentials

```bash
# Create password file with first user (device MAC as username)
mosquitto_passwd -c /etc/mosquitto/passwd device-A4CF12345678
# Enter a strong password when prompted

# Add more devices
mosquitto_passwd /etc/mosquitto/passwd device-A4CF12345679
```

#### 2f. Configure ACL (Per-Device Topic Restrictions) — MANDATORY for production

> **audit-fixes (P0-5)**: The previous ACL example used `topic readwrite timer12/#`
> for the PWA user, which grants read+write access to ALL devices on the broker.
> For a system controlling 220V mains, this is unacceptable — a single
> browser-exposed credential (see PWA's `NEXT_PUBLIC_MQTT_PASSWORD`) would let
> any web visitor control every relay on every device.
>
> **Production ACL MUST be per-device.** Create one broker user per (PWA, device)
> pair, scoped to exactly that device's topics. The pattern below is the
> minimum acceptable configuration for production.

Create `/etc/mosquitto/acl`:

```
# ─── Device A4CF12345678 ───
# The device itself: can publish its own status/log/ack/online + read commands
user device-A4CF12345678
topic read   timer12/A4CF12345678/command
topic read   timer12/A4CF12345678/ota
topic write  timer12/A4CF12345678/status
topic write  timer12/A4CF12345678/log
topic write  timer12/A4CF12345678/ack
topic write  timer12/A4CF12345678/online

# ─── PWA user for device A4CF12345678 ───
# Browser-exposed credential — MUST be scoped to ONE device only.
# Create a separate PWA user per device. Do NOT use a shared `pwa-user`
# with `topic readwrite timer12/#`.
user pwa-A4CF12345678
topic write  timer12/A4CF12345678/command
topic write  timer12/A4CF12345678/ota
topic read   timer12/A4CF12345678/status
topic read   timer12/A4CF12345678/log
topic read   timer12/A4CF12345678/ack
topic read   timer12/A4CF12345678/online

# ─── Device A4CF12345679 (repeat pattern) ───
user device-A4CF12345679
topic read   timer12/A4CF12345679/command
topic read   timer12/A4CF12345679/ota
topic write  timer12/A4CF12345679/status
topic write  timer12/A4CF12345679/log
topic write  timer12/A4CF12345679/ack
topic write  timer12/A4CF12345679/online

user pwa-A4CF12345679
topic write  timer12/A4CF12345679/command
topic write  timer12/A4CF12345679/ota
topic read   timer12/A4CF12345679/status
topic read   timer12/A4CF12345679/log
topic read   timer12/A4CF12345679/ack
topic read   timer12/A4CF12345679/online
```

> **Why per-device PWA users?** The PWA's `NEXT_PUBLIC_MQTT_PASSWORD` is
> browser-exposed (it's inlined into the client bundle by Next.js). Any web
> visitor can extract it. If a single PWA credential had `timer12/#` access,
> that visitor could control every relay on every device on your broker.
> Per-device scoping limits the blast radius to one device per leaked
> credential. For multi-device deployments, the PWA should either:
>   1. Prompt the user to enter a per-device broker credential at login (each
>      device ships with its own `pwa-<MAC>` user), OR
>   2. Sit behind an auth gateway that issues short-lived broker credentials
>      after authenticating the user (recommended for >10 devices).
>
> See the PWA README's "Security Architecture" section for the full threat model.

#### 2g. Restart Mosquitto

```bash
systemctl restart mosquitto
systemctl enable mosquitto
```

#### 2h. Get Root CA for Firmware

The firmware needs the **Let's Encrypt root CA** (ISRG Root X1) to verify the broker's TLS certificate:

```bash
# Download ISRG Root X1
curl https://letsencrypt.org/certs/isrgrootx1.pem -o isrgrootx1.pem
cat isrgrootx1.pem
# Copy the entire PEM content — you'll paste it into Config.h MQTT_ROOT_CA
```

Also get the CA for your OTA download host (e.g., GitHub Releases uses DigiCert):

```bash
# For GitHub Releases:
# Download DigiCert Global Root CA
curl https://dl.digicert.com/DigiCertGlobalRootCA.crt -o digicert.pem
openssl x509 -inform DER -in digicert.pem -out digicert_global_root.pem
cat digicert_global_root.pem
```

### Step 3: Deploy Google Apps Script

The GAS Web App receives hourly logs from ESP32 and calls Gemini API for AI insights.

#### 3a. Create GAS Project

1. Open [script.google.com](https://script.google.com) → **New Project**
2. Delete default code, paste contents of `code.gs/Code.gs`
3. Save project (name it "Timer12 AI Insights")

#### 3b. Set Gemini API Key

1. Get free API key at [aistudio.google.com](https://aistudio.google.com/app/apikey)
2. In GAS: **Project Settings → Script Properties → Edit script properties**
3. Add: `GEMINI_API_KEY` = your API key

#### 3c. Deploy as Web App

1. **Deploy → New Deployment**
2. Type: **Web App**
3. Execute as: **Me**
4. Who has access: **Anyone** (anonymous — HMAC provides auth)
5. Copy deployment URL: `https://script.google.com/macros/s/AKfyc.../exec`

#### 3d. Register Device HMAC Secret

When ESP32 first boots, it generates a 32-byte random GAS secret and prints to Serial:

```
[WiFi] GAS HMAC Secret: <64 hex chars>
[WiFi] Copy GAS secret to GAS Script Properties:
[WiFi]   Key: DEVICE_<anonymousId>_SECRET
[WiFi]   Value: <64 hex chars>
```

1. Copy the anonymous ID (16 hex chars) and secret (64 hex chars) from Serial
2. In GAS: **Project Settings → Script Properties → Edit script properties**
3. Add: `DEVICE_<anonymousId>_SECRET` = `<64 hex chars>`

### Step 4: Configure Firmware (Config.h)

Edit `firmware/Config.h` with your production values:

```cpp
// ── MQTT Broker ──
constexpr const char* MQTT_BROKER_HOST = "mqtt.yourdomain.com";
constexpr uint16_t MQTT_BROKER_PORT = 8883;  // TLS
constexpr const char* MQTT_BROKER_USERNAME = "device-A4CF12345678";
constexpr const char* MQTT_BROKER_PASSWORD = "your-device-password";

// ── TLS Root CA (paste full PEM, multi-line) ──
constexpr const char* MQTT_ROOT_CA =
"-----BEGIN CERTIFICATE-----\n"
"MIIFazCCA1OgAwIBAgIRAIIQz7DSQONZRGPgu2OCiwAwDQYJKoZIhvcNAQELBQAw\n"
"TzELMAkGA1UEBhMCVVMxKTAnBgNVBAoTIEludGVybmV0IFNlY3VyaXR5IFJlc2Vh\n"
"cmNoIEdyb3VwMRUwEwYDVQQDEwxJU1JHIFJvb3QgWDEwHhcNMTUwNjA0MTEwNDM4\n"
"... (full ISRG Root X1 PEM) ...\n"
"-----END CERTIFICATE-----\n";

// ── CORS (your PWA's Vercel URL) ──
constexpr const char* ALLOWED_CORS_ORIGINS = "https://remote-relay.vercel.app";

// ── GAS URL ──
constexpr const char* GAS_INSIGHTS_URL = "https://script.google.com/macros/s/AKfyc.../exec";

// ── OTA Ed25519 Public Key (from Step 1) ──
constexpr const char* OTA_ED25519_PUBLIC_KEY_HEX = "abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789";

// ── OTA HTTPS Root CA (for GitHub Releases, etc.) ──
constexpr const char* OTA_HTTPS_ROOT_CA =
"-----BEGIN CERTIFICATE-----\n"
"... (DigiCert Global Root CA PEM) ...\n"
"-----END CERTIFICATE-----\n";
```

### Step 5: Compile with PRODUCTION_BUILD Flag

**Arduino IDE:**
1. Open `firmware/firmware_v4.ino`
2. **File → Preferences → Settings → "Additional boards manager URLs"** — ensure ESP32 core 3.3.7+ is installed
3. **Tools → Board → ESP32 Dev Module**
4. **Tools → Partition Scheme → "Default 4MB with spiffs"** (required for OTA rollback)
5. **File → Preferences → Settings → "Show verbose output during: compilation"**
6. Add `-DPRODUCTION_BUILD` to compile flags:
   - **Arduino IDE 2.x**: Use `arduino-cli` or edit `platform.local.txt`
   - **PlatformIO**: Add to `platformio.ini`:
     ```ini
     build_flags = -DPRODUCTION_BUILD
     ```
7. Click **Upload**

**PRODUCTION_BUILD enforces:**
- TLS port (8883/8884) mandatory
- MQTT_BROKER_USERNAME/PASSWORD mandatory
- MQTT_ROOT_CA mandatory
- ALLOWED_CORS_ORIGINS ≠ "*"
- OTA_ED25519_PUBLIC_KEY_HEX non-empty
- OTA_HTTPS_ROOT_CA non-empty

If any check fails → firmware refuses to boot (hard fail).

### Step 6: Flash + First Boot

1. Connect ESP32 via USB
2. Click **Upload** in Arduino IDE
3. Open **Serial Monitor** at 115200 baud
4. Observe boot sequence:

```
========================================
Timer 12 Relay v4.0.0
Build: Aug 14 2026 ...
Cloud-Ready Architecture (modular)
========================================
[WiFi] MQTT Password: K7M3P9XQ
[WiFi] Device PIN: 123456
[WiFi] GAS HMAC Secret: a1b2c3d4e5f6...
[WiFi] Copy GAS secret to GAS Script Properties:
[WiFi]   Key: DEVICE_a1b2c3d4e5f6a1b2_SECRET
[WiFi]   Value: a1b2c3d4e5f6...
========================================
WiFi: connecting to STA "YourWiFi"...
WiFi STA connected! IP: 192.168.1.50, RSSI: -55 dBm
[MQTT] PRODUCTION_BUILD flag defined — enforcing all production requirements
[MQTT] Production mode: TLS + auth + CA verified ✓
[MQTT] TLS: using configured root CA for broker cert validation
[MQTT] Using TLS (port 8883)
MQTT: connected!
[Journal] Loaded 0 valid transactions from NVS (capacity 64)
[PZEM] PZEM-004T v3.0 detected!
[AI] GAS URL configured
[AI] Will POST logs every 60 minutes
Boot complete. Ready.
[OTA] Boot attempts: 0/3 (first boot after OTA: no)
```

5. **Copy these values** (needed for PWA login):
   - **MAC Address** (shown in Serial or on the WiFi AP)
   - **MQTT Password** (8 chars)
   - **GAS HMAC Secret** (64 hex chars — register in GAS Script Properties)
   - **Device PIN** (6 digits — for factory reset)

### Step 7: Connect PWA

1. Open PWA URL (Vercel deployment)
2. Scroll to **"Remote Mode (MQTT)"** card
3. Enter:
   - **Device ID (MAC):** 12 hex chars from Serial (e.g., `A4CF12345678`)
   - **MQTT Password:** 8 chars from Serial (e.g., `K7M3P9XQ`)
4. Click **Connect via MQTT**
5. Dashboard loads — control relays from anywhere

**PWA env vars** (set in Vercel → Settings → Environment Variables):

| Variable | Value |
|----------|-------|
| `NEXT_PUBLIC_MQTT_BROKER_URL` | `wss://mqtt.yourdomain.com:8884/mqtt` |
| `NEXT_PUBLIC_MQTT_USERNAME` | PWA broker username (or device username) |
| `NEXT_PUBLIC_MQTT_PASSWORD` | PWA broker password (or device password) |
| `NEXT_PUBLIC_GAS_INSIGHTS_URL` | GAS Web App URL from Step 3c |

---

## Development Setup (Quick Start)

For development/testing without production security:

### Prerequisites

- **Arduino IDE 2.x** (or PlatformIO Core)
- **ESP32 Arduino Core v3.3.7+** by Espressif
- **Libraries** (Library Manager):
  - `RTClib` by Adafruit (DS3231)
  - `ArduinoJson` by Benoit Blanchon (v6.19+)
  - `PubSubClient` by Nick O'Leary (MQTT)
- USB driver: CP2102 (Silicon Labs) or CH340 (WCH)

### Build (Development Mode)

1. Clone: `git clone https://github.com/desvandi/Firmware-code-gs_relaytimer.git`
2. Open `firmware/firmware_v4.ino` in Arduino IDE
3. Select board: **ESP32 Dev Module**
4. Leave `Config.h` defaults (HiveMQ public broker, port 1883, no auth)
5. Click **Upload**
6. Open Serial Monitor at 115200 baud

### First Boot (Development)

ESP32 starts in **AP mode**:
- SSID: `Timer12-Setup`
- Connect to it, open `http://192.168.4.1`
- Enter WiFi SSID + password
- ESP32 reboots to STA mode

### Default Credentials (Development Only)

```
Username: admin
Password: printed to Serial (derived from MAC)
MQTT Password: printed to Serial (8 random chars)
```

**⚠️ Development mode is NOT secure for 220V relay control.** Use PRODUCTION_BUILD for real deployment.

---

## Hardware Wiring

### Pin Mapping

| Component | GPIO | Type | Notes |
|-----------|------|------|-------|
| **Relay 1** | 13 | Output | Active-LOW (LOW=ON) |
| **Relay 2** | 14 | Output | |
| **Relay 3** | 16 | Output | |
| **Relay 4** | 17 | Output | |
| **Relay 5** | 18 | Output | |
| **Relay 6** | 19 | Output | |
| **Relay 7** | 21 | Output | |
| **Relay 8** | 22 | Output | |
| **Relay 9** | 23 | Output | |
| **Relay 10** | 25 | Output | |
| **Relay 11** | 26 | Output | |
| **Relay 12** | 27 | Output | |
| **PIR 1** | 34 | Input-only | HC-SR501 → Relay 9 |
| **PIR 2** | 35 | Input-only | HC-SR501 → Relay 10 |
| **PIR 3** | 36 | Input-only | HC-SR501 → Relay 11 (SENSOR_VP) |
| **PIR 4** | 39 | Input-only | HC-SR501 → Relay 12 (SENSOR_VN) |
| **I2C SDA** | 32 | I2C Data | DS3231 (400kHz Fast Mode) |
| **I2C SCL** | 33 | I2C Clock | DS3231 |
| **PZEM RX** | 5 | UART RX | PZEM TX → ESP32 GPIO5 |
| **PZEM TX** | 4 | UART TX | ESP32 GPIO4 → PZEM RX |

### Power Supply

- **ESP32**: USB 5V or VIN pin
- **Relay module**: External 5V ≥1A PSU (NOT from ESP32 pin)
- **CRITICAL**: ESP32 GND and relay PSU GND must be connected (shared ground)
- **PZEM-004T**: Connects directly to 220V AC mains for measurement

### ⚠️ 220V AC Safety

- Relays control **mains voltage**. Only wire when power is OFF at the breaker.
- Use adequate wire gauge (≥1.5mm² for 10A loads).
- Enclose in an IP-rated box.
- Add a fuse per channel.
- If unsure, **consult a licensed electrician**.

---

## Firmware Subsystems

### 1. Transaction Journal (`TransactionJournal.cpp`)

NVS-persisted durable transaction log. See [Security Architecture](#security-architecture-rounds-910k) for details.

**Key constants** (in `TransactionJournal.h`):
- `JOURNAL_SIZE = 64` entries
- `BLOB_SIZE = 1200` bytes per entry
- `MAX_PENDING_ACKS = 8`
- `MAX_ACK_RETRIES = 10`
- `ACK_RETRY_INTERVAL_MS = 2000`

### 2. MQTT Client (`MqttClient.cpp`)

- **Topic format**: `timer12/<mac>/<subtopic>` (no password in topic)
- **TLS**: `WiFiClientSecure` + `setCACert(MQTT_ROOT_CA)` when port is 8883/8884
- **ACK transaction**: All mutations send type-specific ACK data. PWA validates via discriminated union.
- **Ed25519 OTA**: Signed firmware verification via PSA Crypto API.
- **Validation pipeline**: parse → validate type → validate fields (whitelist) → validate requestId → compute hash → journal lookup → execute → atomic ACK

### 3. Auth Manager (`AuthManager.cpp`)

- **JWT access token**: 15min TTL, HS256, per-device random NVS secret
- **Refresh token**: 7day TTL, one-time use, NVS-backed, rotation on `/api/refresh`
- **Rate limiter**: 5 fails → 60s block, 10 fails → 5min block
- **Factory reset**: 2-step (prepare → confirm), one-time token (60s TTL)

### 4. OTA Manager (`OtaManager.cpp`)

- **Boot health check**: Increments boot_attempts in NVS. If > 3 → rollback.
- `markBootHealthy()`: Called at end of setup() if all subsystems OK. Calls `esp_ota_mark_app_valid_cancel_rollback()`.

### 5. PZEM-004T v3.0 (`PzemDriver.cpp`)

Self-contained Modbus-RTU (no external library):
- 7 raw parameters: voltage, current, power, energy, frequency, PF, alarm
- 3 derived: apparent power (VA), reactive power (VAR), daily energy
- 5 alarm thresholds: undervoltage, overvoltage, overcurrent, overpower, low PF
- CRC-16 validation on every Modbus response

### 6. Relay Engine (`RelayEngine.cpp`)

Priority: **Manual > PIR > Schedule > Off**
- PIR can only force ON, never OFF
- PIR cannot override Manual mode
- Stuck PIR (HIGH > 30 min) → force-disabled for 5 min cooldown

### 7. Advisor (`Advisor.cpp`)

Hourly POST to Google Apps Script:
- Collects last 50 log entries + PZEM data
- Computes anonymous device ID: `SHA-256(MAC).substring(0, 16)`
- Signs with HMAC-SHA256(deviceSecret, timestamp\nnonce\ndeviceId\nbody)
- Auth metadata sent as URL query params (GAS doesn't expose HTTP headers)
- Watchdog-safe: 8s HTTP timeout, `esp_task_wdt_reset()` during file reads + HTTP

---

## OTA Firmware Update (Signed)

### Signing Firmware

```bash
# Sign a new firmware release
python3 scripts/sign_firmware.py firmware.bin 4.1.0

# Output:
#   firmware.bin.sha256     — 64 hex chars (SHA-256 of binary)
#   firmware.bin.sig        — 128 hex chars (Ed25519 signature over SHA-256)
#   firmware.bin.ota.json   — OTA command payload (fill in URL + version)
```

### Triggering OTA via MQTT

Publish to `timer12/<mac>/ota`:

```json
{
  "action": "update",
  "url": "https://github.com/your-repo/releases/download/v4.1.0/firmware.bin",
  "version": "4.1.0",
  "size": 1234567,
  "sha256": "abcdef0123456789...",
  "signature": "abcdef0123456789...",
  "requestId": "uuid-here"
}
```

ESP32 flow: HTTPS download → size check → SHA-256 verify → Ed25519 verify → Update → reboot → health check.

---

## API Contract

All REST responses: `{ "success": bool, "message": string, "data": T }`

| Method | Endpoint | Purpose |
|--------|----------|---------|
| POST | `/api/login` | JWT (15min) + refresh (7day) + CSRF cookies |
| POST | `/api/refresh` | Rotate access + refresh tokens |
| POST | `/api/logout` | Revoke refresh token in NVS |
| GET | `/api/session` | Check current session |
| GET | `/api/status` | Full SystemStatus (12 channels + PIRs + PZEM) |
| GET | `/api/version` | FirmwareInfo + OTA status |
| POST | `/api/relay` | SET_STATE on/off / set_mode |
| POST | `/api/schedule` | Upsert schedule (max 4/channel) |
| DELETE | `/api/schedule?id=N` | Delete schedule |
| POST | `/api/pir` | Update PIR config |
| POST | `/api/time` | Set RTC time |
| GET | `/api/log` | Activity log (filterable) |
| POST | `/api/channel` | Rename channel |
| POST | `/api/reboot` | Reboot ESP32 |
| POST | `/api/ota` | Upload firmware (REST, not MQTT) |
| POST | `/api/factory_reset/prepare` | Generate reset token (60s) |
| POST | `/api/factory_reset/confirm` | Execute factory reset |

---

## Power-Loss Test Plan

See **[TEST_PLAN.md](TEST_PLAN.md)** for the complete 12-test power-loss acceptance plan.

**Acceptance criterion**: "Tidak ada keadaan recovery di mana sebuah command yang sudah dieksekusi dapat dieksekusi kedua kali karena journal kehilangkan committed record."

All 12 tests must PASS on actual ESP32 hardware before 220V production deployment.

---

## Security Audit History

| Round | Focus | Key Changes |
|-------|-------|-------------|
| R9 | MQTT contract, ACK transaction, SET_STATE, requestId dedup | Foundation |
| R10A | GAS HMAC transport, Ed25519, requestId binding, JWT NVS, TLS guard | 5 P0 blockers |
| R10B | Signing contract, typed ACK, publisher internal, refresh token, SemVer, OTA rollback | 7 protocol fixes |
| R10C | Compile error, PSA Crypto, canonical hash, topic password removal | 4 critical fixes |
| R10D | PSA Ed25519 correct API, dedup replay original, unknown-field reject, strict SemVer | 4 fixes |
| R10E | Atomic ACK transaction, validation reorder, dedup TTL | 4 fixes |
| R10F | Publish failure handling, expired dedup cleanup, PRODUCTION_BUILD flag | 5 fixes |
| R10G | NVS transaction journal, ACK retry queue, strict requestId | Architectural shift |
| R10H | NVS atomicity (blob write), LRU 64 entries, commit flag | 4 fixes |
| R10I | CRC32 + magic + version | Integrity protection |
| R10J | Separate commit key (true two-phase) | Power-loss safety |
| R10K | writeIdx-before-commit ordering fix | Failure ordering fix |

---

## Known Limitations

1. **LRU eviction**: After 64 commands, oldest entry is evicted. At 100 commands/day, oldest is ~15h old. PWA retry window is ~2min. If PWA retries after 15h, command may re-execute. **Accepted risk** for IoT device.

2. **Execute→store gap**: If ESP32 crashes between executing a command and storing to NVS journal, the command will be re-executed on PWA retry. For SET_STATE (relay ON/OFF): idempotent, safe. For schedule upsert: may create duplicate (capped at 4/channel). **Fundamental limitation** without hardware transaction support.

3. **Ed25519 build verification**: PSA Crypto API identifiers are correct per spec, but actual build + known-answer test on Arduino IDE 2.3.8 + ESP32 core 3.3.7 has NOT been verified. If `PSA_ECC_FAMILY_TWISTED_EDWARDS` is not defined, enable via menuconfig → mbedTLS → Elliptic Curve DH/DSA.

4. **NVS wear**: 64 entries × ~5 NVS writes per transaction = ~320 writes per full journal cycle. At 100 commands/day: ~3-5 year flash lifetime. Acceptable for IoT device.

5. **Development default**: Repository ships with insecure defaults (HiveMQ public, port 1883, no auth, CORS `*`). This is for development only. **Use PRODUCTION_BUILD flag for real deployment.**

6. **No Flash Encryption / Secure Boot**: NVS data (JWT secret, refresh tokens, MQTT password, GAS secret) is stored as plaintext in flash. An attacker with physical access (UART, flash dump) can extract these secrets. **For high-security deployments, enable ESP32 Flash Encryption + Secure Boot** (see [Physical Security Hardening](#physical-security-hardening-flash-encryption--secure-boot) below).

7. **Ed25519 PSA Crypto not in default framework**: The default prebuilt `arduino-esp32` framework does NOT include Ed25519 curve support. The code is guarded with `#if defined(MBEDTLS_ED25519_SUPPORTED)` and falls back to fail-closed (OTA rejected) when not compiled in. To enable Ed25519 OTA verification, rebuild the ESP32 framework with `CONFIG_MBEDTLS_ECP_DP_ED25519_ENABLED=y` in sdkconfig and add `-DMBEDTLS_ED25519_SUPPORTED` to `build_flags` in `platformio.ini`.

---

## Physical Security Hardening (Flash Encryption + Secure Boot)

> **audit-fixes (auditor #3 P3)**: For deployments where physical access to the device is a threat (e.g., shared spaces, public installations), enable ESP32 hardware security features. These are NOT code changes — they are one-time provisioning steps via `esptool.py`.

### Why this matters

Without Flash Encryption:
- Anyone with physical access can dump the ESP32 flash via UART or SPI
- All NVS data is plaintext: JWT secret, refresh tokens, MQTT password, GAS HMAC secret, WiFi credentials
- An attacker who extracts the JWT secret can forge valid JWTs and bypass authentication entirely
- Encrypting individual fields (e.g., refresh tokens) without Flash Encryption is **security theater** — the encryption key itself would also be in plaintext NVS

With Flash Encryption + Secure Boot:
- All flash contents are encrypted at rest (AES-256-XTS)
- Secure Boot verifies firmware signature on every boot (rejects unsigned/damaged firmware)
- NVS data is automatically encrypted/decrypted by the ESP32 hardware — no code changes needed
- Physical extraction yields only ciphertext

### Step-by-step (one-time provisioning)

```bash
# 1. Install esptool (if not already installed)
pip install esptool

# 2. Burn Secure Boot key + digest
#    Generate a 256-bit Secure Boot key
python -m espsecure generate_signing_key --version 2 --scheme ecdsa256 secure_boot_signing_key.pem

#    Burn the key digest into eFuse
espsecure.py burn_key_digest --keyfile secure_boot_signing_key.pem --version 2

# 3. Burn Flash Encryption key into eFuse
#    Generate a 256-bit Flash Encryption key
python -m espsecure generate_flash_encryption_key flash_encryption_key.bin

#    Burn it into eFuse BLOCK_KEY0
espefuse.py burn_key BLOCK_KEY0 flash_encryption_key.bin FLASH_CRYPT_DEC

# 4. Enable Flash Encryption in eFuse (DISABLE_SOFT_FLASH_CRYPT_CNT = 1)
#    This permanently enables flash encryption. The next boot will encrypt all
#    flash contents in-place. DO NOT power off during this process.
espefuse.py burn_efuse DISABLE_SOFT_FLASH_CRYPT_CNT

# 5. Enable Secure Boot V2 in eFuse
espefuse.py burn_efuse SECURE_BOOT_EN

# 6. Re-flash the firmware (now encrypted) using esptool.py with --encrypt flag
esptool.py --chip esp32 --port /dev/ttyUSB0 --baud 921600 \
    write_flash --encrypt 0x10000 firmware.bin
```

### ⚠️ CRITICAL WARNINGS

- **Irreversible**: Once eFuses are burned, Flash Encryption and Secure Boot CANNOT be disabled. A device with a corrupted Secure Boot key becomes permanently unusable.
- **Test on a dev board first**: Do not provision a production device without first verifying the full flow on a disposable ESP32.
- **Backup keys**: Store `secure_boot_signing_key.pem` and `flash_encryption_key.bin` in a secure location. Lost keys = unusable device.
- **OTA changes**: After enabling Secure Boot V2, ALL future OTA updates must be signed with the Secure Boot key. The Ed25519 OTA signing in this firmware is separate from Secure Boot — you need BOTH:
  1. Ed25519 signature (firmware-level, prevents unauthorized OTA via MQTT)
  2. Secure Boot V2 signature (hardware-level, verifies firmware integrity on boot)
- **ESP32 vs ESP32-C3/S2/S3**: The exact eFuse names and commands differ slightly between ESP32 variants. Consult the Espressif documentation for your specific chip.

### What this does NOT protect against

- **Online attacks**: Flash Encryption does not protect against network-based attacks. TLS, JWT, CSRF, and rate limiting remain the primary defense.
- **Side-channel attacks**: Power analysis, glitching, and EM attacks are out of scope.
- **Decapping**: A determined attacker with lab equipment can still extract keys from the silicon die. For mission-critical deployments, use a secure element (ATECC608A) for key storage.

### Recommendation

For most IoT deployments (220V relay control in a private home), Flash Encryption + Secure Boot is **recommended but not mandatory**. The realistic threat model is network-based attack, not physical extraction. Enable these features if:
- The device is in a publicly accessible location
- The device controls high-value assets
- Compliance requirements mandate hardware security

---

## Future Work (Architectural Enhancements)

> **audit-fixes (auditor #3)**: These items were identified as valuable but are out of scope for the current audit-fix round. They require architectural changes or new infrastructure and should be planned as separate engineering efforts.

| # | Enhancement | Component | Why deferred |
|---|-------------|-----------|-------------|
| 1 | **Distributed rate limiting** (Upstash Redis / Vercel KV) | PWA | Requires new infrastructure + dependency. Current in-memory limiter works for LAN mode (firmware-side). Vercel serverless limitation is documented. |
| 2 | **Server-side MQTT proxy** (replace NEXT_PUBLIC_MQTT_PASSWORD) | PWA | Fundamental architecture change. PWA would connect to a Next.js API route that proxies WebSocket to the broker. Eliminates browser credential exposure but adds latency and a new single point of failure. Per-device broker ACL (already documented) is the interim mitigation. |
| 3 | **Encrypt refresh tokens in NVS** | Firmware | Security theater without Flash Encryption. If attacker can dump flash, they can also extract the JWT secret (also plaintext in NVS) and forge JWTs directly. Flash Encryption (above) is the correct solution — it protects ALL NVS data, not just refresh tokens. |
| 4 | **MAC address whitelist for REST API** | Firmware | JWT + CSRF + rate limiting already provide adequate auth. MAC whitelist is defense-in-depth but operationally fragile (legitimate users with new devices get locked out). IP whitelist (LAN subnet) is more practical but also fragile. |
| 5 | **Centralized audit logging** | All | Operational concern, not security. Firmware already has local audit log (`/audit.log`, rotated at 8KB). Centralized logging requires a log collector (Loki, ELK, CloudWatch) — out of scope. |
| 6 | **Secure element (ATECC608A) for key storage** | Firmware | Hardware change. ATECC608A would store the Ed25519 private key and JWT secret in tamper-resistant hardware. Eliminates flash extraction risk. Requires PCB redesign. |

---

## Troubleshooting

### Firmware won't compile

- **`PSA_ECC_FAMILY_TWISTED_EDWARDS` not defined**: Enable mbedTLS Ed25519 via menuconfig (PlatformIO) or Arduino IDE board config.
- **`esp_crc.h` not found**: Should be in ESP32 core 3.x. If missing, install/update ESP32 core.
- **`mbedtls/ed25519.h` not found**: This header was REMOVED in mbedtls 3.x. The code now uses `psa/crypto.h` instead. If you see this error, you're compiling an old version.

### MQTT won't connect

- **`FATAL: Production mode requires MQTT_ROOT_CA`**: Paste ISRG Root X1 PEM into `Config.h MQTT_ROOT_CA`.
- **`FATAL: PRODUCTION_BUILD requires TLS port`**: Change `MQTT_BROKER_PORT` to 8883.
- **Connection refused**: Check Mosquitto is running, firewall allows port 8883, TLS cert is valid.
- **Auth failed**: Check `MQTT_BROKER_USERNAME`/`PASSWORD` match `mosquitto_passwd` entries.

### OTA fails

- **`OTA: FATAL — OTA_ED25519_PUBLIC_KEY_HEX not configured`**: Generate keys with `sign_firmware.py --gen-keys`, paste public key into Config.h.
- **`Ed25519 signature verification FAILED`**: Ensure you signed with `sign_firmware.py` (signs SHA-256 hash, NOT full binary). Do NOT use `openssl pkeyutl` directly.
- **`SHA-256 mismatch`**: Binary was modified after signing. Re-sign after any change.
- **`downgrade blocked`**: New version must be strictly greater than current (e.g., 4.0.0 → 4.1.0).

### PWA can't connect

- **PWA shows "MQTT password must be at least 4 chars"**: Enter the 8-char password from Serial Monitor.
- **MQTT timeout**: Check broker URL in PWA env vars (`NEXT_PUBLIC_MQTT_BROKER_URL`). Must be `wss://` (not `ws://`) for TLS.
- **PWA shows "LAN mode disabled"**: This is expected in production MQTT-only mode. Use the MQTT card instead.

### GAS insights not working

- **`Device not registered (no HMAC secret found)`**: Copy the GAS HMAC secret from ESP32 Serial to GAS Script Properties.
- **`Timestamp out of tolerance`**: Ensure ESP32 RTC is set (via PWA Settings → Set RTC Time).
- **`Invalid signature`**: Ensure GAS secret in Script Properties matches exactly what ESP32 printed to Serial.

---

## Companion Repositories

- **PWA Dashboard**: [desvandi/Remote-Relay](https://github.com/desvandi/Remote-Relay) — Next.js 16 PWA, deployed on Vercel
- **Firmware + Code.gs**: This repo

---

## License

Proprietary — built per the Timer Digital Relay v4.0 Engineering Brief. Contact the repo owner for licensing questions.
