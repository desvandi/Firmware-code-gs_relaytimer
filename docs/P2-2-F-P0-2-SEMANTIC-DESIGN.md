# F-P0-2 — SEMANTIC DESIGN (Phase B)

**Finding ID:** F-P0-2
**Title:** REST API bypasses TransactionJournal — Durability Closure
**Phase:** B (Semantic Design — for auditor approval before refactor)
**Date:** 2026-08-15
**Predecessor:** F-P0-1 APPROVED (MQTT lifecycle closure)
**Discipline:** semantic design → production-path proof → failure-path proof → regression

---

## 1. Design Principle

The auditor's exact words: *"Dan saya sarankan tetap memakai disiplin yang sama: semantic design → production-path proof → failure-path proof → regression, bukan langsung refactor besar."*

**Option α (per-handler journal wrapping)** is selected — minimum surface change, no architecture refactor.

REJECTED:
- Option β (shared dispatcher) — too invasive; risk regression in F-P0-1 MQTT path.
- Option γ (unified CommandIngress) — explicitly warned against by auditor.

Option α keeps each REST handler in place, adds 4 journal calls per handler, mirrors the F-P0-1 lifecycle matrix exactly. The TransactionJournal API is already sufficient (no new methods needed).

---

## 2. Lifecycle Matrix for REST Endpoints

Mirror of MQTT lifecycle matrix from F-P0-1 closure. Each REST mutation endpoint maps to exactly one of three CommitMode values:

| #  | Endpoint                              | Method  | CommitMode        | Lifecycle                              | Notes                                                  |
|----|---------------------------------------|---------|-------------------|----------------------------------------|--------------------------------------------------------|
| 1  | `/api/relay`                          | POST    | `EXECUTING`       | PENDING→EXECUTING→COMMITTED            | Physical GPIO mutation (mirror MQTT relay)             |
| 2  | `/api/channel`                        | POST    | `FROM_PENDING`    | PENDING→COMMITTED                      | Atomic rename (mirror MQTT channel)                    |
| 3  | `/api/schedule`                       | POST    | `FROM_PENDING`    | PENDING→COMMITTED                      | Atomic upsert (mirror MQTT schedule)                   |
| 4  | `/api/schedule`                       | DELETE  | `FROM_PENDING`    | PENDING→COMMITTED                      | Atomic delete                                          |
| 5  | `/api/pir`                            | POST    | `FROM_PENDING`    | PENDING→COMMITTED                      | Atomic config (mirror MQTT pir)                        |
| 6  | `/api/pir/test`                       | POST    | `EXECUTING`       | PENDING→EXECUTING→COMMITTED            | Transient trigger — physical mutation                  |
| 7  | `/api/time`                           | POST    | `FROM_PENDING`    | PENDING→COMMITTED                      | RTC write (mirror MQTT time)                           |
| 8  | `/api/config`                         | POST    | `FROM_PENDING`    | PENDING→COMMITTED                      | User/password (mirror MQTT config)                     |
| 9  | `/api/config/device`                  | POST    | `FROM_PENDING`    | PENDING→COMMITTED                      | Device name/timezone                                   |
| 10 | `/api/config/password`                | POST    | `FROM_PENDING`    | PENDING→COMMITTED                      | Password hash (sensitive — see §5)                    |
| 11 | `/api/config/import`                  | POST    | `EXECUTING`       | PENDING→EXECUTING→COMMITTED            | Bulk overwrite — physical NVS write                    |
| 12 | `/api/reboot`                         | POST    | `FROM_PENDING`    | PENDING→COMMITTED → restart            | ACK queued (mirror MQTT reboot — TEST 9 APPROVED)     |
| 13 | `/api/ota`                             | POST    | (n/a — disabled in PROD) | n/a                                    | Hard-blocked in PRODUCTION_BUILD                       |
| 14 | `/api/ota/check`                      | POST    | `NONE`            | NO JOURNAL                              | Read-only query (mirror MQTT getStatus — TEST 7)       |
| 15 | `/api/factory_reset/prepare`          | POST    | `NONE`            | NO JOURNAL                              | Generates token only — no mutation to journal-worthy state |
| 16 | `/api/factory_reset/confirm`          | POST    | `EXECUTING`       | PENDING→EXECUTING→COMMITTED → restart  | Bulk NVS wipe — see §6 special case                    |

