# F-P0-2 HANDOVER CHECKPOINT — Session End

**Date:** 2026-08-15
**Session:** ESP32 Firmware Audit — P2-2 F-P0-2 (REST TransactionJournal Closure)
**Engineer:** AI Assistant (Super Z)
**Auditor:** User (human)

---

## CURRENT STATE

### Repository: Firmware + Code.gs
- **Repository:** `desvandi/Firmware-code-gs_relaytimer`
- **Branch:** `engineering-cycle-8c-rev26-final-predicate`
- **Latest commit:** `828e362` — "fix: restore file modes to 100644 for newly added shim files"
- **Commits ahead of origin:** 23 (NOT pushed — GitHub token not provided in session)
- **Working tree:** CLEAN (0 modified, 0 untracked)
- **Push status:** ❌ BLOCKED — no GitHub credentials available in session

### Repository: PWA (Remote-Relay)
- **Repository:** `desvandi/Remote-Relay`
- **Branch:** `main`
- **Working tree:** 2 modified files (pre-existing, NOT part of F-P0-2 work)
- **Push status:** ❌ BLOCKED — no GitHub credentials available in session

---

## COMPLETED (all verified)

### Phase B — Semantic Design (REV.1 → REV.2 → REV.3)
- **REV.1:** Initial semantic design — auditor CONDITIONAL NO-GO (6 revisions)
- **REV.2:** Added PWA caller map, EXECUTING refined definition, RestJournalHelper ownership boundary, password credentialVersion, factory reset crash matrix, HTTP-response ↔ commit contract, coverage-based test acceptance — auditor CONDITIONAL GO (§11 mandatory)
- **REV.3:** Added §11 Cross-Ingress Semantic Contract Matrix (MQTT ↔ REST) — auditor FINAL APPROVAL + GO Phase C
- **Status:** ✅ CLOSED

### Phase C1 — Extract Shared CommandHash
- Extracted `_computeCommandHash()` from `MqttClient.cpp` (file-local static) to `Utils::computeCommandHash()` (shared inline in `firmware/CommandHash.h`)
- Verbatim extraction — no canonical schema changes
- 14 baseline vectors captured pre-extraction, verified byte-identical post-extraction
- Added `LegacyCommandHash.h` test-only oracle (verbatim copy of original body) for edge-case proof
- 17 behavioral + 6 edge/boundary tests = 23 total (26 assertions)
- **Corrections:** C1-CORR (3 corrections: file mode, vector count 13→14, real edge tests), C1-CORR-4 (eliminate circular edge evidence via legacy oracle)
- **Status:** ✅ CLOSED

