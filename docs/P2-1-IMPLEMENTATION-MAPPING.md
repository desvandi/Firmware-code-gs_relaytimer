# P2-1 Implementation Mapping — TransactionJournal Rev26

**Status:** P2-1 implementation in progress (Phase 2 authorized 2026-08-14)
**Parent contract:** `docs/PHASE-2-SCOPE.md` §P2-1
**Normative design:** `docs/CYCLE-8C-REV26-FINAL-PREDICATE.md` + `docs/CYCLE-8C-REV14-MUTATION-CONSOLIDATION.md`

This document maps each Rev26 contract element to its concrete implementation
location in `firmware/TransactionJournal.h` and `firmware/TransactionJournal.cpp`.
It is a companion for code review — the auditor should be able to read each
row, look at the cited source location, and verify one-to-one correspondence
with the Rev26 predicate.

---

## 1. Storage model replacement (pre-Rev26 → Rev26)

| Pre-Rev26 (REMOVED) | Rev26 (NEW) |
|---|---|
| `tj_entry_<idx>` (blob, 1-copy) | `tj_slot_<idx>_a` + `tj_slot_<idx>_b` (dual-copy, each `BLOB_SIZE=1200` bytes) |
| `tj_commit_<idx>` (1-byte commit flag) | (eliminated — commit is encoded in `recordState` byte of canonical payload; durability via dual-copy canonical equivalence) |
| `tj_tomb_<hash>` (clear tombstone) | (eliminated — clearEntry writes EMPTY record to both copies, generation preserved) |
| `_journalIds[idx]`, `_journalHashes[idx]`, `_journalAcks[idx]` etc (parallel RAM arrays) | `JournalRecord _slots[idx]` (single struct per slot, using Phase 1 primitive) |
| `BLOB_VERSION = 2`, 8-byte header | `JOURNAL_SCHEMA_VERSION = 4`, 11-byte header per Phase 1 `JournalRecord.h` |
| `_computeCRC()` (internal CRC) | `Services::computeRecordCRC()` / `Services::verifyRecordCRC()` from Phase 1 |
| Two-phase commit (write blob → flip commit flag) | Single-phase dual-copy write (write A → verify A → write B → verify B → generation++ for next mutation) |
| `storeTransaction()` legacy API | (REMOVED — `storeIntent()` is the only entry path) |

## 2. Invariant enforcement (Rev26 I0 / I0a)

| Rev26 predicate | Implementation location |
|---|---|
| I0: Journal API only from executor task | `TransactionJournal::_assertExecutorContext()` — called at entry of every public API; panics if `xTaskGetCurrentTaskHandle() != s_journalExecutorTask` |
| I0a: Observation/mutation mutually exclusive | `TransactionJournal::_assertMutationAllowed()` — called at entry of every mutation API; panics if `_observing == true` |
| ObservationGuard RAII | `class ObservationGuard` — constructor sets `_observing = true` (panics if already true), destructor resets to false |
| Mutation API list (Rev26 normative) | `storeIntent()`, `markExecuting()`, `commitTransaction()`, `commitTransactionFailed()`, `clearEntry()`, `recoverCorruptedEntry()`, `_repairSlot()`, `_writeCopy()`, `_eraseBlobNVS()`, `_clearSlotNVS()` — all call `_assertMutationAllowed()` at entry |
| Observation API list (Rev26 normative) | `_checkI1Satisfied()`, `_evaluateSlot()`, `reconcilePendingEntries()`, `_loadFromNVS()` (read phase), `_readCopy()` (when called for evaluation) — all use `ObservationGuard` RAII |

## 3. Record storage (Rev26 I1)

