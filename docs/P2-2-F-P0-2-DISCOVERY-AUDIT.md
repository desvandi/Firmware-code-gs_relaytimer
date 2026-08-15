# F-P0-2 — DISCOVERY AUDIT (Phase A — Read-Only)

**Finding ID:** F-P0-2
**Title:** REST API bypasses TransactionJournal — no durability, no dedup, no replay protection
**Severity:** P0 (production blocker)
**Auditor stage:** A (Discovery — read-only, no fixes proposed yet)
**Date:** 2026-08-15

---

## 1. Executive Summary

While MQTT commands (relays, schedules, PIR, channel, time, system, config, OTA)
flow through `MqttClient::_handleCommand()` / `_handleOta()` and are wrapped by
the Rev26 `TransactionJournal` (P2-1 APPROVED, F-P0-1 CLOSED), the **entire REST
API surface mutates device state directly** without ever consulting the journal.

This means a REST-initiated mutation:
- Has no `requestId` → no dedup
- Has no `storeIntent()` → no PENDING evidence on crash
- Has no `markExecuting()` → no EXECUTING evidence during physical mutation
- Has no `commitTransaction()` → no COMMITTED evidence after success
- Cannot be replayed safely (retry may double-mutate)
- Cannot be reconciled on boot (no record to compare against RAM)

The F-P0-1 lifecycle matrix that was just validated for MQTT does NOT apply to
REST. A device controlled solely via REST (LAN mode, PWA, etc.) has zero of the
durability properties that P2-1 + F-P0-1 added for MQTT.

---

## 2. REST Mutation Endpoint Inventory

| #  | Endpoint                              | Method  | Handler                          | Mutation target (direct)                          | Journal consulted? |
|----|---------------------------------------|---------|----------------------------------|----------------------------------------------------|--------------------|
| 1  | `/api/relay`                          | POST    | `handleRelay`                    | `RelayEngine::setManual/setMode` → GPIO             | ❌ NO              |
| 2  | `/api/channel`                        | POST    | `handleChannelRename`            | `Core::channels[idx].name` (RAM)                    | ❌ NO              |
| 3  | `/api/schedule`                       | POST    | `handleScheduleUpsert`           | `Core::channels[idx].sched[]` (RAM) + markDirty    | ❌ NO              |
| 4  | `/api/schedule`                       | DELETE  | `handleScheduleDelete`           | `Core::channels[idx].sched[]` shift-down (RAM)     | ❌ NO              |
| 5  | `/api/pir`                            | POST    | `handlePirConfig`                | `Core::channels[chIdx].pirEnabled/pirHoldTime`     | ❌ NO              |
| 6  | `/api/pir/test`                       | POST    | `handlePirTest`                  | `Drivers::pir.testTrigger` (transient)             | ❌ NO              |
| 7  | `/api/time`                           | POST    | `handleSetTime`                  | `Drivers::rtc.adjust` (RTC write)                  | ❌ NO              |
| 8  | `/api/config`                         | POST    | `handleSetConfig`                | `Core::userConfig.*` + `Storage::config.saveUserConfig` | ❌ NO          |
| 9  | `/api/config/device`                  | POST    | `handleSetDeviceConfig`          | `Core::deviceName/timezone` + save                 | ❌ NO              |
| 10 | `/api/config/password`                | POST    | `handleChangePassword`           | `AuthManager::changePassword` (NVS)                | ❌ NO              |
| 11 | `/api/config/import`                  | POST    | `handleImportConfig`             | `Storage::config.importAll` (bulk overwrite)       | ❌ NO              |
| 12 | `/api/reboot`                         | POST    | `handleReboot`                   | `Storage::config.saveSchedule(true)` → `ESP.restart()` | ❌ NO          |
| 13 | `/api/ota`                             | POST    | `handleOtaResponse/Upload`        | `Update.write()` → flash (DEV only; blocked in PROD) | ❌ NO           |
| 14 | `/api/ota/check`                      | POST    | `handleOtaCheck`                 | `OtaManager` check (HTTPS GET to update server)    | ❌ NO              |
| 15 | `/api/factory_reset/prepare`          | POST    | `handleFactoryResetPrepare`      | NVS flag + token                                   | ❌ NO              |
| 16 | `/api/factory_reset/confirm`          | POST    | `handleFactoryResetConfirm`      | Bulk NVS erase + filesystem wipe                    | ❌ NO              |

