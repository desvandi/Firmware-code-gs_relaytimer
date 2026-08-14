# CYCLE-8B-Rev1: Monotonic State Machine Fix

## Problem Statement (C8B-001)

Cycle 8B used `_saveEntryToNVSAtomic()` for TWO different operations:
1. Creating new PENDING intent (storeIntent)
2. Committing EXECUTING entry (commitTransaction)

The function's Phase 0 always cleared commit flag + set state=PENDING.
For the commit path, this RESET EXECUTING → PENDING before writing new blob.

**Crash scenario (C8B-001):**
```
T0  storeIntent() — state = PENDING
T1  markExecuting() — state = EXECUTING
T2  relay.execute() — GPIO = ON (physical relay changed)
T3  commitTransaction() — calls _saveEntryToNVSAtomic(idx, true)
T4    Phase 0: commit=0, state=PENDING  ← EXECUTING EVIDENCE DESTROYED
T5  power loss
T6  reboot
T7  journal loads: state=PENDING
T8  reconciliation: PENDING + desired=ON → FAILED
T9  PWA retries — command executes AGAIN (double-execute)
```

This is a **non-monotonic state machine** — the forbidden transition
`EXECUTING → PENDING` was happening implicitly during commit.

## Solution: Separated Operations

CYCLE-8B-Rev1 separates the two operations into distinct functions with
different NVS write sequences:

### `_createPendingEntryNVS()` — for NEW PENDING entries only

```
Phase 0: Clear commit flag (invalidate old entry if slot reused)
Phase 1: Write blob (state=PENDING)
Phase 1b: Persist writeIdx (new slot only)
Phase 2: Set state=PENDING (commit flag stays 0)
```