| Rev26 predicate | Implementation location |
|---|---|
| Record layout | `firmware/JournalRecord.h` (Phase 1 — unchanged in P2-1) |
| Magic + schema + generation + CRC + canonical payload | `Services::serializeRecord()` from Phase 1 |
| Safe parse with bounds checks | `Services::deserializeRecord()` from Phase 1 |
| Canonical equivalence | `Services::canonicalEqual()` / `Services::canonicalEqualBlobs()` from Phase 1 |
| Generation ordering (wrap-safe) | `Services::classifyGeneration()` from Phase 1 |
| Generation construction (distance 0 or 1) | `TransactionJournal::_assignGeneration()` — successor of latest committed generation in slot; loader validates distance via `classifyGeneration` |
| Recovery decision table (9 rows) | `TransactionJournal::_reconcileSlot()` — implements the 9-row table from `CYCLE-8C-REV14-MUTATION-CONSOLIDATION.md` §I1 |
| Repair = bitwise restoration | `TransactionJournal::_repairSlot()` — reads VALID copy, writes IDENTICAL record to INVALID copy (same gen, same payload), verifies |
| Quarantine (both INVALID) | `TransactionJournal::_quarantineSlot()` — marks slot as CORRUPTED, does NOT free; operator must call `recoverCorruptedEntry()` |
| `recoverCorruptedEntry()` | Writes `EMPTY(gen=0)` to both copies, verifies both, gen=0 unconditional |

## 4. ACK lifecycle (Rev26 I3)

| Rev26 predicate | Implementation location |
|---|---|
| `tj_ackq` blob (2056 bytes: count + reserved + AckRecord[8] + queueCRC) | `firmware/TransactionJournal.cpp` `_loadAckQueue()` / `_persistAckQueue()` |
| AckRecord format (ackMagic + ackVersion + deliveryState + requestId + commandHash + retryCount + lastAttemptTs + ackJson) | `TransactionJournal::AckRecord` struct |
| ACK delivery states (5): NOT_SENT, PUBLISH_ACCEPTED, BROKER_CONFIRMED, PWA_RECEIVED, FAILED_EXHAUSTED | `enum class AckDeliveryState : uint8_t` |
| Transaction lifecycle independent of ACK delivery | `TransactionJournal::queueAck()` does NOT touch journal entry; `clearEntry()` does NOT touch ackq |
| Boot recovery = MERGE journal + ACK queue | `TransactionJournal::_loadFromNVS()` reads both, merges (existing ACKs kept, missing ones added from journal COMMITTED entries with non-empty ackJson) |
| Orphaned ACKs retained | If transaction evicted but ACK not delivered, ACK stays in `tj_ackq` |
| `ackDigest` (content binding, not auth) | `TransactionJournal::computeAckDigest()` — SHA-256(ackJson)[0:16] hex; used by `ack_confirm` protocol |

## 5. Eviction safety (Rev26 I2 / I2a–I2e + auth gate)

| Rev26 predicate | Implementation location |
|---|---|
| I2a: Retention policy | `TransactionJournal::_isEvictionPermitted()` — checks `journal_is_full && slot_is_needed` |
| I2b: Command class | `TransactionJournal::_classifyCommand()` — IDEMPOTENT / NON_IDEMPOTENT / UNKNOWN |
| I2c: ACK condition | Per `I2` matrix in `CYCLE-8C-REV14-MUTATION-CONSOLIDATION.md` |
| I2d: No unresolved recovery | Check `_slots[idx].recordState == COMMITTED` (not CORRUPTED/QUARANTINED) |
| I2e: Default RETAIN | If any check uncertain → return false |
| Auth gate (NON_IDEMPOTENT) | `AUTH_EVIDENCE_AUTHENTICATED` is UNACHIEVABLE → NON_IDEMPOTENT eviction = NEVER (per Rev26 §2) |
| Current achievable eviction | IDEMPOTENT + (PUBLISH_ACCEPTED+durable_queue OR FAILED_EXHAUSTED) → YES; everything else → NO |

## 6. Command classification (per auditor Q4)