**Read-only endpoints** (NOT in scope for F-P0-2 mutation bypass, but listed
for completeness — these would correctly use `CommitMode::NONE` if routed
through journal):

| Endpoint                  | Handler             | Notes                              |
|---------------------------|---------------------|------------------------------------|
| `/api/status` GET         | `handleStatus`      | Read Core::relayState, channels    |
| `/api/version` GET        | `handleVersion`     | Static firmware version            |
| `/api/health` GET         | `handleHealth`      | Heap + uptime                      |
| `/api/log` GET            | `handleGetLogs`     | Read LogService buffer              |
| `/api/audit_log` GET      | `handleGetAuditLog` | Read audit log file                 |
| `/api/config/export` GET  | `handleExportConfig`| Read-only JSON dump                 |
| `/api/gas_secret` GET     | `handleGasSecret`   | HMAC secret (sensitive — auth req) |

---

## 3. Concrete Bypass Examples (from source review)

### 3.1 `handleRelay` — direct physical mutation, no journal

```cpp
// firmware/RelayHandlers.h line 46-49
if (strcmp(actionStr, "on") == 0) {
  Services::relayEngine.setManual(idx, true);   // ← direct GPIO mutation
} else if (strcmp(actionStr, "off") == 0) {
  Services::relayEngine.setManual(idx, false);  // ← direct GPIO mutation
}
```

Compared to MQTT path (now F-P0-1 APPROVED):
```cpp
// firmware/MqttClient.cpp line 1063, 1085, 1094 (paraphrased)
journal.storeIntent(requestId, commandHash);              // PENDING (durable)
journal.markExecuting(requestId);                          // EXECUTING (durable)
relayEngine.setManual(idx, true);                         // physical mutation
journal.commitTransaction(requestId, ackJson);             // COMMITTED (durable)
```

REST skips all four journal interactions. A REST "on" command on channel 1:
- Cannot be deduplicated on retry (no requestId, no isProcessed check)
- Leaves no PENDING record if device crashes mid-mutation
- Leaves no EXECUTING record if device crashes after `setManual` returns but
  before sendSuccess() flushes
