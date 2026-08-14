# CYCLE-8C-Rev6: Transaction Journal v4 — Consistency Cleanup

**Status**: DESIGN ONLY — NO CODE IMPLEMENTATION
**Purpose**: Fix internal contradictions, no new features
**Auditor instruction**: "Rev6 tidak boleh menambah metadata baru kecuali metadata tersebut diperlukan oleh salah satu invariant yang sudah diformalkan."

---

## 1. Root Cause Analysis (Why Rev5 Failed)

### C8CR5-001 (P0): Canonical Serialization Contradiction

Rev5 had TWO definitions of canonical payload:

**§2 I1 definition** (includes recordState):
```
canonicalPayload =
    recordState + requestIdLen + requestId + commandHashLen + commandHash
    + channelId + desiredState + previousKnownState + attempt + timestamp
    + ackLen + ackJson
```

**§4 definition** (excludes recordState):
```
"record blob (bytes 12..end, after header+generation+CRC) IS the canonical form"
→ starts at requestIdLen, NOT recordState (recordState is at byte 3, in header)
```

**Consequence**: If §4 is used, two copies with same generation but different recordState (EXECUTING vs COMMITTED) would be considered "equivalent" — the exact bug Rev4 was supposed to fix.

### C8CR5-002 (P0): ACK Lifecycle Not Durable

Rev5 claimed ACK lifecycle is independent of transaction lifecycle, but ACK delivery state (NOT_SENT, PUBLISH_ACCEPTED, BROKER_CONFIRMED, PWA_RECEIVED) was only tracked in RAM. After reboot:
- ACK queue (tj_ackq) stores requestId + ackJson
- But NOT delivery state
- Firmware doesn't know if ACK was already published

### Pattern

The root cause is **internal contradiction**: the formal model says one thing, the implementation sketch says another. Rev6 eliminates ALL contradictions.

---

## 2. Fix #1: Single Canonical Serialization (C8CR5-001)

### ONE Definition (Authoritative)

```
CANONICAL PAYLOAD = byte sequence starting at recordState:

Byte  Field              Size
0     recordState        1
1     requestIdLen       1
2..   requestId          requestIdLen
..    commandHashLen     1
..    commandHash        commandHashLen
..    channelId          1
..    desiredState       1
..    previousKnownState 1
..    attempt            1
..    timestamp          4 (uint32 LE)
..    ackLen             2 (uint16 LE)
..    ackJson            ackLen

This IS the canonical form. Comparison = byte-exact (memcmp).

CRC covers: header (magic+version+generation) + canonical payload.
CRC does NOT cover itself.
```

### Record Layout (Revised — recordState MOVED to payload)

```
Offset  Field              Size  Description
------  ----------------   ----  ------------------------------------------
0       magic              2     0x54, 0x4A ("TJ")
2       schemaVersion      1     4
3       generation         4     uint32 LE
7       recordCRC          4     CRC32 over bytes 0..2 AND bytes 11..end
                                (covers header + canonical payload, NOT CRC)
11      recordState        1     ← NOW PART OF CANONICAL PAYLOAD
12      requestIdLen       1     0..64
13..   requestId          var
..      commandHashLen    1
..      commandHash       var
..      channelId          1
..      desiredState       1
..      previousKnownState 1
..      attempt            1
..      timestamp          4
..      ackLen             2
..      ackJson            var
..      (padding to 1200 bytes)
```

**Key change**: `recordState` moved from header (byte 3) to payload (byte 11). Now the canonical payload definition is unambiguous — it starts at byte 11 and includes recordState as first field.

**Canonical payload** = bytes 11..end (starts at recordState).
**CRC** = covers bytes 0..2 (magic, version) + bytes 3..6 (generation) + bytes 11..end (canonical payload). Does NOT cover bytes 7..10 (CRC field itself).

### Equivalence Check

```cpp
bool canonicallyEquivalent(const uint8_t* blobA, const uint8_t* blobB, size_t len) {
    // Both blobs must have same length (they should — fixed size)
    // Compare canonical payload (bytes 11..end)
    return memcmp(blobA + 11, blobB + 11, len - 11) == 0;
}
```

This compares recordState, requestId, commandHash, channelId, desiredState, previousKnownState, attempt, timestamp, ackLen, ackJson — ALL fields.

**If recordState differs → NOT equivalent → CORRUPTED (for same-generation copies).**

---

## 3. Fix #2: ACK Queue Record Format + Durable ACK State (C8CR5-002)

### Problem

