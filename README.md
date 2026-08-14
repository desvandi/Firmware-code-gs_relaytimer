# Timer Digital Relay v4.0 — Firmware + Google Apps Script

> ESP32-WROOM-32 firmware for **12-channel** relay control + 4 PIR sensors + DS3231 RTC + PZEM-004T v3.0 power meter, with NVS-persisted transaction journal, Ed25519-signed OTA, and Google Apps Script AI insights pipeline.

[![Firmware Version](https://img.shields.io/badge/firmware-v4.0.0-blue)](#)
[![Security Audit](https://img.shields.io/badge/audit-Cycle%208C%20Rev26-blue)](#security-audit-history)
[![ESP32 Core](https://img.shields.io/badge/ESP32%20core-3.3.7-green)](#)
[![License](https://img.shields.io/badge/license-proprietary-lightgrey)](#)

This repo holds the **device-side code** for the Timer Digital Relay v4.0 system. The companion PWA dashboard lives in a separate repo: **[desvandi/Remote-Relay](https://github.com/desvandi/Remote-Relay)**.

---

## Branch / Commit Identity (for audit traceability)

> **This section is normative for audit purposes.** All audit claims in this README
> are scoped to the branch and commit listed here. Any other branch/commit must
> be re-audited independently.

| Item | Value |
|------|-------|
| Audited branch | `engineering-cycle-8c-rev26-final-predicate` |
| **Current audited artifact** (submitted for final Phase-1 gate) | `589e9a1` — Closure-F traceability fix + TOC cleanup + Closure-G (this commit) |
| Immediate parent (Phase 1 documentation closure) | `9fd7473` — Closure-C/D/E/F + Phase 2/3 scope contracts |
| Phase 1 implementation baseline (P1-1 strict serializer + P1-2 host test harness) | `c506c80` — 102/102 host tests PASS |
| Phase 1 initial implementation | `2e4de87` — JournalRecord implementation per Rev26 |
| Normative design contract | [`docs/CYCLE-8C-REV26-FINAL-PREDICATE.md`](docs/CYCLE-8C-REV26-FINAL-PREDICATE.md) |
| Foundational record contract | [`docs/CYCLE-8C-REV14-MUTATION-CONSOLIDATION.md`](docs/CYCLE-8C-REV14-MUTATION-CONSOLIDATION.md) (consolidated by Rev26) |
| Phase 2 scope contract | [`docs/PHASE-2-SCOPE.md`](docs/PHASE-2-SCOPE.md) — **not yet authorized to start** |

> **Traceability model (Closure-G, auditor 2026-08-14):**
> The current audited artifact is `589e9a1`. Its immediate parent `9fd7473`
> is the Phase 1 documentation closure (Closure-C/D/E/F). `c506c80` is the
> Phase 1 implementation baseline (P1-1 + P1-2 — the strict serializer fix
> + host test harness that achieved 102/102 PASS). `2e4de87` is the Phase 1
> initial JournalRecord implementation.
>
> Every commit in this chain may be referenced as a historical artefact
> (parent, baseline, prior audit submission), but only `589e9a1` is the
> current audited artefact. Any approval issued against `589e9a1` does NOT
> transitively apply to other commits in the chain unless explicitly
> re-scoped by the auditor.

### Phase gate status (as of 2026-08-14)

| Phase | Status | Notes |
|-------|--------|-------|
| Phase 1 — JournalRecord foundation | 🟡 **NOT YET APPROVED** | Implementation reviewed + host test 102/102 PASS. Auditor returned NO-GO pending Closure-C/D/E/F (documentation alignment, channel architecture statement, known-limitations disclosure, branch/commit traceability). |
| Phase 2 — TransactionJournal Rev26 + command integration + recovery | 🔴 **NO-GO / NOT AUTHORIZED** | Scope document exists ([`docs/PHASE-2-SCOPE.md`](docs/PHASE-2-SCOPE.md)). Engineering may not start until Phase 1 is approved AND auditor explicitly authorizes Phase 2. |
| Phase 3 — 16-channel hardware/architecture migration | 🔴 **NOT AUTHORIZED** | Deferred until Phase 2 done + audited. I/O expander architecture = TBD (no device committed — see "Channel Architecture" below). |
| 220V production | 🔴 **NOT AUTHORIZED** | Requires: Phase 2 done + audited; Phase 3 done + audited; 12 power-loss tests PASS on actual 16-channel ESP32 hardware; Ed25519 PSA runtime verification. |

> **Auditor principle (formalized 2026-08-14):** Approval is granted only after
> artefacts themselves demonstrate that requirements are met — not after
> engineering states that work "has been done". This applies to every gate below.

---

## Channel Architecture: Current vs Target

> **Closure-D (auditor Rev26 Phase-1 review).** This section exists to remove
> implicit ambiguity about channel count. The audited firmware is 12-channel;
> the production target is 16-channel; these are NOT the same artifact.

**Current audited firmware (branch `engineering-cycle-8c-rev26-final-predicate`):**
- `NUM_CHANNELS = 12` (see `firmware/Config.h` line 20)
- `RELAY_PINS[12]` = `{13, 14, 16, 17, 18, 19, 21, 22, 23, 25, 26, 27}` (direct GPIO drive, active-LOW)
- 4 PIR sensors on input-only GPIOs (34, 35, 36, 39)
- DS3231 RTC on I2C (SDA=32, SCL=33)
- PZEM-004T v3.0 on UART2 (GPIO 4/5)
- **No I/O expander** — all 12 relays are driven directly by ESP32 GPIOs

**Production target (NOT YET IMPLEMENTED — Phase 3):**
- 16-channel relay
- Requires I/O expander (4 additional channels beyond ESP32's direct-drive capability)
- **I/O expansion architecture: TBD / requires engineering decision and hardware review.**
  No specific device (PCF8575, MCP23017, or other) is committed. The choice
  must be justified against:
  - output state at boot (default HIGH/LOW, fail-safe)
  - failure behavior on bus loss
  - reset behavior (does expander retain state across ESP32 reset?)
  - address configuration (I2C address conflicts)
  - electrical compatibility (3.3V vs 5V logic, sink/source current)
  - relay-driver interface (active-LOW/active-HIGH, opto-isolation)
  - fail-safe requirement (relay must default to OFF when expander is unresponsive)
- Requires `RelayDriver.cpp` migration — no work started
- Requires `Config.h` `NUM_CHANNELS = 16` + `RELAY_PINS[16]` update
- Requires PWA channel mapping update — no work started
- Requires auditor approval for "Phase 3 — 16-channel migration" before work begins

**Implication for audit:**
- Phase 1 (JournalRecord) is channel-agnostic — `channelId` is a single byte
  in the canonical payload and is not interpreted by the record layer.
- The current audited artifact is a **12-channel firmware**.
- Any 220V production deployment must use the **16-channel target**, which
  does not exist yet. Therefore 220V production is BLOCKED until Phase 3
  (16-channel migration) is complete and separately audited.

---

## Table of Contents

1. [Repository Layout](#repository-layout)
2. [Architecture Overview](#architecture-overview)
3. [Security Architecture (Cycle 8C — Rev26 normative)](#security-architecture-cycle-8c--rev26-normative)
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
│   ├── ── Transaction Layer (Cycle 8C — Rev26 foundation) ──
│   ├── JournalRecord.h                ← Record struct + serialization API (Phase 1)
│   ├── JournalRecord.cpp              ← serialize/deserialize/CRC/canonicalEqual/classifyGeneration
│   ├── JournalRecordTest.cpp (test/host/) ← host test harness (102/102 PASS, compiles real JournalRecord.cpp)
│   ├── TransactionJournal.h           ← NVS-persisted journal (PRE-Rev26 — will be rewritten in Phase 2)
│   ├── TransactionJournal.cpp         ← Pre-Rev26 two-phase commit (tj_entry_N + tj_commit_N) — DO NOT USE as Rev26 reference
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
   PIR 1-4 ─── GPIO 34-39 │  JournalRecord (Phase 1 — Rev26 foundation) │
   DS3231 ──── I2C (32,33) │  TransactionJournal (PRE-Rev26, Phase 2 will rewrite) │
   PZEM-004T ─ UART2 (4,5) │  MqttClient (TLS, ACK, Ed25519 OTA)    │
                          │  AuthManager (JWT 15min + refresh 7d)  │
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

## Security Architecture (Cycle 8C — Rev26 normative)

> **Closure-C (auditor Rev26 Phase-1 review).** The previous version of this
> section described a pre-Rev26 architecture (`two-phase commit` +
> `tj_entry_N`/`tj_commit_N` flag pairs, `[magic:2][version:1][valid:1][CRC32:4][payload]`
> byte layout, "12 rounds of security audit"). That description was
> inconsistent with the actual Rev26 implementation in
> `firmware/JournalRecord.h` and the normative design contract
> `docs/CYCLE-8C-REV26-FINAL-PREDICATE.md`.
>
> This section is now the **single source of truth** for the audited firmware's
> security architecture. Anywhere a stale description appears elsewhere in the
> repository (e.g. historical `docs/CYCLE-*` superseded banners, code comments
> referencing `R10G-R10K`), Rev26 + this section take precedence.

This firmware has been through **12 rounds of security audit (R9 → R10K) +
the Cycle 8C design series (Rev1 → Rev26, 26 design revisions)** by an external
auditor. The Cycle 8C series is normative; R9–R10K findings are subsumed by
Rev26 where they touch the transaction journal.

### Transaction Journal Architecture (Rev26 normative)

The transaction journal is in the middle of a phased migration:

| Phase | Status | What it implements |
|-------|--------|--------------------|
| **Phase 1 — `JournalRecord` foundation** | 🟡 Implemented + host-tested (102/102 PASS), auditor re-review pending | Record primitive: serialize/deserialize, CRC-32/ISO-HDLC, canonical equivalence, generation ordering. Channel-agnostic — `channelId` is an opaque byte. |
| **Phase 2 — `TransactionJournal` Rev26** | 🔴 NOT YET STARTED — see [`docs/PHASE-2-SCOPE.md`](docs/PHASE-2-SCOPE.md) | Dual-copy persistence, ObservationGuard, mutation enforcement, 9-row recovery table, ACK lifecycle separation, command execution integration. |
| **Phase 3 — 16-channel migration** | 🔴 NOT YET STARTED | I/O expander, RelayDriver migration, channel architecture. |

**IMPORTANT:** The current `firmware/TransactionJournal.cpp` is a **pre-Rev26
implementation** (still uses the old `tj_entry_N` + `tj_commit_N` two-phase
commit model with a 1-byte commit flag). It is **NOT** the Rev26 dual-copy
architecture. It will be rewritten in Phase 2. Until Phase 2 is complete and
audited, the journal layer is NOT considered production-safe.

#### JournalRecord byte layout (Rev26 normative — single source of truth)

```
Offset  Field              Size  Description
------  ----------------   ----  ------------------------------------------
0       magic              2     0x54, 0x4A ("TJ")
2       schemaVersion      1     4 (JOURNAL_SCHEMA_VERSION)
3       generation         4     uint32 LE, wrap-safe serial number
7       recordCRC          4     CRC-32/ISO-HDLC over bytes 0..6 + bytes 11..end
--- CANONICAL PAYLOAD (byte 11 onward) ---
11      recordState        1     EMPTY/PENDING/EXECUTING/COMMITTED/...
12      requestIdLen       1     0..64
13..    requestId          var
..      commandHashLen     1     0..64
..      commandHash        var
..      channelId          1     0=N/A, 1..NUM_CHANNELS
..      desiredState       1     0=OFF, 1=ON, 0xFF=N/A
..      previousKnownState 1     0=OFF, 1=ON
..      attempt            1
..      timestamp          4     uint32 LE
..      ackLen             2     uint16 LE, 0..1024
..      ackJson            var
..      (padding to BLOB_SIZE=1200, zeros, NO semantic meaning)
```

Header size = **11 bytes** (`BLOB_HEADER_SIZE`). The previous README
description of `[magic:2][version:1][valid:1][CRC32:4][payload]` (8-byte
header with a 1-byte `valid` flag) is **OBSOLETE** — it describes a pre-Rev26
concept that was replaced by `schemaVersion` + `generation` (5 bytes
combined) to support wrap-safe serial arithmetic and canonical equivalence.

The implementation lives in:
- `firmware/JournalRecord.h` — record struct + API contract
- `firmware/JournalRecord.cpp` — serialize/deserialize/CRC/canonicalEqual/classifyGeneration
- `firmware/test/host/JournalRecordTest.cpp` — host-side Phase 1 verification (102/102 PASS)

#### CRC contract (Rev26 normative)

- **Algorithm:** CRC-32/ISO-HDLC (reflected, poly 0xEDB88320, init 0xFFFFFFFF, final XOR 0xFFFFFFFF)
- **API:** `~esp_crc32_le(0xFFFFFFFF, data, len) & 0xFFFFFFFF`
- **CRC INPUT:** bytes `[0..6]` (header) concatenated with bytes `[11..actualPayloadEnd]` (canonical payload)
- **CRC does NOT cover:** bytes `[7..10]` (the CRC field itself), padding bytes
- **Test vector:** `"123456789"` → `0xCBF43926` (mandatory KAT, verified by host test)

#### Canonical equivalence (Rev26 normative)

`canonicalEqual(A, B)` ≡
1. `A.schemaVersion == B.schemaVersion`
2. `AND A.canonicalLength == B.canonicalLength`
3. `AND memcmp(A.canonicalBytes, B.canonicalBytes, A.canonicalLength) == 0`

Where `canonicalBytes` = bytes starting at `recordState` (byte 11), and
`canonicalLength` = actual payload length derived from safe parse (excludes
padding). Schema version is checked separately because it lives in the
header, not the canonical payload.

#### Generation ordering (Rev26 normative, wrap-safe serial arithmetic)

```
distAB = (uint32_t)(genB - genA)    // forward distance A→B
distBA = (uint32_t)(genA - genB)    // forward distance B→A

if genA == genB                    → GEN_EQUAL    (verify canonicalEqual)
else if distAB == 1                → GEN_NEWER_B  (B is 1 newer than A)
else if distBA == 1                → GEN_NEWER_A  (A is 1 newer than B)
else if distAB == 0x80000000       → GEN_AMBIGUOUS → CORRUPTED
else                               → GEN_INVALID  → CORRUPTED
```

#### Phase 2 will add (NOT YET IMPLEMENTED)

When Phase 2 is authorized, `TransactionJournal.cpp` will be rewritten to
implement the Rev26 normative contract:

- **Dual-copy persistence** — each journal slot stores two copies of the
  record; recovery uses the 9-row decision table (see
  `docs/CYCLE-8C-REV14-MUTATION-CONSOLIDATION.md` §I1 "Recovery Decision
  Table") to pick authoritative copy or quarantine.
- **ObservationGuard** (RAII) + `_assertMutationAllowed()` — observation
  and mutation are mutually exclusive (runtime-enforced, panic on violation).
- **Mutation enforcement** — every mutation API calls `_assertExecutorContext()`
  + `_assertMutationAllowed()` at entry.
- **9-row recovery decision table** — recovery picks VALID copy, repairs
  INVALID copy bitwise, or quarantines slot when both INVALID.
- **ACK lifecycle separation** — transaction lifecycle independent of ACK
  delivery lifecycle; ACK queue (`tj_ackq`) persists separately and survives
  eviction of the originating journal entry.
- **Eviction safety (I2)** — 5 conjunctive predicates (I2a–I2e); default
  is RETAIN when any check is uncertain.
- **Generation construction** — protocol produces distance 0 (same/repair)
  or 1 (adjacent mutation); loader validates distance is 0 or 1, else
  CORRUPTED.

### MQTT Security (R10A–R10F — still in force, not superseded by Rev26)

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
| R10G–R10K | Pre-Rev26 transaction journal (tj_entry_N + tj_commit_N two-phase commit) | **SUPERSEDED by Cycle 8C** — pre-Rev26 `TransactionJournal.cpp` is still in repo but will be rewritten in Phase 2 |
| Cycle 8A | Transaction recovery (TransactionState state machine, GPIO readback reconciliation) | Pre-Rev26 transaction recovery |
| Cycle 8B | Boot recovery phase (PRE_INIT→SAFE_INIT→SNAPSHOT→RECONCILING→RESTORING→RUNNING) | Pre-Rev26 boot recovery |
| Cycle 8B-Rev1 | `_createPendingEntryNVS()` + `_commitExecutingEntryNVS()` split + monotonicity validator | Pre-Rev26 monotonicity |
| Cycle 8C-Rev1 → Rev13 | Dual-copy design, canonical equivalence, formal invariants, ACK lifecycle, recovery observation | Iterative design |
| Cycle 8C-Rev14 | Mutation enforcement + full consolidation (SOLE normative document for JournalRecord byte layout, CRC contract, canonical equivalence, generation ordering, recovery decision table, ACK lifecycle, eviction matrix) | **Foundation contract for Phase 1 implementation** |
| Cycle 8C-Rev15 → Rev25 | ACK transition, auth regression, auth evidence lifetime, authz boundary, verification boundary, auth evidence normalization | Iterative design refinement |
| Cycle 8C-Rev26 | Final eviction predicate (I2a–I2e + auth gate), DEPLOYMENT_AUTH_CONFIGURED vs AUTH_EVIDENCE_AUTHENTICATED separation, complete drive-out predicate | **CURRENT NORMATIVE DESIGN — Phase 1 implementation target** |
| Phase 1 — initial implementation (`2e4de87`) | JournalRecord implementation: serialize/deserialize, CRC-32/ISO-HDLC, canonicalEqual, classifyGeneration | Phase 1 code baseline |
| Phase 1 — closure (`c506c80`) | P1-1 (strict serializer — reject over-limit input) + P1-2 (host test harness, 102/102 PASS) | Phase 1 implementation baseline |
| Phase 1 — documentation closure (`9fd7473`) | Closure-C/D/E/F + Phase 2/3 scope contracts | Phase 1 documentation closure (immediate parent of current audited artifact) |
| Phase 1 — traceability closure (`589e9a1`) | Closure-F traceability fix + TOC cleanup + Closure-G (this commit) | **CURRENT AUDITED ARTEFACT — submitted for final Phase-1 gate** |
| Phase 2 | TransactionJournal Rev26 rewrite + command integration + recovery semantics | **NOT AUTHORIZED** — see [`docs/PHASE-2-SCOPE.md`](docs/PHASE-2-SCOPE.md) |
| Phase 3 | 16-channel migration (I/O expander architecture TBD) | **NOT AUTHORIZED** |
| 220V production | Hardware acceptance: 12 power-loss tests + Ed25519 runtime verification | **NOT AUTHORIZED** |

---

## Known Limitations (as of Rev26 Phase 1 audit — 2026-08-14)

> **Closure-E (auditor Rev26 Phase-1 review).** These are limitations of the
> audited firmware as of current audited artefact `589e9a1` on branch
> `engineering-cycle-8c-rev26-final-predicate` (immediate parent:
> `9fd7473` Phase 1 documentation closure; Phase 1 implementation baseline
> `c506c80`; initial implementation `2e4de87`). They are stated explicitly
> (not disguised as features) per auditor requirement.

1. **Browser credential exposure (P1 — accepted tradeoff, NOT a secret from
   the browser user):** `NEXT_PUBLIC_MQTT_PASSWORD` is exposed to the PWA
   browser. Compromise of a single device's browser credential = compromise
   of THAT device only (broker ACL scopes to `timer12/<mac>/#`).
   This is an accepted tradeoff of the threat model. The browser user can
   technically extract the credential from their own client — it is NOT a
   secret from them. For multi-tenant deployments with mutually-distrusting
   users, per-browser credentials (or broker-issued JWT tokens) would be
   required — this is OUT OF SCOPE and must be flagged as a separate
   security phase if needed.

2. **LRU journal = 64 entries**: After eviction, old requestIds can be
   re-executed. At 100 commands/day, oldest entry is ~15 hours old when
   evicted (PWA retry window is ~2 min, so 15h ≫ 2min). Non-idempotent
   commands (OTA, factory reset, future precharge) must rely on additional
   guards (version monotonicity, signature, etc.), NOT journal retention
   alone. **Rev26 eviction safety (I2a–I2e) governs eviction decisions —
   implemented in Phase 2.**

3. **Execute→store gap (acknowledged, partially closed by Rev26 design):**
   Cycle-7 finding I-004 documents that OTA commands bypass the
   intent-first journal pattern. Rev26's intent-first design (storeIntent →
   execute → commitTransaction) closes this for relay commands in Phase 2.
   OTA path migration to intent-first is **Phase 2 work**, not yet started.

4. **Ed25519 PSA Crypto verification status**: Not yet verified on
   Arduino IDE 2.3.8 + ESP32 core 3.3.7 combination. The
   `-DMBEDTLS_ED25519_SUPPORTED` flag is documented in `platformio.ini` but
   requires a framework rebuild with
   `CONFIG_MBEDTLS_ECP_DP_ED25519_ENABLED=y` in sdkconfig. Without it,
   `ed25519VerifyHash()` returns false (fail-closed) and MQTT OTA is
   rejected. **Status: verified only at compile-time, not at runtime.**
   Phase 3+ hardware acceptance must include Ed25519 KAT on actual ESP32.

5. **NVS wear**: Estimated 3–5 years at 100 commands/day (64 entries ×
   ~5 NVS writes per transaction = ~320 writes per full journal cycle).
   After that, NVS page compaction may begin to fail. Rev14 recommends
   LittleFS migration for large blobs — NOT YET IMPLEMENTED.
   **Status: documented estimate, not measured.**

6. **12-channel vs 16-channel**: See "Channel Architecture" section above.
   Current audited firmware is **12-channel**. 16-channel migration is
   **Phase 3**, requiring separate audit. I/O expansion architecture is
   **TBD** — no device committed.

7. **Default development build is insecure**: HiveMQ public broker, no
   auth, CORS `*`. NEVER flash development build to a device controlling
   220V mains. Production build (`-DPRODUCTION_BUILD`) is mandatory for
   any real deployment.

8. **No Flash Encryption / Secure Boot**: NVS data (JWT secret, refresh
   tokens, MQTT password, GAS secret) is stored as plaintext in flash.
   An attacker with physical access (UART, flash dump) can extract these
   secrets. For high-security deployments, enable ESP32 Flash Encryption +
   Secure Boot (see [Physical Security Hardening](#physical-security-hardening-flash-encryption--secure-boot) below).

9. **Pre-Rev26 TransactionJournal.cpp still active**: The current
   `firmware/TransactionJournal.cpp` uses the old `tj_entry_N` +
   `tj_commit_N` two-phase commit model, NOT the Rev26 dual-copy
   architecture. It will be rewritten in Phase 2. Until then, the journal
   layer is **NOT considered production-safe** — only `JournalRecord`
   (the Phase 1 foundation) has been audited.

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