| Command type | Classification | Semantic proof required |
|---|---|---|
| relay ON / OFF | IDEMPOTENT | f(f(x)) = f(x): setting ON twice = setting ON once ✓ |
| set_mode | IDEMPOTENT | mode is a single enum value, last-write-wins ✓ |
| schedule upsert | OPEN | Must prove: upsert with same identity produces same state regardless of call count. If proof fails → reclassify to NON_IDEMPOTENT |
| schedule delete | OPEN | Must prove: deleting twice = deleting once (already-deleted is no-op) |
| PIR config | IDEMPOTENT | config is a single value, last-write-wins ✓ |
| channel rename | IDEMPOTENT | rename is a single value, last-write-wins ✓ |
| time set | IDEMPOTENT | time is monotonic, last-write-wins (with monotonicity guard) ✓ |
| config set | IDEMPOTENT | config is a single value, last-write-wins ✓ |
| OTA update | NON_IDEMPOTENT | flash write is destructive; re-flash with same binary is safe but re-flash with different binary is not — classify as NON_IDEMPOTENT for safety |
| factory reset | NON_IDEMPOTENT | destructive, cannot be undone |
| (future) precharge | NON_IDEMPOTENT | (BLOCKED — not implemented) |
| UNKNOWN | treat as NON_IDEMPOTENT (default safe) | any command type not yet classified |

**Auditor Q4 proof obligation:** Before P2-2 uses any classification for
eviction safety, engineering must produce a written semantic proof for that
specific command. Until proof is produced, classification must default to
NON_IDEMPOTENT (which means eviction = NEVER per Rev26 §4).

## 7. Boot recovery (Rev26 — preserved from Cycle-8B)

Boot phase management is preserved from Cycle-8B (not changed by Rev26):

| Phase | Action |
|---|---|
| PRE_INIT | Initial state |
| SAFE_INIT | GPIO set to safe defaults (relays OFF) |
| LOADING_NVS | Read `tj_slot_*_a/b` for all slots, reconcile per 9-row table |
| SNAPSHOT | Capture current output state |
| RECONCILING | Reconcile PENDING/EXECUTING entries against snapshot |
| RESTORING | Restore committed states to physical outputs |
| RUNNING | Normal operation; mutations allowed |

## 8. Public API (backward-compatible surface for P2-2)

P2-1 preserves the public API surface that `MqttClient.cpp` / `firmware_v4.ino`
/ `RelayEngine.cpp` currently depend on, so they continue to compile.
**Internal storage model is completely replaced** (dual-copy + generation
+ Rev26 invariants). P2-2 will rewire the command path to use the new
intent-first pattern; P2-1 only ensures the journal compiles + passes host
tests + firmware builds.

| Public API method | Status in P2-1 | Notes |
|---|---|---|
| `begin()` | ✅ implemented | Initializes NVS, loads slots via 9-row reconciliation, loads ackq |
| `setBootPhase()`, `getBootPhase()`, `isRunning()` | ✅ preserved | Boot phase management unchanged |
| `captureOutputSnapshot()`, `getSnapshotState()` | ✅ preserved | Output snapshot unchanged |
| `storeIntent()` | ✅ implemented Rev26 | Writes PENDING entry to both copies with new generation |
| `markExecuting()` | ✅ implemented Rev26 | PENDING → EXECUTING transition, writes both copies |
| `commitTransaction()` | ✅ implemented Rev26 | EXECUTING → COMMITTED, writes both copies |
| `commitTransactionFailed()` | ✅ implemented Rev26 | EXECUTING → FAILED / EXECUTION_FAILED_OUTPUT_MISMATCH |
| `clearEntry()` | ✅ implemented Rev26 | Writes EMPTY to both copies (subject to eviction safety I2a-I2e) |
| `recoverCorruptedEntry()` | ✅ implemented Rev26 | Writes EMPTY(gen=0) to both copies unconditionally |
| `isProcessed()`, `isCommitted()` | ✅ implemented | Lookup against in-RAM slot cache |
| `getTransactionState()`, `getCommandHash()`, `getAckJson()` etc | ✅ implemented | Lookup against in-RAM slot cache |
| `reconcilePendingEntries()`, `reconcileEntry()` | ✅ implemented Rev26 | Uses 9-row table |
| `queueAck()`, `processPendingAcks()`, `dequeueAck()` | ✅ implemented Rev26 | Persists to `tj_ackq` blob |
| `getPendingAckCount()`, `getJournalSize()` | ✅ implemented | Returns in-RAM counters |
| `storeTransaction()` (legacy) | ❌ REMOVED | Was deprecated; P2-1 removes it entirely |