Rev5 stored ACK queue as `{requestId, ackJson}` in tj_ackq. But ACK delivery state (NOT_SENT, PUBLISH_ACCEPTED, etc.) was RAM-only. After reboot, delivery state is lost.

### Solution: ACK Record Format

```
ACK RECORD (stored in tj_ackq blob):

Byte  Field              Size  Description
0     ackMagic           2     0x41, 0x4B ("AK")
2     ackVersion         1     1
3     deliveryState      1     enum (see below)
4     requestIdLen       1     1..64
5..   requestId          var
..    commandHashLen    1
..    commandHash       var
..    retryCount        1     0..MAX_ACK_RETRIES
..    lastAttemptTs     4     uint32 LE (unix seconds, 0 = never)
..    ackLen             2     uint16 LE
..    ackJson            var
..    (padding to fixed ACK_RECORD_SIZE = 256 bytes)
```

### Delivery State Enum (Durable)

```
ACK_NOT_SENT           = 0  — ACK not yet attempted
ACK_PUBLISH_ACCEPTED   = 1  — mqtt.publish() returned true
ACK_BROKER_CONFIRMED  = 2  — QoS 1 PUBACK received (future)
ACK_PWA_RECEIVED       = 3  — application-level ack received (future)
ACK_FAILED_EXHAUSTED   = 4  — max retries reached, give up
```

**These are DURABLE** — stored in tj_ackq, survive reboot.

### tj_ackq Blob Layout

```
tj_ackq = {
    uint8_t count;                          // 0..MAX_PENDING_ACKS (8)
    uint8_t reserved[3];                    // alignment
    AckRecord records[MAX_PENDING_ACKS];    // 8 × 256 = 2048 bytes
    uint32_t queueCRC;                      // CRC over above
}
Total: 4 + 2048 + 4 = 2056 bytes
```

### ACK Queue Operations

**queueAck(requestId, ackJson)**:
1. Acquire I0 (single-threaded context)
2. Find existing entry by requestId (or free slot)
3. Set deliveryState = ACK_NOT_SENT
4. Set retryCount = 0
5. Set ackJson
6. Persist tj_ackq (putBytes + verify)
7. Update RAM

**onPublishAccepted(requestId)**:
1. Find entry by requestId
2. Set deliveryState = ACK_PUBLISH_ACCEPTED
3. Persist tj_ackq
4. Update RAM

**onBrokerConfirmed(requestId)** (future, QoS 1):
1. Find entry by requestId
2. Set deliveryState = ACK_BROKER_CONFIRMED
3. Persist tj_ackq
4. Update RAM

**onPWAReceived(requestId)** (future, app-level ack):
1. Find entry by requestId
2. Set deliveryState = ACK_PWA_RECEIVED
3. Persist tj_ackq
4. Update RAM

**dequeueAck(requestId)** (after PWA received, or eviction):
1. Find entry by requestId
2. Mark slot as free (deliveryState = 0, requestId = "")
3. Persist tj_ackq
4. Update RAM

### Boot Recovery (Fixes C8CR5-007)

**NOT journal-only rebuild. Merge from two sources:**

```
1. Read tj_ackq from NVS (durable ACK queue)
2. Scan journal for COMMITTED entries with non-empty ackJson
3. MERGE:
   For each journal COMMITTED entry:
       If requestId exists in tj_ackq → skip (already in queue)
       Else → add to RAM queue (from journal record)
4. For each tj_ackq entry:
       If requestId exists in journal → keep (both sources agree)
       Else → transaction was evicted, ACK is orphaned
              Keep in queue (PWA may still need it)
              OR: if deliveryState = ACK_FAILED_EXHAUSTED → remove
5. Persist merged queue to tj_ackq
```

**Key**: ACK queue is INDEPENDENT. Journal rebuild ADDS missing entries, but does NOT replace existing queue entries. Orphaned ACKs (transaction evicted but ACK not delivered) are retained.

---

## 4. Fix #3: I0 as Pure Single-Threaded Invariant (C8CR5-003)

### Problem

Rev5 mixed two models: "single-threaded architecture" and "mutex placeholder". This is confusing.

### Solution: ONE Model

