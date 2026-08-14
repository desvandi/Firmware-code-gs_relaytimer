# CYCLE-8C-Rev5: Transaction Journal v4 — Lifecycle Contracts & Stable Observation

**Status**: DESIGN ONLY — NO CODE IMPLEMENTATION
**Purpose**: Define stable observation, eviction safety, and ACK lifecycle separation
**Auditor instruction**: "Rev5 harus membuktikan invariants → protocol → crash matrix → eviction contract secara formal"

---

## 1. Root Cause Analysis (Why Rev4 Failed)

### C8CR4-001 (P0): No Stable Observation

Rev4 defined canonical equivalence (WHAT must be consistent) but not the observation protocol (HOW to read A and B atomically). The `_checkI1Satisfied()` pseudo-code was:
```
1. read A
2. read B
3. compare
```

If another thread writes B between step 1 and step 2, the checker observes a state that never existed simultaneously. This is a TOCTOU (Time-of-Check-Time-of-Use) race.

**Fix**: I0 — Stable Observation. All journal access is serialized through a single mutation executor. No concurrent reads/writes.

### C8CR4-002 (P0): Eviction Destroys Evidence

Rev4 allowed eviction of COMMITTED entries based on `mqtt.publish()==true`. But "publish accepted" is NOT "PWA received". If eviction happens before PWA receives ACK, transaction evidence is destroyed. This violates exactly-once semantics for non-idempotent commands.

**Fix**: I2 — Eviction Safety. Formal COMMITTED lifecycle with explicit retention policy. Default = RETAIN (never evict). Eviction only when ALL conditions met.

### C8CR4-003 (P1): ACK Eviction Contract Contradictory

Rev4 I5 said eviction requires "durable queue AND (publish accepted OR broker ack OR PWA ack)" but then also said "ACK must remain in queue until b/c confirmed". These contradict — if eviction is allowed on publish-accepted, why must ACK remain?

**Fix**: I3 — ACK Lifecycle Separation. Transaction retention and ACK delivery are SEPARATE lifecycles. Transaction can be evicted (retention policy) while ACK queue continues independently.

### Pattern Across All Cycles

| Cycle | Missing Piece |
|-------|--------------|
| 7 | Durable intent |
| 8A | Boot ordering |
| 8B | State monotonicity |
| 8C | Invariant strictness |
| 8C-Rev1 | Atomic write assumption |
| 8C-Rev2 | Generation wrap |
| 8C-Rev3 | Canonical equivalence |
| 8C-Rev4 | **Stable observation + eviction lifecycle** |

**The lesson**: The formal model was incomplete because it didn't account for concurrency (observation atomicity) or lifecycle (retention vs delivery).

---

## 2. Four Formal Contracts (I0-I3)

### I0 — Stable Observation / Concurrency Invariant

```
INVARIANT:
    TransactionJournal has EXACTLY ONE mutation executor.
    No journal read or write may occur concurrently with another.

ENFORCEMENT:
    A mutex (or equivalent serialization mechanism) protects ALL
    journal access — reads, writes, checks, recovery.

    Every public API function acquires the mutex at entry,
    releases at exit.

    The mutex is NOT re-entrant (to detect bugs).

PROTOCOL:
    acquire(mutex)
        ↓
    read copy A
    read copy B
    evaluate (canonical equivalence, generation, recovery decision)
    perform mutation (if any)
    verify (if mutation)
        ↓
    release(mutex)

SINGLE-THREADED ALTERNATIVE:
    If the firmware architecture guarantees single-threaded access
    (no FreeRTOS tasks touching journal, no ISR callbacks), then
    the mutex can be a no-op. But this MUST be documented as:
    
    "Journal access is single-threaded by architecture.
     No RTOS task or ISR may call journal API.
     Mutex is not needed because concurrency is impossible."
    
    This is STRONGER than mutex (mutex allows concurrency but serializes).
    Single-threaded guarantee ELIMINATES concurrency.

DECISION FOR REV5:
    ESP32 firmware uses:
    - loop() for main operations (WebServer, MQTT, RelayEngine tick)
    - MQTT callback (PubSubClient) — runs in loop() context
    - PIR ISR — does NOT touch journal (only sets flag)
    - WiFi events — does NOT touch journal
    
    All journal access happens in loop() context. Single-threaded.
    Document this as formal invariant.
    
    If future architecture changes (e.g., FreeRTOS task for journal):
    Mutex MUST be added. Design includes mutex placeholder.

CONTRACT:
    I0: Journal access is single-threaded (loop() context only).
    No concurrent access is possible by architecture.
    If architecture changes, mutex MUST be added before re-audit.
```

