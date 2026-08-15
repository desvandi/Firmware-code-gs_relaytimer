# P2-2 Transaction Lifecycle Semantic Matrix

**Purpose:** Determine the correct transaction lifecycle for each command type BEFORE coding. Per auditor instruction: "Jangan sekadar menambahkan markExecuting() supaya commitTransaction() tidak gagal."

## Current State (broken)

| Command | Intent | markExecuting? | Mutation | Commit path | Result |
|---|---|---|---|---|---|
| relay (on/off/set_mode) | PENDING ✓ | YES → EXECUTING ✓ | GPIO write + readback | commitTransaction (EXECUTING→COMMITTED) ✓ | Works |
| schedule (upsert/delete) | PENDING ✓ | NO | RAM config + markDirty | commitTransaction (FAILS: PENDING≠EXECUTING) | DURABILITY_FAILURE ACK |
| pir (config/test) | PENDING ✓ | NO | RAM config + markDirty | commitTransaction (FAILS) | DURABILITY_FAILURE ACK |
| channel (rename) | PENDING ✓ | NO | RAM config + markDirty | commitTransaction (FAILS) | DURABILITY_FAILURE ACK |
| time (set) | PENDING ✓ | NO | RTC adjust | commitTransaction (FAILS) | DURABILITY_FAILURE ACK |
| system (reboot/getStatus/reset*) | PENDING ✓ | NO | Various | commitTransaction (FAILS) | DURABILITY_FAILURE ACK |
| config (setDevice) | PENDING ✓ | NO | RAM config + markDirty | commitTransaction (FAILS) | DURABILITY_FAILURE ACK |
| ota (update) | PENDING ✓ | NO | HTTPS download + flash | commitTransaction (FAILS) | DURABILITY_FAILURE ACK |

## Analysis: What does "EXECUTING" mean for each type?

### Relay commands (physical mutation)
- **EXECUTING means**: "GPIO write is in progress; physical side effect may have occurred"
- **Crash during EXECUTING**: relay may be in the new state, but we can't verify
- **Recovery**: UNKNOWN state → PWA gets "AMBIGUOUS" ACK → must verify device state
- **Lifecycle**: PENDING → EXECUTING → GPIO write → readback → COMMITTED (or OUTPUT_MISMATCH)
- **markExecuting placement**: BEFORE GPIO write ✓ (correct as-is)

### Schedule upsert/delete (RAM configuration mutation)
- **EXECUTING means**: "Schedule array is being modified in RAM; config not yet persisted"
- **Crash during EXECUTING**: schedule may be partially modified in RAM (but RAM is lost on crash)
- **Physical side effect**: NONE — schedule only affects future relay evaluation
- **Recovery**: If crash before markDirty+save → old schedule persists in NVS/LittleFS
- **Lifecycle question**: Is there a meaningful "executing" state for an atomic RAM write?

### PIR config (RAM configuration mutation)
- Same as schedule: atomic RAM write, no physical side effect
- **EXECUTING means**: "PIR config is being modified in RAM"
- **Crash during EXECUTING**: old config persists in NVS

### Channel rename (RAM configuration mutation)
- Same: atomic strncpy, no physical side effect
- **EXECUTING means**: "Channel name is being modified in RAM"

### Time set (RTC hardware write)
- **EXECUTING means**: "RTC is being adjusted"
- **Physical side effect**: RTC time changes (affects schedule evaluation)
- **Crash during EXECUTING**: RTC may or may not have been adjusted — but RTC.adjust() is atomic (I2C write)
- **Recovery**: If RTC was adjusted → new time. If not → old time. Either way, system is functional.

### System reboot (terminal action)
- **EXECUTING means**: "Reboot is in progress"
- **Crash during EXECUTING**: system reboots anyway (same outcome)
- **Lifecycle**: PENDING → ACK published → ESP.restart()

### System getStatus (read-only)
- **EXECUTING means**: "Status is being published" (no mutation)
- **Question**: Should read-only commands go through the journal at all?

### System resetEnergyStats/resetDailyStats (RAM reset)
- **EXECUTING means**: "Energy stats are being zeroed in RAM"
- **Physical side effect**: NONE (energy stats are informational, not safety-critical)

### Config setDevice (RAM configuration mutation)
- Same as schedule: atomic RAM write, no physical side effect

### OTA update (destructive flash write)
- **EXECUTING means**: "Firmware is being downloaded and flashed"
- **Physical side effect**: Flash partition is being overwritten
- **Crash during EXECUTING**: May have partial flash → boot may fail → rollback triggers
- **Lifecycle**: PENDING → EXECUTING (download start) → flash write → verify → COMMITTED → reboot