- Cannot be reconciled against RAM on next boot (journal doesn't know it happened)

### 3.2 `handleScheduleUpsert` — direct RAM mutation + dirty flag

```cpp
// firmware/ScheduleHandlers.h line 63-86
Core::channels[idx].sched[sIdx].onTime = ...;     // ← direct RAM write
Core::channels[idx].sched[sIdx].offTime = ...;
Core::channels[idx].sched[sIdx].dayMask = ...;
Core::channels[idx].schedCount++;                  // ← increment in RAM
Storage::config.markDirty();                       // ← deferred save (10s timer)
Services::relayEngine.forceRefresh();
```

Two problems:
1. **No journal record** — same as relay case
2. **Deferred save** — `markDirty()` sets a flag, actual NVS write happens
   10s later (or 60s if SAVE_DELAY_MS is extended). If device crashes in
   that window, schedule is lost from NVS but already mutated in RAM
   until reboot. After reboot, RAM resets to last-saved NVS state, so
   the schedule disappears silently.

   This is the same RAM/NVS divergence pattern that P2-1 R4-C1 fixed for
   the journal layer — but it's still present for schedule saves because
   REST bypasses the journal entirely.

### 3.3 `handleReboot` — same lifecycle concern as MQTT reboot

```cpp
// firmware/SystemHandlers.h line 21-26
inline void handleReboot() {
  if (!requireAuth()) return;
  if (!requireCsrf()) return;
  sendSuccess("System rebooting", "{\"rebooting\":true}");
  if (Core::scheduleDirty) Storage::config.saveSchedule(true);
  Services::Log.append(Core::LogType::Restart, "Reboot triggered", 0);
  delay(500);
  ESP.restart();
}
```

Compare with MQTT reboot (F-P0-1 TEST 9 — APPROVED):
```cpp
// MQTT path: journal.storeIntent → commitTransaction → ACK queued → ESP.restart()
// REST path: sendSuccess → saveSchedule → ESP.restart()  (no journal entry)
```

The MQTT reboot lifecycle was specifically designed so that:
- ACK stays queued (durable evidence of reboot command)
- After reboot, journal shows COMMITTED for the reboot requestId
- PWA can detect "device rebooted as I asked" by polling journal status

REST reboot has none of this. PWA sends reboot, gets 200 OK, then device
reboots — but on next boot there's no evidence the reboot was commanded.
A PWA can't distinguish "device crashed" from "device rebooted because
I asked it to."

### 3.4 `handleSetTime` — RTC mutation without journal

```cpp
// firmware/TimeHandlers.h line 41-43
Drivers::rtc.adjust(y, m, d, h, mi, s);   // ← physical RTC write
sendSuccess("RTC time synced", "{\"synced\":true}");
```

MQTT path (F-P0-1 APPROVED):
```cpp
journal.storeIntent(...);                 // PENDING
journal.commitTransactionFromPending(...); // COMMITTED
// then Drivers::rtc.adjust(...)
```

REST skips both. Time changes are silent — no audit trail, no dedup.

### 3.5 `handlePirConfig`, `handleSetDeviceConfig`, `handleSetConfig`

All follow the same anti-pattern:
1. validate (auth, CSRF, body)
2. mutate RAM directly
3. `markDirty()` or `saveXxxConfig()` (deferred)
4. `sendSuccess()`

No journal interaction at any step.

### 3.6 `handleOtaResponse` / `handleOtaUpload`

This is HARD-DISABLED in PRODUCTION_BUILD (returns 403), so production is safe.
In dev mode, OTA upload writes to flash via `Update.write()` with no journal
entry. **Dev-only concern, not a production blocker** — but worth noting
for completeness.

---

## 4. Concrete Failure Scenarios

### 4.1 Crash mid-mutation, REST path

1. PWA sends `POST /api/relay {channelId:1, action:"on"}` (REST, no requestId)
2. `relayEngine.setManual(0, true)` runs, GPIO goes HIGH, relay closes
3. Device crashes BEFORE `sendSuccess()` returns to PWA
4. PWA retries (no requestId → no dedup possible)
5. On reboot: device RAM resets to "off" (no journal says otherwise)
6. PWA sees relay off, retries again — relay turns on
7. User sees two relay toggles for one user intent

For the MQTT path, the same scenario is now safe:
- storeIntent(requestId) durable before mutation
- markExecuting(requestId) durable during mutation
- commitTransaction(requestId) durable after mutation
- On reboot, journal shows the command was already processed
- Retry hits isProcessed() check, replays ACK, no double-mutation

### 4.2 Schedule deferred-save race

1. PWA sends `POST /api/schedule` with new schedule
2. `Core::channels[1].sched[2]` updated in RAM, `markDirty()` called
3. User immediately sends `POST /api/reboot`
4. `handleReboot` calls `saveSchedule(true)` — but the FIRST schedule mutation
   may not yet be flushed (depending on whether the 10s timer fired)
5. Reboot happens; on next boot NVS may or may not have the new schedule

The MQTT path doesn't have this race because `commitTransactionFromPending`
writes to BOTH NVS copies BEFORE returning to caller. REST has no such
synchronization point.

### 4.3 Replay attack vector (lower severity due to CSRF + auth)

A malicious actor who captures a valid `POST /api/relay` request (with valid
CSRF token + JWT) can replay it indefinitely because there's no requestId
dedup. Each replay causes a fresh mutation.

For idempotent commands (on/off) this is mostly benign. For non-idempotent
commands (config import, factory reset confirm, schedule delete), replay can
cause cascading state damage.

MQTT path is protected by `isProcessed(requestId)` check at the start of
`_handleCommand` / `_handleOta`.

---

## 5. Scope Boundary — What F-P0-2 is NOT

- **NOT a CSRF / auth issue** — REST endpoints already enforce requireAuth()
  and requireCsrf(). The bypass is at the JOURNAL layer, not the auth layer.
- **NOT a production OTA issue** — `handleOtaResponse/Upload` are hard-blocked
  in PRODUCTION_BUILD. Dev-only concern, deferred.
- **NOT a read-only endpoint issue** — GET endpoints don't mutate state and
  would correctly use CommitMode::NONE if routed through journal. Out of scope.
- **NOT an architecture change to TransactionJournal** — the journal API
  is sufficient (storeIntent, markExecuting, commitTransaction,
  commitTransactionFromPending, clearEntry). The fix is to ROUTE REST
  handlers THROUGH that existing API, not to add new journal methods.

---

## 6. Initial Hypothesis for Correction Strategy (NOT a proposal yet)

Three candidate architectures, listed from least to most invasive:

### Option α (lightest): Per-handler journal wrapping

Each mutation handler adds the 4-step pattern:
1. Generate or extract requestId from request body / header
2. `journal.storeIntent(requestId, hash)` — PENDING
3. Execute existing mutation (setManual, saveSchedule, etc.)
4. `journal.commitTransaction(requestId, ackJson)` — COMMITTED

Pros: Minimal change to handler logic. Journal calls are localized.
Cons: Each handler must repeat the pattern. Easy to forget on new handlers.
      Doesn't fix the deferred-save race in §4.2 (still need to call
      `Storage::config.saveSchedule(true)` synchronously before commitTransaction).