### I1 — Canonical Equivalence + Recovery (Revised from Rev4)

```
INVARIANT:
    Two valid copies are canonically equivalent if and only if:
    1. GEN_EQUAL: genA == genB
    2. ByteEqual: canonicalSerialization(A) == canonicalSerialization(B)

CANONICAL SERIALIZATION (byte-level, fixes C8CR4-008):
    The record is serialized to a FIXED byte sequence:
    
    [recordState:1]
    [requestIdLen:1] [requestId:requestIdLen]
    [commandHashLen:1] [commandHash:commandHashLen]
    [channelId:1]
    [desiredState:1]
    [previousKnownState:1]
    [attempt:1]
    [timestamp:4]
    [ackLen:2] [ackJson:ackLen]
    
    This is the EXACT byte sequence stored in the record (after header+generation+CRC).
    Comparison is byte-exact (memcmp).
    
    ackJson MUST be canonicalized BEFORE storage:
    - Use ArduinoJson with consistent serialization (no pretty-print, key order stable)
    - Or: store pre-serialized ackJson that was built deterministically
    - No re-parsing/re-serializing on comparison

RECOVERY DECISION TABLE (from Rev4, unchanged):
    See §3 below.

REPAIR SEMANTICS (fixes C8CR4 finding #6):
    Repair is BITWISE RESTORATION, not state transition.
    
    REPAIR(B) when A=VALID, B=INVALID:
        1. Read A's full record (including generation)
        2. Write IDENTICAL record to B (same generation, same payload)
        3. Verify B (re-read + CRC + byte-compare with A)
        4. If verify fails → CORRUPTED
        5. If verify passes → A and B are identical, I1 satisfied
    
    Repair does NOT increment generation.
    Repair does NOT change recordState.
    Repair does NOT modify any field.
    Repair = "make B a copy of A" (or vice versa).

RECOVERY GENERATION (fixes C8CR4 finding #7):
    When both copies are INVALID (CORRUPTED):
    
    Option A (Rev5 choice): DO NOT auto-recover with arbitrary generation.
        recoverCorruptedEntry() writes EMPTY with:
        generation = max(readable generation) + 1
        If NO generation is readable (both copies have corrupt headers):
            generation = 0 AND slot is marked "EPOCH_RESET" in forensic log
            "EPOCH_RESET" means: generation counter restarted, old generations invalid.
            This is explicitly logged as a break in generation continuity.
            Future writes to this slot continue from 0+1=1.
    
    Option B (deferred): Separate epoch counter in NVS metadata.
        More complex, deferred to implementation if Option A proves insufficient.
    
    CONTRACT:
        If generation=0 is used for recovery, it MUST be logged as EPOCH_RESET.
        The forensic record MUST note: "generation continuity broken, epoch reset to 0".
        Operator must acknowledge this in recovery procedure.
```

### I2 — Eviction Safety / Evidence Retention

