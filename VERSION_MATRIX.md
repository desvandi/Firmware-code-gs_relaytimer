# VERSION_MATRIX.md — Timer Digital Relay v4.3.8

> **Single Source of Truth** for all version contracts across firmware, PWA,
> protocol, schemas, and documentation. Phase H closure directive 2026-08-20.

---

## 1. Current Authoritative Versions

| Component                  | Version  | Source of truth                                              |
| -------------------------- | -------- | ------------------------------------------------------------ |
| Firmware                   | 4.3.8    | `firmware/firmware/Config.h::FIRMWARE_VERSION`               |
| PWA                        | 4.3.8    | `pwa/package.json::version`                                 |
| Protocol                   | 5        | `firmware/firmware/CommandCanonicalizer.h::CANONICAL_COMMAND_VERSION` |
| Config schema              | 2        | `firmware/firmware/Config.h::CONFIG_VERSION`                 |
| Transaction journal        | 1        | `firmware/firmware/TransactionJournal.h::BLOB_VERSION`       |
| Telemetry schema           | 5        | `firmware/firmware/TelemetrySpool.h::SPOOL_SCHEMA_VERSION`   |
| OTA manifest               | 1        | `firmware/firmware/OtaManager.h::OTA_MANIFEST_VERSION`       |
| GAS AI insights            | 1        | `firmware/code.gs/Code.gs::GAS_SCHEMA_VERSION`               |
| Documentation              | 4.3.8    | All `.md` title lines reference `v4.3.8` (or version-agnostic) |

Enforced by: `firmware/scripts/assert_version_contract.py`.

---

## 2. Git Tag ↔ Binary ↔ Documentation Contract

Every release MUST satisfy the following bijective mapping:

```
git tag v4.3.8 ──┐
                 ├──► firmware.bin (SHA-256 published in release notes)
                 ├──► Config.h::FIRMWARE_VERSION = "4.3.8"
                 ├──► pwa/package.json::version = "4.3.8"
                 └──► All .md title lines reference v4.3.8
                      (or are version-agnostic — no stale v4.2.0/v4.1.0/v4.0.0
                       in current-state sentences; compat-table rows + history
                       sentences are exempt)
```

**Signing contract**: `firmware.bin.sig` is an Ed25519 signature over the
SHA-256 of the binary (NOT the full binary — see Config.h R10B-1 block).

| Release | Git tag | Binary SHA-256 (production env)                            |
| ------- | ------ | -------------------------------------------------------- |
| v4.3.8  | `v4.3.8` | `1714fd769338ad60f8881737729f1fdda9c09dc5453b5c10eed28675a421241a` |

Tag at HEAD is verified by `assert_version_contract.py` — if HEAD is tagged,
the tag (after stripping `v` prefix) must equal `FIRMWARE_VERSION`.

---

## 3. Protocol v5 Field Set (v2 → v3 → v4 → v5 evolution)

| Protocol | Introduced in | Notable field set additions / changes                                              |
| -------- | -------------- | ---------------------------------------------------------------------------------- |
| v2       | v4.0.0         | Initial `requestId` envelope. No transaction journal.                              |
| v3       | v4.1.0         | `transactionId` added (alias of `requestId`). Two-phase commit transaction journal. |
| v4       | v4.2.0         | `commandHash` (SHA-256 over canonical command). Cross-ingress dedup.                |
| **v5**   | **v4.3.8**     | **Method prefix in HMAC canonical** (`POST\n…` / `GET\n…`). OTA Ed25519 signature. Config schema v2. PWA `compatibility.ts` enforced (stale-cache gating). |

### v5 canonical command contract (firmware `CommandCanonicalizer.cpp`)

```
canonical = "v{version}|{type_lower}|{action_lower}|field1=val1|field2=val2|..."
commandHash = SHA-256(canonical)  // hex
```

- `requestId` / `transactionId` are EXCLUDED from the canonical form
  (they identify the transaction, not the command).
- Two requests with the same logical command but different `requestId`s
  produce the same `commandHash` → enables cross-ingress duplicate detection.
- Field order is FIXED per type (see `buildCanonicalString()`).

### v5 HMAC canonical contract (firmware `Advisor.cpp` + `Code.gs`)