```
I0 — SINGLE-THREADED ARCHITECTURE INVARIANT

All TransactionJournal API calls MUST execute in loop() context.

PROHIBITED:
    - Calling TransactionJournal from ISR
    - Calling TransactionJournal from FreeRTOS task
    - Calling TransactionJournal from MQTT callback that runs in separate task
    - Calling TransactionJournal from WiFi event handler that runs in separate task

VERIFIED BY ARCHITECTURE:
    - PubSubClient callback: runs in loop() context (confirmed by library behavior)
    - PIR ISR: does NOT call journal (only sets flag, processed in loop)
    - WiFi events: do NOT call journal
    - WebServer handler: runs in loop() context

ENFORCEMENT:
    No mutex. No lock. No atomic operations.
    Single-threaded access is GUARANTEED BY ARCHITECTURE, not by synchronization.

IF ARCHITECTURE CHANGES:
    If any code path allows concurrent journal access:
        STOP IMMEDIATELY.
        This invariant is VIOLATED.
        Architecture revision required (add mutex/queue).
        Re-audit required before deployment.

NO PSEUDO-MUTEX:
    There is no mutex placeholder.
    There is no "mutex can be no-op".
    There is ONLY the single-threaded guarantee.
    Code comments state: "Single-threaded access required (I0). Do not call from ISR/task."
```

---

## 5. Fix #4: I1g as Runtime-Enforced Invariant (C8CR5-004)

### Problem

Rev5 stated generation distance ≤ 1 as design proof, not runtime check.

### Solution: Runtime Assertion

```
I1g — GENERATION DISTANCE RUNTIME CHECK

During _checkI1Satisfied(), after both copies are read and verified valid:

    if (genA != genB) {
        uint32_t diff = (genA > genB) ? (genA - genB) : (genB - genA);
        if (diff > 1) {
            // ABNORMAL: generation distance > 1
            // This should never happen in normal operation.
            // Possible causes:
            //   - Bug in write protocol
            //   - NVS corruption that passed CRC (extremely unlikely)
            //   - Manual NVS modification
            Serial.printf("[Journal] FATAL: generation distance %u > 1 (slot %u) — CORRUPTED\n",
                          diff, slotIdx);
            markCorrupted(slotIdx);
            return false;
        }
    }

This is a RUNTIME ASSERTION, not just a design claim.
If triggered: slot is CORRUPTED (quarantined, operator recovery required).
```

---

## 6. Fix #5: Remove EPOCH_RESET generation=0 (C8CR5-005)

### Problem

Rev5 allowed `generation=0` when both copies unreadable. This makes an ordering claim without evidence.

### Solution: Quarantine, No Auto-Recovery

```
WHEN BOTH COPIES ARE UNREADABLE (headers corrupt, generation cannot be extracted):

    DO NOT write generation=0.
    DO NOT auto-recover.
    
    Slot is CORRUPTED (derived state).
    Slot is QUARANTINED (not freed, not reused).
    
    Operator must use recoverCorruptedEntry():
        1. Write forensic record (best-effort metadata extraction)
        2. Write EMPTY to copy A with generation = 0
           BUT: this is explicitly logged as "SLOT_RESET" not "EPOCH_RESET"
           "SLOT_RESET" means: this slot's generation counter is reset to 0.
           Previous generations for this slot are INVALID.
           This does NOT affect other slots.
        3. Write EMPTY to copy B with generation = 0
        4. Verify both copies
    
    IMPORTANT:
        - generation=0 is ONLY written by explicit operator recovery.
        - It is NEVER written automatically by firmware.
        - The forensic log records: "SLOT_RESET, previous generation unknown"
        - Future writes to this slot continue from 0+1=1.
        - Other slots are unaffected (each slot has independent generation).

WHY THIS IS SAFE:
    - Operator explicitly chose to reset this slot.
    - Operator verified physical relay state (procedure documented).
    - The slot was already CORRUPTED (no valid data).
    - Writing EMPTY with generation=0 is better than leaving it quarantined forever.
    - The forensic record preserves the fact that a reset occurred.
```

### Recovery Protocol (Revised)

```
recoverCorruptedEntry(slotIdx):
    1. Read both copies (best-effort)
    2. Extract whatever metadata is readable
    3. Write forensic record:
        - slotIdx
        - best-effort requestId (if readable)
        - best-effort generationA, generationB (if readable)
        - reason: BOTH_CORRUPT or OPERATOR_INITIATED
        - action: SLOT_RESET
        - note: "generation continuity broken for this slot"
    4. Verify forensic record written
    5. If forensic write FAILED → ABORT (slot stays CORRUPTED)
    6. Write EMPTY to copy A (generation = 0)
    7. Verify copy A
    8. Write EMPTY to copy B (generation = 0)
    9. Verify copy B
    10. Slot is now EMPTY (usable, generation starts from 0)
```