```
INVARIANT:
    A COMMITTED transaction MUST NOT transition to EMPTY
    unless ALL of the following are true:
    
    1. RETENTION POLICY: Journal is full AND slot is needed for new transaction
    2. COMMAND CLASS: Command is evictable (idempotent: relay ON/OFF, schedule, config)
    3. ACK CONDITION (one of):
       a. ACK_PWA_RECEIVED (application-level ack from PWA)
       b. ACK_SENT_TO_BROKER AND ACK is in durable queue
       c. ACK_PUBLISH_ACCEPTED AND ACK is in durable queue AND command is idempotent
    4. NO UNRESOLVED RECOVERY: Slot is not in CORRUPTED or RECOVERY state

DEFAULT POLICY:
    If command class is UNKNOWN (not yet classified):
        DO NOT EVICT. Retain until classified.

NON-IDEMPOTENT COMMANDS (future: precharge, OTA):
    NEVER evict based on ACK_PUBLISH_ACCEPTED alone.
    MUST have ACK_SENT_TO_BROKER or ACK_PWA_RECEIVED.
    Retention policy for non-idempotent commands may be "never evict"
    until explicitly cleared by operator.

EVICTION IS NOT DATA DELETION:
    Eviction writes EMPTY with higher generation.
    The old COMMITTED record remains in the inactive copy until overwritten.
    This is acceptable — the inactive copy is stale, not lost.
    But: the transaction is no longer "tracked" by journal.
    PWA cannot use requestId for dedup after eviction.

CONTRACT:
    Eviction = "journal no longer tracks this requestId".
    Eviction ≠ "transaction evidence physically destroyed" (old copy may persist).
    Eviction = "exactly-once guarantee no longer applies to this requestId".
```

### I3 — ACK Lifecycle Separation

```
INVARIANT:
    Transaction retention and ACK delivery are SEPARATE lifecycles.

    Transaction lifecycle:
        PENDING → EXECUTING → COMMITTED → (retained) → EVICTED (EMPTY)
        
    ACK lifecycle:
        NOT_SENT → PUBLISH_ACCEPTED → BROKER_CONFIRMED → PWA_RECEIVED
                                                    (or) → DROPPED (max retries)

    These lifecycles are INDEPENDENT:
    
    - Transaction can be COMMITTED (durable in journal)
      while ACK is still NOT_SENT (if MQTT disconnected)
    
    - Transaction can be EVICTED (EMPTY)
      while ACK is still in durable queue (waiting for delivery)
    
    - ACK can be PWA_RECEIVED
      while transaction is still COMMITTED (not yet evicted)

ACK QUEUE INDEPENDENCE:
    ACK queue (tj_ackq) persists INDEPENDENTLY of journal records.
    
    When transaction is evicted:
        - Journal record → EMPTY
        - ACK queue entry REMAINS (if not yet delivered)
    
    When ACK is delivered (PWA_RECEIVED):
        - ACK queue entry removed
        - Transaction record may still be COMMITTED (not yet evicted)

EVICTION DOES NOT REQUIRE ACK DELIVERY:
    Eviction requires:
        1. Retention policy permits (journal full, slot needed)
        2. Command class is evictable
        3. ACK is in durable queue (so PWA can still receive it)
           OR ACK has been PWA_RECEIVED
    
    If ACK is RAM-only (not in durable queue, not delivered):
        Eviction is BLOCKED.

ACK QUEUE REBUILD:
    On boot, ACK queue is rebuilt from journal COMMITTED entries.
    If journal entry was evicted before boot:
        - ACK queue entry from previous boot may persist in tj_ackq
        - OR: ACK is lost (if tj_ackq was also lost)
    
    This is ACCEPTED:
        - Evicted + ACK lost = PWA must re-query status
        - Journal guarantees physical state, not delivery
        - PWA can always query /status to learn current relay state
```

---

## 3. Formal COMMITTED Lifecycle

```
                    storeIntent()
(none/EMPTY) ──────────────────────► PENDING
                                      │
                                      │ markExecuting()
                                      ▼
                                   EXECUTING
                                      │
                    ┌─────────────────┼─────────────────┐
                    │                 │                 │
                    │ commitTx()      │ commitTxFailed()│
                    │                 │                 │
                    ▼                 │                 ▼
                 COMMITTED            │      EXECUTION_FAILED_OUTPUT_MISMATCH
                    │                 │       (terminal, never evicted)
                    │                 │
                    │ reconcile        │
                    │ (boot)          │
                    ▼                 │
              COMMITTED_UNKNOWN        │
              (terminal, durable)     │
                    │                 │
                    │                 │
   ┌────────────────┘                 │
   │                                  │
   ▼                                  │
RETAIN (default)                     │
   │                                  │
   │ ACK sent (publish accepted)      │
   ▼                                  │
ACK_PENDING                           │
   │                                  │
   │ Broker confirms (QoS 1 PUBACK)   │
   ▼                                  │
ACK_BROKER_CONFIRMED                  │
   │                                  │
   │ PWA receives (app-level ack)     │
   ▼                                  │
ACK_PWA_RECEIVED                      │
   │                                  │
   │ Retention policy: journal full   │
   │ AND command is evictable         │
   ▼                                  │
EVICTABLE                             │
   │                                  │
   │ clearEntry() / eviction          │
   ▼                                  │
EMPTY                                 │
                                      │
                    ┌─────────────────┘
                    │ (reconcile, cannot determine)
                    ▼
                 UNKNOWN (clearable)
                    │
                    │ clearEntry()
                    ▼
                  EMPTY

PENDING ──reconcile──► FAILED (proven not executed)
                           │
                           │ clearEntry()
                           ▼
                         EMPTY

Any state ──both copies corrupt──► CORRUPTED (derived, terminal safety)
                                      │
                                      │ recoverCorruptedEntry()
                                      │ (operator-initiated, forensic log)
                                      ▼
                                    EMPTY
```