**Symmetry argument:** every REST endpoint that has an MQTT counterpart uses the same CommitMode as that counterpart. This is the F-P0-2 ↔ F-P0-1 symmetry closure.

---

## 3. requestId Source

**Decision:** REST handler requires client-supplied `requestId` in JSON body. Same format as MQTT (UUID, max 64 chars, `[a-zA-Z0-9-_]+`).

**Rationale:**
- PWA already generates requestId for MQTT commands (proven by F-P0-1 TEST 1-13).
- Server-side generation would force a response-then-retry pattern (PWA must learn the requestId to retry). Worse for HTTP than MQTT because HTTP is request-response.
- CSRF token is single-use but tied to session, not to specific command — wrong granularity for dedup.

**Backward compatibility:**
- If `requestId` is absent in body → respond 400 with `{"success":false,"message":"requestId required","data":null}`.
- PWA must be updated to send requestId on all mutation endpoints.
- This is a breaking change but small and necessary (same break PWA already accepted for MQTT).
- Dev builds may set `ALLOW_MISSING_REQUEST_ID_REST` to skip the check (gated behind build flag, NOT in PRODUCTION_BUILD).

---

## 4. Per-Handler Pattern (Option α — concrete code shape)

Each mutation handler gains the same 4-step pattern. Example for `/api/relay`:

```cpp
inline void handleRelay() {
  if (!requireAuth()) return;
  if (!requireCsrf()) return;
  if (!requireBody(1024)) return;
  // ... parse JSON ...

  String requestId = doc["requestId"] | "";
  if (requestId.length() == 0) {
    sendError(400, "requestId required");
    return;
  }
  if (requestId.length() > 64) {
    sendError(400, "requestId too long (max 64)");
    return;
  }
  // (charset validation — same as MQTT _handleCommand lines 890-898)

  // Compute hash — same canonical schema as MQTT (reuse _computeCommandHash)
  String commandHash = _computeRestCommandHash(doc, "relay");

  // === JOURNAL INTERACTION (mirror MQTT) ===

  // 1. DEDUP CHECK
  if (Services::journal.isProcessed(requestId)) {
    String prevHash = Services::journal.getCommandHash(requestId);
    if (prevHash != commandHash) {
      sendError(409, "requestId reuse with different command — rejected");
      return;
    }
    if (Services::journal.isCommitted(requestId)) {
      // Replay original ACK
      String ackJson = Services::journal.getAckJson(requestId);
      if (ackJson.length() > 0) {
        Web::sendSecurityHeaders();
        Web::http.send(200, "application/json", ackJson);
      } else {
        sendSuccess("Duplicate command (already executed)");
      }
      return;
    }
    // PENDING/EXECUTING — surface to PWA
    sendError(409, "requestId in progress (PENDING or EXECUTING) — retry later");
    return;
  }

  // 2. STORE INTENT (PENDING)
  if (!Services::journal.storeIntent(requestId, commandHash,
                                       intentChannelId, intentDesiredState,
                                       intentPreviousKnown)) {
    sendError(503, "DURABILITY_FAILURE: cannot store transaction intent — retry");
    return;
  }

  // 3a. FOR EXECUTING MODE: markExecuting right before physical mutation
  if (commitMode == CommitMode::EXECUTING) {
    if (!Services::journal.markExecuting(requestId)) {
      Services::journal.clearEntry(requestId);
      sendError(503, "Internal error: cannot mark transaction as executing");
      return;
    }
  }

  // 3b. EXECUTE MUTATION (existing handler logic — unchanged)
  Services::relayEngine.setManual(idx, true);

  // 4. COMMIT + RESPOND
  String ackJson = _buildRelayAckJson(requestId, channelId, idx);
  bool committed;
  if (commitMode == CommitMode::EXECUTING) {
    committed = Services::journal.commitTransaction(requestId, ackJson);
  } else {
    committed = Services::journal.commitTransactionFromPending(requestId, ackJson);
  }
  if (!committed) {
    sendError(503, "DURABILITY_FAILURE: transaction could not be committed — retry");
    return;
  }

  Web::sendSecurityHeaders();
  Web::http.send(200, "application/json", ackJson);
}
```

**Pattern uniformity:** every mutation handler follows this skeleton. Only the "execute mutation" middle differs.

---