```
POST canonical = "POST\n" + timestamp + "\n" + nonce + "\n" + deviceId + "\n" + body
GET  canonical = "GET\n"  + timestamp + "\n" + nonce + "\n" + deviceId + "\n" + ""
signature = HMAC-SHA256(deviceSecret, canonical).hex().upper()
```

- Method prefix (`POST\n` vs `GET\n`) prevents GET signature replay on POST endpoint.
- Single-char change in body causes signature mismatch (verified by test #4 in `test_pwa_esp32_gas_security_chain.py`).

---

## 4. AI Insights Schema (Phase 5 reconciliation)

### Categories (whitelist)

| Category                  | Description                                                       |
| ------------------------- | ----------------------------------------------------------------- |
| `habit_analysis`          | Patterns in user habits (schedules, manual overrides).            |
| `energy_analysis`         | Wh / kW anomalies, power factor, daily/weekly trends.             |
| `fault_detection`         | Hardware faults: relay stuck, PIR stuck, PZEM out-of-range.       |
| `predictive_maintenance`  | Pre-emptive warnings based on wear/heuristics.                     |
| `pir_recommendation`      | PIR placement / hold-time tuning suggestions.                     |
| `battery_analysis`        | Cell imbalance, SoH degradation, charging profile advice.          |

### Severities (whitelist)

`info` | `warning` | `critical`

### Action types (whitelist)

| Action type          | Effect                                                                              |
| -------------------- | ----------------------------------------------------------------------------------- |
| `apply_suggestion`  | **ADVISORY ONLY** — non-actuating. PWA renders as advisory card. NO relay mutation. |
| `review`            | Display for user review; user takes action manually.                                |
| `dismiss`           | User-dismissed.                                                                     |

### Insight envelope

| Field          | Type                  | Required | Notes                                                          |
| -------------- | --------------------- | -------- | -------------------------------------------------------------- |
| `id`           | string                | yes      | Unique per insight.                                            |
| `category`     | enum (whitelist)      | yes      | See table above.                                               |
| `severity`     | enum (whitelist)       | yes      | See table above.                                               |
| `title`        | string                | yes      | Short headline.                                                |
| `body`         | string                | yes      | Detailed explanation.                                          |
| `channelId`    | number \| null         | no       | 1..12 if channel-scoped.                                       |
| `action`       | `{ label, type }`     | no       | Action button config.                                          |
| `generatedAt`  | number (ms)           | yes      | Unix epoch ms.                                                 |
| `source`       | `'gemini'` \| `'mock'` | yes     | Whether from real Gemini or fallback mock.                     |
| `advisoryOnly` | boolean               | yes      | **Must NOT be `false`** — PWA validator rejects `advisoryOnly === false`. |

### Source

`gemini` (real) or `mock` (fallback when GAS not configured / parse error / HTTP error / WiFi down).

### advisoryOnly enforcement (PH6-1)

`validateInsight_()` in `pwa/src/lib/aiInsights.ts` rejects any insight with
`advisoryOnly === false`. This is the runtime guarantee that AI insights
cannot trigger an actuator mutation on the PWA side.

The firmware `InsightsHandlers.h` also stamps `advisoryOnly: true` on all
mock insights it generates.

---

## 5. Authentication Contract Per Layer (Phase 12)

| Layer         | From        | To        | Mechanism                                                          | Enforcement                                          |
| ------------- | ----------- | --------- | ------------------------------------------------------------------ | ---------------------------------------------------- |
| PWA → ESP32   | Browser     | ESP32     | Cookie (HTTP-only) JWT access token (15 min) + CSRF token          | `AuthManager.cpp` + `csrfTokenCache` in `api.ts`     |
| PWA → MQTT    | Browser     | Mosquitto | TLS (8883) + per-device username/password + ACL                     | `mosquitto_acl.conf` (Phase H)                       |
| ESP32 → GAS   | ESP32       | GAS Web App | HMAC-SHA256 signature over method-prefixed canonical               | `Advisor.cpp::_buildAuthenticatedUrl()` + Code.gs    |
| ESP32 → OTA   | ESP32       | GitHub Releases / VPS | HTTPS download + Ed25519 signature verification         | `OtaManager.cpp` + `OTA_ED25519_PUBLIC_KEY_HEX`       |

### PWA → ESP32 (cookies + CSRF)

- Access token (JWT, 15 min TTL) stored in HTTP-only, SameSite=Strict cookie.
- Refresh token (32-hex, 7 day TTL) stored in NVS, one-time use per login (LRU 4 max).
- CSRF token (16 bytes hex) attached as `X-CSRF-Token` header on every state-changing request.

### PWA → MQTT (TLS + ACL)

- TLS 1.2+ required (port 8883/8884).
- Per-device credentials via Mosquitto `password_file`.
- Per-device ACL via `mosquitto_acl.conf` (Phase H) — see §11.
- `allow_anonymous false` enforced in `mosquitto.conf`.
- Default-deny: any topic not explicitly listed is denied.

### ESP32 → GAS (HMAC)

- 32-byte random secret per device, stored in NVS at first boot.
- HMAC-SHA256 over method-prefixed canonical: `{METHOD}\n{timestamp}\n{nonce}\n{deviceId}\n{body}`.
- `timestamp` ±5 min tolerance, `nonce` replay-protected (10 min cache).
- Both `doGet` and `doPost` require signature (PH2-1 — anonymous GET no longer allowed).

### ESP32 → OTA (HTTPS + Ed25519)

- HTTPS download from allowlisted hosts (`OTA_ALLOWED_HOSTS`).
- `OTA_HTTPS_ROOT_CA` must be non-empty in production.
- `OTA_ED25519_PUBLIC_KEY_HEX` must be non-empty in production.
- Signature is over the 32-byte SHA-256 hash of the binary (NOT the full binary).

---

## 6. Telemetry Persistence Contract (Phase 7)

| Layer            | Storage            | Persistence | Wear concern                                          |
| ---------------- | ------------------ | ----------- | ---------------------------------------------------- |
| Regular telemetry | RAM ring (16 slots) | EPHEMERAL   | 10s interval → 3.15M writes/year → sector death ~12 days |
| Critical events  | NVS (8 slots, fixed-size blobs) | PERSISTENT  | ~10/day → 3,650/year → 27 YEARS per sector            |
| CacheService (GAS) | GAS Script Cache | EPHEMERAL   | Google-managed; not relied upon for durability        |

### Critical events that survive power loss

- BOOT (sequence + uptime)
- ALARM (overcurrent, overvoltage, brownout, PIR stuck)
- FAULT (relay stuck, schedule conflict, I2C failure)
- SAFETY (maxOnTime lockout, manual override rejection)

### Compaction + recovery (Phase D)

`TelemetrySpool::_loadCriticalFromNvs()` on boot:

1. Verify schema version (mismatch → NVS cleared, fresh start).
2. For each slot: read fixed-size blob, verify CRC-16/CCITT.
3. Empty-slot detection: `if (rec.sequence == 0 && rec.payloadLen == 0) continue`.
4. Corrupted records skipped, `firstInvalid` index recorded.
5. If `validCount < criticalCount` → compact (move valid records to front, persist).

### CacheService is EPHEMERAL

The GAS `CacheService.getScriptCache()` is documented (Google) as best-effort
and may be evicted at any time. It is used for:

- Rate-limit counters (rate-limit may reset early — acceptable)
- Nonce replay protection (nonce may expire early — acceptable, forces re-sign)
- Cached insights (cache miss → regenerate — acceptable)

**It is NOT relied upon for any durability guarantee.**

---

## 7. Version Drift Audit (Phase 27)

Historical record of v4.2 → v4.3.8 reconciliation.

| Pre-reconciliation state                       | Reconciliation action                                              | Status     |
| ---------------------------------------------- | ------------------------------------------------------------------ | ---------- |
| Docs referenced `v4.2`/`v4.1.0`/`v4.0.0` titles | All title lines bumped to `v4.3.8` (or version-agnostic)           | DONE       |
| Config.h `BUILD_DATE` used `__DATE__ __TIME__` | Replaced with reproducible `v4.3.8-release` constant               | DONE       |
| Config.h had `JWT_SECRET_DEFAULT = "Timer12-v4.0-CHANGE-ME-IN-PRODUCTION"` | REMOVED — ConfigStore generates per-device random secret in NVS | DONE       |
| `firmware_v4.ino` referenced `v4.0` banner     | Updated to v4.3.8 banner                                            | DONE       |
| PWA `package.json` at older version             | Bumped to 4.3.8                                                    | DONE       |
| Compat matrix showed v4.2 as "current"         | Updated to v4.3.8 current; v4.2.x demoted to "supported legacy"   | DONE       |
| PROTOCOL.md showed protocol v4 only            | Added protocol v5 row + evolution table                            | DONE       |
| Doc body sentences referenced "v4.2.0 had 4 KB buffer" | Rewritten as "firmware v4.1.0 had 4 KB buffer, v4.1.1+ has 16 KB — current v4.3.8 retains 16 KB" | DONE |

**Audit script**: `firmware/scripts/assert_version_contract.py` enforces
the title-line strict scan + body-scan (two-tier) on every commit.

---

## 8. Open Version-Related Defects

| Defect ID | Description                                                                   | Status | Notes                                                                                  |
| --------- | ----------------------------------------------------------------------------- | ------ | -------------------------------------------------------------------------------------- |
| VD-001    | OTA `OTAManager.h::OTA_MANIFEST_VERSION` constant declared but not yet referenced in OTA manifest JSON | OPEN   | Schema versioning scaffolded; bump on next OTA manifest schema change.                 |
| VD-002    | Doc body referenced stale `v4.2.0` as "current"                                | CLOSED | assert_version_contract.py now enforces.                                              |
| VD-003    | Config.h `JWT_SECRET_DEFAULT` compile-time backdoor                            | CLOSED | Removed in Phase I; ConfigStore generates random per-device secret.                  |
| VD-004    | OTA manifest schema not versioned in JSON payload                               | OPEN   | `OtaManager` reads `manifest.version` field but does not yet reject on mismatch.      |
| VD-005    | PWA `compatibility.ts` not yet created (manual contract only)                  | OPEN   | `test_pwa_protocol_gating.py` defines the contract in Python; TypeScript impl pending.|
| VD-006    | Mosquitto ACL not enforced (default broker = public HiveMQ)                    | CLOSED | `mosquitto_acl.conf` + Phase H directive; production deployment MUST use private broker. |
| VD-007    | Telemetry spool NVS persistence missing                                       | CLOSED | Phase D added `_loadCriticalFromNvs()` + compaction + power-loss safe atomic commit.  |

---

## 9. Phase B–K Closure Summary

| Phase | Description                                                         | Status | Evidence                                                                                          |
| ----- | ------------------------------------------------------------------- | ------ | ------------------------------------------------------------------------------------------------ |
| B     | PWA fetches AI insights from ESP32 authenticated `/api/insights`   | PASS   | `InsightsHandlers.h` + `HttpServer.cpp` route + `Advisor::fetchInsights()` + `api.ts::insights()` |
| C     | GAS canonical HMAC includes method prefix (`GET\n`/`POST\n`)        | PASS   | `Code.gs::doGet/doPost` + `Advisor.cpp::_buildAuthenticatedUrl`                                 |
| D     | TelemetrySpool NVS persistence (critical events survive reboot)     | PASS   | `TelemetrySpool.cpp::_loadCriticalFromNvs` + compaction; verified by `test_telemetry_spool_persistence.py` (7/7) |
| E     | TransactionJournal two-phase commit + magic + CRC32                 | PASS   | `TransactionJournal.h::BLOB_*` constants; `test_transaction_conflict_matrix.py` (4/4)            |
| F     | Public MQTT broker fail-closed (production refuses to connect)       | PASS   | `MqttClient.cpp` Phase 10 block (inside `PRODUCTION_BUILD` guard)                                |
| G     | OTA Ed25519 signature verification                                 | PASS   | `OtaManager.cpp` + `sign_firmware.py` + `test_ed25519_rfc8032_kat.py` (RFC 8032 KAT)             |
| H     | Mosquitto per-device ACL (`mosquitto_acl.conf`)                     | PASS   | `mosquitto_acl.conf` + `test_mqtt_acl_isolation.py` (9/9)                                        |
| I     | `JWT_SECRET_DEFAULT` removed; per-device NVS-generated secret      | PASS   | `Config.h` Phase I block; `ConfigStore.cpp` generates 32-byte random secret at first boot.       |
| J     | AI insights `advisoryOnly=true` enforcement                        | PASS   | `Code.gs::validateInsight()` + `aiInsights.ts::isValidInsight_()`; `test_ai_actuator_isolation.py` (5/5) |
| K     | PWA → ESP32 → GAS security chain (no direct browser→GAS)            | PASS   | `test_pwa_esp32_gas_security_chain.py` (7/7)                                                     |

---

## 10. Honest Disclosure — What Remains NOT EXECUTED — HARDWARE REQUIRED

The following tests are defined in the codebase (or by the directive) but
**cannot be executed in software-only CI** because they require physical
hardware or out-of-band infrastructure.

| Test ID | Description                                                              | Blocker                              | Hardware / infra required                              |
| ------- | ------------------------------------------------------------------------ | ------------------------------------ | ------------------------------------------------------ |
| HW-001  | Ed25519 known-answer test on real ESP32 (firmware-side verify)           | Requires ESP32 + signed binary       | ESP32-WROOM-32 + USB serial + signed `firmware.bin`    |
| HW-002  | Power-loss during OTA (12-case matrix — see directive P1-013)            | Requires ESP32 + controllable PSU    | ESP32 + bench PSU + GPIO-triggered reset               |
| HW-003  | Secure Boot V2 provisioning (eFuse burn)                                 | One-time, irreversible                | ESP32 + esptool `burn_efuse`                          |
| HW-004  | Flash Crash Test (write 1M cycles, verify integrity)                     | ~12 days continuous test              | ESP32 + automated runner                              |
| HW-005  | Relay contact arc suppression (12-channel stress, 230 VAC load)           | Real-world arc-suppression test       | 12 relays + 230 VAC load bank + oscilloscope           |
| HW-006  | MQTT TLS round-trip against production Mosquitto (8883 + ACL)            | Production Mosquitto deployment      | VPS with Mosquitto + Let's Encrypt + test client       |
| HW-007  | GAS HMAC round-trip against deployed GAS Web App                         | GAS Web App deployed + Script Props  | GAS project + `DEVICE_<id>_SECRET` script property    |
| HW-008  | Brownout detection (sag Vcc below 2.7 V, verify ESP32 reset cleanly)     | Variable bench PSU                   | ESP32 + bench PSU (e.g., RIGOL DP832)                  |
| HW-009  | PZEM-004T v3 calibration against known load (220 VAC, 5 A)               | Calibrated reference meter           | PZEM + reference AC meter + known load                 |
| HW-010  | DS3231 RTC drift over 30 days (≤ 2 min/year spec)                        | 30-day continuous test                | DS3231 + NTP reference + log collector                 |
| HW-011  | OTA rollback (factory partition boots when OTA fails to verify)          | ESP32 with OTA partitions flashed    | ESP32 + factory + OTA_A + OTA_B partitions              |
| HW-012  | 12-channel relay end-to-end (PWA → MQTT → ESP32 → relay contact closure) | 12-channel relay module              | 12-relay module + 230 VAC indicator loads              |

### Status of each HW test

- **NOT EXECUTED** by software CI (`firmware/scripts/*.py` + `pwa/scripts/*.py`).
- Will be executed in the field-deployment phase, by the on-site engineer.
- Each HW test has a documented pass/fail criterion in `firmware/TEST_PLAN.md`.
- Until ALL HW tests pass, the firmware must NOT be deployed to production
  230 VAC loads.

---

### Document control

| Field        | Value                                      |
| ------------ | ------------------------------------------ |
| Version      | 4.3.8                                      |
| Phase        | H (Blocker Closure Directive 2026-08-20)   |
| Authoritative| YES — single source of truth for versions  |
| Enforcement  | `firmware/scripts/assert_version_contract.py` (every commit) |

> When in doubt, the firmware `Config.h::FIRMWARE_VERSION` is the ultimate
> source of truth. All other version references MUST match it (or be
> version-agnostic).