### Lifecycle State Definitions

| State | Description | Evictable? | Clearable? |
|-------|-------------|------------|------------|
| EMPTY | Slot is free | N/A | N/A |
| PENDING | Intent stored | No | Yes |
| EXECUTING | Execute called | No | Yes |
| COMMITTED | Execute + commit | See lifecycle above | No |
| COMMITTED_UNKNOWN | Reconciled, durable | No (terminal) | No |
| RETAIN | COMMITTED, ACK not sent | No | No |
| ACK_PENDING | COMMITTED, publish accepted | If idempotent + queue | No |
| ACK_BROKER_CONFIRMED | COMMITTED, broker acked | If queue or PWA ack | No |
| ACK_PWA_RECEIVED | COMMITTED, PWA acked | Yes (if retention needed) | No |
| EVICTABLE | All conditions met | Yes | No |
| UNKNOWN | Cannot determine | No | Yes |
| FAILED | Proven not executed | No | Yes |
| CORRUPTED | Derived from invalid copies | No | recoverCorruptedEntry() only |
| EXECUTION_FAILED_OUTPUT_MISMATCH | Execute ran, wrong output | No (terminal) | No |

**Note**: RETAIN, ACK_PENDING, ACK_BROKER_CONFIRMED, ACK_PWA_RECEIVED, EVICTABLE are **sub-states of COMMITTED**. The `recordState` field stores `COMMITTED`, but the journal tracks ACK delivery status separately (in RAM + durable queue).

---

## 4. Byte-Level Canonical Serialization (Fixes C8CR4-008)

### Problem in Rev4

Rev4 said "canonicalPayload == canonicalPayload" but didn't define byte-level comparison. Struct comparison (memcmp) is unsafe due to padding, alignment, string terminators, JSON formatting.

### Rev5 Definition

```
CANONICAL SERIALIZATION:

The record blob (bytes 12..end, after header+generation+CRC) IS the canonical form.

Serialization produces EXACT byte sequence:
    [requestIdLen:1] [requestId:N]
    [commandHashLen:1] [commandHash:N]
    [channelId:1]
    [desiredState:1]
    [previousKnownState:1]
    [attempt:1]
    [timestamp:4] (uint32 LE)
    [ackLen:2] (uint16 LE)
    [ackJson:ackLen]

Comparison = memcmp(blobA_payload, blobB_payload, payloadLen)
    where payloadLen = totalLen - 12 (header + generation + CRC = 12 bytes)

ACK JSON CANONICALIZATION:
    ackJson is stored as the EXACT string produced by ArduinoJson serializeJson().
    ArduinoJson produces deterministic output for same input:
    - Keys in insertion order
    - No whitespace (compact mode)
    - No trailing newline
    
    If ackJson is received externally (e.g., from firmware), it must be
    re-serialized through ArduinoJson before storage to ensure canonical form.
    
    Example:
        Input: {"success": true, "channelId": 1}
        ArduinoJson compact: {"success":true,"channelId":1}
        Stored: {"success":true,"channelId":1}
        Comparison: byte-exact

EMPTY RECORD CANONICAL FORM:
    requestIdLen = 0
    commandHashLen = 0
    channelId = 0
    desiredState = 0
    previousKnownState = 0
    attempt = 0
    timestamp = 0
    ackLen = 0
    ackJson = (empty)
    
    Two EMPTY records with same generation → byte-identical payloads → equivalent.
```