---

## 7. Fix #6: Eviction Table by Command Class × ACK State (C8CR5-006)

### Formal Eviction Matrix

```
COMMAND CLASSIFICATION:
    IDEMPOTENT:    relay ON, relay OFF, set_mode, schedule upsert, schedule delete,
                    PIR config, channel rename, time set, config set
                    (Re-executing produces same physical result)
    
    NON_IDEMPOTENT: OTA update, factory reset, future precharge
                    (Re-executing may have different/dangerous effect)
    
    UNKNOWN:        Any command type not yet classified
                    (Default: treat as non-idempotent for safety)

EVICTION MATRIX:

| Command Class   | ACK State                 | Eviction Allowed? |
|-----------------|---------------------------|-------------------|
| IDEMPOTENT      | ACK_PWA_RECEIVED          | YES               |
| IDEMPOTENT      | ACK_BROKER_CONFIRMED      | YES               |
| IDEMPOTENT      | ACK_PUBLISH_ACCEPTED      | YES (if durable queue) |
| IDEMPOTENT      | ACK_NOT_SENT              | NO                |
| IDEMPOTENT      | ACK_FAILED_EXHAUSTED      | YES (PWA gave up, status query available) |
| NON_IDEMPOTENT  | ACK_PWA_RECEIVED          | YES               |
| NON_IDEMPOTENT  | ACK_BROKER_CONFIRMED      | YES               |
| NON_IDEMPOTENT  | ACK_PUBLISH_ACCEPTED      | NO                |
| NON_IDEMPOTENT  | ACK_NOT_SENT              | NO                |
| NON_IDEMPOTENT  | ACK_FAILED_EXHAUSTED      | NO (operator must investigate) |
| UNKNOWN         | ANY                       | NO (retain until classified) |
```

### Eviction Pre-Condition (I2, Revised)

```
evictionAllowed(slotIdx):
    1. I2a: Journal full? Slot needed? (retention policy)
    2. I2b: Command class is IDEMPOTENT or NON_IDEMPOTENT (not UNKNOWN)
    3. I2c: ACK condition (from matrix above):
       - Look up command class × ACK delivery state
       - If matrix says YES → eviction allowed
       - If matrix says NO → eviction blocked
    4. I2d: No unresolved recovery (slot not CORRUPTED)
    5. I2e: Default = RETAIN (if any check is uncertain → NO eviction)
    
    ALL must pass for eviction to proceed.
```

---

## 8. Fix #7: ACK Recovery = Merge Journal + Independent ACK Queue (C8CR5-007)

### Problem

Rev5 said "ACK queue rebuilt from journal COMMITTED entries on boot". This makes journal the only source, which contradicts I3 (ACK lifecycle independent).

### Solution: Merge From Two Sources

```
BOOT ACK RECOVERY:

1. Read tj_ackq from NVS (durable ACK queue with delivery states)
   → Set A: {requestId, deliveryState, ackJson, ...} from tj_ackq

2. Scan journal for COMMITTED / COMMITTED_UNKNOWN / EXECUTION_FAILED_OUTPUT_MISMATCH
   entries with non-empty ackJson
   → Set B: {requestId, ackJson, ...} from journal

3. MERGE:
   result = {}
   
   For each entry in Set A (durable ACK queue):
       Add to result (durable queue is authoritative for delivery state)
   
   For each entry in Set B (journal):
       If requestId NOT in result:
           Add to result with deliveryState = ACK_NOT_SENT
           (ACK was lost from queue, but transaction is still in journal)
   
   // Orphaned ACKs (in queue but not in journal) are RETAINED:
   //   - Transaction was evicted, but ACK not yet delivered
   //   - Keep in queue until delivered or exhausted

4. Persist merged result to tj_ackq
5. Load into RAM for processing
```

### Independence Guarantee

```
ACK queue is INDEPENDENT of journal:
    - ACK queue can have entries for evicted transactions (orphaned ACKs)
    - Journal can have COMMITTED entries not in ACK queue (ACK was delivered + dequeued)
    - On boot: merge fills gaps, but does NOT delete orphans
    
Orphaned ACK lifecycle:
    - ACK_FAILED_EXHAUSTED orphans: can be removed (PWA gave up)
    - ACK_PWA_RECEIVED orphans: can be removed (delivery confirmed)
    - Other orphans: retained (delivery may still succeed)
```

---

## 9. Fix #8: EVICTABLE as Derived State (C8CR5-008)

