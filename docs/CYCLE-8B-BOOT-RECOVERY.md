# CYCLE-8B: Deterministic Relay State & Boot Recovery

## Problem Statement (C8A-001)

Cycle 8A's boot reconciliation ran AFTER `RelayEngine.forceRefresh()` had already
changed GPIO outputs based on scheduler/PIR/manual logic. This contaminated the
evidence — GPIO no longer reflected "state at crash" but "state after boot
initialization". This produced:
- False FAILED (scheduler turned relay OFF, journal thought execute didn't run)
- False COMMITTED_UNKNOWN (scheduler turned relay ON, journal thought execute ran)

## Solution: BootRecoveryPhase

### Boot Sequence (CYCLE-8B)

```
BOOT
  ↓
[PRE_INIT]        — Before any initialization (relays in unknown state)
  ↓
Watchdog init, PIR begin, LittleFS, OTA health, Log, load configs
  ↓
[LOADING_NVS]    — Loading config, journal, RTC from NVS
  ↓
Drivers::rtc.begin()
Drivers::relay.begin()  ← sets all relays to OFF (known safe state)
  ↓
[SAFE_INIT]       — GPIO set to OUTPUT, all OFF (known safe state)
  ↓
Services::journal.captureOutputSnapshot()
  ↓                ← SNAPSHOT captures safe-OFF state (BASELINE for reconciliation)
[SNAPSHOT]        — Capturing raw GPIO output state (read-only)
  ↓
WiFi, Auth, MQTT.begin() (loads journal from NVS, does NOT reconcile)
  ↓
Services::journal.reconcilePendingEntries()  ← uses SNAPSHOT, not live GPIO
  ↓
[RECONCILING]     — Reconciling incomplete transactions
  ↓
[RESTORING]       — Restoring application state
  ↓
Services::relayEngine.forceRefresh()  ← FIRST time RelayEngine runs
  ↓                ← NOW scheduler/PIR/manual logic can modify GPIO
Web::server.begin()
  ↓
[RUNNING]         — Normal operation — RelayEngine active, commands accepted
  ↓
Boot complete
```

### Why This Order Matters

Before CYCLE-8B:
```
RelayDriver.begin()   ← GPIO = OFF (safe)
RelayEngine.forceRefresh()  ← GPIO = scheduler/PIR/manual result
...
TransactionJournal.begin()  ← reconcile reads GPIO (ALREADY CHANGED!)
```
Result: GPIO reflects post-boot logic, not pre-crash state → false reconciliation.

After CYCLE-8B:
```
RelayDriver.begin()   ← GPIO = OFF (safe)
captureOutputSnapshot()  ← snapshot = OFF (baseline)
...
TransactionJournal.begin()  ← loads journal entries
reconcilePendingEntries()  ← uses snapshot (OFF), NOT live GPIO
...
RelayEngine.forceRefresh()  ← NOW GPIO can change (after recovery is done)
```
Result: reconciliation uses deterministic baseline → correct recovery decisions.

## Reconciliation Logic (CYCLE-8B)

### PENDING entries (execute DEFINITELY didn't run)

Journal state says PENDING → `markExecuting()` was never called → execute never ran.

| desiredState | Snapshot (GPIO) | Reconciliation Result | Reasoning |
|--------------|------------------|----------------------|-----------|
| ON | OFF (safe init) | FAILED | Execute didn't run, GPIO is OFF, desired is ON → retry needed |
| OFF | OFF (safe init) | COMMITTED_UNKNOWN | Idempotent — can't tell if execute ran (but end state is correct) |

### EXECUTING entries (execute MAY have run)

Journal state says EXECUTING → `markExecuting()` was called, but `commitTransaction()`
was not. Execute may or may not have completed before crash.

| desiredState | Snapshot (GPIO) | Reconciliation Result | Reasoning |
|--------------|------------------|----------------------|-----------|
| any | OFF (safe init) | COMMITTED_UNKNOWN | Cannot determine — GPIO was reset to safe-OFF, so we can't tell if execute ran |

### COMMITTED entries (execute + commit both succeeded)

No reconciliation needed — entry is durable. ACK replayed on retry.

### FAILED entries (reconciled as failed)

`clearEntry()` allows retry with same requestId.

## RelayEngine Guard (C8A-007 fix)

`RelayEngine::tick()` now checks boot phase before modifying relay state:

```cpp
void RelayEngine::tick() {
  if (!Services::journal.isRunning() &&
      Services::journal.getBootPhase() != BootPhase::RESTORING) {
    return;  // Skip during PRE_INIT through RECONCILING
  }
  // ... normal tick logic
}
```

This prevents:
- Scheduler from changing GPIO during snapshot phase
- PIR from triggering during reconciliation
- Manual overrides from contaminating recovery

## MQTT Command Guard (C8A-007 fix)

`_handleCommand()` now rejects commands during boot recovery:

```cpp
if (!Services::journal.isRunning()) {
  _publishAck(requestId, false,
    "System not ready (boot recovery in progress) — please retry in a moment");
  return;
}
```

PWA receives failure ACK and knows to retry.

## Validation Ordering (C8A-005 fix)

Before CYCLE-8B:
```
storeIntent() → validate channel → validate action → execute
```
Problem: invalid commands left PENDING entries in journal.

After CYCLE-8B:
```
validate EVERYTHING → storeIntent → markExecuting → execute
```
Invalid commands never create journal entries.

## ACK Semantics (C8A-008 fix)

Before CYCLE-8B:
```
GPIO mismatch → success=true + warning string
```
Problem: `success=true` is a dangerous contract for relay mains.

After CYCLE-8B:
```
GPIO mismatch → success=false, message="OUTPUT_MISMATCH"
```
PWA knows command failed and can retry or investigate.

## clearEntry() Fix (C8A-005)

Before CYCLE-8B:
```
clearEntry() only allowed for FAILED entries
```
Problem: PENDING entries from invalid commands couldn't be cleared.

After CYCLE-8B:
```
clearEntry() allowed for PENDING, EXECUTING, and FAILED
NOT allowed for COMMITTED, COMMITTED_UNKNOWN (durable)
```

## Honest Limitations

### 1. Snapshot reflects safe-OFF, not pre-crash state

`RelayDriver.begin()` sets all relays to OFF (safe state). The snapshot is
captured AFTER this, so it reflects safe-OFF, NOT the pre-crash GPIO state.

This means:
- We CANNOT determine if execute ran before crash (only journal state tells us)
- PENDING entries: execute DEFINITELY didn't run (journal says so)
  → desired=ON → FAILED (correct)
  → desired=OFF → COMMITTED_UNKNOWN (conservative, idempotent)
- EXECUTING entries: execute MAY have run, but GPIO is now OFF
  → COMMITTED_UNKNOWN (conservative)

This is the best we can do without battery-backed GPIO register.
For true pre-crash GPIO recovery, hardware revision with GPIO state
preservation (or latching relays) is needed.

### 2. GPIO output ≠ physical relay contact state

`readLogicalState()` reads GPIO output register, NOT physical relay contact.
A welded relay could have GPIO=ON but contact=OFF (or vice versa).
Without contact feedback hardware, we CANNOT verify physical state.

### 3. Non-relay commands cannot be reconciled via GPIO

Schedule/config commands (channelId=0) are marked COMMITTED_UNKNOWN at boot.
For true recovery, per-command-type verifiers are needed:
- Schedule upsert: check if config.json contains the schedule
- Config change: check if NVS has the updated config
- Time set: check if RTC time matches commanded time

This is deferred to future cycle.

### 4. Hardware power-loss testing NOT RUN

This document describes DESIGNED behavior.
Actual hardware power-loss testing has NOT been performed.
Build PASS does not prove recovery correctness.

## State-Transition Matrix

### Transaction States

```
NEW (transient, not in journal)
  ↓ storeIntent()
PENDING
  ↓ markExecuting()
EXECUTING
  ↓ commitTransaction()
COMMITTED (durable)
  ↓ (on boot reconciliation)
COMMITTED_UNKNOWN (conservative — cannot verify)
FAILED (execute didn't run or failed)
  ↓ clearEntry()
(cleared — allows retry)
```

### Boot Phases

```
PRE_INIT
  ↓ (watchdog, PIR, LittleFS, OTA, Log, configs)
LOADING_NVS
  ↓ (RTC, RelayDriver.begin)
SAFE_INIT
  ↓ (captureOutputSnapshot)
SNAPSHOT
  ↓ (WiFi, Auth, MQTT.begin, reconcilePendingEntries)
RECONCILING
  ↓ (RelayEngine.forceRefresh)
RESTORING
  ↓ (Web server, markBootHealthy)
RUNNING
```

### What Can Modify Relay State in Each Phase

| Phase | RelayDriver | RelayEngine | MQTT Commands | PIR Triggers | Scheduler |
|-------|-------------|-------------|---------------|--------------|-----------|
| PRE_INIT | No (not initialized) | No | No | No | No |
| LOADING_NVS | No | No | No | No | No |
| SAFE_INIT | Yes (begin sets OFF) | No (guarded) | No (guarded) | No | No |
| SNAPSHOT | No | No (guarded) | No (guarded) | No | No |
| RECONCILING | No | No (guarded) | No (guarded) | No | No |
| RESTORING | No | Yes (forceRefresh) | No (guarded) | No | No |
| RUNNING | Yes (via setChannel) | Yes (tick) | Yes | Yes | Yes |