---

## 5. NVS Contract Clarification (Fixes C8CR4 finding #4)

### Honest Statement

```
NVS CRASH CONSISTENCY (per Espressif documentation):

NVS IS designed to be crash-consistent:
    - Existing data is NOT corrupted by power loss during write.
    - New write MAY be lost (incomplete write discarded on boot).
    - NVS uses internal page-level journaling.

NVS IS NOT optimized for:
    - Large blobs (1.2KB is large by NVS standards)
    - Frequent large writes
    - Espressif recommends LittleFS/SPIFFS for this use case

DUAL-COPY RATIONALE (corrected):

NOT: "NVS is unsafe, therefore dual-copy makes it safe"

BUT: "NVS provides crash consistency, AND dual-copy provides
      application-level evidence redundancy."

Dual-copy protects against:
    - Application bugs (wrong data written)
    - NVS key corruption (single key lost, other copy survives)
    - Record-level corruption (if NVS internal CRC fails to detect
      something — extremely rare but defense-in-depth)

Dual-copy does NOT protect against:
    - NVS page failure (both copies may be on same page)
    - Partition corruption
    - Flash chip failure

CONTRACT:
    NVS is trusted for crash consistency (power-loss protection).
    Dual-copy is trusted for application-level redundancy.
    These are COMPLEMENTARY, not redundant.
```

---

## 6. Partition Sizing (Fixes C8CR4 finding #5)

### Honest Requirement

```
Rev4 calculated raw bytes and recommended 128KB.
Rev5 requires EMPIRICAL VERIFICATION.

CALCULATION (starting point):
    32 slots × 2 copies × 1200 bytes = 76,800 bytes (raw journal data)
    + ACK queue (tj_ackq): ~4KB max
    + Forensic log: LittleFS (separate, not in NVS)
    + Other NVS data: WiFi creds, config, JWT secret, etc. ~4KB
    + NVS internal overhead: ~25% (metadata, page headers, GC)
    = ~108KB minimum

RECOMMENDATION:
    Start with 128KB NVS partition.
    VERIFY during implementation:
        1. Flash firmware
        2. Fill journal (32 slots × 2 copies)
        3. Use nvs_get_stats() to check free entries
        4. Verify other NVS data still fits (WiFi, config, etc.)
        5. If insufficient: increase to 256KB or reduce to 16 slots

ESPRESSIF RECOMMENDATION:
    For frequent large blob updates, consider LittleFS instead of NVS.
    LittleFS has better wear-leveling for this pattern.
    But: LittleFS has higher latency.
    
DECISION FOR REV5:
    Use NVS for simplicity in Rev5 implementation.
    If testing shows wear/space issues: migrate to LittleFS in future cycle.
    Partition size: verify empirically, do not assume.
```

---

## 7. Generation Distance Bound (Fixes C8CR4 finding #9)

```
INVARIANT:
    No two valid copies in the same slot may have generations
    differing by ≥ 2^31.

ENFORCEMENT:
    This is guaranteed by the write protocol:
    - Each write increments generation by 1.
    - Copies alternate: write to A, then B, then A, then B...
    - Maximum difference between A and B = 1 (one write ahead).
    
    Even if repair writes same generation:
    - A=GEN 100, B=INVALID → repair B to GEN 100 → diff = 0
    
    The only way to get diff ≥ 2^31 is:
    - 2^31 writes to the same slot without ever writing to the other copy.
    - This requires 2 billion writes to ONE slot = impossible in practice.

CONTRACT:
    Generation distance between A and B is ALWAYS ≤ 1 for normal operation.
    If distance > 1 is detected → CORRUPTED (abnormal state).
    
    This makes GEN_AMBIGUOUS (diff == 2^31) impossible in practice.
    But: we still check for it (defense-in-depth).
```

---

## 8. fsync Verification (Fixes C8CR4 finding #11)