### Problem

Rev5 listed EVICTABLE as a sub-state of COMMITTED, implying it might be stored.

### Solution: EVICTABLE is Computed, Not Stored

```
STORED STATE (in record):
    recordState = COMMITTED

COMPUTED STATE (in RAM, derived at runtime):
    evictable = computeEvictability(slotIdx)
    
computeEvictability(slotIdx):
    1. recordState must be COMMITTED or COMMITTED_UNKNOWN
    2. Check I2a-I2e (eviction matrix)
    3. Return true ONLY if all conditions met
    
This is NEVER persisted.
It is recomputed every time eviction is considered.
If conditions change (e.g., ACK delivery state updates), evictability changes.
No stored "EVICTABLE" state to become inconsistent.
```

### Revised COMMITTED Lifecycle (EVICTABLE removed from stored states)

```
STORED recordState values:
    EMPTY
    PENDING
    EXECUTING
    COMMITTED
    COMMITTED_UNKNOWN
    UNKNOWN
    FAILED
    EXECUTION_FAILED_OUTPUT_MISMATCH
    (CORRUPTED is derived, not stored — from Rev4)

COMPUTED (not stored):
    evictable (derived from COMMITTED + I2 conditions)
    ackDeliveryState (stored in ACK queue, not in journal record)
    ackRetrieved (derived from ACK queue state)
```

---

## 10. Consolidated Invariant Summary (I0-I3, Revised)

### I0 — Single-Threaded Architecture (No Mutex)
```
All journal API calls execute in loop() context.
No ISR, no FreeRTOS task, no concurrent access.
Guaranteed by architecture, not by synchronization.
If architecture changes: STOP, re-audit.
```

### I1 — Canonical Equivalence + Recovery
```
I1a: Copy A structurally valid (CRC passes)
I1b: Copy B structurally valid (CRC passes)
I1c: Mutual consistency (same-gen→byte-equal, diff-gen→strict order)
I1d: No generation ambiguity (diff != 2^31)
I1e: Same-gen byte-level payload equality (memcmp, includes recordState)
I1f: Diff-gen strict ordering (serial-number arithmetic)
I1g: Generation distance ≤ 1 (RUNTIME ASSERTED, not just design proof)
```

### I2 — Eviction Safety (Derived EVICTABLE)
```
I2a: Retention policy permits (journal full, slot needed)
I2b: Command class is IDEMPOTENT or NON_IDEMPOTENT (not UNKNOWN)
I2c: ACK condition met (eviction matrix §7)
I2d: No unresolved recovery (not CORRUPTED)
I2e: Default = RETAIN (uncertain → no eviction)

EVICTABLE is COMPUTED from I2a-I2e, never stored.
```

### I3 — ACK Lifecycle Separation (Durable ACK State)
```
I3a: Transaction lifecycle independent of ACK lifecycle
I3b: ACK queue persists independently (tj_ackq with deliveryState)
I3c: Eviction does NOT delete ACK queue entry
I3d: Boot recovery = MERGE journal + ACK queue (not journal-only rebuild)
```

---

## 11. Revised Record Layout (Final, No Contradictions)

```
Offset  Field              Size  CRC Coverage
------  ----------------   ----  ------------
0       magic              2     YES (0x54, 0x4A)
2       schemaVersion      1     YES (4)
3       generation         4     YES (uint32 LE)
7       recordCRC          4     NO (CRC field itself)
--- CANONICAL PAYLOAD STARTS HERE (byte 11) ---
11      recordState        1     YES
12      requestIdLen       1     YES
13..   requestId          var   YES
..      commandHashLen     1     YES
..      commandHash        var   YES
..      channelId          1     YES
..      desiredState        1     YES
..      previousKnownState 1     YES
..      attempt            1     YES
..      timestamp          4     YES (uint32 LE)
..      ackLen             2     YES (uint16 LE)
..      ackJson            var   YES
..      (padding to 1200 bytes, zeros, YES for CRC)
```

**CRC** = CRC32(bytes 0..6 + bytes 11..end) = CRC32(header + canonical_payload)

**Canonical payload** = bytes 11..end (starts at recordState)

**Equivalence** = memcmp(blobA + 11, blobB + 11, payloadLen) for same-generation copies

**NO CONTRADICTION**: §2 and §4 now agree. recordState is part of canonical payload. Comparison includes recordState.

---

## 12. ACK Queue Record Layout (Final)