## 9. Host test coverage (P2-1 acceptance criterion B)

Host test file: `firmware/test/host/TransactionJournalTest.cpp`

| Test group | Coverage |
|---|---|
| Dual-copy write + read back | Write record to slot N copies A and B; read both; verify canonical equivalence |
| 9-row recovery decision table | For each row: VALID+INVALID → repair; INVALID+VALID → repair; both INVALID → quarantine; GEN_NEWER_A → load A; GEN_NEWER_B → load B; GEN_EQUAL+canonicalEqual → load either; GEN_EQUAL+divergent → CORRUPTED; GEN_AMBIGUOUS → CORRUPTED; GEN_INVALID → CORRUPTED |
| Generation assignment | New record gets gen = latest+1; repair gets gen = source gen; recovery gets gen=0 |
| ObservationGuard panic on nested observation | Construct guard while another guard active → panic |
| Mutation panic during observation | Call `storeIntent()` while `_observing==true` → panic |
| Executor context panic | Call public API from non-registered task → panic (host test simulates via task handle swap) |
| ACK queue persistence | Queue ACK, persist, reload, verify state preserved |
| ACK queue merge on boot | Pre-populate journal with COMMITTED entries + pre-populate ackq; reload; verify merge adds missing |
| Eviction safety I2a-I2e | For each command class × ACK state combination, verify eviction predicate returns correct result |
| `recoverCorruptedEntry()` | Quarantined slot → recoverCorruptedEntry → both copies EMPTY(gen=0) |
| Repair bitwise identity | VALID A + INVALID B → repair → A and B byte-identical (same generation, same payload) |
| `clearEntry()` constraints | Cannot clear COMMITTED / CORRUPTED / QUARANTINED via clearEntry; only recoverCorruptedEntry can resolve those |

## 10. Out of scope for P2-1

The following are explicitly NOT implemented in P2-1 (deferred to P2-2 / P2-3):

| Item | Deferred to |
|---|---|
| MqttClient.cpp command path integration | P2-2 |
| RelayEngine.cpp requestId parameter | P2-2 |
| OTA path intent-first migration | P2-2 |
| Schedule upsert/delete semantic proof | P2-2 (auditor Q4) |
| Hardware power-loss tests | P2-3 + hardware acceptance |
| 12 mutation point recovery semantics documentation | P2-3 |
| ESP32 runtime verification | P2-3 + hardware acceptance |

## 11. Auditor verification checklist (P2-1 closure review)

When P2-1 implementation is submitted, the auditor should verify:

1. ✅ Source `TransactionJournal.h` and `TransactionJournal.cpp` are completely rewritten per Rev26
2. ✅ No `tj_entry_N` / `tj_commit_N` references in active code (grep verification)
3. ✅ No "two-phase commit" reference in active code (grep verification)
4. ✅ Every mutation API calls `_assertMutationAllowed()` (read source)
5. ✅ Every observation API uses `ObservationGuard` RAII (read source)
6. ✅ 9-row recovery table is implemented (read `_reconcileSlot()`)
7. ✅ `tj_ackq` blob is 2056 bytes with schema + CRC validation (read source)
8. ✅ Eviction predicate implements I2a-I2e (read `_isEvictionPermitted()`)
9. ✅ NON_IDEMPOTENT eviction = NEVER (since AUTH_EVIDENCE_AUTHENTICATED is UNACHIEVABLE)
10. ✅ `recoverCorruptedEntry()` writes EMPTY(gen=0) to both copies unconditionally
11. ✅ Repair = bitwise restoration (read `_repairSlot()`)
12. ✅ Host tests PASS (run `firmware/test/run_host_tests.sh`)
13. ✅ Firmware builds (development + production) without errors
14. ✅ Diff vs Phase 1 baseline shows only TransactionJournal + new test files changed (no other firmware source touched)
