# F-P0-2 — SEMANTIC DESIGN REV.3 (Phase B Revision 3)

**Finding ID:** F-P0-2
**Title:** REST API bypasses TransactionJournal — Durability Closure + Cross-Ingress Synchronization
**Phase:** B (Semantic Design — REV.3 in response to auditor's CONDITIONAL GO)
**Date:** 2026-08-15
**Predecessor:** F-P0-1 APPROVED (MQTT lifecycle closure)
**Discipline:** semantic design → production-path proof → failure-path proof → regression

---

## 0. Revision Summary

Auditor returned CONDITIONAL GO on REV.2 with 1 mandatory addition + 7
condition-strengthening acceptances. REV.3 incorporates all 8:

| # | Auditor directive                                                       | Section   |
|---|-------------------------------------------------------------------------|-----------|
| 1 | §11 — Cross-Ingress Semantic Contract Matrix (MQTT ↔ REST) — MANDATORY  | §11 (new) |
| 2 | Tighten PWA requestId spec: UUID per logical mutation, NOT per fetch   | §1.3.1    |
| 3 | Expand matrix to 3 columns (mutation_started, retry_safe, requires_EXECUTING) | §3   |
| 4 | RestJournalHelper ownership boundary: journal lifecycle ONLY, no business dispatch | §4.4 |
| 5 | Password: 4 boundary tests (no password in hash/ack/journal; credVer increments only on success; retry does NOT re-mutate) | §5.6 |
| 6 | Factory reset: prove crash windows are SAFE (not just theoretically idempotent) | §6.6 |
| 7 | HTTP-response ↔ commit: hard invariant (HTTP 200 only after commit success) | §9.4 |
| 8 | Test acceptance — semantic branch coverage, not count minimum            | §10.4     |

NO production code changes in this revision — still DESIGN-ONLY.

**Auditor's exact gate condition:**

> Saya belum memberi izin Phase C sampai satu revisi design kecil
> ditambahkan: §11 — Cross-Ingress Semantic Contract Matrix (MQTT ↔ REST)
> dan tambahkan acceptance criteria bahwa REST mutation harus menghasilkan
> semantic journal state yang ekuivalen dengan MQTT untuk command yang
> ekuivalen.

§11 is added in this REV.3. Phase C remains BLOCKED until auditor
approves REV.3.

---

## 1. PWA requestId Caller Map — Tightened (REV.3-1)

### 1.3.1 UUID-per-logical-mutation (REV.3 clarification)

Auditor's directive:

> requestId dibuat sebelum mutation request pertama dikirim, dan seluruh
> retry HTTP untuk logical operation tersebut menggunakan requestId yang
> sama.
>
> Jangan membuat helper apiPost() yang otomatis menghasilkan UUID setiap
> invocation jika React Query retry memanggilnya ulang.

**Tightened spec:** requestId MUST be generated OUTSIDE the retry loop.
Specifically:

```typescript
// ❌ FORBIDDEN — generates new UUID on every fetch retry
async function apiPost(url, body) {
  const requestId = crypto.randomUUID();  // WRONG — new UUID per call
  return fetch(url, { body: { ...body, requestId } });
}

// ✅ CORRECT — UUID generated once, passed through retry
async function mutationHook(mutation) {
  const requestId = crypto.randomUUID();  // 1 UUID per logical mutation
  return apiPostWithRetry('/api/relay', mutation, requestId);  // retry uses same UUID
}

async function apiPostWithRetry(url, body, requestId) {
  for (let attempt = 0; attempt <= MAX_RETRIES; attempt++) {
    try {
      return await fetch(url, { body: { ...body, requestId } });
    } catch (err) {
      if (attempt === MAX_RETRIES) throw err;
      await delay(RETRY_DELAYS[attempt]);
      // SAME requestId used for next attempt — NOT regenerated
    }
  }
}
```

### 1.3.2 Why this matters

If React Query's retry mechanism calls `mutationFn` again (e.g., due to
network error), the SAME requestId MUST be sent. Otherwise:
- First attempt: PWA sends UUID-A → ESP32 stores PENDING → network dies
- React Query retries → mutationFn runs again → PWA sends UUID-B
- ESP32 now has two journal entries: PENDING(UUID-A) and PENDING(UUID-B)
- The first one is orphaned — never reaches COMMITTED, never cleared
- Journal slot exhaustion risk + audit trail corruption

By generating UUID at mutation-hook boundary (outside React Query's
retry loop), we guarantee 1 logical operation = 1 journal entry.

### 1.3.3 React Query retry config

Each mutation hook MUST set `retry` to a number (NOT true) to prevent
infinite loops, AND the retry MUST go through our `apiPostWithRetry`
(not React Query's automatic retry, which would call `mutationFn`
afresh and re-generate the UUID if we did it wrong).

Two valid configurations:

**Configuration A — Disable React Query retry, do retry internally:**
```typescript
useMutation({
  mutationFn: async (mutation) => {
    const requestId = crypto.randomUUID();
    return apiPostWithRetry('/api/relay', mutation, requestId);
  },
  retry: false,  // we handle retry ourselves
});
```

