# Phase 2 — TransactionJournal Rev26 + Command Integration + Recovery Semantics

**Status:** 🔴 NOT AUTHORIZED — this document is the contract for Phase 2 work.
Engineering may NOT begin Phase 2 implementation until:
1. Phase 1 is approved by auditor (after Closure-C/D/E/F re-audit).
2. Auditor explicitly authorizes "Phase 2 may begin" against this scope document.

**Parent contract:** [`CYCLE-8C-REV26-FINAL-PREDICATE.md`](CYCLE-8C-REV26-FINAL-PREDICATE.md)
**Foundation:** Phase 1 — `JournalRecord` (audit target: HEAD of `origin/engineering-cycle-8c-rev26-final-predicate` — SHA resolved externally by auditor per "Audit Traceability Rule" in [`README.md`](../README.md); historical chain: documentation closure `9fd7473`, implementation baseline `c506c80` with host test 102/102 PASS, initial implementation `2e4de87`)
**Auditor mandate:** This document was created in response to the auditor's
2026-08-14 disposition requiring a formal Phase-2 scope contract BEFORE
implementation begins.

---

## 1. Purpose

Phase 1 proved the `JournalRecord` primitive (serialize/deserialize/CRC/
canonicalEqual/classifyGeneration). Phase 2 proves the **system** that uses
that primitive: `TransactionJournal.cpp` rewritten to the Rev26 normative
contract, integrated with `MqttClient.cpp` command execution, with crash/
recovery semantics proven at every mutation point.

Phase 2 is **boundary-separated** from Phase 3 (16-channel migration) per
auditor instruction. No I/O expander work, no RelayDriver migration, no
channel-count changes in Phase 2.

---

## 2. Phase 2 Scope (P2-1, P2-2, P2-3)

### P2-1 — TransactionJournal Rev26 implementation

Rewrite `firmware/TransactionJournal.h` and `firmware/TransactionJournal.cpp`
to implement the Rev26 normative contract. The current pre-Rev26
implementation (using `tj_entry_N` + `tj_commit_N` two-phase commit) MUST be
removed entirely — no dead code, no fallback path, no "legacy mode".

Minimum implementation must cover:

| Rev26 contract element | Implementation deliverable |
|---|---|
| Dual-copy persistence | Each journal slot stores two NVS keys (`tj_slot_N_a`, `tj_slot_N_b`), each `BLOB_SIZE=1200` bytes. |
| Generation ordering | Use `Services::classifyGeneration()` from Phase 1. Generation assigned by journal, not by caller. |
| Canonical equivalence | Use `Services::canonicalEqual()` / `Services::canonicalEqualBlobs()` from Phase 1. |
| CRC validation | Use `Services::verifyRecordCRC()` from Phase 1. |
| `ObservationGuard` (RAII) | Sets `_observing=true` on construction, panic on nested observation, resets on destruction. |
| `_assertMutationAllowed()` | Called at entry of every mutation API. Panics if `_observing==true`. |
| `_assertExecutorContext()` | Called at entry of every public API. Panics if caller is not the journal executor task. |
| 9-row recovery decision table | Per `CYCLE-8C-REV14-MUTATION-CONSOLIDATION.md` §I1. Picks VALID copy, repairs INVALID copy bitwise, or quarantines when both INVALID. |
| Recovery semantics | `recoverCorruptedEntry()` writes `EMPTY(gen=0)` to both copies, verifies both, gen=0 unconditional. |
| Repair = bitwise restoration | `REPAIR(B)` when A=VALID, B=INVALID: read A's full record (incl. generation), write IDENTICAL record to B (same gen, same payload), verify B. |
| ACK lifecycle separation (I3) | `tj_ackq` blob persists ACK delivery states independently of journal entries. |
| Eviction safety (I2a–I2e) | 5 conjunctive predicates; default RETAIN when uncertain. NON_IDEMPOTENT eviction = NEVER (since `AUTH_EVIDENCE_AUTHENTICATED` is currently UNACHIEVABLE). |
| Generation construction | Protocol produces distance 0 (same/repair) or 1 (adjacent mutation). Loader validates distance is 0 or 1, else CORRUPTED. |
| Mutation API list | `storeIntent()`, `markExecuting()`, `commitTransaction()`, `commitTransactionFailed()`, `clearEntry()`, `recoverCorruptedEntry()`, `_repairSlot()`, `_writeCopy()`, `_eraseBlobNVS()`, `_clearSlotNVS()` — all MUST call `_assertMutationAllowed()`. |
| Observation API list | `_checkI1Satisfied()`, `_evaluateSlot()`, `reconcilePendingEntries()`, `_loadFromNVS()` (read phase), `_readCopy()` (when called for evaluation) — all use `ObservationGuard`. |

