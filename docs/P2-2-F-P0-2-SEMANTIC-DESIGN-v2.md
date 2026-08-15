# F-P0-2 — SEMANTIC DESIGN REV.2 (Phase B Revision)

**Finding ID:** F-P0-2
**Title:** REST API bypasses TransactionJournal — Durability Closure
**Phase:** B (Semantic Design — REV.2 in response to auditor's CONDITIONAL NO-GO)
**Date:** 2026-08-15
**Predecessor:** F-P0-1 APPROVED (MQTT lifecycle closure)
**Discipline:** semantic design → production-path proof → failure-path proof → regression

---

## 0. Revision Summary

Auditor returned CONDITIONAL NO-GO with 6 required revisions. This REV.2
incorporates all 6 plus the additional directives:

| # | Auditor directive                                                              | Section |
|---|--------------------------------------------------------------------------------|---------|
| 1 | Add PWA REST requestId caller map (verify UUID survives retry)                 | §1      |
| 2 | Add factory-reset crash/retry semantic matrix (3 crash windows)                | §6      |
| 3 | Add password-change canonical hash with credentialVersion (no plaintext)       | §5      |
| 4 | Add complete mutation/failure invariant matrix (shared across 16 endpoints)    | §7      |
| 5 | Add REST HTTP-response ↔ journal-commit contract (HTTP 200 only after commit)  | §9      |
| 6 | Expand test acceptance criteria — coverage matrix, not fixed count             | §10     |
| 7 | EXECUTING = "may produce unsafe-to-repeat side effects" (not just physical)    | §3      |
| 8 | Shared helper internal (avoid contract drift across 16 endpoints)              | §4      |
| 9 | Reboot lifecycle must retain F-P0-1 invariant (no ACK dequeue before restart)  | §8      |

NO production code changes in this revision — still DESIGN-ONLY.

---

## 1. PWA REST requestId Caller Map (REV.1)

### 1.1 Current state (audited from pwa-repo/ source)

The PWA has two command transport modes:

| Mode | File                          | requestId source                                  | Retry mechanism                                          |
|------|-------------------------------|---------------------------------------------------|----------------------------------------------------------|
| MQTT | `src/lib/mqttTransaction.ts`  | `crypto.randomUUID()` in `sendCommandWithAck`     | Pending map keyed by requestId, 5s timeout, single try   |
| REST | `src/lib/api.ts`              | **NONE — requestId never added to body**           | **NONE — user must click again**                          |

REST mode today has:
- No `requestId` field in `RelayMutation`, `Schedule`, or any other mutation type.
- No retry logic — every mutation is single-shot via `fetch()`.
- No dedup mechanism — same logical click sent twice produces two mutations.

### 1.2 Caller chain (current REST path)

```
React component (e.g., relay-toggle.tsx)
    ↓ calls useRelayMutation().mutate(mutation)
useApi.ts (useRelayMutation)
    ↓ mutationFn calls api.relay(mutation)
api.ts (api.relay)
    ↓ request<{channel}>('/api/relay', { method: 'POST', body: mutation })
fetch() — body is JSON.stringify(mutation)
    ↓ HTTP POST /api/relay with body {channelId, action, mode?, manualState?}
ESP32 handleRelay() — receives body, has no requestId
```

Same pattern for every other REST mutation endpoint. The body never contains
a `requestId` field.

### 1.3 Required PWA changes (before F-P0-2 Phase C implementation)

The PWA must be updated in lockstep with firmware. Specific changes:

**1.3.1 — Generate requestId at mutation boundary (NOT in api.ts):**

The requestId must be created at the React component / mutation hook boundary
so the same logical click uses the same UUID across retries. `api.ts` should
accept a `requestId` parameter (not generate one itself):

```typescript
// src/lib/api.ts (revised)
relay: (mutation: RelayMutation, requestId: string) =>
  request<{ channel: Channel }>('/api/relay', {
    method: 'POST',
    body: { ...mutation, requestId },  // requestId merged into body
  }),
```

**1.3.2 — useApi.ts hooks generate requestId once per logical mutation:**

```typescript
// src/hooks/useApi.ts (revised — useRelayMutation example)
export function useRelayMutation() {
  const qc = useQueryClient();
  const { t } = useLanguage();
  const { isMqttMode } = useAuth();

  return useMutation({
    mutationFn: async (mutation: RelayMutation) => {
      // ONE requestId per logical mutation — same UUID for retry
      const requestId = crypto.randomUUID();
      
      if (isMqttMode) {
        // MQTT path already uses sendCommandWithAck which generates its own
        // requestId internally — but we want to share it with REST retry.
        // Decision: keep MQTT path's internal generation (P2-2 doesn't touch
        // F-P0-1 APPROVED path). For REST path below, use our generated UUID.
        const ack = await sendCommandWithAck({ /* ... */ });
        return { channel: null, ack } as const;
      }
      
      // REST path — retry with SAME requestId
      const channel = await apiRelayWithRetry(mutation, requestId);
      return { channel, ack: undefined } as const;
    },
    // ...
  });
}

// New helper: REST retry with same requestId
async function apiRelayWithRetry(mutation: RelayMutation, requestId: string): Promise<Channel> {
  const MAX_RETRIES = 3;
  const RETRY_DELAYS = [1000, 2000, 4000]; // exponential backoff
  
  for (let attempt = 0; attempt <= MAX_RETRIES; attempt++) {
    try {
      return await api.relay(mutation, requestId);
    } catch (err) {
      const isLast = attempt === MAX_RETRIES;
      const isNetwork = err instanceof ApiError && err.status === 0;
      const isTimeout = err instanceof ApiError && err.status >= 500;
      
      if (isLast || (!isNetwork && !isTimeout)) {
        throw err;  // permanent failure — surface to UI
      }
      // Network/timeout error — retry with SAME requestId
      await new Promise(r => setTimeout(r, RETRY_DELAYS[attempt]));
    }
  }
  throw new Error('unreachable');
}
```

**1.3.3 — Same pattern for ALL mutation hooks:**

| Hook                           | requestId generated where              | Retry policy                                  |
|--------------------------------|----------------------------------------|------------------------------------------------|
| `useRelayMutation`             | mutationFn (UUID per click)            | 3 retries, same requestId, exponential backoff |
| `useRenameChannel`             | mutationFn (UUID per click)            | 3 retries                                      |
| `useScheduleMutation`          | mutationFn (UUID per click)            | 3 retries                                      |
| `useScheduleDelete`            | mutationFn (UUID per click)            | 3 retries                                      |
| `usePirMutation`               | mutationFn (UUID per click)            | 3 retries                                      |
| `usePirTest`                   | mutationFn (UUID per click)            | 3 retries                                      |
| `useTimeMutation`              | mutationFn (UUID per click)            | 3 retries                                      |
| `useReboot`                    | mutationFn (UUID per click)            | NO retry — reboot is destructive               |
| `useDeviceConfigMutation`      | mutationFn (UUID per click)            | 3 retries                                      |
| `useChangePassword`            | mutationFn (UUID per click)            | 3 retries                                      |
| `useImportConfig`              | mutationFn (UUID per click)            | NO retry — bulk overwrite is destructive       |
| `useFactoryResetConfirm`       | mutationFn (UUID per click)            | NO retry — destructive                         |

**1.3.4 — Distinguish "destructive" from "retryable" mutations:**

Destructive mutations (reboot, factory_reset_confirm, config/import) MUST NOT
retry automatically. Reasoning:
- Reboot retry after device comes back: PWA can't tell if the original reboot
  happened or not. Better to wait for device reconnect then check status.
- Factory reset retry: if the reset partially completed, retry could corrupt
  a fresh journal.
- Config import retry: bulk overwrite — could overwrite a different config
  that was set in the meantime.

PWA shows explicit "device may be restarting, please wait..." UI for these.

### 1.4 Firmware-side requestId validation rules

Firmware validates `requestId` per the same rules as MQTT `_handleCommand`
(MqttClient.cpp lines 876-898), to ensure cross-path contract symmetry:

```
1. requestId must be present in body            → else HTTP 400
2. requestId.length() must be in [1, 64]        → else HTTP 400
3. requestId must match [a-zA-Z0-9-_]+          → else HTTP 400
4. (No UUID format enforcement — non-UUID still works,
    just not recommended. PWA uses UUID.)
```

### 1.5 Backward compatibility

- Old PWA builds (no requestId in body) → firmware responds HTTP 400.
- PWA must be updated BEFORE firmware is deployed.
- A build flag `ALLOW_MISSING_REQUEST_ID_REST` (defined in dev builds only,
  NEVER in PRODUCTION_BUILD) permits missing requestId for testing convenience.
  Production builds always require it.

### 1.6 Caller map summary (what we proved)

```
React click → useApi hook → api.ts → fetch → ESP32
                ↑              ↑               ↑
                generates     passes          validates
                UUID once      through        (charset, length)
                              body
                                              dedup check via
                                              journal.isProcessed
                                              
                ┌────── retry keeps same UUID ──────┐
                ↓                                    ↓
            attempt 1 fails (timeout) → attempt 2 (same UUID) → ESP32 sees duplicate
                                                → replays ACK, no double mutation
```

**Verified:** requestId survives retry. Same logical click = same UUID. No
server-side generation. Cross-path contract symmetry with MQTT.

---

## 2. Open: Awaiting confirmation that PWA contract audit is sufficient

The auditor may want additional verification:
- Does the PWA need to handle `409 Conflict` (requestId in progress) by polling?
- Does the PWA need to display "your last action was already applied" when
  an ACK replay arrives?
- What is the user-visible behavior when retry returns the original ACK?

**These are PWA UX questions, not firmware questions.** Firmware contract
is: same requestId → same response. PWA decides how to display it.

---

## 3. EXECUTING Definition Refined (REV.7)

Auditor's correction:

> "EXECUTING = physical mutation" tidak boleh menjadi definisi universal.
> EXECUTING harus berarti: state di mana command sudah memasuki fase yang
> mungkin menghasilkan side effect yang tidak aman untuk diulang secara
> otomatis.

### 3.1 Revised definition

```
EXECUTING = command has entered a phase where retrying it (with the same
            requestId) may produce unsafe or irreversible side effects.

            This includes:
              - Physical GPIO mutation (relay on/off)
              - Physical RTC write (time set)
              - Bulk NVS overwrite (config import)
              - Destructive wipe (factory reset confirm)
              - Flash write (OTA)
              - Device restart (reboot)
            
            It does NOT include:
              - Pure RAM mutations that are atomic and idempotent
                (channel rename, schedule upsert, PIR config, device config,
                 password change — these mutate RAM + NVS in a single
                 deterministic operation)
```

### 3.2 Why EXECUTING vs FROM_PENDING matters

Both modes call `storeIntent` (PENDING) before mutation. The difference is
whether `markExecuting` is called between PENDING and the mutation:

```
FROM_PENDING:   PENDING ─→ [atomic mutate + commit] → COMMITTED
                          (single step — no side effects observable before commit)

EXECUTING:      PENDING ─→ markExecuting ─→ [mutate] ─→ commitTransaction → COMMITTED
                          ↑                              ↑
                          durable evidence              durable evidence
                          that mutation started          that mutation completed
```

EXECUTING is required when the mutation phase can be observed externally
(e.g., GPIO goes HIGH and a relay clicks) and may not be safely repeated.
If a crash happens between PENDING and COMMITTED, the journal shows
EXECUTING — a recovery handler can decide whether to re-apply or surface
the ambiguity.

FROM_PENDING is sufficient when the mutation is atomic (single NVS write
or single RAM swap with no observable intermediate state).

### 3.3 Revised matrix with retry-safety + irreversibility columns

| #  | Endpoint                       | Method  | Commit mode     | Retry-safe? | Side effect irreversible? | Rationale                                |
|----|--------------------------------|---------|------------------|-------------|---------------------------|------------------------------------------|
| 1  | `/api/relay`                   | POST    | EXECUTING        | YES (idempotent) | YES (GPIO state visible)         | Physical mutation observed externally    |
| 2  | `/api/channel`                 | POST    | FROM_PENDING     | YES         | NO (rename is overwritable)         | Atomic RAM + NVS write                  |
| 3  | `/api/schedule`                | POST    | FROM_PENDING     | YES         | NO (upsert overwrites)              | Atomic RAM + NVS write                  |
| 4  | `/api/schedule`                | DELETE  | FROM_PENDING     | YES         | NO (slot can be re-added)           | Atomic RAM + NVS write                  |
| 5  | `/api/pir`                     | POST    | FROM_PENDING     | YES         | NO (config overwritable)            | Atomic RAM + NVS write                  |
| 6  | `/api/pir/test`                | POST    | EXECUTING        | YES (idempotent) | YES (transient trigger observed) | Physical transient — relay may click    |
| 7  | `/api/time`                    | POST    | FROM_PENDING     | perlu review | NO (RTC adjustable)                | RTC write — atomic from caller's view   |
| 8  | `/api/config`                  | POST    | FROM_PENDING     | YES         | NO (user/pass overwritable)         | Atomic NVS write                       |
| 9  | `/api/config/device`           | POST    | FROM_PENDING     | YES         | NO (overwritable)                   | Atomic NVS write                       |
| 10 | `/api/config/password`         | POST    | FROM_PENDING     | YES         | NO (password re-changeable)        | Atomic NVS write                       |
| 11 | `/api/config/import`           | POST    | EXECUTING        | NO          | YES (bulk overwrite)                | Destructive — no auto retry             |
| 12 | `/api/reboot`                  | POST    | FROM_PENDING*    | NO          | YES (device restarts)               | Special lifecycle — see §8              |
| 13 | `/api/ota`                      | POST    | (n/a — disabled) | n/a         | n/a                                  | Hard-blocked in PROD                    |
| 14 | `/api/ota/check`               | POST    | NONE             | YES         | NO (read-only query)                | No mutation → no journal               |
| 15 | `/api/factory_reset/prepare`   | POST    | NONE             | YES         | NO (token generation only)           | No state mutation → no journal         |
| 16 | `/api/factory_reset/confirm`   | POST    | EXECUTING (special) | NO        | YES (NVS wipe)                       | Special lifecycle — see §6             |

**`/api/time` retry-safety "perlu review"**: RTC writes ARE idempotent
(setting the same time twice is safe), but the concern is if the user
manually adjusted the RTC between retries. The journal would replay
"set time to X" even if user has since set it to Y. Resolution: treat
as idempotent for the same requestId (which carries the same datetime in
its hash) — replay only fires for the exact same logical command.

**`/api/reboot` marked FROM_PENDING* with asterisk**: lifecycle is
FROM_PENDING for the commitTransaction step, but the reboot itself is
destructive and the ACK queue must NOT be dequeued before restart.
See §8 for full lifecycle specification.

---

## 4. Shared Helper Internal (REV.8)

Auditor's directive:

> Minimal harus ada helper internal bersama untuk:
> requestId validation → command hash generation → duplicate lookup →
> storeIntent → mutation → commit/clear/failure handling
>
> Kalau tidak, 16 endpoint × pola yang sama akan sangat mudah mengalami
> contract drift.

### 4.1 Helper API surface

A new internal helper file `firmware/RestJournalHelper.h` provides the
shared primitives. Each handler calls these helpers; no handler implements
its own dedup/hash/lifecycle.

```cpp
// firmware/RestJournalHelper.h (NEW — internal header, not exposed to PWA)
namespace Web { namespace Rest {

// Step 1: requestId validation (charset, length)
// Returns true if valid. On false, sends HTTP 400 to client.
inline bool validateRequestId(const String& requestId);

// Step 2: command hash generation (per-type canonical schema)
// Reuses the SAME canonical schemas as MqttClient::_computeCommandHash
// to ensure cross-path contract symmetry.
inline String computeCommandHash(const DynamicJsonDocument& doc, const char* commandType);

// Step 3: duplicate lookup + ACK replay
// Returns true if requestId was already processed (caller should NOT re-execute).
// On true, this function has already sent the appropriate HTTP response
// (replay ACK or 409 Conflict).
inline bool checkDuplicateAndRespond(const String& requestId, const String& commandHash);

// Step 4: storeIntent wrapper
// Returns true on success. On false, sends HTTP 503 DURABILITY_FAILURE.
inline bool storeIntentOrReject(const String& requestId, const String& commandHash,
                                  uint8_t intentChannelId, bool intentDesiredState,
                                  bool intentPreviousKnown);

// Step 5: markExecuting wrapper (for EXECUTING mode only)
// Returns true on success. On false, sends HTTP 503 + clearEntry.
inline bool markExecutingOrAbort(const String& requestId);

// Step 6a: commit (FROM_PENDING path)
// Returns true on success. On false, sends HTTP 503 (state preserved as evidence).
inline bool commitFromPendingOrFailure(const String& requestId, const String& ackJson);

// Step 6b: commit (EXECUTING path)
// Returns true on success. On false, sends HTTP 503 (state preserved as evidence).
inline bool commitExecutingOrFailure(const String& requestId, const String& ackJson);

// Step 6c: clearEntry on validation failure (after storeIntent, before mutation)
// Returns true on success. On false, logs error (caller should still send 4xx).
inline bool clearEntryOnValidationFailure(const String& requestId);

}} // namespace Web::Rest
```

### 4.2 Per-handler skeleton (revised)

```cpp
inline void handleRelay() {
  if (!requireAuth()) return;
  if (!requireCsrf()) return;
  if (!requireBody(1024)) return;
  
  DynamicJsonDocument doc(512);
  if (deserializeJson(doc, Web::http.arg("plain"))) {
    sendError(400, "Invalid JSON");
    return;
  }
  
  // ... extract channelId, actionStr from doc ...
  
  // === JOURNAL WRAP (all 6 steps via shared helpers) ===
  String requestId = doc["requestId"] | "";
  if (!Web::Rest::validateRequestId(requestId)) return;  // sends 400 if invalid
  
  String commandHash = Web::Rest::computeCommandHash(doc, "relay");
  if (Web::Rest::checkDuplicateAndRespond(requestId, commandHash)) return;
  
  if (!Web::Rest::storeIntentOrReject(requestId, commandHash,
                                       intentChannelId, intentDesiredState,
                                       intentPreviousKnown)) return;
  
  // EXECUTING mode → markExecuting before physical mutation
  if (!Web::Rest::markExecutingOrAbort(requestId)) return;
  
  // === ACTUAL MUTATION (unchanged from current production code) ===
  if (strcmp(actionStr, "on") == 0) {
    Services::relayEngine.setManual(idx, true);
  } else if (strcmp(actionStr, "off") == 0) {
    Services::relayEngine.setManual(idx, false);
  } else if (strcmp(actionStr, "set_mode") == 0) {
    // ... mode handling ...
  }
  
  // === COMMIT (EXECUTING path) ===
  String ackJson = _buildRelayAckJson(requestId, channelId, idx);
  if (!Web::Rest::commitExecutingOrFailure(requestId, ackJson)) return;
  
  // === SEND HTTP 200 ===
  Web::sendSecurityHeaders();
  Web::http.send(200, "application/json", ackJson);
}
```

**Pattern uniformity:** every mutation handler follows this skeleton. Only the
"ACTUAL MUTATION" middle differs. The 6 helper calls are identical across
all 16 endpoints (modulo COMMIT path: FROM_PENDING vs EXECUTING).

### 4.3 Failure semantics encapsulated

Each helper handles its own failure case and sends the appropriate HTTP
response. The handler does not need to repeat the failure matrix logic —
it's centralized. New endpoints automatically inherit correct failure
handling by using the helpers.

---

## 5. Password Change Canonical Hash (REV.3)

Auditor's directive:

> commandHash = SHA256("password_change" "|user=" + canonicalUser
>                       "|credentialVersion=" + version)
> atau equivalent yang tidak mengandung secret.

### 5.1 Canonical hash schema (revised)

```cpp
// For /api/config/password:
String canonical = "password_change"
                  "|user=" + canonicalUser       // current authenticated user
                  "|credentialVersion=" + credVer; // monotonic counter, NOT password

// For /api/config (when body contains 'pass' field — sets both user and pass):
String canonical = "config_set_credentials"
                  "|user=" + canonicalUser
                  "|credentialVersion=" + credVer;

// For /api/config (when body has only 'user' field, no password):
String canonical = "config_set_user"
                  "|user=" + newUser
                  "|credentialVersion=" + credVer;
```

### 5.2 credentialVersion source

`credentialVersion` is a monotonic counter persisted in NVS that increments
on every successful credential change. Stored in NVS key `cred_ver`
(uint32_t). Initialized to 0 on first boot, incremented before each
password/user change commit.

This ensures:
- Same logical request → same hash (same user + same credentialVersion)
- Different credential mutations → different hash (credentialVersion increments)
- Password plaintext NEVER appears in the canonical string
- ACK replay works correctly: same requestId + same hash → replay

### 5.3 Why this is safe

- **Uniqueness:** Two different password changes for the same user get
  different credentialVersion values → different hashes → no collision.
- **Replay safety:** Same requestId + same hash → firmware replays ACK
  without re-executing password change. Correct because password is
  already changed.
- **No secret leakage:** The journal stores `requestId`, `commandHash`,
  `intentChannelId`, `intentDesiredState`, `intentPreviousKnown`. The
  hash itself is a SHA-256 of non-secret metadata. Even if an attacker
  reads the journal, they learn only "a password change happened for
  user X at version N" — not the new password.

### 5.4 What is NOT hashed (explicit denial)

The following are NEVER included in the canonical string:
- Password plaintext (current or next)
- Password hash (PBKDF2 output)
- Salt value
- JWT secret
- MQTT password

### 5.5 Cross-path consistency

The MQTT path (`MqttClient::_handleCommand` for `type:"config"`) currently
uses a different canonical schema (line 2161 of MqttClient.cpp):
```
canonical += "|deviceName=" + ...
canonical += "|timezone=" + ...
```

This is INCONSISTENT with the password change path. For Phase C
implementation, we have two options:

**Option A (recommended):** Keep MQTT password-change path unchanged
(F-P0-1 APPROVED, don't touch it). REST password-change uses the
credentialVersion schema. The two paths produce different hashes for
the "same" logical command — but they're not the same command (MQTT
config command doesn't change password, REST does).

**Option B:** Unify both paths to use credentialVersion. This would
require modifying MQTT `_handleConfig` to also use credentialVersion.
OUT OF SCOPE for F-P0-2 (would re-open F-P0-1).

**Decision:** Option A. REST and MQTT password-change are semantically
different commands (MQTT config doesn't change password, only REST
config/password does). Different hashes are correct.

---

## 6. Factory Reset Crash/Retry Semantic Matrix (REV.2)

Auditor's directive:

> Harus ada explicit crash matrix:
> Crash sebelum destructive wipe: PENDING/EXECUTING → reboot → journal recovery → command MUST NOT silently disappear
> Crash saat wipe: journal destruction partially completed → reboot → what is authoritative?
> Crash setelah wipe tetapi sebelum response: factory reset succeeded, ACK unavailable → retry same requestId → MUST NOT execute factory reset again

### 6.1 Factory reset lifecycle (revised)

```
1. POST /api/factory_reset/confirm {token, confirm: "RESET", requestId}
2. requireAuth, requireCsrf, body parse
3. Validate token (consumeFactoryResetToken — single-use)
4. validateRequestId(requestId)
5. computeCommandHash(doc, "factory_reset")
6. checkDuplicateAndRespond — if requestId already processed, replay
7. storeIntent (PENDING)
8. markExecuting (EXECUTING)  ← durable evidence that wipe is starting
9. Send HTTP 200 response (BEFORE wipe — TCP must flush before NVS dies)
   - delay(500) for TCP flush
10. PERFORM WIPE:
    - Drivers::relay.allOff()
    - Drivers::pir.resetAll()
    - Storage::config.resetChannels()
    - Storage::config.saveSchedule(true)
    - Storage::config.initDefaultUserConfig()
    - Storage::config.saveUserConfig()
    - [WIPE JOURNAL NVS keys: tj_slot_N_a, tj_slot_N_b, tj_ackq_*]
11. ESP.restart()
```

### 6.2 Crash windows

| # | Crash window                          | Journal state on crash    | Recovery on next boot                                   |
|---|----------------------------------------|---------------------------|---------------------------------------------------------|
| A | Before step 7 (storeIntent)           | (no journal entry)        | Token already consumed → retry same requestId → 403 (token expired) — CORRECT, no reset happened |
| B | Between step 7 (PENDING) and step 8 (markExecuting) | PENDING       | Boot recovery sees PENDING → reconcileEntry → UNKNOWN. Operator must investigate. |
| C | Between step 8 (markExecuting) and step 9 (HTTP response) | EXECUTING    | Boot recovery sees EXECUTING → reconcileEntry → UNKNOWN. Operator must investigate. |
| D | Between step 9 (HTTP response sent) and step 10 (wipe begins) | EXECUTING    | Boot recovery sees EXECUTING. PWA already received 200 OK. PWA may retry — but retry hits checkDuplicateAndRespond which sees EXECUTING and returns 409 "in progress". Operator must manually trigger factory reset again. |
| E | During step 10 (partial wipe)          | PARTIALLY DESTROYED       | Boot recovery may see corrupt journal → quarantine mode. Config may be partially reset (channels cleared but user config intact, or vice versa). |
| F | After step 10 (wipe complete) but before step 11 (restart) | Journal keys gone (empty NVS) | Boot sees fresh empty journal → normal first-boot path. Config is reset to defaults. PWA retry hits checkDuplicateAndRespond → not processed (journal empty) → would re-execute factory reset. This is SAFE because the device is already in factory state — re-running reset is idempotent. |
| G | During step 11 (restart in progress)   | (irrelevant — restart completes) | Normal boot. Same as case F. |

### 6.3 Authoritative recovery rules

| Crash case | What is authoritative?                                       | Action                                                                          |
|------------|--------------------------------------------------------------|---------------------------------------------------------------------------------|
| A          | Token consumed, no reset happened                            | Operator must re-issue factory_reset/prepare → new token → re-confirm. CORRECT. |
| B, C       | Journal entry exists (PENDING or EXECUTING)                   | Boot recovery: reconcileEntry returns UNKNOWN. Device boots to RUNNING state but operator must verify config integrity manually. PWA retry sees "in progress" or "ambiguous" response. |
| D          | Journal EXECUTING, HTTP 200 sent, wipe not started            | Boot recovery: reconcileEntry returns UNKNOWN. Operator must investigate whether wipe happened. PWA retry blocked by dedup (EXECUTING). |
| E          | Partial wipe                                                  | Boot recovery sees corrupt journal (CRC mismatch on slot keys). Device enters SAFE_INIT mode. Operator must factory reset manually via serial console. |
| F, G       | Wipe complete, journal empty                                  | Boot recovery: fresh empty journal, normal first-boot. Device is in factory state. PWA retry re-runs factory reset (idempotent — already in factory state). CORRECT. |

### 6.4 Idempotency guarantee for retry

The auditor's key question:

> Crash setelah wipe tetapi sebelum response: factory reset succeeded,
> ACK unavailable → retry same requestId → MUST NOT execute factory
> reset again.

**Resolution:** After wipe completes (case F, G), the journal is empty.
A retry with the same requestId will:
1. `checkDuplicateAndRespond` → not in journal (empty) → falls through
2. `storeIntent` → creates fresh PENDING entry
3. `markExecuting` → EXECUTING
4. Wipe runs again — but device is ALREADY in factory state, so:
   - `Drivers::relay.allOff()` — already off, no-op
   - `Storage::config.resetChannels()` — already at defaults, no-op
   - `Storage::config.initDefaultUserConfig()` — already at defaults, no-op
5. ESP.restart()

The retry is SAFE because factory reset is idempotent — re-running it on
an already-reset device produces the same state. The only side effect is
a second reboot, which is acceptable.

**Why not block retry after wipe?** We can't — the journal has no memory
of the prior wipe (it was wiped). Blocking retry would require preserving
journal entries across the wipe, which defeats the purpose of factory reset.

**Operator guidance:** If PWA receives "device not responding" after
factory_reset_confirm, the operator should:
1. Wait 30 seconds for reboot
2. Try to reconnect to device AP
3. If AP appears with default SSID "Timer12CH", reset succeeded
4. If AP does not appear, investigate device serial console

### 6.5 Special lifecycle matrix entry

| Endpoint                          | Commit mode              | Lifecycle                                | COMMITTED? | Notes                                                       |
|-----------------------------------|---------------------------|------------------------------------------|------------|-------------------------------------------------------------|
| `/api/factory_reset/confirm`      | EXECUTING (special)       | PENDING → EXECUTING → wipe → restart     | NEVER      | Journal destroyed by design. Retry is safe (idempotent).    |

The COMMITTED state is unreachable by design. The lifecycle matrix must
document this explicitly — not as a "bug" but as a deliberate semantic
choice.

---

## 7. Complete Mutation/Failure Invariant Matrix (REV.4)

Auditor's directive:

> Failure point | Mutation occurred? | clearEntry? | Journal state
> (8 rows, must be shared invariant across all 16 endpoints)

### 7.1 Universal failure invariant matrix

Applies to ALL 16 mutation endpoints. No endpoint may deviate.

| # | Failure point                                         | Mutation occurred? | clearEntry?    | Journal state after failure         | HTTP response                                              |
|---|-------------------------------------------------------|--------------------|----------------|-------------------------------------|------------------------------------------------------------|
| 1 | Validation BEFORE storeIntent (auth/CSRF/body/field) | NO                 | N/A            | None (no entry created)             | 400 / 401 / 403 (validation error)                         |
| 2 | storeIntent failure (NVS write error)                 | NO                 | NO             | None (slot not allocated)            | 503 DURABILITY_FAILURE                                    |
| 3 | markExecuting failure (EXECUTING mode only)            | NO                 | YES            | EMPTY (slot cleared)                | 503 Internal error: cannot mark executing                 |
| 4 | Validation AFTER storeIntent (rare — pre-validation should catch) | NO      | YES            | EMPTY (slot cleared)                | 400 validation error                                      |
| 5 | Mutation execution succeeded, persistence failure       | YES                | NO             | PENDING or EXECUTING (evidence preserved) | 503 DURABILITY_FAILURE (state preserved for investigation) |
| 6 | journal.commitTransaction failure                       | YES                | NO             | EXECUTING (evidence preserved)       | 503 DURABILITY_FAILURE                                    |
| 7 | commit success, HTTP response publish failure            | YES                | NO             | COMMITTED + ACK queued              | (TCP fails — PWA retries, gets ACK replay from queue)     |
| 8 | commit success, HTTP response publish success            | YES                | NO             | COMMITTED                           | 200 OK with ACK JSON                                      |

### 7.2 Critical invariants

**INVARIANT A (rules 1, 2, 3, 4):** If no mutation occurred, journal state
MUST be EMPTY (no entry or entry cleared). This prevents the journal from
accumulating "ghost" entries for commands that never executed.

**INVARIANT B (rules 5, 6):** If mutation occurred but persistence/commit
failed, journal state MUST preserve evidence (PENDING or EXECUTING).
`clearEntry` is FORBIDDEN — it would destroy the audit trail.

**INVARIANT C (rule 7):** If commit succeeded but HTTP response fails to
deliver, the journal is COMMITTED and the ACK is queued. PWA retry will
hit `checkDuplicateAndRespond` which replays the ACK from the queue.
This is the same pattern as MQTT F-P0-1 (TEST 9 reboot lifecycle).

**INVARIANT D (rule 8):** Happy path — commit succeeds, HTTP 200 sent,
ACK is durable. Future retries replay ACK without re-executing.

### 7.3 Schedule-specific failure (special case for §7 rule 5)

Schedule endpoint has additional persistence step (`saveSchedule(true)`)
between mutation and commit. The failure matrix for schedule is:

| Sub-step                                  | Mutation occurred? | clearEntry? | Journal state | HTTP response                                              |
|-------------------------------------------|--------------------|-------------|---------------|------------------------------------------------------------|
| RAM mutation succeeded                    | YES (RAM only)     | NO          | PENDING       | (continuing to persistence step)                            |
| saveSchedule(true) succeeded              | YES (RAM + NVS)    | NO          | PENDING       | (continuing to commit)                                      |
| saveSchedule(true) FAILED                 | YES (RAM mutated)  | NO          | PENDING       | 503 DURABILITY_FAILURE — journal preserves PENDING evidence  |

The journal entry stays in PENDING state when persistence fails. RAM is
mutated but NVS schedule file is not written. On next boot:
- Journal recovery sees PENDING entry → reconcileEntry → UNKNOWN
- Schedule file may have stale data (last successful save)
- RAM was reset on boot, so the unsaved mutation is gone

This is the correct behavior: the journal preserves evidence that
something happened, but the actual schedule state is ambiguous. Operator
must verify schedule state manually after a durability failure.

**Why not clearEntry on saveSchedule failure?** Auditor's directive:

> Jika saveSchedule(true) gagal: JANGAN commit journal. Dan jangan
> clearEntry() apabila mutation RAM sudah terjadi. Harus menjadi:
> PENDING → RAM mutation → persistence failure → journal remains
> PENDING/evidence preserved → failure ACK

This is exactly what the matrix specifies.

### 7.4 Implementation in shared helper

The shared helper (`RestJournalHelper.h`) encapsulates these invariants:

```cpp
// Step 6c: clearEntry on validation failure (rules 3, 4)
inline bool clearEntryOnValidationFailure(const String& requestId) {
  // Pre-condition: no mutation has occurred
  // Post-condition: journal slot is EMPTY
  if (!Services::journal.clearEntry(requestId)) {
    // clearEntry itself failed — log error, but don't change HTTP response
    Serial.println("[REST] WARNING: clearEntry failed — slot may need manual cleanup");
  }
  return true;
}

// Step 6a/6b: commit failure handler (rules 5, 6)
// On commit failure, do NOT clearEntry. Journal preserves evidence.
// HTTP 503 is sent by caller.
// (No helper needed — caller just doesn't call clearEntry.)
```

The helper ensures no handler accidentally violates invariant B (calling
clearEntry after a mutation occurred).

---

## 8. Reboot Lifecycle — F-P0-1 Invariant Retention (REV.9)

Auditor's directive:

> Khusus reboot, saya tidak ingin hanya melihat FROM_PENDING; semantic
> design harus mempertahankan invariant yang sudah disetujui di F-P0-1:
> PENDING → durable COMMITTED + ACK queued → publish ACK → restart
> dan jangan dequeue ACK sebelum reboot.

### 8.1 Reboot lifecycle (REST, mirror of MQTT F-P0-1 TEST 9)

```
1. POST /api/reboot {requestId}
2. requireAuth, requireCsrf, body parse
3. validateRequestId(requestId)
4. computeCommandHash(doc, "system_reboot")
5. checkDuplicateAndRespond
6. storeIntent (PENDING)
7. (NO markExecuting — reboot is FROM_PENDING, not EXECUTING)
8. commitTransactionFromPending (COMMITTED)
   - ACK is queued via journal.queueAck()
   - ACK is NOT dequeued (critical invariant from F-P0-1)
9. Send HTTP 200 response with ACK JSON
10. delay(500) for TCP flush
11. Storage::config.saveSchedule(true) if dirty
12. Services::Log.append(Restart, "Reboot triggered via REST", 0)
13. delay(500)
14. ESP.restart()
```

### 8.2 Invariant retention

| F-P0-1 invariant                                              | REST implementation                                              |
|---------------------------------------------------------------|------------------------------------------------------------------|
| storeIntent creates durable PENDING before any action           | Step 6 — same journal.storeIntent() call                         |
| commitTransaction creates durable COMMITTED before restart      | Step 8 — commitTransactionFromPending() writes both NVS copies   |
| ACK queued (not dequeued) so it survives reboot                | Step 8 — journal.queueAck() called by commitTransactionFromPending |
| ESP.restart() called AFTER commit succeeds                     | Step 14 — only reached if step 8 succeeded                        |
| PWA can detect "device rebooted as I asked" after reconnect    | Journal shows COMMITTED for requestId on next boot               |

### 8.3 Difference from MQTT reboot

MQTT reboot (F-P0-1 TEST 9):
- ACK is published via `_mqtt.publish()` after commit
- ACK is also queued via `journal.queueAck()` for retry
- On immediate publish success: `dequeueAck()` is called
- On immediate publish failure: ACK stays in queue

REST reboot (this design):
- ACK is sent as HTTP 200 response body
- ACK is also queued via `journal.queueAck()` for potential replay
- ACK is NOT dequeued (because device is about to restart — no point)
- On retry after reboot: HTTP layer can't replay (device was offline),
  but journal shows COMMITTED → if PWA polls status, sees device healthy

### 8.4 Why no markExecuting for reboot

Reboot itself is the "mutation" — but it's not a GPIO/RTC/NVS mutation.
It's a system state change. The journal entry captures the intent and
the commit. markExecuting would imply "physical mutation in progress"
which doesn't apply to reboot.

If the device crashes between commitTransactionFromPending and
ESP.restart(), the journal shows COMMITTED — recovery is clean
(reconcileEntry sees COMMITTED → no action needed).

If markExecuting were called, a crash in that window would leave
EXECUTING → reconcileEntry returns UNKNOWN → ambiguous. Worse than
the FROM_PENDING path.

---

## 9. REST HTTP-Response ↔ Journal-Commit Contract (REV.5)

Auditor's directive:

> HTTP 200 tidak boleh dikirim sebagai success sebelum mutation +
> required persistence + journal commit berhasil.
> Jika commit gagal: HTTP 5xx / explicit durability error, bukan
> HTTP 200 success:true.

### 9.1 REST response contract

| Journal state                                  | HTTP status | Body                                                                                |
|------------------------------------------------|-------------|-------------------------------------------------------------------------------------|
| (no entry — validation failed before store)    | 400 / 401 / 403 | `{"success":false,"message":"<reason>","data":null}`                              |
| storeIntent failed                            | 503         | `{"success":false,"message":"DURABILITY_FAILURE: cannot store intent — retry","data":null}` |
| markExecuting failed                          | 503         | `{"success":false,"message":"Internal error: cannot mark executing","data":null}`  |
| clearEntry called (validation post-store)     | 400         | `{"success":false,"message":"<validation reason>","data":null}`                     |
| Mutation succeeded, persistence failed         | 503         | `{"success":false,"message":"DURABILITY_FAILURE: persistence failed — state may be inconsistent","data":{"requestId":"..."}}` |
| commitTransaction failed                       | 503         | `{"success":false,"message":"DURABILITY_FAILURE: cannot commit — retry","data":null}` |
| commitTransaction succeeded                    | 200         | `{"success":true,"message":"<action>","data":{...,"requestId":"..."}}`              |
| Duplicate requestId (COMMITTED)                | 200         | (replay original ACK JSON from journal)                                              |
| Duplicate requestId (PENDING/EXECUTING)        | 409         | `{"success":false,"message":"requestId in progress — retry later","data":{"requestId":"..."}}` |
| Duplicate requestId with different hash        | 409         | `{"success":false,"message":"requestId reuse with different command — rejected","data":null}` |
| Missing requestId                              | 400         | `{"success":false,"message":"requestId required","data":null}`                       |
| Malformed requestId                            | 400         | `{"success":false,"message":"requestId invalid (max 64 chars, [a-zA-Z0-9-_])","data":null}` |

### 9.2 ACK JSON envelope (for successful mutations)

```json
{
  "success": true,
  "message": "<human-readable action description>",
  "data": {
    "requestId": "<UUID>",
    "channelId": 1,
    "state": true,
    "source": "manual",
    "modeAuto": false,
    "...": "<endpoint-specific fields>"
  }
}
```

The `requestId` is included in `data` so PWA can correlate response with
its pending command (useful when retry replays an ACK — PWA can show
"this was your previous action").

### 9.3 Critical ordering guarantee

```
1. mutation completes (RAM + persistence + commit)
2. ACK JSON constructed (includes requestId, mutation result)
3. HTTP 200 sent with ACK JSON body
```

Step 3 is NEVER reached before step 1 completes successfully. If step 1
fails (any sub-step), HTTP 5xx is sent instead.

This is the direct analog of MQTT's `_finalizeAndPublishAck()` rule
(MqttClient.cpp lines 540-579): publish happens AFTER commit succeeds.

### 9.4 What this means for PWA retry semantics

- HTTP 200 → command definitely succeeded (commit is durable)
- HTTP 4xx → command definitely did NOT succeed (validation failed)
- HTTP 5xx → command MAY OR MAY NOT have succeeded (mutation may have
  occurred before persistence/commit failed)
- HTTP 0 (network error) → unknown state, retry with same requestId

For HTTP 5xx and HTTP 0, PWA should retry with the SAME requestId:
- If original command committed → retry sees duplicate, replays ACK
- If original command failed before commit → retry re-executes
- If original command failed after mutation → retry sees duplicate
  (state preserved), replays ACK with possibly stale data — PWA should
  verify device state

### 9.5 Schedule endpoint specific contract

```
1. validate (auth, CSRF, body, fields)
2. computeHash + checkDuplicate
3. storeIntent (PENDING)
4. (FROM_PENDING — no markExecuting)
5. Mutate RAM (channels[idx].sched[] = ...)
6. saveSchedule(true) — synchronous NVS write
   - On failure: HTTP 503, journal stays PENDING (invariant §7 rule 5)
7. commitTransactionFromPending (COMMITTED)
8. HTTP 200 with ACK JSON
```

If step 6 fails, step 7 is NOT called. Journal stays PENDING. HTTP 503
sent with `DURABILITY_FAILURE` message. PWA retry sees PENDING (not
COMMITTED) → 409 "in progress" — PWA waits and retries, or operator
investigates.

### 9.6 Reboot endpoint specific contract

```
1. validate
2. computeHash + checkDuplicate
3. storeIntent (PENDING)
4. commitTransactionFromPending (COMMITTED, ACK queued, NOT dequeued)
5. HTTP 200 with ACK JSON
6. delay(500)
7. saveSchedule(true) if dirty
8. ESP.restart()
```

If step 4 fails, HTTP 503, no restart. If step 4 succeeds, HTTP 200
sent, then device restarts. PWA retry after restart sees COMMITTED
→ journal replays ACK (or PWA polls status and sees device healthy).

---

## 10. Test Acceptance Criteria — Coverage Matrix (REV.6)

Auditor's directive:

> 25 tests terlalu kecil kalau benar-benar production-path.
> Minimum acceptance harus mencakup:
> Production routing: setiap mutation endpoint, duplicate same requestId,
> same requestId + different hash, missing requestId, malformed requestId,
> read-only endpoint, reboot, factory reset, password change, schedule persistence
> Failure: storeIntent failure, mutation validation failure, persistence failure,
> journal A write failure, journal B write failure, ACK queue failure, duplicate
> after reboot, retry after ambiguous state

### 10.1 Coverage matrix (not fixed count)

Tests are organized by COVERAGE GOAL, not by test count. Each row must
have at least one test that exercises the actual production handler
(not a reimplementation).

#### 10.1.1 Production routing (per-endpoint mutation proof)

Each mutation endpoint MUST have at least one test that calls the real
handler with valid JSON and verifies:
- Journal state == COMMITTED
- Mutation effect (GPIO / RAM / NVS / RTC)
- HTTP 200 response shape
- requestId echoed in response

| # | Endpoint                                | Mutation effect to verify                            |
|---|-----------------------------------------|------------------------------------------------------|
| P1 | `/api/relay` on                        | `relayState[0] == true`                              |
| P2 | `/api/relay` off                        | `relayState[0] == false`                             |
| P3 | `/api/relay` set_mode auto             | `channels[0].modeAuto == true`                       |
| P4 | `/api/channel` rename                   | `channels[0].name == "Kitchen"`                     |
| P5 | `/api/schedule` upsert                  | `channels[0].schedCount == 1`                        |
| P6 | `/api/schedule` delete                  | `channels[0].schedCount == 0`                       |
| P7 | `/api/pir` config                       | `channels[8].pirEnabled == true`                    |
| P8 | `/api/pir/test`                         | `Drivers::pir.testTrigger called`                   |
| P9 | `/api/time` set                         | `Drivers::rtc.adjust called with correct args`      |
| P10 | `/api/config` set user                  | `userConfig.wwwUser == "admin"`                     |
| P11 | `/api/config/device`                   | `deviceName == "TestDevice"`                        |
| P12 | `/api/config/password`                 | `userConfig.passHashHex changed`, password NOT in journal |
| P13 | `/api/config/import`                   | `Storage::config.importAll called`, channels reset  |
| P14 | `/api/reboot`                          | `journal.isCommitted(rid)`, `g_espRestartCalled`    |
| P15 | `/api/ota/check`                       | `!journal.isProcessed(rid)` (NONE mode)             |
| P16 | `/api/factory_reset/prepare`           | `!journal.isProcessed(rid)` (NONE mode)             |
| P17 | `/api/factory_reset/confirm`           | `journal.getTransactionState(rid) == EXECUTING`, `g_espRestartCalled` |

#### 10.1.2 Duplicate handling

| # | Scenario                                            | Assertion                                              |
|---|-----------------------------------------------------|--------------------------------------------------------|
| D1 | Duplicate requestId (COMMITTED, same hash)           | HTTP 200, original ACK replayed, no double-mutation    |
| D2 | Duplicate requestId (PENDING, same hash)             | HTTP 409 "in progress"                                  |
| D3 | Duplicate requestId (EXECUTING, same hash)           | HTTP 409 "in progress"                                  |
| D4 | requestId reuse with different hash                  | HTTP 409 "requestId reuse with different command"      |
| D5 | Missing requestId                                    | HTTP 400 "requestId required"                          |
| D6 | requestId too long (>64 chars)                      | HTTP 400 "requestId too long"                          |
| D7 | requestId with invalid charset (e.g., "rid!/bad")    | HTTP 400 "requestId contains invalid characters"       |
| D8 | Duplicate after "reboot" (simulate reboot via journal re-init) | journal shows COMMITTED, ACK replayed        |

#### 10.1.3 Failure injection

| #  | Scenario                                                | Injection                                            | Assertion                                                        |
|----|---------------------------------------------------------|------------------------------------------------------|------------------------------------------------------------------|
| F1 | storeIntent failure (NVS write fails on copy A)          | `Preferences::setFailNextPut("tj_slot_0_a")`         | HTTP 503, no mutation, journal empty                             |
| F2 | storeIntent failure (NVS write fails on copy B)          | `Preferences::setFailNextPut("tj_slot_0_b")`         | HTTP 503, no mutation, slot cleared (candidate pattern)          |
| F3 | markExecuting failure (copy A)                           | `Preferences::setFailNextPut("tj_slot_0_a")` post-store | HTTP 503, clearEntry called, no mutation                     |
| F4 | markExecuting failure (copy B)                           | `Preferences::setFailNextPut("tj_slot_0_b")` post-store | HTTP 503, clearEntry called, no mutation                     |
| F5 | commitTransaction failure (copy A)                      | `Preferences::setFailNextPut("tj_slot_0_a")` post-markExecuting | HTTP 503, state stays EXECUTING (mutation occurred), NO clearEntry |
| F6 | commitTransaction failure (copy B)                      | `Preferences::setFailNextPut("tj_slot_0_b")` post-markExecuting | HTTP 503, state stays EXECUTING, NO clearEntry              |
| F7 | Validation failure after storeIntent (schedule bad time) | `onTime="invalid"`                                  | clearEntry called, journal empty, HTTP 400                      |
| F8 | Validation failure after storeIntent (PIR id out of range) | `id=5`                                              | clearEntry called, journal empty, HTTP 400                      |
| F9 | Validation failure after storeIntent (channel name too long) | `name="abcdefghij..."` (25 chars)                | clearEntry called, journal empty, HTTP 400                      |
| F10 | Validation failure after storeIntent (password too weak) | `next="abc"`                                        | clearEntry called, journal empty, HTTP 403                      |
| F11 | saveSchedule(true) failure (schedule endpoint)         | `Preferences::setFailNextPut("schedule.json")` shim  | HTTP 503, journal stays PENDING, NO clearEntry                   |
| F12 | ACK queue failure                                       | `Preferences::setFailNextPut("tj_ackq_hdr")`         | HTTP 200 still sent (commit succeeded), ACK in queue for retry  |
| F13 | Factory reset token expired                              | Token TTL elapsed before confirm                     | HTTP 403, clearEntry called, no wipe                             |
| F14 | Retry after ambiguous state (UNKNOWN)                   | Manually set slot to UNKNOWN via journal API, then retry | HTTP 409 "ambiguous — verify device state"                  |

#### 10.1.4 Cross-cutting

| #  | Scenario                                                  | Assertion                                                       |
|----|-----------------------------------------------------------|-----------------------------------------------------------------|
| X1 | All mutation endpoints use shared RestJournalHelper        | Source review: each handler calls validateRequestId, computeCommandHash, checkDuplicateAndRespond, storeIntentOrReject, commit* |
| X2 | Password plaintext never appears in journal                | After password change, grep all journal NVS keys for password string → not found |
| X3 | credentialVersion increments on each password change        | Two consecutive password changes → different credentialVersion → different commandHash |
| X4 | Reboot does NOT dequeue ACK before restart                 | After reboot handler runs, `journal.getPendingAckCount() >= 1`  |
| X5 | Factory reset confirm does NOT reach COMMITTED              | After handler runs (with failure injection before wipe), state == EXECUTING |
| X6 | HTTP 200 only sent after commit succeeds                    | Source review: no `Web::http.send(200, ...)` before commitTransaction call in any handler |
| X7 | HTTP 5xx sent on any persistence/commit failure             | Source review: every failure path returns 5xx, never 200        |

### 10.2 Test count

Minimum: 17 (production) + 8 (duplicate) + 14 (failure) + 7 (cross-cutting) = 46 tests.

This is a MINIMUM, not a ceiling. Auditor may add more during review.

### 10.3 Test discipline (carried from F-P0-1)

- Tests MUST call actual production handlers (handleRelay, handleSchedule, etc.)
- Tests MUST NOT reimplement handler logic
- Tests MUST compile against real `HttpServer.cpp` + handler headers + `TransactionJournal.cpp`
- Shims for ESP32 dependencies (WebServer, AuthManager, etc.) are allowed
- Shims for production functions under test are NOT allowed
- `#define private public` is allowed to access `Web::http` internal state
- Failure injection via `Preferences::setFailNextPut(key)` is the primary mechanism

---

## 11. Implementation Plan (Phase C — still blocked)

Sequential, no parallelism. Will not start until auditor approves Phase B REV.2.

1. Build `firmware/RestJournalHelper.h` with the 6 shared helpers.
2. Modify `Common.h` — add `requireRequestId()` wrapper.
3. Modify each mutation handler to use the helper-based skeleton.
4. Build `WebServerTest.cpp` + `Makefile.ws` — production-path proof (≥17 tests).
5. Add duplicate-handling tests (≥8 tests).
6. Add failure-injection tests (≥14 tests).
7. Add cross-cutting tests (≥7 tests).
8. Regression run: TransactionJournal 194 + CommandRouting 133 + MqttClient 31 + WebServer ≥46 = ≥404 assertions.

**No production source changes that affect MQTT path.** All MQTT handler code is unchanged.

---

## 12. Status

**Phase A (Discovery): COMPLETE**
**Phase B (Semantic Design) REV.1: COMPLETE → auditor CONDITIONAL NO-GO**
**Phase B (Semantic Design) REV.2: COMPLETE** — this document, addresses all 6 + 3 additional directives.

**Awaiting auditor review of REV.2.**

If approved: Phase C (implementation) begins, following same iterative
discipline as F-P0-1:
- C1: Build RestJournalHelper + per-handler refactor
- C2: Build WebServerTest production-path tests (≥17)
- C3: Add duplicate tests (≥8)
- C4: Add failure-injection tests (≥14)
- C5: Add cross-cutting tests (≥7)
- C6: Regression run (≥404 assertions across 4 binaries)

Each Ci phase produces a patch for auditor review before proceeding to Ci+1.

---

## 13. Auditor Decision Needed (REV.2)

1. Approve PWA caller map (§1) — sufficient to verify UUID survives retry?
2. Approve EXECUTING refined definition (§3) — "unsafe-to-repeat side effects"?
3. Approve shared helper internal (§4) — RestJournalHelper.h with 6 functions?
4. Approve password hash with credentialVersion (§5) — no plaintext, monotonic counter?
5. Approve factory reset crash matrix (§6) — 7 crash windows, idempotent retry?
6. Approve universal failure invariant matrix (§7) — 8 rules, no clearEntry after mutation?
7. Approve reboot lifecycle (§8) — FROM_PENDING, no ACK dequeue before restart?
8. Approve REST HTTP-response ↔ journal-commit contract (§9) — HTTP 200 only after commit?
9. Approve test coverage matrix (§10) — 46 minimum, coverage-based not count-based?
10. Approve Phase C plan (§11) — 6 sub-phases, each producing patch for review?

On approval, will proceed to Phase C1 (RestJournalHelper + handler refactor).