**Configuration B — Use React Query retry, but UUID is stable across retries:**
```typescript
const requestIdRef = useRef<string | null>(null);
useMutation({
  mutationFn: async (mutation) => {
    if (!requestIdRef.current) requestIdRef.current = crypto.randomUUID();
    return apiPost('/api/relay', { ...mutation, requestId: requestIdRef.current });
  },
  retry: 3,
  onSettled: () => { requestIdRef.current = null; },  // reset for next logical mutation
});
```

**Decision:** Configuration A — simpler, no `useRef` lifecycle management.
Internal retry helper in `api.ts` is the single retry point.

---

## 2. Three-Column Matrix (REV.3-2)

Auditor's directive:

> Saya justru menyarankan matrix final mempunyai tiga kolom terpisah:
> mutation already started | retry-safe | requires EXECUTING
> Karena ketiganya tidak identik.

### 2.1 Refined matrix

| #  | Endpoint                       | Method  | mutation already started? | retry-safe? | requires EXECUTING? | Commit mode     |
|----|--------------------------------|---------|---------------------------|-------------|---------------------|------------------|
| 1  | `/api/relay`                   | POST    | YES (GPIO write)          | YES (idempotent) | YES         | EXECUTING        |
| 2  | `/api/channel`                 | POST    | YES (RAM+NVS write)       | YES         | NO                  | FROM_PENDING     |
| 3  | `/api/schedule`                | POST    | YES (RAM+NVS write)       | YES         | NO                  | FROM_PENDING     |
| 4  | `/api/schedule`                | DELETE  | YES (RAM+NVS write)       | YES         | NO                  | FROM_PENDING     |
| 5  | `/api/pir`                     | POST    | YES (RAM+NVS write)       | YES         | NO                  | FROM_PENDING     |
| 6  | `/api/pir/test`                | POST    | YES (transient trigger)   | YES (idempotent) | YES    | EXECUTING        |
| 7  | `/api/time`                    | POST    | YES (RTC write)           | perlu review | YES            | EXECUTING        |
| 8  | `/api/config`                  | POST    | YES (NVS write)           | YES         | NO                  | FROM_PENDING     |
| 9  | `/api/config/device`           | POST    | YES (NVS write)           | YES         | NO                  | FROM_PENDING     |
| 10 | `/api/config/password`         | POST    | YES (NVS write)           | YES (idempotent for same user+credVer) | NO | FROM_PENDING |
| 11 | `/api/config/import`           | POST    | YES (bulk NVS write)      | NO          | YES                 | EXECUTING        |
| 12 | `/api/reboot`                  | POST    | NO (commit before restart)| NO          | NO                  | FROM_PENDING*    |
| 13 | `/api/ota`                      | POST    | (disabled in PROD)        | n/a         | n/a                 | (disabled)       |
| 14 | `/api/ota/check`               | POST    | NO (read-only)            | YES         | NO                  | NONE             |
| 15 | `/api/factory_reset/prepare`   | POST    | NO (token gen only)        | YES         | NO                  | NONE             |
| 16 | `/api/factory_reset/confirm`   | POST    | YES (destructive wipe)    | NO          | YES                 | EXECUTING (special) |

### 2.2 Why three columns are NOT identical

Three illustrative cases:

| Endpoint       | mutation_started | retry_safe | requires_EXECUTING | Explanation                                                |
|----------------|------------------|------------|---------------------|------------------------------------------------------------|
| `/api/relay`   | YES              | YES        | YES                 | Physical mutation that IS safely retryable (idempotent on/off) |
| `/api/reboot`  | NO               | NO         | NO                  | No "mutation" in GPIO/NVS sense; restart is destructive; FROM_PENDING is sufficient because commit-before-restart is atomic |
| `/api/config/import` | YES        | NO         | YES                 | Bulk overwrite that's NOT safely retryable; EXECUTING required because partial-import on retry could leave device in mixed state |

**`/api/reboot` clarification:** "mutation_started = NO" because the
journal commit happens BEFORE the actual restart. There is no window
where mutation has started but commit hasn't — commit IS the only
"mutation". After commit, restart is the side effect. This is why
reboot uses FROM_PENDING (commit is the mutation) rather than
EXECUTING (mutation before commit).

### 2.3 `requires_EXECUTING` decision rule

A handler requires EXECUTING mode if and only if:
- (mutation_started == YES) AND
- (mutation is observable externally before commit, OR
   mutation is not safely retryable)

For `/api/relay`: GPIO goes HIGH during mutation → externally observable
→ EXECUTING.

For `/api/channel`: name is written to RAM then NVS — no external observer
until ACK is sent → FROM_PENDING.

For `/api/config/import`: bulk overwrite is not safely retryable → EXECUTING
(journal marks "in progress" so retry sees EXECUTING and 409s instead
of re-importing).

---

## 3. (Section merged into §2 in REV.3 — no separate EXECUTING definition section needed)

---

## 4. RestJournalHelper Ownership Boundary (REV.3-3)

Auditor's directive:

> Helper tidak boleh menjadi service/business dispatcher tersembunyi.
> Ia boleh mengurus: requestId validation, duplicate detection, hash,
> journal lifecycle, commit/failure semantics.
> Ia tidak boleh memutuskan bagaimana relay/schedule/PIR/config
> melakukan mutation.
>
> Dengan begitu ownership tetap jelas:
> handler → validate/domain mutation → RestJournalHelper → journal
> bukan:
> handler → generic helper → arbitrary business execution.

