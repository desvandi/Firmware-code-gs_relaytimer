# CYCLE-8C: Durable Transaction Journal v3 / Corruption-Safe State Machine

## Problem Statement (C8BR1-001, C8BR1-002)

Cycle 8B-Rev1 achieved monotonic state transitions but had two P0 failures
in **integrity metadata handling**:

### C8BR1-001: Corrupted blob erased execution evidence
When `_loadFromNVS()` detected CRC/magic/version failure, it freed the slot.
This DESTROYED evidence that transaction had reached EXECUTING (physical side
effect may have occurred). After corruption, requestId was lost, allowing
blind re-execution.

### C8BR1-002: clearEntry() resurrected transactions after reboot
`_clearSlotNVS()` only updated commit flag + state byte + RAM. The blob
(tj_entry_N) was NOT erased. After reboot, `_loadFromNVS()` would deserialize
the old blob and RESURRECT the cleared transaction.

## Solution: Corruption-Safe State Machine

### NEW STATE: CORRUPTED (terminal safety)

When blob CRC fails OR state byte invalid OR commit/state invariant violated:
- Entry is NOT freed, NOT cleared, NOT reused
- Marked CORRUPTED in NVS state byte (durable)
- `isProcessed()` returns true (requestId blocked — no blind re-execution)
- On retry: PWA receives "JOURNAL_CORRUPTED — recovery required" ACK
- No automatic retry, no automatic clear
- Operator must use `recoverCorruptedEntry()` (separate path)

### NEW STATE: EXECUTION_FAILED_OUTPUT_MISMATCH (terminal durable)

When GPIO readback != desired after execute:
- Transaction committed to journal with this state (durable)
- ackJson stored with failure ACK
- commit flag flipped to 1 (durable)
- `isProcessed()` returns true (requestId blocked)
- No auto-retry (operator must investigate physical relay)
- Different from UNKNOWN (cannot determine) — OUTPUT_MISMATCH means execute
  ran but produced wrong output (hardware problem, not transient)

### DURABLE TOMBSTONE for clearEntry() (fixes C8BR1-002)

Tombstone is a separate NVS key: `tj_tomb_<hash_of_requestId>`

**clearEntry() sequence:**
1. Write tombstone (durable marker that requestId was cleared)
2. Erase blob (tj_entry_N)
3. Clear commit flag + state byte
4. Update RAM

**On reboot, _loadFromNVS():**
- If blob is valid BUT tombstone exists → honor clear, do NOT resurrect
- Erase blob + clear slot

**Crash safety:**
- If crash after step 1 (tombstone written, blob not erased): on reboot,
  tombstone exists → entry NOT resurrected (blob is erased during load)
- If crash after step 2 (tombstone + blob erased): clean state
- If crash after step 3 (everything done): clean state

### BLOB/STATE/COMMIT INVARIANT VALIDATION (fixes C8BR1-004)

`_validateInvariant()` checks valid combinations:

| commit flag | state byte | Valid? | Action |
|-------------|------------|--------|--------|
| 0 | PENDING | ✅ | Accept as PENDING |
| 0 | EXECUTING | ✅ | Accept as EXECUTING |
| 0 | COMMITTED | ⚠️ | Transitional — treat as EXECUTING → reconcile UNKNOWN |
| 0 | UNKNOWN | ✅ | Accept as UNKNOWN |
| 0 | FAILED | ✅ | Accept as FAILED |
| 0 | CORRUPTED | ✅ | Accept as CORRUPTED |
| 0 | EXECUTION_FAILED_OUTPUT_MISMATCH | ✅ | Accept (durable but commit=0 is transitional) |
| 1 | COMMITTED | ✅ | Accept as COMMITTED (durable) |
| 1 | EXECUTION_FAILED_OUTPUT_MISMATCH | ✅ | Accept (durable failure) |
| 1 | anything else | ❌ | Mark CORRUPTED |

Invalid combinations → mark CORRUPTED (NOT free).

### STATE ENUM VALIDATION (fixes C8BR1-005)

`isValidTransactionState(raw)` checks raw is in valid enum range (0-7).
If invalid → mark CORRUPTED (NOT free).

### OUTPUT_MISMATCH as durable terminal (fixes C8BR1-008)

`commitTransactionFailed()` commits transaction with failure state:
- Stores ackJson (with failure ACK + diagnostic data)
- Sets state = EXECUTION_FAILED_OUTPUT_MISMATCH
- Flips commit flag to 1 (durable)
- No auto-retry (operator must investigate)

## State Machine (CYCLE-8C complete)

```
                    storeIntent()
(none) ──────────────────────────────► PENDING
                                          │
                                          │ markExecuting()
                                          ▼
                                       EXECUTING
                                          │
                           ┌──────────────┼──────────────┐
                           │              │              │
                           │              │              │ commitTransactionFailed()
                           │              │              │ (OUTPUT_MISMATCH)
                           │              │              ▼
                           │              │    EXECUTION_FAILED_OUTPUT_MISMATCH
                           │              │       (terminal durable)
                           │              │
                           │ commitTransaction()
                           │              │
                           ▼              ▼
                        COMMITTED    (reconcile)
                        (terminal)       │
                                         ▼
                                      UNKNOWN
                                     (clearable)

                    reconcilePendingEntries()
PENDING ──────────────────────────────────► FAILED (proven not executed)
                                           (clearable)

Any state ──► CORRUPTED (invariant violation detected)
              (terminal safety — only recoverCorruptedEntry can remove)
```

