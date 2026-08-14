# CYCLE-8A: Transaction Recovery State Machine — T0-T10 Crash Matrix

<!-- SUPERSEDED BANNER -->
<!-- ╔══════════════════════════════════════════════════════════╗ -->
<!-- ║  ⚠️  HISTORICAL DOCUMENT — DESIGN CONTEXT ONLY             ║ -->
<!-- ║  This document is part of the pre-Rev26 Cycle 8A design     ║ -->
<!-- ║  series. It describes the OLD transaction recovery model   ║ -->
<!-- ║  (TransactionState + 2PC commit flag flip).                 ║ -->
<!-- ║                                                              ║ -->
<!-- ║  The CURRENT normative design is:                            ║ -->
<!-- ║    docs/CYCLE-8C-REV26-FINAL-PREDICATE.md                    ║ -->
<!-- ║                                                              ║ -->
<!-- ║  References to "Phase 2" in this document refer to the 2PC   ║ -->
<!-- ║  commit-flag-flip phase, NOT to the Phase 2 Rev26 migration  ║ -->
<!-- ║  scope (see docs/PHASE-2-SCOPE.md). Do not confuse the two.  ║ -->
<!-- ╚══════════════════════════════════════════════════════════╝ -->
<!-- END SUPERSEDED BANNER -->

This document defines the behavior of the transaction recovery state machine
at each of the 10 critical time points (T0-T10) during command processing.
For each time point, we describe: what happens on crash, what state the journal
is in, and what recovery action is taken on reboot + retry.

## Transaction States

```
NEW              — command received, not yet persisted (transient)
  ↓ storeIntent()
PENDING          — intent stored to NVS, execute NOT yet called
  ↓ markExecuting()
EXECUTING        — execute called, commit NOT yet done (GPIO may or may not have changed)
  ↓ commitTransaction()
COMMITTED        — execute + commit succeeded (durable success)
  ↓ (on boot reconciliation)
COMMITTED_UNKNOWN — GPIO matches desired (likely succeeded, unprovable)
FAILED           — GPIO doesn't match desired (execute didn't happen or failed)
```

## Time Points

### T0 — Command received (before JSON parse)
- **Journal state**: no entry
- **Crash effect**: command lost, no side effects
- **Recovery**: PWA timeout → retry with NEW requestId (or same if PWA is idempotent)
- **Result**: SAFE — no state corruption

### T1 — JSON parsed, validation in progress
- **Journal state**: no entry
- **Crash effect**: command lost, no side effects
- **Recovery**: PWA timeout → retry
- **Result**: SAFE — no state corruption

### T2 — Command hash computed, journal lookup complete (no existing entry found)
- **Journal state**: no entry
- **Crash effect**: command lost, no side effects
- **Recovery**: PWA timeout → retry → fresh execution
- **Result**: SAFE — no state corruption

### T3 — storeIntent() succeeded (PENDING in NVS), execute NOT yet called
- **Journal state**: PENDING, channelId + desiredState recorded
- **GPIO state**: unchanged (execute never ran)
- **Crash effect**: command lost, but intent is durable
- **Recovery on boot**: reconcilePendingEntries() reads GPIO → GPIO != desiredState → mark FAILED
- **Recovery on retry**: isProcessed() returns false (FAILED excluded) → clearEntry() → fresh execution
- **Result**: SAFE — command is retried, relay reaches desired state