### 4.4 Helper ownership boundary (REV.3)

**Helper OWNS (journal lifecycle):**
- `validateRequestId(requestId)` — charset + length validation
- `computeCommandHash(doc, commandType)` — canonical hash (per-type schema)
- `checkDuplicateAndRespond(requestId, commandHash)` — dedup + ACK replay
- `storeIntentOrReject(requestId, commandHash, ...)` — PENDING entry
- `markExecutingOrAbort(requestId)` — EXECUTING transition
- `commitFromPendingOrFailure(requestId, ackJson)` — FROM_PENDING commit
- `commitExecutingOrFailure(requestId, ackJson)` — EXECUTING commit
- `clearEntryOnValidationFailure(requestId)` — pre-mutation clear

**Helper does NOT own (handler keeps):**
- Domain-specific validation (channelId range, time format, password strength)
- Actual mutation calls (`relayEngine.setManual`, `Storage::config.saveSchedule`, etc.)
- Response body construction (handler builds its own ACK JSON)
- HTTP response sending (`Web::http.send(200, ...)`)

### 4.5 Flow diagram (revised)

```
Handler entry
  ↓
[handler] requireAuth, requireCsrf, requireBody, parse JSON
  ↓
[handler] domain validation (channelId, time format, etc.)
  ↓
[helper] validateRequestId(requestId)         ──── sends 400 if invalid
  ↓
[helper] computeCommandHash(doc, type)        ──── returns hash string
  ↓
[helper] checkDuplicateAndRespond(rid, hash)  ──── sends 200 (replay) or 409 if dup
  ↓ (only if not duplicate)
[helper] storeIntentOrReject(rid, hash, ...)  ──── sends 503 if NVS fails
  ↓
[helper] markExecutingOrAbort(rid)             ──── sends 503 + clearEntry if fails
  ↓ (EXECUTING mode only)
[handler] ACTUAL MUTATION                      ──── domain-specific code
  ↓                                                (relayEngine.setManual, etc.)
[handler] build ACK JSON                       ──── domain-specific shape
  ↓
[helper] commit*OrFailure(rid, ackJson)        ──── sends 503 if commit fails
  ↓
[handler] Web::http.send(200, ackJson)         ──── success response
```

The helper calls frame the mutation, but the mutation itself is purely
in the handler's domain. The helper is a journal-lifecycle utility,
NOT a command dispatcher.

### 4.6 Anti-pattern check

If a future engineer tries to add a helper like:

```cpp
// ❌ FORBIDDEN — this would make the helper a business dispatcher
inline bool dispatchAndCommit(const String& requestId, const String& type,
                              const DynamicJsonDocument& doc) {
  if (strcmp(type, "relay") == 0) {
    relayEngine.setManual(...);
  } else if (strcmp(type, "schedule") == 0) {
    Storage::config.saveSchedule(...);
  } else if (...) {
    // ... 13 more branches ...
  }
  return commitTransaction(...);
}
```

This would re-create the dispatch problem. REV.3 forbids this — code
review must reject any such pattern. The helper must remain
journal-lifecycle-only.

---

## 5. Password credentialVersion — 4 Boundary Tests (REV.3-4)

Auditor's directive:

> Yang perlu diwajibkan:
> - password tidak pernah masuk commandHash;
> - password tidak pernah masuk ackJson;
> - password tidak pernah masuk journal;
> - credentialVersion hanya berubah setelah credential mutation berhasil;
> - retry dengan requestId yang sama tidak boleh melakukan credential mutation kedua kali.
>
> Saya akan CONDITIONAL ACCEPT, pending test yang membuktikan keempat
> boundary tersebut.

### 5.6 Boundary tests (mandatory)

| #  | Test                                                | Injection / Setup                                       | Assertion                                                                  |
|----|-----------------------------------------------------|---------------------------------------------------------|----------------------------------------------------------------------------|
| B1 | Password NOT in commandHash                         | Send POST /api/config/password with next="MySecret123"   | After success, grep `commandHash` field in journal NVS — must not contain "MySecret123" (raw or hashed) |
| B2 | Password NOT in ackJson                             | Same as B1                                              | Grep `ackJson` field in journal NVS — must not contain password string     |
| B3 | Password NOT in journal slot metadata               | Same as B1                                              | Grep ALL fields in slot (`requestId`, `commandHash`, `intentChannelId`, `intentDesiredState`, `intentPreviousKnown`) — none contain password |
| B4 | credentialVersion unchanged on failure              | Send password change that fails (wrong `current` password) | Read NVS key `cred_ver` before and after → unchanged                       |
| B5 | credentialVersion increments only on success         | Send valid password change                              | `cred_ver` before = N, after = N+1                                         |
| B6 | Retry with same requestId does NOT re-mutate        | Send valid password change (succeeds), then send SAME requestId+hash again | Second request returns 200 with replayed ACK. credentialVersion stays at N+1 (does NOT become N+2). `userConfig.passHashHex` is unchanged. |