## 5. Sensitive Payloads (Password Change)

`/api/config/password` and `/api/config` (with `pass` field) accept password in body. The `commandHash` must NOT include the password itself — only metadata about the change.

**Hash schema for password change:**
```
canonical = "config|password|user=" + username + "|ts=" + millis()
```
- `user` identifies which account changed (audit trail)
- `ts` makes hashes unique across multiple password changes (so two `changePassword` calls don't collide)
- Password value is NEVER hashed into the journal record — only the *intent* to change password

**On replay (same requestId):** ACK is replayed, password is NOT re-hashed. Correct because the password was already changed.

---

## 6. Special Case: Factory Reset Confirm

`/api/factory_reset/confirm` wipes NVS — including the journal itself. This breaks the journal's own durability model.

**Decision: pre-commit only.**

```cpp
inline void handleFactoryResetConfirm() {
  // ... auth, CSRF, token validation ...

  // STORE INTENT (PENDING) — durable record that reset was commanded
  String commandHash = _computeRestCommandHash(doc, "factory_reset");
  if (!Services::journal.storeIntent(requestId, commandHash, 0, false, false)) {
    sendError(503, "DURABILITY_FAILURE");
    return;
  }

  // MARK EXECUTING — begin physical wipe
  if (!Services::journal.markExecuting(requestId)) {
    Services::journal.clearEntry(requestId);
    sendError(503, "Internal error");
    return;
  }

  // Send response BEFORE the wipe (HTTP client needs the response)
  String ackJson = "...";
  // We CANNOT commitTransaction() — the wipe will destroy the journal.
  // Publish the ACK immediately as best-effort (PWA must handle non-receipt).
  Web::sendSecurityHeaders();
  Web::http.send(200, "application/json", ackJson);

  // Small delay to let TCP flush
  delay(500);

  // PERFORM WIPE — journal will be destroyed here
  Drivers::relay.allOff();
  Drivers::pir.resetAll();
  Storage::config.resetChannels();
  Storage::config.saveSchedule(true);
  Storage::config.initDefaultUserConfig();
  Storage::config.saveUserConfig();

  // Reboot — new boot will see fresh journal
  ESP.restart();
}
```

**Lifecycle:** PENDING → EXECUTING → (wipe destroys journal) → fresh boot. The EXECUTING state was durable for the brief window between markExecuting and the wipe. After wipe, the journal starts empty (correct — device is reset).

**Auditor note:** This is the only endpoint that cannot reach COMMITTED. The lifecycle matrix entry should reflect this: "PENDING→EXECUTING (no COMMITTED — journal destroyed by design)".

---

## 7. Deferred Save Race Fix (§4.2 of discovery)

The discovery audit identified a RAM/NVS divergence race in `handleScheduleUpsert`:
- `markDirty()` schedules a 10s deferred save
- If reboot happens within 10s, schedule mutation is lost

**Fix:** REST schedule handler calls `Storage::config.saveSchedule(true)` synchronously BEFORE `commitTransaction()`.

```cpp
// handleScheduleUpsert, after the existing mutation:
Storage::config.markDirty();
Storage::config.saveSchedule(true);   // SYNCHRONOUS save (force=true)
Services::relayEngine.forceRefresh();

// THEN commit transaction
bool committed = Services::journal.commitTransactionFromPending(requestId, ackJson);
```

**Trade-off:** +50-100ms latency on schedule endpoint (NVS write). Acceptable — schedule changes are infrequent (user-initiated, not high-throughput).

**Symmetry:** MQTT `_handleSchedule` already does this implicitly via the journal's NVS write. REST now matches.

---

## 8. Test Plan — Production-Path Proof

Build `WebServerTest.cpp` that compiles REAL `HttpServer.cpp` + handler headers + `TransactionJournal.cpp` + `MqttClient.cpp` (for `_computeCommandHash` reuse). Use `#define private public` to access Web::http internal state.

Test cases (mirror F-P0-1 TEST 1-13):

| Test | Endpoint                                | Method  | Assertion                                                       |
|------|-----------------------------------------|---------|-----------------------------------------------------------------|
| R1   | `/api/relay` {channelId:1, action:on}   | POST    | state == COMMITTED, relayState[0] == true                       |
| R2   | `/api/channel` {channelId:1, name:X}    | POST    | state == COMMITTED, channels[0].name == "X"                     |
| R3   | `/api/schedule` {channelId:1, ...}      | POST    | state == COMMITTED, schedCount == 1                             |
| R4   | `/api/schedule?id=...`                  | DELETE  | state == COMMITTED, schedCount back to 0                       |
| R5   | `/api/pir` {id:1, enabled:true}         | POST    | state == COMMITTED, pirEnabled == true                          |
| R6   | `/api/time` {datetime:...}              | POST    | state == COMMITTED, rtc.adjust called                            |
| R7   | `/api/config/device`                    | POST    | state == COMMITTED, deviceName updated                          |
| R8   | `/api/config/password`                  | POST    | state == COMMITTED, password hash NOT in journal                |
| R9   | `/api/reboot`                           | POST    | state == COMMITTED, espRestartCalled == true, ACK queued        |
| R10  | `/api/ota/check`                        | POST    | isProcessed == false (NONE — no journal entry)                  |
| R11  | `/api/factory_reset/prepare`            | POST    | isProcessed == false (NONE)                                     |
| R12  | `/api/factory_reset/confirm`            | POST    | state == EXECUTING (no COMMITTED — wipe), espRestartCalled == true |
| R13a | Duplicate requestId (COMMITTED)         | POST    | HTTP 200, original ACK replayed, no double-mutation             |
| R13b | Duplicate requestId (PENDING)            | POST    | HTTP 409, "in progress"                                         |
| R13c | requestId reuse w/ diff hash            | POST    | HTTP 409, "requestId reuse rejected"                           |
| R14  | Validation failure after storeIntent    | POST    | isProcessed == false, clearEntry called                         |
| R15  | Missing requestId                       | POST    | HTTP 400                                                         |
| R16  | Malformed requestId (>64 chars)         | POST    | HTTP 400                                                         |
| R17  | Invalid charset requestId               | POST    | HTTP 400                                                         |

All tests call REAL `handleXxx()` functions (NOT replicated logic). Same harness pattern as MqttClientTest.

---

## 9. Test Plan — Failure-Path Proof

| Test | Scenario                                         | Injection                                            | Assertion                                        |
|------|--------------------------------------------------|------------------------------------------------------|--------------------------------------------------|
| F1   | NVS write fails during storeIntent (relay)       | `Preferences::setFailNextPut("tj_slot_0_a")`         | HTTP 503 DURABILITY_FAILURE, no mutation          |
| F2   | NVS write fails during commitTransaction         | `Preferences::setFailNextPut("tj_slot_0_a")` post-markExecuting | HTTP 503, state stays EXECUTING (NOT COMMITTED), relay already mutated (physical side effect — documented limitation) |
| F3   | Schedule invalid time format (after storeIntent) | `onTime="invalid"`                                  | clearEntry called, isProcessed == false          |
| F4   | Schedule limit exceeded (max 4 per channel)       | Add 5th schedule                                     | clearEntry called, isProcessed == false          |
| F5   | PIR id out of range (1-4)                        | `id=5`                                               | clearEntry called                                |
| F6   | Channel name too long (>20 chars)                | name="abcdefghijk..." (25 chars)                     | clearEntry called                                |
| F7   | Password too weak                                | `next="abc"`                                         | clearEntry called                                |
| F8   | Factory reset token expired                       | Token TTL elapsed                                    | HTTP 403, no wipe, clearEntry called              |

---

## 10. Test Plan — Regression

- TransactionJournalTest: 194/194 unchanged
- CommandRoutingTest: 133/133 unchanged
- MqttClientTest: 31/31 unchanged
- New WebServerTest: 17 production-path + 8 failure-path = 25 tests
- Total: 194 + 133 + 31 + 25 = 383 assertions across 4 test binaries

---

## 11. Implementation Plan (Phase C)

Sequential, no parallelism:

1. Add `_computeRestCommandHash()` helper (or refactor `_computeCommandHash` to be shared between MQTT and REST).
2. Modify `Common.h` — add `requireRequestId()` helper used by all mutation handlers.
3. Modify `RelayHandlers.h` — add 4-step journal wrap.
4. Modify `ScheduleHandlers.h` — add 4-step wrap + synchronous save.
5. Modify `ChannelHandlers.h` — add 4-step wrap.
6. Modify `PirHandlers.h` — add 4-step wrap (both `handlePirConfig` and `handlePirTest`).
7. Modify `TimeHandlers.h` — add 4-step wrap.
8. Modify `ConfigHandlers.h` — add 4-step wrap (handleSetConfig, handleSetDeviceConfig, handleChangePassword).
9. Modify `SystemHandlers.h` — add 4-step wrap to handleReboot.
10. Modify `OtaHandlers.h` — `handleOtaCheck` uses CommitMode::NONE (no journal). `handleOtaResponse/Upload` unchanged (already disabled in PROD).
11. Modify `FactoryResetHandlers.h` — `handleFactoryResetPrepare` NONE; `handleFactoryResetConfirm` special-case (§6).
12. Build `WebServerTest.cpp` + Makefile.ws — production-path proof.
13. Build failure-path tests (failure injection shims already exist in Preferences.h).
14. Regression run: all 4 binaries.

**No production source changes that affect MQTT path.** All MQTT handler code is unchanged — only REST handlers gain the journal wrap.

---

## 12. Open Questions Resolved (from discovery §7)

| Q | Discovery question                                        | Resolution                                                            |
|---|-----------------------------------------------------------|-----------------------------------------------------------------------|
| 1 | requestId source                                          | Client-supplied (PWA UUID), same as MQTT (§3)                         |
| 2 | CSRF as requestId proxy?                                  | No — granularity mismatch. CSRF is per-session, requestId is per-command. |
| 3 | Deferred save race                                        | Force `saveSchedule(true)` before commitTransaction (§7)             |
| 4 | Factory reset wipes journal                               | Pre-commit only (PENDING→EXECUTING, no COMMITTED) — see §6          |
| 5 | OTA check journal entry?                                  | No — CommitMode::NONE (read-only query, like getStatus)              |
| 6 | Sensitive payload hash                                    | Hash metadata only (user, ts), never the password value (§5)         |

---

## 13. What This Design Does NOT Do

- Does NOT refactor MQTT `_handleCommand()` — that path is F-P0-1 APPROVED.
- Does NOT introduce a `CommandIngress` abstraction — explicitly rejected.
- Does NOT add new TransactionJournal API methods — existing API is sufficient.
- Does NOT change authentication, CSRF, CORS, or TLS posture.
- Does NOT touch read-only endpoints (GET) — they don't mutate state.
- Does NOT change `handleOtaResponse/Upload` (already disabled in PROD).

---

## 14. Risk Assessment

| Risk                                              | Likelihood | Mitigation                                                          |
|---------------------------------------------------|------------|---------------------------------------------------------------------|
| PWA regression — old builds don't send requestId   | High       | PWA must be updated simultaneously. Document breaking change.      |
| NVS write latency on schedule endpoint             | Low        | +50-100ms is acceptable for user-initiated config changes.          |
| Handler forgets journal wrap (future handler)     | Medium     | Add lint rule / requireRequestId() helper enforces presence.        |
| Factory reset commitTransaction unreachable       | By design  | Documented in §6 — lifecycle matrix reflects this.                  |
| REST OTA dev mode unjournal'd                     | Low        | Dev-only — already hard-blocked in PROD. Documented.                  |

---

## 15. Status

**Phase A (Discovery): COMPLETE** — see P2-2-F-P0-2-DISCOVERY-AUDIT.md
**Phase B (Semantic Design): COMPLETE** — this document, awaiting auditor review.

**Phase C (Implementation): PENDING** — will not start until auditor approves Phase B.
- C1: Production-path proof (WebServerTest.cpp, 17 tests)
- C2: Failure-path proof (8 tests with NVS failure injection)
- C3: Regression (383 assertions across 4 binaries)

**Phase D (Closure): PENDING** — patch file, commit, worklog update.

---

## 16. Auditor Decision Needed

1. **Approve Option α** (per-handler wrap) — or direct alternative?
2. **Approve requestId requirement** with breaking change for PWA — or alternative?
3. **Approve factory reset special case** (no COMMITTED) — or alternative?
4. **Approve password hash schema** (metadata only) — or alternative?
5. **Approve deferred-save fix** (synchronous saveSchedule) — or accept race?
6. **Approve test plan** (17 production + 8 failure) — or request more?

On approval, will proceed to Phase C (implementation) following the same iterative
discipline: build → test → fix → re-test → submit for next audit round.