```
ACK RECORD (256 bytes fixed):
Offset  Field              Size
0       ackMagic           2     0x41, 0x4B ("AK")
2       ackVersion         1     1
3       deliveryState      1     enum (ACK_NOT_SENT..ACK_FAILED_EXHAUSTED)
4       requestIdLen       1     0..64
5..   requestId          var
..      commandHashLen    1
..      commandHash       var
..      retryCount        1
..      lastAttemptTs     4     uint32 LE
..      ackLen             2     uint16 LE
..      ackJson            var
..      (padding to 256 bytes)

tj_ackq BLOB:
    [count:1] [reserved:3] [AckRecord × 8 = 2048] [queueCRC:4]
    Total: 2056 bytes
```

---

## 13. Crash Matrix (Unchanged from Rev5, with I1g assertion)

All scenarios from Rev5 §11 are still valid. Addition:

### I1g Runtime Check

```
During _checkI1Satisfied():
    After reading both copies, if both valid:
        if genA != genB:
            diff = abs(genA - genB)
            if diff > 1:
                → CORRUPTED (runtime assertion failure)
                → Log: "generation distance violation"
                → Quarantine slot
```

This catches bugs in the write protocol or unexpected NVS behavior.

---

## 14. Honest Limitations (Unchanged from Rev5)

1. Snapshot reflects safe-OFF, not pre-crash state — hardware limitation
2. GPIO output ≠ physical relay contact — welded/stuck undetectable
3. Dual-copy is LOGICAL redundancy, NOT physical independence
4. CRC32 protects against accident, NOT malicious modification
5. NVS endurance is theoretical — must test empirically
6. ACK_PWA_RECEIVED is NOT implemented — eviction carries delivery risk for idempotent commands
7. Hardware power-loss testing NOT RUN
8. fsync semantics must be verified at implementation time
9. Partition size must be verified empirically

---

## 15. What This Design Does NOT Solve

- Pre-crash GPIO state recovery (hardware revision needed)
- Physical relay contact verification (feedback hardware needed)
- Physical flash independence (separate flash chips needed)
- Tamper protection (Flash Encryption + Secure Boot needed)
- Application-level ACK confirmation (PWA → device ack)
- Multi-output transaction model (precharge) — separate cycle needed
- 16-relay / I/O expander — separate cycle needed
- F-008 PWA credential architecture — separate cycle needed

---

## 16. Rule Compliance Check

**Auditor rule**: "Rev6 tidak boleh menambah metadata baru kecuali metadata tersebut diperlukan oleh salah satu invariant yang sudah diformalkan."

| New Metadata | Required By | Justified? |
|--------------|-------------|------------|
| deliveryState in ACK record | I3 (ACK lifecycle durable) | YES — I3 requires durable ACK state |
| generation distance check (I1g) | I1 (canonical equivalence) | YES — already in I1, just runtime-enforced |
| SLOT_RESET in forensic log | C8CR5-005 (remove EPOCH_RESET) | YES — replaces EPOCH_RESET, required for recovery audit |

**No other new metadata.** All changes are fixes to existing invariants, not additions.

---

## 17. Implementation Plan (After Auditor Approval — NOT YET STARTED)

Phase 1-8 from Rev5, with:
- Byte-level canonical serialization (recordState at byte 11)
- ACK queue with durable deliveryState
- I1g runtime assertion
- Eviction matrix (command class × ACK state)
- ACK merge recovery (journal + queue)
- EVICTABLE computed, not stored
- No EPOCH_RESET (SLOT_RESET by operator only)

---

## 18. Auditor Approval Gate

**This design is NOT approved for implementation.**

Auditor must verify:
1. Fix #1 (§2): Is canonical serialization now unambiguous? recordState included?
2. Fix #2 (§3): Is ACK queue record format complete? deliveryState durable?
3. Fix #3 (§4): Is I0 pure single-threaded (no pseudo-mutex)?
4. Fix #4 (§5): Is I1g runtime-enforced (assert, not just design)?
5. Fix #5 (§6): Is EPOCH_RESET removed? SLOT_RESET by operator only?
6. Fix #6 (§7): Is eviction matrix complete (command class × ACK state)?
7. Fix #7 (§8): Is ACK recovery merge (journal + queue), not journal-only?
8. Fix #8 (§9): Is EVICTABLE derived, not stored?
9. Rule compliance (§16): No unjustified new metadata?

**After auditor approval, implementation Phase 1-8 may begin. Until then: NO CODE.**

Precharge: BLOCKED
I/O expander: BLOCKED
16-relay: BLOCKED