### 5.7 Boundary test implementation note

Tests B1-B3 use a "password canary" technique: send a known distinctive
password string like "MySecretCanary123" then dump all NVS keys +
journal slots and search for the canary string. If found anywhere
outside the live `userConfig.passHashHex` (which is a PBKDF2 hash, not
plaintext), the test FAILS.

Test B4 uses failure injection: send wrong `current` password, verify
`AuthManager::changePassword` returns false, verify `cred_ver` unchanged.

Test B5 verifies the happy path: send valid password change, verify
`cred_ver` increments.

Test B6 verifies the dedup-replay path: after a successful password
change, send the EXACT same request again (same requestId, same hash).
ESP32 should detect duplicate via `checkDuplicateAndRespond` and replay
the original ACK. The mutation must NOT re-execute.

### 5.8 Why these tests are sufficient

- B1+B2+B3 cover "password never appears in journal" (3 storage locations)
- B4 covers "credentialVersion only changes on success" (failure path)
- B5 covers "credentialVersion increments on success" (happy path)
- B6 covers "retry doesn't re-mutate" (idempotency guarantee)

Together, these prove the credentialVersion-based hash schema is safe.

---

## 6. Factory Reset — Crash Window Safety Proof (REV.3-5)

Auditor's directive:

> Pernyataan "COMMITTED unreachable by design karena journal dihancurkan"
> bisa diterima hanya jika recovery semantics benar-benar dibuktikan,
> bukan sekadar didokumentasikan.
>
> Test harus mencakup crash window: PENDING → EXECUTING → wipe
> dan retry setelah reboot.
>
> Khususnya harus dibuktikan bahwa retry tidak menyebabkan operasi
> yang tidak aman, bukan hanya bahwa command "idempotent" secara teori.

### 6.6 Crash window safety tests (mandatory)

| #  | Crash window simulated                                                | Test procedure                                                                                              | Assertion                                                                                          |
|----|------------------------------------------------------------------------|-------------------------------------------------------------------------------------------------------------|-----------------------------------------------------------------------------------------------------|
| CR1 | PENDING (crash between storeIntent and markExecuting)                    | 1. Call factory_reset_prepare, get token<br>2. Call factory_reset_confirm with shim that aborts AFTER storeIntent but BEFORE markExecuting<br>3. Reboot test (re-init journal)<br>4. Send same requestId again | New request returns 409 "in progress" or "ambiguous" (NOT 200 silent success, NOT re-wipe)         |
| CR2 | EXECUTING (crash between markExecuting and wipe)                       | Same as CR1 but abort AFTER markExecuting, BEFORE wipe                                                       | New request sees EXECUTING state → returns 409. Operator must investigate. Wipe did NOT happen.    |
| CR3 | Partial wipe (abort mid-wipe)                                          | Inject failure into `Storage::config.resetChannels`                                                         | Device enters safe mode. Manual recovery required. Journal may be quarantined.                       |
| CR4 | Full wipe complete, no restart                                          | Allow full wipe, but `ESP.restart()` is shimmed to no-op                                                     | Journal is empty. State is factory defaults. Operator can manually reboot.                          |
| CR5 | Retry after successful wipe (case F/G from §6.2)                       | 1. Run factory_reset_confirm to completion (wipe + restart shim)<br>2. Send SAME requestId again             | Second request: storeIntent creates new PENDING (journal was wiped). markExecuting. wipe runs again. **Safe because idempotent** — device is already in factory state, so `Drivers::relay.allOff()`, `Storage::config.resetChannels()`, `initDefaultUserConfig()` are all no-ops on already-reset device. credentialVersion stays at 0 (was wiped). |
| CR6 | Retry after PARTIAL wipe (case E from §6.2)                            | 1. Inject failure mid-wipe (e.g., resetChannels succeeds but initDefaultUserConfig fails)<br>2. Reboot<br>3. Send same requestId | Journal may be quarantined (CRC mismatch on partially-wiped slots). Test verifies device does NOT silently re-execute. Operator must investigate via serial console. |

### 6.7 "Safe" definition for retry

A retry is SAFE if and only if:
1. The device state after retry is identical to device state after
   successful first attempt, OR
2. The retry is blocked (409 "in progress" or "ambiguous") and the
   operator is notified.

CR5 satisfies criterion 1 — idempotent re-run on already-reset device.
CR1, CR2 satisfy criterion 2 — blocked retry.
CR3, CR6 satisfy neither — operator must investigate (acceptable for
destructive operations; cannot be auto-recovered safely).

### 6.8 Why CR5 is provably safe

After a successful factory reset:
- `Core::channels[i]` are all at defaults (empty schedules, default names)
- `Core::userConfig` is at default admin/admin state
- `Core::deviceName` is "Timer12CH" (default)
- Journal NVS keys are all erased