If crash during this: entry is PENDING (correct — execute didn't run).

### `_commitExecutingEntryNVS()` — for EXECUTING → COMMITTED only

```
(NO commit flag clear — preserves EXECUTING evidence)
Phase 1: Write blob with new ackJson (commit still 0, state still EXECUTING in NVS)
Phase 1b: Set state=COMMITTED (commit still 0 — not yet atomic)
Phase 2: Flip commit flag 0 → 1 (ATOMIC COMMIT POINT)
```

If crash during Phase 1: entry remains EXECUTING (blob may be partial,
but state is still EXECUTING — reconciliation marks UNKNOWN, NOT FAILED).

If crash during Phase 2: commit flag may be 0 or 1.
- If 0: entry is EXECUTING → reconciliation UNKNOWN
- If 1: entry is COMMITTED → replay ACK

### `markExecuting()` — persists attempt atomically

Previous: `attempt++` in RAM, then `_setTransactionStateNVS(EXECUTING)`.
If crash between: attempt lost on reboot.

Now: rewrites blob with incremented attempt + state=EXECUTING in single
NVS write sequence (blob write + state byte write).

## New State: UNKNOWN (fixes C8B-002)

Cycle 8B used `COMMITTED_UNKNOWN` for "cannot determine" cases. But
`COMMITTED_UNKNOWN` is durable (cannot be cleared), which means PWA could
never retry those commands.

CYCLE-8B-Rev1 introduces `UNKNOWN` as a separate state:
- **FAILED**: PROVEN not executed (only from PENDING + desired=ON + snapshot=OFF)
- **UNKNOWN**: CANNOT determine (from EXECUTING after crash, non-relay commands,
  idempotent PENDING+desired=OFF, or retry during RUNNING)
- **COMMITTED_UNKNOWN**: GPIO matches desired at boot reconciliation (durable,
  but with "physical contact state unknown" disclaimer — kept for backward compat
  with entries that have ackJson)

`UNKNOWN` is clearable (allows retry), `COMMITTED_UNKNOWN` is NOT clearable (durable).

## Monotonicity Validator

`_isTransitionAllowed(from, to)` enforces the state machine:

### ALLOWED transitions (forward only)
```
(none) → PENDING          (storeIntent — new entry)
PENDING → EXECUTING       (markExecuting)
EXECUTING → COMMITTED     (commitTransaction)
PENDING → UNKNOWN         (reconciliation — cannot determine)
EXECUTING → UNKNOWN       (reconciliation — cannot determine)
PENDING → FAILED          (reconciliation — proven not executed)
```

### FORBIDDEN transitions (would violate monotonicity)
```
EXECUTING → PENDING       (was C8B-001 bug — NOW BLOCKED)
COMMITTED → anything      (terminal)
COMMITTED_UNKNOWN → anything  (terminal)
UNKNOWN → PENDING/EXECUTING/COMMITTED  (semi-terminal — only clearable)
FAILED → PENDING/EXECUTING/COMMITTED   (semi-terminal — only clearable)
```

`_setTransactionStateNVS()` now checks `_isTransitionAllowed()` before writing.
If transition is forbidden, it logs an error and returns false.

## Reconciliation Logic (CYCLE-8B-Rev1)

### Boot reconciliation (uses SNAPSHOT)

| Journal State | channelId | desiredState | Snapshot | Result | Reasoning |
|---------------|-----------|--------------|----------|--------|-----------|
| PENDING | 0 (non-relay) | N/A | N/A | UNKNOWN | Cannot verify via GPIO |
| PENDING | >0 (relay) | ON | OFF | FAILED | Proven not executed (PENDING means execute never ran) |
| PENDING | >0 (relay) | OFF | OFF | UNKNOWN | Idempotent — cannot determine |

| Journal State | channelId | desiredState | Snapshot | Result | Reasoning |
|---------------|-----------|--------------|----------|--------|-----------|
| EXECUTING | any | any | OFF | UNKNOWN | Execute MAY have run — cannot determine |

**KEY CHANGE from Cycle 8B:**
- EXECUTING → UNKNOWN (was COMMITTED_UNKNOWN)
- PENDING + desired=OFF → UNKNOWN (was COMMITTED_UNKNOWN)
- This distinguishes "proven not executed" (FAILED) from "cannot determine" (UNKNOWN)

### Runtime reconciliation (during RUNNING phase)

`reconcileEntry()` now ALWAYS returns UNKNOWN (fixes C8B-004).

**Reason:** During RUNNING, GPIO is controlled by RelayEngine (scheduler/PIR/manual).
Live GPIO read does NOT prove whether THIS transaction's execute ran — it only
shows current RelayEngine output. Using GPIO equality as proof was the Cycle 8A/8B bug.

Callers must handle UNKNOWN explicitly:
- For idempotent commands (relay ON/OFF): retry is safe, treat like FAILED
- For non-idempotent commands: do NOT retry, surface to operator

## clearEntry() Fix (C8B-007)

`_clearSlotNVS()` now returns `bool` (success status).
`clearEntry()` checks return value before updating RAM state.

If NVS write fails:
- RAM state is NOT updated (prevents journal resurrection)
- Error logged
- Caller receives `false` return

## State-Transition Matrix (Complete)

### States
```
PENDING            — Intent stored, execute NOT yet called
EXECUTING          — Execute called, commit NOT yet done
COMMITTED          — Execute + commit succeeded (terminal, durable)
COMMITTED_UNKNOWN  — Reconciled: GPIO matches desired (terminal, durable, with disclaimer)
UNKNOWN            — Cannot determine (clearable, allows retry with caution)
FAILED             — Proven not executed (clearable, allows retry)
```

### Transitions

| From | To | Trigger | Function |
|------|-----|---------|----------|
| (none) | PENDING | storeIntent() | _createPendingEntryNVS() |
| PENDING | EXECUTING | markExecuting() | markExecuting() (blob rewrite + state write) |
| EXECUTING | COMMITTED | commitTransaction() | _commitExecutingEntryNVS() |
| PENDING | FAILED | reconcilePendingEntries() | _setTransactionStateNVS() |
| PENDING | UNKNOWN | reconcilePendingEntries() | _setTransactionStateNVS() |
| EXECUTING | UNKNOWN | reconcilePendingEntries() | _setTransactionStateNVS() |
| PENDING | UNKNOWN | reconcileEntry() (RUNNING) | _setTransactionStateNVS() |
| EXECUTING | UNKNOWN | reconcileEntry() (RUNNING) | _setTransactionStateNVS() |
| PENDING | (cleared) | clearEntry() | _clearSlotNVS() |
| EXECUTING | (cleared) | clearEntry() | _clearSlotNVS() |
| FAILED | (cleared) | clearEntry() | _clearSlotNVS() |
| UNKNOWN | (cleared) | clearEntry() | _clearSlotNVS() |

### Forbidden (blocked by _isTransitionAllowed)
```
EXECUTING → PENDING       (C8B-001 fix)
COMMITTED → any            (terminal)
COMMITTED_UNKNOWN → any   (terminal)
UNKNOWN → PENDING/EXECUTING/COMMITTED
FAILED → PENDING/EXECUTING/COMMITTED
```

## PWA Handling (CYCLE-8B-Rev1)

`_handleCommand()` in MqttClient.cpp now handles each state explicitly:

| State | PWA ACK | Action |
|-------|---------|--------|
| COMMITTED | success=true, replay ACK | None (duplicate) |
| COMMITTED_UNKNOWN | success=true, disclaimer | None (duplicate) |
| FAILED | success=false, "proven not executed" | clearEntry + retry |
| UNKNOWN | success=false, "AMBIGUOUS" | Do NOT auto-retry (surface to PWA) |
| PENDING/EXECUTING | reconcile → UNKNOWN | Then handle as UNKNOWN |

**KEY:** UNKNOWN does NOT auto-retry. PWA receives "AMBIGUOUS" message and
must decide based on command idempotency:
- Relay ON/OFF: idempotent, retry is safe
- Other commands: operator must verify device state

## Honest Limitations (unchanged from Cycle 8B)

1. **Snapshot reflects safe-OFF, not pre-crash state** — hardware limitation
2. **GPIO output ≠ physical relay contact** — welded/stuck undetectable
3. **Non-relay commands cannot be reconciled via GPIO** — marked UNKNOWN
4. **Hardware power-loss testing NOT RUN** — designed behavior only

## What This Cycle Does NOT Do

- Does NOT achieve pre-crash GPIO state recovery (hardware revision needed)
- Does NOT verify physical relay contact state (feedback hardware needed)
- Does NOT implement precharge (BLOCKED — needs multi-output transaction model)
- Does NOT add I/O expander support (BLOCKED — needs Cycle 8C architecture)
- Does NOT run hardware power-loss tests (T0-T10 matrix documents design only)

## Build Verification

- Development env: ✅ SUCCESS (92.1% flash, 16.8% RAM)
- Production env: ✅ SUCCESS
- Typecheck (PWA): N/A (no PWA changes in this cycle)
- Lint (PWA): N/A

**NOTE:** Build PASS only proves compilation. Does NOT prove:
- State machine correctness at runtime
- Crash recovery behavior
- Hardware power-loss safety