**Acceptance:** Every line of `TransactionJournal.cpp` must be traceable
one-to-one to a Rev26 predicate. Auditor will verify by reading source
against `CYCLE-8C-REV26-FINAL-PREDICATE.md`. No `tj_entry_N` or
`tj_commit_N` references may remain in the codebase after P2-1 completes.

### P2-2 — Command execution integration

Prove that the actual command lifecycle uses the Rev26 journal — not just
that `TransactionJournal` compiles in isolation. The trace must be
demonstrable end-to-end:

```
MQTT command received
    ↓
authentication / validation (JWT, requestId format, command schema)
    ↓
command classification (IDEMPOTENT vs NON_IDEMPOTENT vs UNKNOWN)
    ↓
journal lookup / dedup check (requestId against recent entries)
    ↓
execution decision (skip+replay-ACK if found, or proceed if new)
    ↓
storeIntent() — write PENDING entry to journal (intent-first)
    ↓
markExecuting() — transition PENDING → EXECUTING
    ↓
physical/state mutation (RelayEngine.execute(), Scheduler.upsert(), etc.)
    ↓
GPIO readback reconciliation (verify output matches desiredState)
    ↓
commitTransaction() — transition EXECUTING → COMMITTED (or COMMITTED_UNKNOWN if readback mismatch)
    ↓
ACK generation (typed ACK per command type)
    ↓
ACK persistence to tj_ackq (delivery state = ACK_NOT_SENT)
    ↓
ACK publish to MQTT broker
    ↓
broker PUBACK received → ACK_BROKER_CONFIRMED
    ↓
PWA ack_confirm received → ACK_PWA_RECEIVED
```

**Critical invariant (auditor-specified):** There MUST NOT exist any code
path of the form:

```
execute → ACK → journal
```

This re-opens the execute→store gap that Rev26 is designed to close.
Auditor will grep for any mutation-then-journal-write ordering and reject
the implementation if found. The ONLY acceptable ordering is:

```
journal-write (intent) → execute → journal-commit
```

Files in scope for P2-2:
- `firmware/MqttClient.cpp` — `_handleCommand()` and `_handleOta()` must both route through `TransactionJournal.storeIntent()` before any side effect.
- `firmware/RelayEngine.cpp` — `execute()` must accept a `requestId` parameter and rely on the journal for idempotency, not internal state.
- `firmware/Scheduler.cpp` — schedule upserts/deletes must be journaled (NON_IDEMPOTENT classification pending — see Rev26 I2 matrix).
- `firmware/OtaManager.cpp` — OTA must use intent-first pattern (closes Cycle-7 finding I-004).

Files NOT in scope for P2-2 (deferred to Phase 3):
- `firmware/RelayDriver.cpp` — driver-level code is channel-count dependent; remains 12-channel.
- `firmware/Config.h` — `NUM_CHANNELS` remains 12 in Phase 2.

### P2-3 — Crash/recovery semantics

For each mutation point defined by Rev26, the implementation MUST clearly
document and prove (via hardware test) the answers to 7 questions:

| # | Question | Required answer |
|---|----------|-----------------|
| 1 | What is already persistent at this point? | Named NVS keys + their committed state. |
| 2 | What is NOT yet persistent at this point? | Named NVS keys + their pending state. |
| 3 | What happens if ESP32 resets here? | Boot recovery decision tree branch. |
| 4 | Which record is authoritative after reboot? | Copy A, Copy B, or quarantined. |
| 5 | Can the command be re-executed? | YES (and is it safe?) or NO (and how is it blocked?). |
| 6 | Can the ACK be reconstructed? | YES (from where?) or NO (and what's the fallback?). |
| 7 | Is duplicate execution possible? | YES (acceptable per threat model?) or NO. |

Mutation points that MUST be covered (per Rev26 contract):

1. Before `storeIntent()` write
2. After `storeIntent()` write, before `markExecuting()`
3. After `markExecuting()`, before physical relay mutation
4. After physical relay mutation, before GPIO readback
5. After GPIO readback, before `commitTransaction()`
6. After `commitTransaction()`, before ACK generation
7. After ACK generation, before ACK persistence to `tj_ackq`
8. After ACK persistence, before MQTT publish
9. After MQTT publish, before broker PUBACK
10. After broker PUBACK, before PWA `ack_confirm`
11. After PWA `ack_confirm`, before ACK delivery state update
12. After ACK delivery state update, before `tj_ackq` re-persist

(12 mutation points — distinct from the 12 power-loss tests, which are
hardware acceptance criteria. These 12 are recovery-semantics documentation
requirements that the implementation MUST answer in code comments + design
docs, and the hardware tests must prove each.)

---

## 3. Phase 2 Acceptance Criteria

Phase 2 acceptance MUST NOT be "102/102 host test PASS" — that was Phase 1's
criterion. Phase 2 acceptance is multi-tiered per auditor specification:

### A. Static / code evidence

- `firmware/TransactionJournal.cpp` rewritten; every function traceable to
  Rev26 predicate.
- No `tj_entry_N` / `tj_commit_N` references remain in codebase
  (grep verification).
- Every mutation API calls `_assertMutationAllowed()` and
  `_assertExecutorContext()`.
- Every observation API uses `ObservationGuard` RAII.
- Code review report mapping each function → Rev26 contract section.

### B. Host tests (extend Phase 1 harness)

- New test file: `firmware/test/host/TransactionJournalTest.cpp`.
- Tests must cover: dual-copy reconciliation, 9-row recovery decision table,
  generation assignment + ordering, eviction safety (I2a–I2e), ACK lifecycle
  separation, ObservationGuard panic on nested observation, mutation panic
  during observation.
- All host tests MUST PASS with exit code 0.

### C. ESP32 runtime tests

- Flash firmware to ESP32-WROOM-32.
- Verify boot completes without panic.
- Verify serial output shows journal initialization (slot count, recovered
  entries, quarantined slots if any).
- Verify MQTT command round-trip (relay ON, relay OFF, schedule upsert).
- Verify journal persistence across reboot (send command, reboot, send same
  requestId → ACK replayed, NOT re-executed).

### D. Reboot / recovery tests

- 12 mutation points from P2-3, each tested with forced ESP32 reset.
- For each: capture pre-reset serial log, post-reset serial log, verify
  recovery matches documented semantics.
- All 12 mutation points MUST produce documented recovery behavior.

### E. Power-loss tests (12 scenarios, re-baselined on Rev26)

- `TEST_PLAN.md` rewritten for Rev26 dual-copy architecture.
- Each test scenario cuts power at a specific mutation point.
- Pass criteria: no duplicate execution of committed commands; no ACK
  loss without `tj_ackq` recovery; no slot corruption without quarantine.
- All 12 MUST PASS on actual ESP32 hardware, 3 consecutive runs each
  (to rule out flaky results).

### F. Integration tests

- Full PWA → MQTT broker → ESP32 → relay → ACK → PWA round-trip.
- Includes: WiFi disconnect mid-command, broker disconnect mid-ACK,
  PWA retry after timeout, PWA `ack_confirm` after ACK delivery.
- Each scenario documented with serial log + PWA console log.

---

## 4. Out of Scope for Phase 2

The following are explicitly OUT OF SCOPE and must NOT be touched in Phase 2:

| Item | Reason |
|---|---|
| 16-channel migration | Phase 3 — auditor requires separate audit gate. |
| I/O expander (PCF8575, MCP23017, etc.) | Phase 3 — architecture TBD. |
| `RelayDriver.cpp` channel-count changes | Phase 3. |
| `Config.h` `NUM_CHANNELS` change | Phase 3. |
| PWA channel mapping update | Phase 3. |
| Precharge circuit | BLOCKED in Rev26 (requires `AUTH_EVIDENCE_AUTHENTICATED` which is UNACHIEVABLE). |
| Sender-auth / MQTT ACL enforcement | BLOCKED in Rev26 (DEPLOYMENT_AUTH_CONFIGURED is broker-side, not firmware-verifiable). |
| Flash Encryption / Secure Boot provisioning | Out of audit scope (operational hardening, not Phase 2 deliverable). |
| LittleFS migration for large NVS blobs | Phase 2 may add it if NVS wear becomes blocking, but not required by Rev26. |

---

## 5. Audit Gate Sequence

```
PHASE 1 (JournalRecord foundation)
    │
    ▼
[AUDIT GATE — auditor re-reviews Closure-C/D/E/F]
    │
    ▼  (if approved)
PHASE 2 (this document)
    │
    ▼  (when implementation complete)
[AUDIT GATE — auditor re-reviews P2-1/P2-2/P2-3 + acceptance A–F]
    │
    ▼  (if approved)
PHASE 3 (16-channel migration — see docs/PHASE-3-16-CHANNEL-MIGRATION-SCOPE.md)
    │
    ▼  (when implementation complete)
[AUDIT GATE — auditor re-reviews hardware migration]
    │
    ▼  (if approved)
HARDWARE ACCEPTANCE
    │  (12 power-loss tests on actual 16-channel ESP32 hardware,
    │   Ed25519 KAT on actual ESP32, brownout tests, integration tests)
    ▼  (if all PASS)
220V PRODUCTION REVIEW
```

---

## 6. Auditor Principle (formalized 2026-08-14)

> Approval is granted only after artefacts themselves demonstrate that
> requirements are met — not after engineering states that work "has been
> done".

For Phase 2, this means:
- "I rewrote TransactionJournal.cpp" → ❌ not approval evidence.
- "Here is the rewritten TransactionJournal.cpp + a function-by-function
  mapping to Rev26 predicates + host tests PASS + ESP32 runtime tests PASS +
  12 power-loss tests PASS" → 🟡 candidate evidence, pending auditor
  independent inspection.

Engineering must submit artefacts, not reports.

---

## 7. Open Questions for Auditor (pre-Phase 2)

Before Phase 2 implementation begins, engineering requests clarification on:

1. **NVS key naming for dual-copy**: Rev26 does not specify exact NVS key
   names. Engineering proposes `tj_slot_<idx>_a` and `tj_slot_<idx>_b` for
   the two copies, `tj_slot_<idx>_gen` for the slot's last-assigned
   generation (if needed). Auditor confirmation requested.

2. **`tj_ackq` blob size**: Rev26 specifies 2056 bytes (256 bytes per ACK
   record × 8 records + 4 byte CRC + 4 byte header). NVS blob limit on
   ESP32 is ~50KB, so this fits. Auditor confirmation requested.

3. **ObservationGuard panic vs. graceful degradation**: Rev26 specifies
   `panic()` on nested observation or mutation during observation. In
   production, panic = device reboot. Auditor confirmation that this is
   acceptable (vs. returning an error code) requested.

4. **NON_IDEMPOTENT command list**: Rev26 lists OTA update, factory reset,
   future precharge as NON_IDEMPOTENT. Schedule upsert/delete are currently
   classified as IDEMPOTENT in Rev14 §I2 — but they can create duplicates
   (capped at 4/channel). Auditor confirmation that IDEMPOTENT classification
   for schedule upsert is acceptable, or reclassification requested.

5. **`AUTH_EVIDENCE_AUTHENTICATED` unachievable status**: Rev26 acknowledges
   this is currently UNACHIEVABLE, so NON_IDEMPOTENT eviction = NEVER.
   Is this an acceptable permanent state, or should Phase 2 (or a future
   phase) begin work on sender-auth to enable NON_IDEMPOTENT eviction?

Engineering will await auditor answers before starting P2-1 implementation.