A retry sends same requestId + same hash. ESP32:
1. `validateRequestId` → passes
2. `computeCommandHash` → produces same hash as original (command is identical)
3. `checkDuplicateAndRespond` → journal is empty, no duplicate → falls through
4. `storeIntent` → creates new PENDING (slot 0, gen 1)
5. `markExecuting` → EXECUTING
6. `Drivers::relay.allOff()` → all relays already off, no-op
7. `Drivers::pir.resetAll()` → PIR state already reset, no-op
8. `Storage::config.resetChannels()` → already at defaults, no-op
9. `Storage::config.saveSchedule(true)` → writes default schedule file (already default)
10. `Storage::config.initDefaultUserConfig()` → already at defaults, no-op
11. `Storage::config.saveUserConfig()` → writes default user config (already default)
12. Wipe journal NVS keys (already erased, no-op)
13. `ESP.restart()`

Net effect: device reboots one more time. State is unchanged from
post-reset state. Safe.

### 6.9 Test for CR5 acceptance

The CR5 test must explicitly verify:
- `Core::channels[0].schedCount` is 0 before AND after retry
- `Core::userConfig.wwwUser` is "admin" before AND after retry
- `Core::deviceName` is "Timer12CH" before AND after retry
- `Services::journal.getJournalSize()` is 0 before retry
- After retry completes (restart shim), all of the above are STILL at defaults

This proves retry did not change anything — it just re-ran the reset
on an already-reset device.

---

## 7. Universal Failure Invariant Matrix (unchanged from REV.2)

(No revision — auditor accepted this section unconditionally.)

---

## 8. Reboot Lifecycle (unchanged from REV.2)

(No revision — auditor accepted this section unconditionally.)

---

## 9. HTTP-Response ↔ Journal-Commit Hard Invariant (REV.3-7)

Auditor's directive:

> Ini harus menjadi hard invariant:
> mutation success + journal commit success → HTTP 200 success:true
> mutation happened + journal commit failed → HTTP 5xx / durability failure
> Tidak boleh: mutation succeeded + journal commit failed + HTTP 200 success:true
> karena itu menciptakan false-success yang justru bertentangan dengan tujuan F-P0-2.

### 9.4 Hard invariant (formal specification)

**INVARIANT HTTP-200-AFTER-COMMIT:**

For every REST mutation endpoint, the following implication MUST hold:

```
Web::http.send(200, ..., ackJson)
  ↓ implies
journal.getTransactionState(requestId) == TransactionState::COMMITTED
  AND
journal.getAckJson(requestId) == ackJson
```

Equivalently (contrapositive):

```
journal.getTransactionState(requestId) != COMMITTED
  ↓ implies
Web::http.send was NOT called with status 200
```

### 9.5 Implementation enforcement

The shared helper `commit*OrFailure` is the ONLY function that returns
true (success) to the handler, allowing the handler to send HTTP 200.
If the helper returns false, the handler MUST NOT send HTTP 200 — the
helper has already sent HTTP 5xx.

```cpp
// In handler:
if (!Web::Rest::commitExecutingOrFailure(requestId, ackJson)) {
  // Helper already sent HTTP 503. Do NOT send another response.
  return;
}
// Helper returned true — commit succeeded. NOW we can send HTTP 200.
Web::sendSecurityHeaders();
Web::http.send(200, "application/json", ackJson);
```

### 9.6 Code review check

The code review for Phase C must verify (cross-cutting test X6 from §10.1.4):

For every mutation handler in `firmware/*Handlers.h`:
- Search for `Web::http.send(200` calls
- Each one MUST be preceded by a successful `commit*OrFailure` call
- If a handler sends HTTP 200 without prior commit, that's a CRITICAL bug

This can be enforced via grep-based lint:
```bash
# For each handler file, check that every send(200 is preceded by commit*OrFailure
# within the same function scope. Manual review required (or AST analysis).
```

### 9.7 Test for invariant (test X6 in §10.1.4)

Test X6 explicitly verifies this by source review:
- Open each handler file
- For each `Web::http.send(200` call, verify there's a `commit*OrFailure`
  call earlier in the same function with `if (!commit...) return;`
- Any violation is a test failure

This is a static check, not a runtime test. It complements the dynamic
tests (F5, F6) which verify HTTP 5xx is returned when commit fails.

---

## 10. Test Acceptance — Semantic Branch Coverage (REV.3-8)

Auditor's directive:

> 46 adalah minimum, bukan acceptance criterion final.
> Acceptance harus berdasarkan semantic branches, terutama:
> every mutation endpoint; every CommitMode; duplicate same hash;
> duplicate different hash; store failure; mutation failure; commit A
> failure; commit B failure; ACK persistence failure; reboot; factory
> reset; credential mutation; schedule persistence.

### 10.4 Semantic branch acceptance criteria

The acceptance criterion is NOT a test count — it's coverage of every
semantic branch in the matrix below. Each branch MUST have at least
one test that exercises the actual production handler.