## State Properties

| State | isProcessed | isCommitted | Clearable | Durable | Terminal |
|-------|-------------|------------|-----------|---------|----------|
| PENDING | ❌ | ❌ | ✅ | ❌ | ❌ |
| EXECUTING | ❌ | ❌ | ✅ | ❌ | ❌ |
| COMMITTED | ✅ | ✅ | ❌ | ✅ | ✅ |
| COMMITTED_UNKNOWN | ✅ | ✅ | ❌ | ✅ | ✅ |
| UNKNOWN | ❌ | ❌ | ✅ | ❌ | ❌ |
| FAILED | ❌ | ❌ | ✅ | ❌ | ❌ |
| CORRUPTED | ✅ (blocks requestId) | ❌ | ❌ (use recoverCorruptedEntry) | ✅ | ✅ (safety) |
| EXECUTION_FAILED_OUTPUT_MISMATCH | ✅ | ✅ | ❌ | ✅ | ✅ |

## NVS-WRITE Crash Matrix (C8BR1 formal documentation)

### _createPendingEntryNVS()
```
Phase 0: putUChar(commitKey, 0)
  Crash → commit=0 (old entry invalidated, slot appears free)
Phase 1: putBytes(entryKey, blob)
  Crash → blob may be partial, commit=0 → on load, CRC fails → CORRUPTED
Phase 1b: putUChar(NVS_KEY_TJ_WIDX, nextWriteIdx)  [new slot only]
  Crash → writeIdx not advanced, slot may be overwritten → acceptable (old entry was invalid)
Phase 2: putUChar(stateKey, PENDING)
  Crash → state=PENDING (default) → on load, treated as PENDING → correct
```

### _commitExecutingEntryNVS()
```
Phase 1: putBytes(entryKey, blob_with_ackJson)
  Crash → blob may be partial, commit=0, state=EXECUTING → on load, CRC fails → CORRUPTED
Phase 1b: putUChar(stateKey, targetState)
  Crash → state=targetState, commit=0 → on load, invariant check: commit=0 + state=COMMITTED → transitional → reconcile UNKNOWN
Phase 2: putUChar(commitKey, 1)  [ATOMIC COMMIT POINT]
  Crash before → commit=0 → entry is EXECUTING (reconcile UNKNOWN)
  Crash after → commit=1 → entry is COMMITTED (durable)
```

### markExecuting()
```
Phase 1: putBytes(entryKey, blob_with_attempt)
  Crash → blob may be partial, commit=0, state=PENDING → on load, CRC fails → CORRUPTED
Phase 2: putUChar(stateKey, EXECUTING)
  Crash → state=PENDING → on load, treated as PENDING → reconcile (FAILED if desired=ON)
  (attempt counter may be lost, but state is correct)
```

### clearEntry() (with tombstone)
```
Phase 1: _writeTombstoneNVS(requestId) — putULong(tombstone_key, timestamp)
  Crash → tombstone exists → on load, entry NOT resurrected (even if blob valid)
Phase 2: _eraseBlobNVS(idx) — prefs.remove(entryKey)
  Crash → blob may still exist, but tombstone protects → on load, blob erased + slot freed
Phase 3: _clearSlotNVS(idx) — putUChar(commitKey, 0) + putUChar(stateKey, PENDING)
  Crash → commit=0, state=PENDING, tombstone exists → on load, slot freed correctly
```

## Honest Limitations (unchanged)

1. **Snapshot reflects safe-OFF, not pre-crash state** — hardware limitation
2. **GPIO output ≠ physical relay contact** — welded/stuck undetectable
3. **Non-relay commands cannot be reconciled via GPIO** — marked UNKNOWN
4. **Hardware power-loss testing NOT RUN** — designed behavior only

## What This Cycle Does NOT Do

- Does NOT achieve pre-crash GPIO state recovery (hardware revision needed)
- Does NOT verify physical relay contact state (feedback hardware needed)
- Does NOT implement precharge (BLOCKED — needs multi-output transaction model)
- Does NOT add I/O expander support (BLOCKED — needs Cycle 8C architecture)
- Does NOT run hardware power-loss tests (crash matrix documents design only)
- Does NOT move ALL validation before storeIntent for non-relay commands (C8BR1-003 — partial fix, schedule/PIR/channel validation still after storeIntent)

## Build Verification

- Development env: ✅ SUCCESS (92.7% flash, 16.8% RAM)
- Production env: ✅ SUCCESS

**NOTE:** Build PASS only proves compilation. Does NOT prove:
- State machine correctness at runtime
- Crash recovery behavior
- Hardware power-loss safety