## Proposed Lifecycle Model

### Option A: Two lifecycle paths

**Physical mutation commands** (relay, OTA):
```
PENDING → EXECUTING → physical mutation → verify → COMMITTED
```
- EXECUTING is meaningful: physical side effect is in progress
- markExecuting before physical mutation
- commitTransaction after verification

**Configuration commands** (schedule, PIR, channel, time, system reset, config):
```
PENDING → atomic RAM mutation → COMMITTED
```
- No EXECUTING state needed — mutation is atomic (RAM write)
- But commitTransaction() requires EXECUTING → need new API or different flow

**Read-only commands** (getStatus):
```
No journal entry — just respond
```
- Should NOT create journal entry at all (no mutation, no dedup needed)

### Option B: Introduce commitFromPending() API

Add a new TransactionJournal API:
```
bool commitTransactionFromPending(requestId, ackJson)
```
- Transitions PENDING → COMMITTED directly
- Used by configuration commands that have no physical execution phase
- Same durability guarantees as commitTransaction (both copies written)
- Does NOT require markExecuting

### Option C: Use markExecuting for all, but with honest semantics

```
PENDING → markExecuting("configuration mutation in progress") → atomic mutation → COMMITTED
```
- EXECUTING means: "mutation is in progress" (even if atomic)
- Crash during EXECUTING → UNKNOWN → "AMBIGUOUS: configuration may or may not have been applied"
- This is honest: if we crash between markExecuting and the RAM write, the config was NOT applied
- If we crash after the RAM write but before commit → config IS applied but not durable in journal

## Recommendation

**Option B** is the cleanest:
- Does not fake EXECUTING state for atomic operations
- Does not change P2-1 TransactionJournal semantics (adds API, doesn't change existing)
- Configuration commands get: PENDING → COMMITTED (via commitFromPending)
- Physical commands get: PENDING → EXECUTING → COMMITTED (existing path)
- Read-only commands (getStatus): skip journal entirely

**Option C** is simpler but less honest:
- markExecuting for all commands
- EXECUTING means "mutation in progress" generically
- Pro: no new API, minimal code change
- Con: EXECUTING is semantically misleading for atomic operations (there's no "in progress" — it's either done or not)

## Read-only commands

System `getStatus` should NOT create a journal entry:
- No mutation → no durability needed → no dedup needed
- Remove `storeIntent` call for getStatus
- If PWA retries getStatus, each retry gets a fresh status publish (which is correct)

## Failure ACK path

Currently, failure ACKs (line 517): `!success || commandHash.length() == 0` → publish immediately, do NOT commit.

For non-relay commands that fail validation BEFORE storeIntent:
- No journal entry created (return before storeIntent) ✓
- ACK published with `success=false` ✓

For non-relay commands that fail AFTER storeIntent (e.g., schedule limit reached):
- Journal entry is PENDING
- Failure ACK published (not committed)
- Journal entry stays PENDING → fills up journal after 64 failures

**Fix needed**: Call `clearEntry(requestId)` before returning on failure paths that occur AFTER storeIntent.

## Summary table: Proposed lifecycle

| Command | Journal entry? | Lifecycle | markExecuting? | Commit via | Clear on failure? |
|---|---|---|---|---|---|
| relay (on/off/set_mode) | YES | PENDING→EXECUTING→COMMITTED | YES (before GPIO) | commitTransaction | YES (clearEntry) |
| schedule (upsert/delete) | YES | PENDING→COMMITTED | NO | commitFromPending | YES (clearEntry) |
| pir (config) | YES | PENDING→COMMITTED | NO | commitFromPending | YES (clearEntry) |
| pir (test) | YES | PENDING→COMMITTED | NO | commitFromPending | YES (clearEntry) |
| channel (rename) | YES | PENDING→COMMITTED | NO | commitFromPending | YES (clearEntry) |
| time (set) | YES | PENDING→COMMITTED | NO | commitFromPending | YES (clearEntry) |
| system (reboot) | YES | PENDING→COMMITTED | NO | commitFromPending | N/A (terminal) |
| system (getStatus) | NO | No journal | N/A | N/A | N/A |
| system (resetEnergy/Daily) | YES | PENDING→COMMITTED | NO | commitFromPending | YES (clearEntry) |
| config (setDevice) | YES | PENDING→COMMITTED | NO | commitFromPending | YES (clearEntry) |
| ota (update) | YES | PENDING→EXECUTING→COMMITTED | YES (before download) | commitTransaction | YES (clearEntry) |