| # | Semantic branch                                            | Required tests                                           |
|---|-----------------------------------------------------------|----------------------------------------------------------|
| 1 | Every mutation endpoint (16 total)                         | P1-P17 (production routing, at least 1 per endpoint)     |
| 2 | Every CommitMode (NONE / FROM_PENDING / EXECUTING / special) | At least 1 test exercising each mode                   |
| 3 | Duplicate same hash → replay ACK                          | D1 (COMMITTED), D2 (PENDING), D3 (EXECUTING)             |
| 4 | Duplicate different hash → reject                         | D4                                                       |
| 5 | Store failure                                              | F1 (copy A), F2 (copy B)                                  |
| 6 | markExecuting failure                                      | F3 (copy A), F4 (copy B)                                  |
| 7 | Mutation succeeded + commit A failure                     | F5                                                       |
| 8 | Mutation succeeded + commit B failure                      | F6                                                       |
| 9 | ACK persistence failure                                    | F12 (ackq_hdr write fails)                               |
| 10 | Validation failure after storeIntent → clearEntry        | F7, F8, F9, F10 (different validation failures)          |
| 11 | Reboot lifecycle (FROM_PENDING, no dequeue, restart)      | P14 (production), X4 (no-dequeue cross-cutting)          |
| 12 | Factory reset (EXECUTING special, no COMMITTED)           | P17, CR1-CR6 (crash windows), X5 (no-COMMITTED cross-cutting) |
| 13 | Credential mutation (password change, credentialVersion) | P12, B1-B6 (boundary tests)                              |
| 14 | Schedule persistence (synchronous saveSchedule)           | P5, F11 (saveSchedule failure)                           |
| 15 | Missing/malformed requestId                               | D5, D6, D7                                               |
| 16 | Read-only endpoint (NONE mode)                            | P15 (ota/check), P16 (factory_reset/prepare)             |
| 17 | Cross-ingress semantic equivalence (MQTT ↔ REST)         | X8-X14 — see §11.5                                       |

### 10.5 Test count re-estimated

Based on semantic branches, minimum test count is:
- 17 production routing (P1-P17)
- 8 duplicate handling (D1-D8 — added D8 for "duplicate after reboot")
- 14 failure injection (F1-F14)
- 7 cross-cutting (X1-X7)
- 6 credential boundary (B1-B6)
- 6 factory reset crash window (CR1-CR6)
- 7 cross-ingress equivalence (X8-X14 — see §11.5)

Total: 17 + 8 + 14 + 7 + 6 + 6 + 7 = **65 tests minimum** (up from 46 in REV.2)

This is STILL a minimum, not a ceiling. Auditor may add more.

### 10.6 Acceptance is coverage, not count

Even if 65 tests pass, the design is NOT accepted if any semantic branch
in §10.4 lacks coverage. The acceptance criterion is: every row in
§10.4 has at least one test, AND all tests pass.

---

## 11. Cross-Ingress Semantic Contract Matrix (REV.3-1 — NEW SECTION)

Auditor's directive:

> Saya belum memberi izin Phase C sampai satu revisi design kecil
> ditambahkan: §11 — Cross-Ingress Semantic Contract Matrix (MQTT ↔ REST)
> dan tambahkan acceptance criteria bahwa REST mutation harus menghasilkan
> semantic journal state yang ekuivalen dengan MQTT untuk command yang
> ekuivalen.

### 11.1 Why this matters

The auditor's deeper observation:

> F-P0-2 sebenarnya bukan sekadar "menambahkan journal ke REST". Ia adalah
> contract synchronization antara dua command ingress.

If MQTT and REST have different journal semantics for the "same" logical
command (e.g., relay on/off), then:
- A PWA could send the same command via MQTT (remote) and REST (LAN)
  with the same requestId
- The two paths would produce different journal states
- Dedup would break: the second path would either reject (false 409) or
  re-execute (false success)
- Audit trail becomes inconsistent

The contract must be: **for equivalent commands, MQTT and REST produce
identical journal state.**

### 11.2 Cross-Ingress Semantic Contract Matrix

For every semantic dimension, MQTT and REST MUST produce identical
behavior. The matrix below is the formal contract.

