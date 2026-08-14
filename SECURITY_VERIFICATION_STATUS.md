# Security Verification Status

> **audit-fixes-v2 final status (after auditor #1-#6 review cycle)**
>
> This document uses the **3-tier verification framework** proposed by auditor #6
> to distinguish between "source fixed", "build verified", and "runtime proven".
> A finding marked FIXED-SOURCE means the code is correct; it does NOT mean the
> security property has been proven on actual hardware.

---

## Verification Tier Definitions

| Tier | Meaning | Evidence Required |
|------|---------|-------------------|
| 🔧 **FIXED-SOURCE** | Code has been modified to address the finding | Diff in `audit-fixes` / `audit-fixes-v2` branches, now in `main` |
| 🔨 **VERIFIED-BUILD** | Source compiles + lints + builds clean | `pio run -e production` SUCCESS, `bun run lint` PASS, `bun run build` SUCCESS |
| ⚡ **VERIFIED-RUNTIME** | Behavior proven on actual ESP32 hardware + broker + GAS | Integration test report (NOT YET DONE — see Hardware Test Matrix below) |

**Critical rule**: A finding marked FIXED-SOURCE + VERIFIED-BUILD is NOT the same as "production-ready". For a system controlling 220V mains with OTA + persistent journal, runtime verification on actual hardware is the final gate before production deployment.

---

## Status Matrix (audit target: `origin/engineering-cycle-8c-rev26-final-predicate` — SHA resolved externally by auditor per "Audit Traceability Rule" in README.md)

> **Closure-C / Closure-G update (auditor Rev26 Phase-1 review, 2026-08-14):**
> This matrix reflects the pre-Rev26 security audit (R9–R10K). The Cycle 8C
> design series (Rev1 → Rev26) supersedes R10G–R10K findings that touch
> the transaction journal. Phase 1 (`JournalRecord` foundation, implementation
> baseline `c506c80`) is implemented and host-tested (102/102 PASS) but
> NOT YET APPROVED by auditor.
>
> Documentation closure history:
>   - `9fd7473` — Closure-C/D/E/F + Phase 2/3 scope contracts (historical).
>   - `589e9a1` — Closure-F traceability fix + TOC cleanup (rejected by
>     auditor: self-referential SHA claim).
>   - `8bfb036` — Closure-G attempt 2 (rejected by auditor: still
>     self-referential).
>   - Final traceability correction (this commit series) — adopted Option A
>     model: audit target = branch ref, not embedded SHA. See "Audit
>     Traceability Rule" in [`README.md`](README.md).
>
> Phase 2 (`TransactionJournal` Rev26 rewrite) is NOT AUTHORIZED. See
> [`README.md`](README.md) for the authoritative phase gate status.

### 🔴 P0 Findings

| ID | Finding | Tier | Status |
|----|---------|------|--------|
| P0-1 | REST OTA upload unauthenticated | 🔧 FIXED-SOURCE + 🔨 VERIFIED-BUILD | `OtaHandlers.h` — `handleOtaUpload()` returns 403 in PRODUCTION_BUILD. ⚡ Runtime test pending (REST OTA must return 403 on real device). |
| P0-2 | `NEXT_PUBLIC_MQTT_PASSWORD` browser-exposed | 🟠 UNRESOLVED (architectural) | Per-device broker ACL (in firmware README) limits blast radius. Full fix requires server-side MQTT proxy — documented as Future Work. NOT a code fix. |

### 🟠 P1 Findings

| ID | Finding | Tier | Status |
|----|---------|------|--------|
| P1-1 | `/api/channel` missing in firmware REST | 🔧 FIXED-SOURCE + 🔨 VERIFIED-BUILD | `ChannelHandlers.h` + route registered. ⚡ Contract test pending (POST /api/channel → state persists → MQTT status reflects rename → PWA cache updates). |
| P1-2 | Schedule ID semantics REST/MQTT mismatch | 🔧 FIXED-SOURCE + 🔨 VERIFIED-BUILD (partial) | `ScheduleHandlers.h` DELETE accepts composite + per-channel + legacy. 🟠 **Auditor #6 recommendation**: deprecate legacy global-sequential ID, create canonical `resolveSchedule(channelId, scheduleId)` helper used by ALL paths. PENDING. |
| P1-3 | REST toggle non-idempotent | 🔧 FIXED-SOURCE + 🔨 VERIFIED-BUILD | `RelayHandlers.h` rejects `toggle` with 400. ⚡ Runtime test pending (toggle must return 400). |
| P1-4 | Refresh token no server-side expiry | 🔧 FIXED-SOURCE + 🔨 VERIFIED-BUILD | `AuthManager.cpp` format `<token>.<issuedAt>`, rejects tokens older than 7 days. ⚡ Boundary tests pending (now-7d±1sec, malformed timestamp, future timestamp). |
| P1-5 | CSRF not regenerated on refresh | 🔧 FIXED-SOURCE + 🔨 VERIFIED-BUILD | `AuthManager.cpp:306` calls `generateCsrfToken()` before `getCsrfToken()`. ⚡ Critical test pending: **old CSRF + new access token → 403** (not just new CSRF → 200). |
| P1-6 | Default REST password MAC-derived | 🔧 FIXED-SOURCE + 🔨 VERIFIED-BUILD | `ConfigStore.cpp` uses `esp_random()` CSPRNG. 🟠 **Auditor #6 note**: provisioning design now needs secure initial credential path (not Serial.print in production). PENDING. |
| P1-7 | REST JSON string concatenation | 🟠 UNRESOLVED (technical debt) | All firmware REST responses still use `String("{\"...")` concatenation. Channel names with quotes/backslashes can produce invalid JSON. Auditor #6 recommendation: refactor endpoint-by-endpoint to ArduinoJson. PENDING (backlog). |
| P1-8 | Device ACL too broad in README | 🔧 FIXED-SOURCE | README already has per-topic ACL pattern (read command, write status, etc.). |
| P1-9 | GAS GET insights uses device ID as authorization | 🟠 UNRESOLVED (architectural) | `doGet()` accepts `?mac=<anonymousId>` without HMAC. Insights may reveal occupancy patterns. Auditor #6 recommendation: revisit threat model — if telemetry is private, add HMAC to GET. PENDING (architectural). |
| P1-10 | PWA hardcoded `signatureVerified: true` | 🔧 FIXED-SOURCE + 🔨 VERIFIED-BUILD | `useApi.ts` returns `null` for unverified fields in MQTT mode. |
| P1-11 | OTA version source hardcoded | 🟠 UNRESOLVED (release engineering) | `LATEST_VERSION = "4.0.0"` in firmware + PWA. Auditor #6 recommendation: signed OTA manifest (version + size + sha256 + signature + url). PENDING (backlog). |

### 🟡 P2 Findings

| ID | Finding | Tier | Status |
|----|---------|------|--------|
| P2-1 | PWA MQTT status/log runtime schema not validated | 🟠 UNRESOLVED | `mqtt.ts` does `JSON.parse(msg) as SystemStatus` without runtime validation. Validator in `mqttTransaction.ts` catches invalid ACKs for pending commands, but status/log callbacks receive unvalidated objects. PENDING (backlog). |
| P2-2 | Journal "permanent" wording incorrect | 🟠 DOCUMENTATION | Source says "requestId is permanent" but journal is 64-entry ring with eviction. Should say "durable until journal eviction". PENDING (wording fix). |
| P2-3 | Refresh token "LRU" is actually FIFO | 🟠 DOCUMENTATION | `MAX_REFRESH_TOKENS=4` with slot-0 eviction is FIFO, not LRU. Documentation should reflect this. PENDING (wording fix). |
| P2-4 | Login rate limiter IP-only | 🟠 UNRESOLVED | 8 IP slots, attacker with many IPs can cause eviction. Auditor #6 classifies as P2 hardening. PENDING (backlog). |
| P2-5 | GAS rate limit best-effort CacheService | 🟠 DOCUMENTATION | CacheService is not durable. Should be documented as "best-effort abuse protection" not "strict quota". PENDING (wording fix). |
| P2-6 | Development config permissive | 🔧 FIXED-SOURCE | `PRODUCTION_BUILD` flag fail-closed mitigates. Default Config.h is dev-only. |

---

## ✅ Areas Auditor #6 Confirmed Strong (DO NOT REFACTOR)

These areas passed static review across all 6 auditors. Do not refactor them "for cleanup" — they are working correctly.

- **MQTT topic contract**: `timer12/<deviceId>/{command,status,ack,log,online,ota}` — PWA ↔ firmware consistent, no password in topic
- **MQTT ACK transaction**: requestId + timeout + ACK validation + pending cleanup
- **MQTT command validation pipeline**: parse → type → unknown-field rejection → requestId → hash → journal lookup → execute
- **TransactionJournal**: 64-entry ring + CRC32 + magic + two-phase commit + ACK retry queue — **NOTE**: This describes the PRE-Rev26 `TransactionJournal.cpp` still in the repo. The Rev26 normative design (`docs/CYCLE-8C-REV26-FINAL-PREDICATE.md`) replaces two-phase commit with dual-copy + generation ordering + canonical equivalence. Rev26 implementation is Phase 2 work (NOT YET STARTED — see [`docs/PHASE-2-SCOPE.md`](docs/PHASE-2-SCOPE.md)). Phase 1 `JournalRecord` foundation is implemented + host-tested (102/102 PASS).
- **MQTT TLS production guard**: fail-closed on TLS/auth/CA/CORS/OTA pubkey
- **MQTT OTA cryptographic chain**: HTTPS → CA → host allowlist → size → SHA-256 → Ed25519 → anti-downgrade → install → rollback
- **Code.gs HMAC**: LockService + nonce + timestamp + constant-time comparison
- **Gemini prompt isolation**: `<UNTRUSTED_DATA>` wrappers + sanitizeForPrompt + output schema validation
- **REST JWT/CSRF foundation**: 15min access + 7day refresh rotation + HttpOnly/Secure cookies + CSRF double-submit

---

## ⚡ Hardware Test Matrix (REQUIRED before production deployment)

> Auditor #6: "After PR #3 merge, do NOT do another source audit round. The highest ROI now is hardware testing."

### Authentication Tests

| Test | Expected | Status |
|------|----------|--------|
| No JWT cookie | 401 | ⏳ Pending |
| Expired JWT (>15min) | 401 | ⏳ Pending |
| Invalid JWT signature | 401 | ⏳ Pending |
| Wrong CSRF token | 403 | ⏳ Pending |
| **Old CSRF + new access token (post-refresh)** | **403** | ⏳ Critical — verifies P1-5 fix |
| New CSRF + new access token (post-refresh) | 200 | ⏳ Pending |
| Refresh token expired (>7 days) | 401 + cookie cleared | ⏳ Critical — verifies P1-4 fix |
| Refresh token replay (reuse rotated token) | 401 + security log | ⏳ Pending |
| Malformed issuedAt timestamp in NVS | 401 (treated as expired) | ⏳ Critical — verifies P1-4 boundary |
| Future issuedAt timestamp | 401 (reject) | ⏳ Critical — verifies P1-4 boundary |

### OTA Tests

| Test | Expected | Status |
|------|----------|--------|
| REST OTA upload in PRODUCTION_BUILD | 403 | ⏳ Critical — verifies P0-1 fix |
| Valid MQTT OTA (signed) | Success + reboot | ⏳ Pending |
| Wrong SHA-256 | Reject + log | ⏳ Pending |
| Wrong Ed25519 signature | Reject + log | ⏳ Pending |
| Unallowed host (not in OTA_ALLOWED_HOSTS) | Reject + log | ⏳ Pending |
| HTTP instead of HTTPS | Reject | ⏳ Pending |
| Version downgrade (4.1.0 → 4.0.0) | Reject | ⏳ Pending |
| Corrupted OTA (boot fails 3x) | Rollback to previous | ⏳ Pending |

### MQTT Tests

| Test | Expected | Status |
|------|----------|--------|
| Normal command | 1 execution + 1 ACK | ⏳ Pending |
| Duplicate requestId (same hash) | ACK replay, 0 extra execution | ⏳ Pending |
| Same requestId + changed payload | Reject as security violation | ⏳ Pending |
| Timeout + retry | Exactly-once semantic | ⏳ Pending |
| Reconnect | Pending transactions cleaned | ⏳ Pending |
| Stale ACK (no matching pending) | Ignored | ⏳ Pending |

### REST Contract Tests

| Test | Expected | Status |
|------|----------|--------|
| POST /api/channel (rename) | 200 + state persists + MQTT status reflects new name | ⏳ Critical — verifies P1-1 fix |
| POST /api/relay action=toggle | 400 | ⏳ Critical — verifies P1-3 fix |
| DELETE /api/schedule?channelId=3&id=2 | 200 + schedule deleted | ⏳ Critical — verifies P1-2 preferred format |
| DELETE /api/schedule?id=32 (composite) | 200 + same schedule deleted | ⏳ Verifies P1-2 composite format |
| DELETE /api/schedule?id=5 (legacy) | 200 (backward compat) | ⏳ Verifies P1-2 legacy format |

### Power-Loss Tests (CRITICAL for 220V deployment)

> Auditor #6: "This is very important. Do this multiple times at different timings."

| Test | Expected | Status |
|------|----------|--------|
| Command → journal write → **CUT POWER** → reboot → same requestId | Original command must NOT execute twice | ⏳ Critical |
| Command → execute → **CUT POWER before journal** → reboot → retry | Idempotent commands safe (relay on/off). Non-idempotent may duplicate (capped at 4/channel). | ⏳ Accepted risk |
| OTA download → **CUT POWER mid-download** → reboot | Update.abort(), no partial install | ⏳ Pending |
| OTA install → **CUT POWER before markBootHealthy** → reboot | Boot attempts increment, rollback after 3 | ⏳ Pending |

### GAS HMAC Tests

| Test | Expected | Status |
|------|----------|--------|
| Valid HMAC POST | 200 + insights generated | ⏳ Pending |
| Wrong HMAC signature | 401 | ⏳ Pending |
| Old timestamp (>5min) | 401 | ⏳ Pending |
| Reused nonce | 401 (replay detected) | ⏳ Pending |
| Modified body (HMAC mismatch) | 401 | ⏳ Pending |
| Wrong deviceId in body vs query param | 401 | ⏳ Pending |
| Rate limit (>10 POSTs/hour/device) | 429 | ⏳ Pending |

---

## 🟠 Backlog (Architectural / Future Work)

These items are NOT blocking production deployment (after hardware tests pass), but should be addressed for long-term security posture.

### 1. P0-2: Server-side MQTT proxy (architectural)
Replace `NEXT_PUBLIC_MQTT_PASSWORD` with a backend that issues short-lived, scoped broker credentials after user authentication. See PWA README "Security Architecture" section.

### 2. P1-2: Canonical contract sync (recommended by auditor #5 + #6)
Create `contracts/` directory with JSON schemas, or shared `@remote-relay/contracts` package. CI cross-repo verification. This would have caught P1-1 (/api/channel missing) and P1-2 (schedule ID mismatch) automatically.

### 3. P1-7: REST JSON serialization refactor
Migrate firmware REST responses from `String("{\"...")` concatenation to `ArduinoJson` `JsonDocument` + `serializeJson()`. Do endpoint-by-endpoint: auth → channel → schedule → relay → config → status.

### 4. P1-9: GAS GET insights authorization
Revisit threat model. If telemetry (relay usage, PIR activity, time-of-use patterns) is considered private, add HMAC to GET endpoint. If public, document `deviceId ≠ authorization credential` explicitly.

### 5. P1-11: Signed OTA manifest
Create signed manifest: `{version, size, sha256, signature, url}`. Both PWA and firmware verify manifest signature before download. Eliminates `LATEST_VERSION` hardcoding.

### 6. P1-6: Secure provisioning path
Random default password (now CSPRNG) needs a secure initial credential delivery mechanism. Options: factory provisioning script, QR code on device label, secure element (ATECC608A).

### 7. P2-1: PWA runtime schema validation
Add `parseStatus()`, `parseActivityLog()`, `parseAck()` schema validators before callbacks. Defense-in-depth against malformed MQTT payloads.

### 8. P2-4: Distributed rate limiting
PWA `/api/login` rate limiter is in-memory (per-instance on Vercel serverless). For production with >1 instance, use Upstash Redis or Vercel KV.

---

## Process Recommendation (from auditor #6)

> "From now on, use three separate labels on audit findings:
> - FIXED-SOURCE: code has been fixed
> - VERIFIED-BUILD: source compiles/lints/builds clean
> - VERIFIED-RUNTIME: behavior proven on actual ESP32/network/GAS"

**Do NOT conflate FIXED-SOURCE with FIXED.** For a system with relay control + OTA + persistent journal, the difference matters.

### Current overall status

- 🔧 FIXED-SOURCE: ✅ All P0 (except P0-2 architectural) + 8/11 P1 findings
- 🔨 VERIFIED-BUILD: ✅ Firmware `pio run -e production` SUCCESS, PWA `bun run lint && bun run build` SUCCESS
- ⚡ VERIFIED-RUNTIME: ❌ NOT YET PROVEN — hardware integration test is the next gate

**Production deployment decision**: BLOCKED until hardware test matrix passes. Source-level security is mature enough to enter verification gate, but not enough to claim operational safety.

---

## Audit History Summary

| Auditor | Round | Result |
|---------|-------|--------|
| #1 (engineer) | 12-point verification gate | ✅ LULUS (build + 63 test cases) |
| #2 | 9 findings (3 valid, 3 already fixed, 3 invalid) | 3 fixes applied, 3 refuted with code evidence |
| #3 (re-audit) | "Lolos dengan catatan opsional" + 7 recommendations | 1 already implemented, 1 documented (Flash Encryption), 5 future work |
| #4 | 2 P0 + 8 P1 | P0-1 + P1-1/2/3/6/10 fixed, P0-2 + P1-4/5/7/9/11 documented as backlog |
| #5 | 2 P0 + 11 P1 + 6 P2 | P1-1/2/5 fixed (new findings), P1-8 already fixed, rest confirmed |
| #6 (final) | 3-tier verification framework | This document — distinguishes source/build/runtime |

**No further source-code audit rounds recommended**. Next step: hardware integration test.