### Option β (medium): Shared `_handleRestCommand()` dispatcher

Mirror the MQTT `_handleCommand()` switch-on-type pattern for REST:
build a JSON envelope `{type, action, requestId, ...}` from the REST
body + URL, then dispatch to a shared `_handleCommand()` (or a REST
variant `_handleRestCommand()`).

Pros: Single journal-wrap point. New commands automatically get journal.
Cons: REST URL semantics (`/api/schedule` POST vs DELETE) don't map cleanly
      to `{type:"schedule", action:"upsert"}` — REST has separate POST/DELETE
      endpoints, MQTT has one command with action field. May need adapter.
      CSRF/auth checks are different (MQTT has its own auth, REST uses
      requireAuth/requireCsrf).

### Option γ (heaviest): Unified command ingress layer

Build a `CommandIngress` class that BOTH MQTT and REST feed into. Both
paths produce a `Command` struct with type/action/requestId/payload, then
`CommandIngress.dispatch(cmd)` does auth + journal + execute + ack.

Pros: Single source of truth. F-P0-3 (command classifier) naturally fits
      here. Future command sources (websocket, BLE, etc.) get journal
      for free.
Cons: Largest refactor. Risk of regression in MQTT path that just got
      F-P0-1 APPROVED. Auditor explicitly warned: "jangan langsung refactor
      besar" (don't do large refactor upfront).

**No recommendation yet** — Option selection requires auditor discussion.
This is Phase A (Discovery), not Phase B (Semantic Design).

---

## 7. Open Questions for Auditor

1. **requestId source for REST**: Should REST require a `requestId` field in
   the JSON body (PWA-generated UUID), or should the firmware generate one
   server-side (e.g., from JWT nonce + timestamp)?

2. **CSRF token as requestId proxy**: If the CSRF token is single-use, can
   it serve as the dedup key? Or do we need a separate requestId?

3. **Deferred save race (§4.2)**: Should REST handlers force synchronous
   save (`saveSchedule(true)`) before commitTransaction? This eliminates
   the race but adds latency.

4. **Endpoints that don't fit**: `handleFactoryResetConfirm` wipes NVS
   itself — including the journal. How should the journal wrap a command
   that destroys the journal? (Special-case? Pre-commit only?)

5. **OTA check**: `handleOtaCheck` doesn't mutate device state, just queries
   the update server. Should it create a journal entry anyway for audit
   trail, or treat it as CommitMode::NONE (read-only)?

6. **Auth change**: `handleChangePassword` overwrites the user's password
   hash in NVS. If we journal this, the requestId + commandHash become
   durable evidence of who changed the password when. But the password
   hash itself should NOT be in the journal (sensitive). How to handle
   commandHash for commands whose payload is sensitive?

---

## 8. Status

**Phase A (Discovery): COMPLETE**
- All REST mutation endpoints identified (16 of them)
- 6 concrete bypass patterns documented (§3.1-3.6)
- 3 failure scenarios analyzed (§4.1-4.3)
- 3 candidate architectures sketched (§6 α/β/γ) — NOT a proposal
- 6 open questions for auditor (§7)

**Phase B (Semantic Design): PENDING**
- Awaiting auditor discussion of open questions
- Awaiting auditor's preferred Option (α/β/γ) or alternative
- Awaiting decision on requestId source

**Phase C-I (Production-path proof, failure-path proof, regression): PENDING**
- Will follow same discipline as F-P0-1:
  semantic design → production-path proof → failure-path proof → regression
- No large refactor upfront (per auditor guidance)

---

## 9. Reference

- F-P0-1 closure: validated lifecycle matrix for MQTT path
  (PENDING → EXECUTING → COMMITTED → ACK, with clearEntry on validation failure)
- F-P0-2 is the symmetric closure for REST path
- Auditor's discipline reminder: "semantic design → production-path proof →
  failure-path proof → regression, bukan langsung refactor besar"