| Semantic dimension                          | MQTT (F-P0-1 APPROVED)                                            | REST (F-P0-2 REV.3)                                              | Required equivalence                                   |
|---------------------------------------------|-------------------------------------------------------------------|------------------------------------------------------------------|--------------------------------------------------------|
| requestId source                            | PWA generates UUID via `crypto.randomUUID()` in `sendCommandWithAck` | PWA generates UUID at mutation hook boundary (§1.3.1)          | Identical: client-generated UUID                       |
| requestId validation (charset, length)       | `_handleCommand` lines 876-898 (MqttClient.cpp)                    | `Web::Rest::validateRequestId` (same rules)                     | Identical: [a-zA-Z0-9-_], 1-64 chars                   |
| requestId required?                          | YES (rejected with 400 if missing)                                 | YES (rejected with 400 if missing)                                | Identical: required                                    |
| commandHash computation                      | `_computeCommandHash` per-type canonical schema (MqttClient.cpp lines 2126-2175) | `Web::Rest::computeCommandHash` (same canonical schemas)        | Identical: same canonical string for same logical command |
| commandHash stored in journal                | YES (in `commandHash` field of slot record)                        | YES (same field, same slot structure)                            | Identical: same NVS key structure                      |
| Duplicate (same hash) → COMMITTED            | Replay ACK from journal (`getAckJson`)                              | Replay ACK from journal (`getAckJson`)                            | Identical: replay original ACK JSON                    |
| Duplicate (same hash) → PENDING              | Surface "in progress" (return, no re-execute)                       | HTTP 409 "in progress"                                            | Identical: no re-execute, surface to client            |
| Duplicate (same hash) → EXECUTING            | Reconcile to UNKNOWN, surface "ambiguous"                           | HTTP 409 "ambiguous"                                              | Identical: no re-execute, surface to client            |
| Duplicate (different hash) → security reject | `_publishAck(rid, false, "requestId reuse with different command")` + Log AuthFail | HTTP 409 "requestId reuse rejected" + Log AuthFail                | Identical: reject + audit log                          |
| Validation failure BEFORE storeIntent        | Return (no journal entry)                                          | HTTP 4xx (no journal entry)                                       | Identical: no journal state change                     |
| Validation failure AFTER storeIntent         | `clearEntry(requestId)` → EMPTY                                    | `clearEntryOnValidationFailure` → EMPTY                          | Identical: journal slot cleared                        |
| Mutation succeeded + commit A failure        | State stays EXECUTING (mutation evidence preserved, NO clearEntry) | HTTP 503, state stays EXECUTING (NO clearEntry)                   | Identical: evidence preserved                         |
| Mutation succeeded + commit B failure        | State stays EXECUTING (NO clearEntry)                              | HTTP 503, state stays EXECUTING (NO clearEntry)                   | Identical: evidence preserved                         |
| Read-only command (getStatus / ota/check)   | `CommitMode::NONE`, no journal entry                               | `CommitMode::NONE`, no journal entry                              | Identical: no journal state change                     |
| Reboot lifecycle                             | FROM_PENDING: commitTransactionFromPending → ACK queued (NOT dequeued) → ESP.restart() | FROM_PENDING: commitTransactionFromPending → ACK queued (NOT dequeued) → ESP.restart() | Identical: same lifecycle, ACK not dequeued            |
| Destructive operation (OTA / factory_reset) | Explicit lifecycle (EXECUTING → COMMITTED for OTA success; PENDING→EXECUTING→wipe for factory_reset) | Explicit lifecycle (mirrors MQTT)                                 | Identical: explicit EXECUTING phase                    |
| ACK JSON envelope shape                      | `{requestId, success, message, timestamp, data}` (MQTT topic)      | `{success, message, data:{...,requestId}}` (HTTP body)            | Semantically equivalent: requestId + success + message + data |
| Failure ACK semantics                        | `success:false` published to MQTT topic, NOT stored in journal      | HTTP 4xx/5xx with `success:false` body, NOT stored in journal      | Identical: failure ACKs are not durable                |

### 11.3 Equivalence acceptance criterion

For each command type that exists in BOTH MQTT and REST:

```
Given: same logical command (e.g., relay on, channelId=1)
   AND: same requestId
   AND: same commandHash (canonical schemas match)
When: command processed via MQTT path
  AND: command processed via REST path (fresh journal each time)
Then: journal.getTransactionState(requestId) is identical
  AND: journal.getAckJson(requestId) is semantically equivalent
       (same success flag, same message, same data fields modulo envelope shape)
  AND: side effects on device state are identical
       (e.g., relayState[0] == true in both cases)
```

### 11.4 Canonical hash schema unification

To guarantee hash equivalence, the canonical schemas used by MQTT
(`_computeCommandHash` in MqttClient.cpp) and REST
(`computeCommandHash` in RestJournalHelper.h) MUST be byte-for-byte
identical for each command type.

**Implementation strategy:** extract the canonical schema logic into
a shared function that both MQTT and REST call. This avoids divergence.

Two options:

**Option α-shared (recommended):** Move `_computeCommandHash` from
`MqttClient.cpp` (private static) to a shared header
(e.g., `firmware/CommandHash.h`). Both MQTT and REST include it.

**Option β-duplicate:** Each path has its own copy of the canonical
schemas. Tested for equivalence via X8 (see §11.5).

**Decision:** Option α-shared. Single source of truth. No risk of
divergence. F-P0-1 MQTT path is unchanged (the function just moves
file location, behavior identical).

**Note for auditor:** Moving `_computeCommandHash` to a shared header
is technically a "change to MQTT path", but it's a refactor with no
behavioral change — the function body is identical. The F-P0-1
APPROVED tests (MqttClientTest TEST 1-13) will still pass because
the function still produces the same hashes. We will re-run the full
MqttClientTest suite to prove no regression.

### 11.5 Cross-ingress equivalence tests

| #  | Test                                                                                          | Procedure                                                                                       | Assertion                                                              |
|----|-----------------------------------------------------------------------------------------------|-------------------------------------------------------------------------------------------------|------------------------------------------------------------------------|
| X8 | Canonical hash equivalence for `relay`                                                        | Send relay-on command via MQTT _handleCommand and via REST handleRelay (fresh journal each)     | `journal.getCommandHash(requestId)` is byte-identical in both paths    |
| X9 | Canonical hash equivalence for `schedule`                                                     | Same as X8 but for schedule upsert                                                              | Hashes identical                                                       |
| X10 | Canonical hash equivalence for `pir`                                                         | Same as X8 but for pir config                                                                   | Hashes identical                                                       |
| X11 | Canonical hash equivalence for `channel`                                                      | Same as X8 but for channel rename                                                               | Hashes identical                                                       |
| X12 | Canonical hash equivalence for `time`                                                        | Same as X8 but for time set                                                                     | Hashes identical                                                       |
| X13 | Canonical hash equivalence for `config` (setDevice)                                          | Same as X8 but for config setDevice                                                             | Hashes identical                                                       |
| X14 | Journal state equivalence — relay via MQTT vs REST                                            | Send relay-on via MQTT, capture journal state. Reset. Send relay-on via REST with same requestId+hash. | `getTransactionState`, `getCommandHash`, `getAckJson` all identical (modulo envelope shape) |