### T4 — markExecuting() succeeded (EXECUTING in NVS), GPIO write in progress
- **Journal state**: EXECUTING
- **GPIO state**: may or may not have changed (write may be partial)
- **Crash effect**: GPIO state uncertain
- **Recovery on boot**: reconcilePendingEntries() reads GPIO → compare to desiredState
  - If GPIO == desired → COMMITTED_UNKNOWN (likely succeeded)
  - If GPIO != desired → FAILED (execute didn't complete)
- **Recovery on retry**:
  - COMMITTED_UNKNOWN → replay ACK with disclaimer
  - FAILED → clearEntry → fresh execution
- **Result**: SAFE — either command succeeded (and we acknowledge) or it's retried

### T5 — GPIO write complete, ACK JSON construction in progress
- **Journal state**: EXECUTING
- **GPIO state**: changed (execute completed)
- **Crash effect**: GPIO changed, but ACK not yet sent
- **Recovery on boot**: reconcilePendingEntries() reads GPIO → GPIO == desired → COMMITTED_UNKNOWN
- **Recovery on retry**: COMMITTED_UNKNOWN → replay ACK with disclaimer
- **Result**: SAFE — command succeeded, PWA eventually receives ACK

### T6 — commitTransaction() in progress (NVS write)
- **Journal state**: EXECUTING (commit flag not yet flipped)
- **GPIO state**: changed
- **Crash effect**: GPIO changed, commit may be partial
- **Recovery on boot**:
  - If commit flag = 0 (Phase 0/1 complete, Phase 2 not started): EXECUTING → reconcile
  - If commit flag = 1 (Phase 2 complete): COMMITTED (durable)
- **Recovery on retry**:
  - COMMITTED → replay ACK
  - EXECUTING → reconcile → COMMITTED_UNKNOWN → replay ACK with disclaimer
- **Result**: SAFE — command succeeded, PWA receives ACK

### T7 — commitTransaction() NVS write complete (commit flag = 1)
- **Journal state**: COMMITTED
- **GPIO state**: changed
- **Crash effect**: command fully durable
- **Recovery on boot**: no reconciliation needed (COMMITTED)
- **Recovery on retry**: COMMITTED → replay ACK
- **Result**: SAFE — full durable success

### T8 — ACK JSON ready, immediate publish() in progress
- **Journal state**: COMMITTED
- **Crash effect**: ACK may or may not have been published
- **Recovery on boot**: COMMITTED → ACK queued for re-delivery
- **Recovery on retry**: COMMITTED → replay ACK
- **Result**: SAFE — PWA eventually receives ACK

### T9 — publish() returned, dequeueAck() not yet called
- **Journal state**: COMMITTED (ACK still in retry queue)
- **Crash effect**: ACK published, but queue entry not cleared → duplicate ACK possible
- **Recovery on boot**: COMMITTED → ACK queued for re-delivery (may cause duplicate)
- **Recovery on retry**: COMMITTED → replay ACK
- **Result**: SAFE — PWA may receive duplicate ACK (idempotent handling in PWA)

### T10 — dequeueAck() complete, command fully done
- **Journal state**: COMMITTED, ACK queue cleared
- **Crash effect**: none (command fully complete)
- **Recovery**: none needed
- **Result**: SAFE — full success

## Summary: Crash Safety Matrix

| Time Point | Journal State | GPIO Changed? | Recovery Action | Safe? |
|------------|---------------|---------------|-----------------|-------|
| T0 | (none) | No | Retry fresh | ✅ |
| T1 | (none) | No | Retry fresh | ✅ |
| T2 | (none) | No | Retry fresh | ✅ |
| T3 | PENDING | No | Reconcile → FAILED → retry | ✅ |
| T4 | EXECUTING | Maybe | Reconcile → COMMITTED_UNKNOWN or FAILED | ✅ |
| T5 | EXECUTING | Yes | Reconcile → COMMITTED_UNKNOWN | ✅ |
| T6 | EXECUTING | Yes | Reconcile → COMMITTED_UNKNOWN or COMMITTED | ✅ |
| T7 | COMMITTED | Yes | Replay ACK | ✅ |
| T8 | COMMITTED | Yes | Replay ACK | ✅ |
| T9 | COMMITTED | Yes | Replay ACK (may duplicate) | ✅ |
| T10 | COMMITTED | Yes | None needed | ✅ |

## Known Limitations (Honest Documentation)

### 1. GPIO output ≠ physical relay contact state
- `readLogicalState()` reads the GPIO output register, NOT the physical relay contact.
- A welded relay could have GPIO=ON but contact=OFF (or vice versa).
- Without contact feedback hardware, we CANNOT verify physical state.
- **Mitigation**: ACK messages include "physicalState: unknown" disclaimer for COMMITTED_UNKNOWN.
- **Future**: Cycle 8B will define a Relay State Model with `physicalState` field.
  Hardware revision with contact feedback is needed for true physical verification.

### 2. Non-relay commands (schedule/config) cannot be reconciled via GPIO
- channelId=0 for non-relay commands.
- These are marked COMMITTED_UNKNOWN at boot (best-effort, no GPIO verification).
- For schedule commands: the schedule is stored in LittleFS (config.json), which
  has its own atomic write mechanism. If config.json has the schedule, the command
  succeeded. If not, it failed. (Not yet implemented — future work.)

### 3. Idempotent no-op case (desiredState == previousKnownState)
- If relay was already ON and command is ON:
  - GPIO reads ON (because it was already ON, not because execute ran)
  - Reconciliation marks COMMITTED_UNKNOWN
  - This is correct (end state is ON, which is what command wanted)
  - But we can't tell if execute actually ran or was a no-op
- **Result**: SAFE — end state is correct regardless

### 4. NVS endurance not tested
- Each transaction writes ~1.2KB to NVS (blob + commit flag + state flag).
- With 64-entry journal and LRU eviction, worst case is 64 writes per cycle.
- ESP32 NVS wear-leveling helps, but multi-year endurance is unverified.
- **Future**: Cycle 8B/8C should include endurance testing.

### 5. Hardware testing NOT RUN
- This document describes the DESIGNED behavior.
- Actual hardware power-loss testing at T0-T10 has NOT been performed.
- Build PASS does not prove recovery correctness.
- **Requirement**: Before 220V deployment, inject power loss at each time point
  and verify the documented recovery behavior.