### Phase C2 — RestJournalHelper + Relay Proof-of-Pattern
- Built `firmware/RestJournalHelper.h` — 8 helper functions, journal lifecycle ONLY (NOT business dispatch)
- Added `validateRequestId()` + `requireRequestId()` to `Common.h`
- Refactored `RelayHandlers.h` (handleRelay) — EXECUTING mode, full journal wrap
- 8 production-path tests (P1-P8) + 8 failure-path tests (F1-F8)
- **F7 (markExecuting failure):** REAL behavioral test using `setFailPutOnNthOccurrence("tj_slot_0_a", 2)` — fails 2nd put (markExecuting's copy A write), proves HTTP 503 + no mutation + journal EMPTY (INVARIANT A)
- **F8 (commitTransaction failure):** REAL behavioral test using `setFailPutOnNthOccurrence("tj_slot_0_a", 3)` — fails 3rd put (commit's copy A write), proves HTTP 503 + mutation occurred + journal EXECUTING preserved + NO clearEntry (INVARIANT B)
- **Source-level proof:** `_writeCopy()` does ONE `putBytes()` per call, immediate `return false` on failure, NO retry/fallback to copy B. Runtime trace confirms put sequence: storeIntent #1 → markExecuting #2 → commitTransaction #3.
- **Corrections:** C2-CORR (real F7/F8 via `setFailPutOnNthOccurrence`, P7 byte-identical ACK, P8 no-mutation + journal-intact)
- **Status:** ✅ CLOSED

### Phase C3 — Schedule Handler Journal Wrap + Synchronous saveSchedule Fix
- Refactored `ScheduleHandlers.h` (handleScheduleUpsert + handleScheduleDelete) — FROM_PENDING mode
- **Critical fix:** `markDirty()` (deferred 10s save) → `saveScheduleWithResult(true)` (synchronous) BEFORE commit
- Added `saveScheduleWithResult(bool) → bool` to `ConfigStore.h/.cpp` (original `saveSchedule` delegates, ignores result)
- Injected `doc["type"]="schedule"` / `doc["action"]="upsert"/"delete"` for cross-ingress hash symmetry
- Also applied type injection to relay handler (`doc["type"]="relay"`)
- 6 schedule production tests (P9-P14) + 3 failure tests (F9-F11)
- **F11 (saveSchedule failure):** REAL behavioral test using `setFailNextAtomicWrite()` — proves HTTP 503 + RAM mutation occurred + journal PENDING (NOT COMMITTED) + journal entry preserved (NO clearEntry — INVARIANT B)
- **Status:** ✅ COMPLETE (submitted for auditor review — NOT yet reviewed)

---

## VERIFIED — Regression Results

| Test Suite | Count | Status |
|-----------|-------|--------|
| TransactionJournalTest | 194/194 | ✅ PASS |
| CommandRoutingTest | 133/133 | ✅ PASS |
| MqttClientTest (F-P0-1) | 31/31 | ✅ PASS |
| CommandHashEquivalenceTest (C1) | 26/26 | ✅ PASS |
| CommandHashBaseline | 14 vectors | ✅ Captured |
| WebServerTest (C2+C3) | 111/111 | ✅ PASS |
| **TOTAL** | **495 assertions** | **all green** |

### F7/F8 Source-Level Verification (auditor-verified)
- `_writeCopy()` (TransactionJournal.cpp line 413): ONE `putBytes()` per call, immediate `return false` on failure
- `storeIntent/markExecuting/commitTransaction`: all write copy A FIRST, copy B only if A succeeded
- `_readCopy()`: reads ONE specific copy, NO fallback during write path
- Runtime trace confirms: put #2 = markExecuting, put #3 = commitTransaction
- **F7 proves INVARIANT A:** no mutation → journal EMPTY (clearEntry allowed)
- **F8 proves INVARIANT B:** mutation occurred → evidence preserved (NO clearEntry)

---

## NOT COMPLETED

1. **GitHub push** — 23 commits on `engineering-cycle-8c-rev26-final-predicate` branch are NOT pushed. GitHub token was referenced as "[akan disertakan]" but was NOT included in the session message. Push requires credentials.

2. **C3 auditor review** — C3 (schedule handler) is COMPLETE and committed but NOT yet reviewed by auditor. The auditor's next session should review C3 before proceeding to C4.

3. **Remaining 14 REST endpoints** — only `/api/relay` and `/api/schedule` are refactored. The other 14 endpoints (channel, pir, time, config, system, ota, factory_reset) still bypass the TransactionJournal. These are C4+ scope.

4. **PWA requestId changes** — PWA's `api.ts` and `useApi.ts` do NOT yet send `requestId` in REST mutation requests. PWA must be updated in lockstep with firmware deployment. This is documented in Phase B REV.3 §1.3 but not yet implemented.

5. **PWA repo changes** — `/home/z/my-project/pwa-repo` has 2 modified files (`SECURITY_VERIFICATION_STATUS.md`, `src/lib/mqttPending.ts`) that are pre-existing modifications from a prior session, NOT part of F-P0-2 work. They are NOT committed.

---

## NEXT STARTING POINT

> **Engineer berikutnya harus mulai dari sini:**

### Step 1: Push to GitHub
The 23 commits on `engineering-cycle-8c-rev26-final-predicate` must be pushed first:
```bash
cd /home/z/my-project/firmware-work
git push origin engineering-cycle-8c-rev26-final-predicate
```
**Prerequisite:** GitHub token for `desvandi/Firmware-code-gs_relaytimer` repository.

### Step 2: Verify push succeeded
```bash
git log --oneline -1 origin/engineering-cycle-8c-rev26-final-predicate
# Should show: 828e362 fix: restore file modes to 100644 for newly added shim files
```

### Step 3: Auditor reviews C3
C3 (schedule handler journal wrap + synchronous saveSchedule fix) is committed but NOT yet auditor-reviewed. The auditor should:
1. Read this checkpoint document
2. Read commit `4cd5d0b` (C3 main) + `4d8e9b5` (file mode fix)
3. Check source tree: `firmware/ScheduleHandlers.h`, `firmware/ConfigStore.h/.cpp`, `firmware/RelayHandlers.h`
4. Verify regression: `cd firmware/test/host && make -f Makefile.ws run` → 111/111 PASS
5. Specifically verify F11 (saveSchedule failure → journal PENDING, INVARIANT B)
6. If approved → C3 CLOSED, GO to C4

---

## NEXT DIRECTION

### C4 — Next REST Handler Refactor (channel rename)
- **Endpoint:** `/api/channel` POST (rename)
- **Commit mode:** FROM_PENDING (atomic config mutation, no physical execution phase)
- **Pattern:** Same as schedule — `storeIntent` → mutate RAM → `saveScheduleWithResult(true)` → `commitFromPendingOrFailure` → HTTP 200
- **Simpler than schedule:** No time format validation, no schedule limit, just name length check
- **Proof-of-pattern:** Validates that the RestJournalHelper pattern scales to simpler endpoints

### C5+ — Remaining Handlers (in priority order)
1. `/api/pir` POST — FROM_PENDING (config mutation)
2. `/api/time` POST — FROM_PENDING (RTC write, but atomic from caller's view)
3. `/api/config/device` POST — FROM_PENDING
4. `/api/config/password` POST — FROM_PENDING + credentialVersion hash (Phase B REV.3 §5)
5. `/api/config` POST — FROM_PENDING (user/pass change)
6. `/api/reboot` POST — FROM_PENDING* (special: no ACK dequeue before restart, §8)
7. `/api/factory_reset/confirm` POST — EXECUTING special (§6, journal destroyed by design)
8. `/api/config/import` POST — EXECUTING (bulk overwrite, not retry-safe)

### After all handlers refactored:
- Cross-ingress equivalence tests (X8-X14 from Phase B REV.3 §11.5)
- PWA requestId updates (api.ts + useApi.ts)
- Full integration testing

---

## KNOWN LIMITATIONS

1. **Only `/api/relay` + `/api/schedule` refactored** — 14 other REST endpoints still bypass TransactionJournal
2. **PWA does not send requestId** for REST mutations — PWA must be updated before firmware deployment
3. **F7/F8 use `setFailPutOnNthOccurrence`** — only tracks per-key counter, not per-namespace. Sufficient for current tests (each slot has unique key `tj_slot_<idx>_a/b`).
4. **F11 uses FileSystem failure injection** (`setFailNextAtomicWrite`) — does not test NVS failure during journal write for schedule (that's covered by relay's F7/F8 which use same journal API)
5. **type/action injection** modifies the parsed JSON doc in-place. Safe because doc is not re-serialized or sent to client. But handler's later use of `doc["type"]` would see injected value. Not an issue for current handlers.
6. **WebServerTest does not verify HTTP response headers** (ACAO, security headers) — only status code + body
7. **No concurrent-request test** — same requestId from two HTTP clients simultaneously. Journal's atomic NVS writes should handle this, but no explicit test.
8. **CommandHash.h comment vs behavior mismatch** (KNOWN LIMITATION #6) — comment says "unknown fields cause REJECTION" but `computeCommandHash` only IGNORES them. Actual rejection happens in `_handleCommand` (MQTT path). C2+ REST handlers MUST replicate this rejection for §11 cross-ingress contract. **NOT yet implemented in REST handlers.**

---

## GITHUB VERIFICATION

- **HEAD == origin/branch:** ❌ NO (23 commits ahead, not pushed)
- **Push blocked by:** Missing GitHub token
- **Local commit integrity:** ✅ All commits verified, working tree clean

### Commits to push (23 total):
```
828e362 fix: restore file modes to 100644 for newly added shim files
6d55b6d chore(test): persist F-P0-2 test infrastructure shims + discovery audit doc
4d8e9b5 fix(firmware): restore file modes to 100644 for C3 files
4cd5d0b feat(firmware): P2-2 F-P0-2 C3 — schedule handler journal wrap + synchronous saveSchedule fix
3e5adc1 fix(firmware): restore Preferences.h file mode to 100644
d01c4b0 fix(firmware): P2-2 F-P0-2 C2-CORR — real F7/F8 tests + P7/P8 strengthening
d31da05 fix(firmware): restore MqttClientDeps.h file mode to 100644
04ce47b feat(firmware): P2-2 F-P0-2 C2 — RestJournalHelper + relay handler journal wrap
7c6b980 fix(firmware): P2-2 F-P0-2 C1-CORR-4 — eliminate circular edge evidence via legacy oracle
d0e9cad fix(firmware): P2-2 F-P0-1 correction pass — 3 auditor corrections
... (13 more commits from earlier in session)
```

---

## AUDITOR NEXT SESSION INSTRUCTIONS

1. **Read this checkpoint document** (`docs/P2-2-F-P0-2-HANDOVER-CHECKPOINT.md`)
2. **Read commit `4cd5d0b`** (C3 main) — schedule handler journal wrap + synchronous saveSchedule
3. **Check source tree:**
   - `firmware/ScheduleHandlers.h` — refactored to use RestJournalHelper
   - `firmware/ConfigStore.h/.cpp` — `saveScheduleWithResult(bool) → bool` added
   - `firmware/RelayHandlers.h` — type='relay' injection for hash symmetry
4. **Verify regression:** `cd firmware/test/host && make -f Makefile.ws run` → 111/111 PASS
5. **Specifically verify F11:** `./web_server_test_bin 2>&1 | grep -A 10 F11`
   - saveSchedule failure → HTTP 503
   - RAM mutation occurred (schedCount == 1)
   - journal NOT COMMITTED (commit skipped)
   - journal entry preserved (clearEntry NOT called — INVARIANT B)
   - journal state == PENDING
6. **If approved → C3 CLOSED, GO to C4**
7. **If corrections needed →** follow same iterative discipline (build → test → fix → submit patch → wait for auditor)

---

## KEY PRINCIPLES (carry forward)

1. **Utils::computeCommandHash() is single source of truth** — do NOT change canonical hash schema
2. **INVARIANT A:** no mutation → journal EMPTY (clearEntry allowed)
3. **INVARIANT B:** mutation occurred → evidence preserved (NO clearEntry)
4. **HARD INVARIANT:** HTTP 200 implies journal == COMMITTED
5. **Source-level proof > test count** — F7/F8 are defensible because source control-flow + targeted failure injection + runtime trace are consistent
6. **RestJournalHelper = journal lifecycle ONLY** — NOT a business dispatcher (§4.4/§4.6)
7. **Cross-ingress contract (§11):** REST hash == MQTT hash for equivalent commands (type/action injection ensures this)