```
NOTE FOR IMPLEMENTATION:

LittleFS on ESP32 Arduino:
    file.flush() — flushes write buffer to LittleFS
    LittleFS internally handles power-loss via COW metadata
    
    But: "flush()" may not force flash write immediately.
    True fsync() (wait for flash write complete) may require:
        file.flush();
        LittleFS.commit(); // or equivalent
    
    This MUST be verified during implementation:
        1. Check Arduino LittleFS API for flush vs commit semantics
        2. If flush is not sufficient: use commit or close+reopen
        3. Test with power-loss injection

DESIGN CONTRACT:
    "fsync" in Rev5 design = "ensure data is physically written to flash".
    Implementation MUST verify that the chosen API provides this guarantee.
    If not: find alternative API or accept risk (document explicitly).
```

---

## 9. Revised Invariant Summary (I0-I3)

| Invariant | What It Enforces | Fixes |
|-----------|-------------------|-------|
| **I0** | Stable Observation: single-threaded journal access, no concurrency | C8CR4-001 |
| **I1** | Canonical Equivalence + Recovery: byte-level comparison, bitwise repair | C8CR4-006, #7, #8 |
| **I2** | Eviction Safety: default retain, command class, ACK condition | C8CR4-002 |
| **I3** | ACK Lifecycle Separation: transaction ≠ ACK delivery | C8CR4-003 |

### Sub-invariants of I1

| Sub | What It Checks |
|-----|----------------|
| I1a | Copy A structurally valid (CRC) |
| I1b | Copy B structurally valid (CRC) |
| I1c | Mutual consistency (same-gen→byte-equal, diff-gen→strict order) |
| I1d | No generation ambiguity (diff < 2^31) |
| I1e | Same-gen byte-level payload equality |
| I1f | Diff-gen strict ordering |
| I1g | Generation distance ≤ 1 (normal operation) |

### Sub-invariants of I2

| Sub | What It Checks |
|-----|----------------|
| I2a | Retention policy permits eviction (journal full, slot needed) |
| I2b | Command class is evictable (idempotent) |
| I2c | ACK condition met (durable queue + one of publish/broker/PWA) |
| I2d | No unresolved recovery (slot not CORRUPTED) |
| I2e | Default = retain (never evict unknown command class) |

### Sub-invariants of I3

| Sub | What It Enforces |
|-----|------------------|
| I3a | Transaction lifecycle independent of ACK lifecycle |
| I3b | ACK queue persists independently of journal records |
| I3c | Eviction does NOT delete ACK queue entry |
| I3d | ACK queue rebuild on boot (from COMMITTED entries) |

---

## 10. Revised Recovery Decision Table (with I0-I3)

The table from Rev4 (§5) is still valid. Key additions:

### Pre-Mutation Check (with I0)

```
1. I0: Acquire journal mutex (or verify single-threaded)
2. Read copy A (stable, no concurrent modification)
3. Read copy B (stable, no concurrent modification)
4. Evaluate using decision table (Rev4 §5)
5. If I1 not satisfied → repair or CORRUPTED
6. If I1 satisfied → proceed with COW mutation
7. Verify (I3: re-read + CRC)
8. Release mutex (or return to single-threaded context)
```

### Eviction Check (with I2)

```
1. I0: Acquire mutex
2. Find oldest COMMITTED entry (LRU by generation)
3. Check I2a: Journal full? Slot needed?
4. Check I2b: Command class evictable?
   - Relay ON/OFF: idempotent → evictable
   - Schedule/config: idempotent → evictable
   - Unknown class: NOT evictable (retain)
5. Check I2c: ACK condition
   - ACK in durable queue? (check tj_ackq)
   - OR ACK_PWA_RECEIVED?
6. Check I2d: No unresolved recovery
7. If ALL pass → write EMPTY (COW, higher generation)
8. Release mutex
```

---

## 11. Honest Limitations (Unchanged + Refined)

1. **Snapshot reflects safe-OFF, not pre-crash state** — hardware limitation
2. **GPIO output ≠ physical relay contact** — welded/stuck undetectable
3. **Dual-copy is LOGICAL redundancy, NOT physical independence**
4. **CRC32 protects against accident, NOT malicious modification**
5. **NVS endurance is theoretical** — must test empirically
6. **ACK queue is single-copy NVS** — relies on NVS internal protection
7. **Forensic log is LittleFS single-copy** — relies on LittleFS power-loss protection
8. **ACK_PWA_RECEIVED is NOT implemented** — eviction carries delivery risk for idempotent commands
9. **Hardware power-loss testing NOT RUN**
10. **fsync semantics must be verified** at implementation time
11. **Partition size must be verified empirically** — raw bytes ≠ actual NVS capacity

