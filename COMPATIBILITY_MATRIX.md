# Compatibility Matrix — Timer Digital Relay v4.2

> Implements compatibility requirements from the Industrial-Grade
> Implementation Directive §66, §67.

---

## 1. Version Scheme

| Component | Version pattern | Where stored |
|---|---|---|
| Firmware | `MAJOR.MINOR.PATCH` (e.g., `4.2.0`) | `Config.h::FIRMWARE_VERSION` |
| PWA | `MAJOR.MINOR.PATCH` (e.g., `4.2.0`) | `package.json::version` |
| Protocol | Single integer (e.g., `4`) | Implicit in API field set |
| Config schema | Single integer (e.g., `2`) | `Config.h::CONFIG_VERSION` |
| Transaction journal schema | Single integer | `TransactionJournal.h::JOURNAL_VERSION` |

---

## 2. Current Versions (v4.2.0 release)

| Component | Version |
|---|---|
| Firmware | 4.2.0 |
| PWA | 4.2.0 |
| Protocol | 4 (implicit — adds health/alarms/telemetrySequence fields) |
| Config schema | 2 (unchanged from v4.0 — schedule.json format stable) |
| Transaction journal schema | 1 (unchanged from PD-001) |

---

## 3. Compatibility Matrix

| PWA Version | Firmware Version | Protocol Version | Status | Notes |
|---|---|---|---|---|
| **4.2.x** | **4.2.x** | 4 | ✅ Full | All features: battery + powerFlow + environment + health + alarms + telemetrySequence |
| 4.2.x | 4.1.x | 3 | ✅ Backward-compatible | Battery + powerFlow + environment. Health/alarms fields omitted → PWA shows "N/A". |
| 4.2.x | 4.0.x | 2 | ✅ Backward-compatible | Relay + PZEM only. Battery/health/alarms fields omitted → PWA shows "N/A". |
| 4.1.x | 4.2.x | 3 | ✅ Forward-compatible | v4.1 PWA ignores new v4.2 fields. Battery + powerFlow still rendered. |
| 4.0.x | 4.2.x | 2 | ✅ Forward-compatible | v4.0 PWA ignores v4.1/v4.2 fields. Relay + PZEM only. |
| 4.1.x | 4.1.x | 3 | ✅ Compatible | v4.1 release set |
| 4.0.x | 4.1.x | 2 | ✅ Compatible | Forward-compat |
| 4.0.x | 4.0.x | 2 | ✅ Compatible | v4.0 release set |

**Key principle**: PWA must always render gracefully when firmware omits
new fields. All v4.2 fields are `?: optional` in TypeScript — never required.

---

## 4. Protocol Evolution

### Protocol v2 (firmware v4.0)
- Initial release
- Endpoints: relay, schedule, channel, pir, time, log, config, ota, factory_reset
- MQTT topics: status, command, log, online, ack, ota
- HMAC-GAS integration

### Protocol v3 (firmware v4.1)
- Added: battery, powerFlow, environment, dcEnergy nested objects in /api/status
- Added: MQTT buffer size increased to 16 KB (from 4 KB)
- Added: per-cell resistance, pack resistance with quality fields
- Backward compatible: v4.0 PWA ignores new fields

### Protocol v4 (firmware v4.2)
- Added: health object (uptimeSeconds, bootCount, resetReason, watchdogResets, brownoutResets, freeHeap, etc.)
- Added: systemAlarms array (AlarmSeverity INFO/WARNING/CRITICAL)
- Added: telemetrySequence monotonic counter
- Added: per-channel safety fields (maxOnTimeSec, minOnTimeSec, minOffTimeSec, minSwitchIntervalSec, bootPolicy)
- Added: RTC state machine (rtcStatus VALID/INVALID/UNSYNCED in health block)
- Added: sensor data quality states (SensorStatus VALID/STALE/ERROR/UNAVAILABLE)
- Backward compatible: v4.1 PWA ignores new fields

### Future Protocol v5 (firmware v4.3+ — not yet released)
- Command arbitration engine (priority numeric: SAFETY 1000, EMERGENCY 900, MANUAL 800, etc.)
- Interlock groups (configuration-driven mutual exclusion)
- Device shadow (desired vs reported state)
- Store-and-forward telemetry backlog
- Multi-user RBAC (OWNER/ADMIN/OPERATOR/VIEWER)

---

## 5. OTA Anti-Downgrade Rules

Firmware refuses OTA install if:
- New version < current version (anti-downgrade — brief §49)
- New version's protocol < current protocol (incompatible)
- New binary's SHA-256 doesn't match expected hash
- New binary's Ed25519 signature doesn't verify against embedded public key

**Owner's responsibility**: when downgrading is intentional (e.g., emergency
rollback after bad release), use USB flashing to bypass OTA checks.

---

## 6. PWA Cache Strategy

PWA uses Next.js 16 with the following cache strategy:

| Resource | Cache | Invalidation |
|---|---|---|
| Static assets (/_next/static/*) | 1 year immutable | Filename hash changes → URL changes |
| API GET /api/status | no-store | Real-time telemetry |
| API GET /api/log | no-store | Real-time logs |
| Service worker cache | StaleWhileRevalidate | App shell cached, API always fresh |
| Demo state persistence | localStorage (per browser) | Cleared on factory reset |

When PWA receives a `telemetrySequence` lower than the previous one:
- Detect device reboot
- Refresh all data (force re-fetch /api/status)
- Show "Device rebooted at <timestamp>" notification

---

## 7. Breaking Change Policy

A breaking change requires:
1. Major version bump (4.x → 5.0)
2. New protocol version
3. Migration guide
4. Compatibility window (6 months minimum — both old and new versions work)
5. Forced OTA window announced 2 weeks in advance

Examples of breaking changes:
- Removing a public API endpoint
- Changing a field's meaning (e.g., `alarms` from object to array — this is why we use `systemAlarms` for the new array)
- Changing MQTT topic structure
- Changing HMAC canonical format
- Changing JWT format

Examples of NON-breaking changes:
- Adding optional fields to existing objects
- Adding new endpoints
- Adding new MQTT topics
- Adding new alarm codes
- Increasing buffer sizes