### 11.6 Cross-ingress divergence detection (lint check)

During Phase C implementation, a CI check should verify that the
canonical schema strings in `_computeCommandHash` (MQTT) and
`computeCommandHash` (REST) are identical. If Option α-shared is used,
this is automatically satisfied (single source). If Option β-duplicate
is used, a test must compare the schemas.

**Decision:** Option α-shared eliminates this check entirely. Recommended.

### 11.7 Cross-ingress contract summary

The cross-ingress contract is the formal closure of F-P0-2:

> The TransactionJournal is the single source of truth for command
> durability and dedup. Both MQTT and REST are ingress paths that
> produce identical journal state for equivalent commands. The journal
> does not care which path a command came from — only that it has a
> valid requestId and commandHash.

This is the architectural statement that F-P0-2 closes. Without §11,
F-P0-2 would only "add journal to REST" — but with §11, F-P0-2
synchronizes the two ingress paths into a unified contract.

---

## 12. Implementation Plan (Phase C — still blocked, REV.3 plan)

Sequential, no parallelism. Will not start until auditor approves Phase B REV.3.

1. Extract `_computeCommandHash` from `MqttClient.cpp` to shared `firmware/CommandHash.h`. (Option α-shared from §11.4.) Re-run MqttClientTest to verify no regression.
2. Build `firmware/RestJournalHelper.h` with the 8 helper functions (journal lifecycle only — see §4.4 ownership boundary).
3. Modify `Common.h` — add `requireRequestId()` wrapper (calls helper).
4. Modify each mutation handler to use the helper-based skeleton (§4.5 flow).
5. Build `WebServerTest.cpp` + `Makefile.ws` — production-path proof (P1-P17, 17 tests).
6. Add duplicate-handling tests (D1-D8, 8 tests).
7. Add failure-injection tests (F1-F14, 14 tests).
8. Add cross-cutting tests (X1-X7, 7 tests).
9. Add credential boundary tests (B1-B6, 6 tests).
10. Add factory reset crash window tests (CR1-CR6, 6 tests).
11. Add cross-ingress equivalence tests (X8-X14, 7 tests).
12. Regression run: TransactionJournal 194 + CommandRouting 133 + MqttClient 31 + WebServer ≥65 = ≥423 assertions.

**Phase C sub-phases (revised):**
- C1: Extract shared CommandHash + build RestJournalHelper + handler refactor
- C2: WebServerTest production-path tests (P1-P17)
- C3: Duplicate + failure tests (D1-D8, F1-F14)
- C4: Cross-cutting + credential + factory reset tests (X1-X7, B1-B6, CR1-CR6)
- C5: Cross-ingress equivalence tests (X8-X14) — needs MQTT path included in test harness
- C6: Regression run + final patch

Each Ci phase produces a patch for auditor review before proceeding to Ci+1.

---

## 13. Status

**Phase A (Discovery): COMPLETE**
**Phase B REV.1: COMPLETE → auditor CONDITIONAL NO-GO (6 revisions)**
**Phase B REV.2: COMPLETE → auditor CONDITIONAL GO (1 mandatory addition + 7 strengthenings)**
**Phase B REV.3: COMPLETE** — this document, addresses all 8 directives including §11.

**Awaiting auditor review of REV.3.**

If approved: Phase C (implementation) begins per §12 plan.

---

## 14. Auditor Decision Needed (REV.3 — reduced from 10 to 4)

Given that 8 of the 10 prior decisions are now conditionally accepted
(with the strengthening revisions in REV.3), the remaining decisions
are:

1. **Approve §11 Cross-Ingress Semantic Contract Matrix** — sufficient to
   guarantee MQTT ↔ REST semantic equivalence?
2. **Approve Option α-shared** (extract `_computeCommandHash` to shared header) —
   or require Option β-duplicate with equivalence test?
3. **Approve Phase C plan** (6 sub-phases, each producing patch for review)?
4. **Approve test acceptance as coverage-based** (§10.4 semantic branches,
   not count-based) — minimum 65 tests, no ceiling?

On approval of all 4, Phase C1 begins.

---

## 15. What This Revision Does NOT Do

- Does NOT change MQTT path behavior (F-P0-1 APPROVED tests must still pass)
- Does NOT introduce CommandIngress abstraction (Option γ rejected)
- Does NOT add new TransactionJournal API methods (existing API sufficient)
- Does NOT change authentication, CSRF, CORS, or TLS posture
- Does NOT touch read-only endpoints (GET) — they don't mutate state
- Does NOT enable REST OTA in production (still hard-blocked in PROD_BUILD)

The only production code change in C1 is extracting `_computeCommandHash`
to a shared header — a pure refactor with no behavioral change.