---

## 12. What This Design Does NOT Solve

- Pre-crash GPIO state recovery (hardware revision needed)
- Physical relay contact verification (feedback hardware needed)
- Physical flash independence (separate flash chips needed)
- Tamper protection (Flash Encryption + Secure Boot needed)
- Application-level ACK confirmation (PWA → device ack)
- Multi-output transaction model (precharge) — separate cycle needed
- 16-relay / I/O expander — separate cycle needed
- F-008 PWA credential architecture — separate cycle needed

---

## 13. Implementation Plan (After Auditor Approval — NOT YET STARTED)

### Phase 1: Core Data Structure
1. Define `JournalRecord` struct with byte-level canonical serialization
2. Implement serialize/deserialize with CRC
3. Implement canonical payload comparison (memcmp on blob payload)

### Phase 2: I0 — Stable Observation
4. Verify single-threaded architecture (no RTOS task touches journal)
5. Add mutex placeholder (no-op in single-threaded, ready for future)
6. Document single-threaded guarantee in code comments

### Phase 3: Dual-Copy Operations + I1 Enforcement
7. Implement `_readCopy()`, `_writeCopy()` (with verify)
8. Implement `_checkI1Satisfied()` (I1a-I1g)
9. Implement `_evaluateSlot()` (decision table)
10. Implement `_repairSlot()` (BITWISE copy, NOT generation++)

### Phase 4: State Machine + Lifecycle
11. Implement `storeIntent()`, `markExecuting()`, `commitTransaction()`
12. Implement `commitTransactionFailed()`
13. Implement `reconcilePendingEntries()`, `reconcileEntry()`
14. Implement `clearEntry()` (write EMPTY, COW)
15. Implement `recoverCorruptedEntry()` (with forensic record + EPOCH_RESET)

### Phase 5: ACK Lifecycle (I3)
16. Implement ACK queue persistence (NVS `tj_ackq`)
17. Implement ACK queue rebuild from journal on boot
18. Implement ACK delivery status tracking (PENDING/BROKER/PWA)

### Phase 6: Eviction (I2)
19. Implement eviction pre-condition check (I2a-I2e)
20. Implement command class classification (idempotent vs non-idempotent)
21. Implement LRU eviction (oldest EVICTABLE entry)

### Phase 7: Boot Sequence
22. Implement `_loadFromNVS()` with dual-copy recovery + I1 enforcement
23. Implement `captureOutputSnapshot()`
24. Update `firmware_v4.ino` boot sequence

### Phase 8: Integration + Testing
25. Update `MqttClient.cpp` for new API
26. Update `RelayEngine.cpp` boot phase guard
27. Update partition table (verify 128KB empirically)
28. Hardware power-loss injection (every crash point)
29. Divergence injection (write different payloads, same generation)
30. Eviction testing (verify I2 conditions)
31. ACK lifecycle testing (separate from transaction)
32. fsync verification (LittleFS flush vs commit)

---

## 14. Auditor Approval Gate

**This design is NOT approved for implementation.**

Auditor must review:
1. I0 (§2) — is single-threaded guarantee sufficient? Is mutex placeholder acceptable?
2. I1 (§2) — is byte-level canonical serialization correct? Is bitwise repair correct?
3. I2 (§2) — is eviction safety complete? Is default-retain correct?
4. I3 (§2) — is ACK lifecycle separation clean?
5. COMMITTED lifecycle (§3) — are all states and transitions correct?
6. Canonical serialization (§4) — is byte-level definition precise?
7. NVS contract (§5) — is the corrected rationale honest?
8. Partition sizing (§6) — is empirical verification acceptable?
9. Generation distance bound (§7) — is ≤1 guarantee correct?

**After auditor approval, implementation Phase 1-8 may begin. Until then: NO CODE.**

Precharge: BLOCKED
I/O expander: BLOCKED
16-relay: BLOCKED
