# CYCLE-8C-Rev4: Transaction Journal v4 — Canonical Record Equivalence & Recovery Semantics

**Status**: DESIGN ONLY — NO CODE IMPLEMENTATION
**Purpose**: Define canonical record equivalence and full recovery decision table
**Auditor instruction**: "Definisikan Canonical Record Equivalence + Recovery Rules"

---

## 1. Root Cause Analysis (Why Rev3 Failed)

### C8CR3-001 (P0): Same-generation divergent copies not detected

Rev3 I1 stated:
> "Before any mutation: copy A is VALID AND copy B is VALID"

But VALID means "CRC passes" — it does NOT mean "same logical record". Two copies can both be CRC-valid but contain different payloads:

```
COPY A: generation=100, state=EXECUTING, requestId=ABC, CRC=VALID
COPY B: generation=100, state=COMMITTED, requestId=ABC, CRC=VALID
```

Rev3 selector: `sameGeneration(genA, genB) → pick A (arbitrary)`.

**This is logical corruption.** One of these copies is wrong. Picking arbitrarily can select COMMITTED when the transaction was actually EXECUTING (or vice versa). The journal must detect this divergence and mark CORRUPTED, not silently pick one.

### C8CR3-002 (P0): ACK publish ≠ PWA received

Rev3 I5 stated:
> "ACK has been delivered to PWA (mqtt.publish returned true)"

`mqtt.publish()` returning true means:
- MQTT client library accepted the publish call
- The bytes were handed to the TCP stack
- The broker MAY have received them
- PWA MAY have received the ACK

It does NOT mean:
- Broker processed the message
- PWA subscribed to the topic
- PWA processed the ACK
- Network didn't drop the packet between broker and PWA

**Conflating "publish accepted" with "PWA received" is a contract error.** Eviction based on false "delivered" assumption loses transaction history before PWA knows the result.

### C8CR3-007 (P1): I1 is not an integrity invariant

I1 ("both copies valid") is a **structural validity** check, not an **integrity** check. Integrity requires:
- Structural validity (CRC passes)
- Mutual consistency (both copies represent same logical record)
- Generation relationship valid (not ambiguous)

### Pattern Across All Cycles

| Cycle | Missing Piece |
|-------|--------------|
| 7 | Durable intent |
| 8A | Boot ordering |
| 8B | State monotonicity |
| 8C | Invariant strictness |
| 8C-Rev1 | Atomic write assumption |
| 8C-Rev2 | Generation wrap |
| 8C-Rev3 | **Canonical equivalence** |

**The lesson**: Every cycle found a missing formal definition. Rev4 completes the formal model by defining **Canonical Record Equivalence** — the exact conditions under which two copies are considered the same logical record.

---

## 2. Copy State Classification

When reading a copy (A or B), we classify it into one of these states:

### CopyValidity

```
COPY_INVALID     — CRC fails, or blob missing, or magic wrong, or schema version mismatch
COPY_VALID       — CRC passes, all structural checks pass
```

### CopyContent (only meaningful if COPY_VALID)

Once we know a copy is structurally valid, we extract:
- `generation` (uint32)
- `recordState` (enum: EMPTY, PENDING, EXECUTING, COMMITTED, etc.)
- `requestId` (string)
- `commandHash` (string)
- `channelId`, `desiredState`, `previousKnownState`, `attempt`, `timestamp`
- `ackJson` (string)

### Canonical Payload

The "canonical payload" of a valid copy is the tuple:
```
(recordState, requestId, commandHash, channelId, desiredState,
 previousKnownState, attempt, timestamp, ackJson)
```

**Note**: `generation` is NOT part of canonical payload. Two copies with same generation but different generation values is impossible (generation is compared separately). Two copies with same generation must have identical canonical payload to be considered equivalent.

---

## 3. Generation Relationship Classification

When comparing two valid copies with generations genA and genB:

```
GEN_NEWER      — isNewer(genA, genB) == true  → A is newer
GEN_OLDER      — isOlder(genA, genB) == true  → B is newer (A is older)
GEN_EQUAL      — genA == genB                  → same generation
GEN_AMBIGUOUS  — isAmbiguous(genA, genB)       → cannot determine (diff == 2^31)
```

### Serial-Number Arithmetic (from Rev3, corrected)

```cpp
bool isNewer(uint32_t a, uint32_t b) {
    return (int32_t)(a - b) > 0;
}

bool isAmbiguous(uint32_t a, uint32_t b) {
    if (a == b) return false;
    return (a - b) == 0x80000000;
}
```

### Generation Lifetime (Fixes C8CR3-006 math error)

Rev3 had contradictory numbers. Corrected:

```
Generation is PER-SLOT (each slot has its own generation counter).
Per-slot write rate: assume 100 writes/day (worst case for one slot)
Generation space: 2^32 = 4,294,967,296

Time to wrap (per slot):
    4,294,967,296 / 100 writes/day = 42,949,672 days = 117,588 years

Time to ambiguity (diff == 2^31):
    2,147,483,648 / 100 = 21,474,836 days = 58,831 years

Note: 58,831 years ≈ 58.8 thousand years, NOT 58 million years (Rev3 error corrected).
```

**Acceptable**: Ambiguity will never occur in practice.

---

## 4. Canonical Record Equivalence

### Definition

Two valid copies A and B are **canonically equivalent** if and only if:

```
1. GEN_EQUAL: genA == genB
2. PayloadEqual: canonicalPayload(A) == canonicalPayload(B)
```

Where `canonicalPayload` is the tuple defined in §2 (excluding generation).

### Equivalence Check

```cpp
bool canonicallyEquivalent(Copy A, Copy B) {
    if (A.generation != B.generation) return false;
    if (A.recordState != B.recordState) return false;
    if (A.requestId != B.requestId) return false;
    if (A.commandHash != B.commandHash) return false;
    if (A.channelId != B.channelId) return false;
    if (A.desiredState != B.desiredState) return false;
    if (A.previousKnownState != B.previousKnownState) return false;
    if (A.attempt != B.attempt) return false;
    if (A.timestamp != B.timestamp) return false;
    if (A.ackJson != B.ackJson) return false;
    return true;
}
```

### Divergence Detection

If `GEN_EQUAL` is true but `PayloadEqual` is false → **COPY_DIVERGENT**.

This is the case Rev3 missed:
```
COPY A: generation=100, state=EXECUTING, CRC=VALID
COPY B: generation=100, state=COMMITTED, CRC=VALID
→ same generation, different payload → DIVERGENT → CORRUPTED
```

---

## 5. Full Recovery Decision Table

This is the core of Rev4. Every possible combination of copy A and copy B states, and the recovery action.

### Notation

- **V** = COPY_VALID (CRC passes)
- **I** = COPY_INVALID (CRC fails / missing)
- **genA, genB** = generations of A, B
- **payloadA, payloadB** = canonical payloads

### Table

| # | Copy A | Copy B | Gen Relationship | Payload Match? | Result | Action |
|---|--------|--------|-------------------|----------------|--------|--------|
| 1 | INVALID | INVALID | N/A | N/A | CORRUPTED | Quarantine, operator recovery |
| 2 | VALID | INVALID | N/A | N/A | REPAIR | Copy A to B, verify, recheck |
| 3 | INVALID | VALID | N/A | N/A | REPAIR | Copy B to A, verify, recheck |
| 4 | VALID | VALID | GEN_NEWER (A > B) | N/A | NEW=A | Load A, mark B for overwrite |
| 5 | VALID | VALID | GEN_OLDER (A < B) | N/A | NEW=B | Load B, mark A for overwrite |
| 6 | VALID | VALID | GEN_EQUAL | YES (canonical eq) | VALID | Load A (or B, identical) |
| 7 | VALID | VALID | GEN_EQUAL | NO (divergent) | CORRUPTED | Quarantine, operator recovery |
| 8 | VALID | VALID | GEN_AMBIGUOUS | N/A | CORRUPTED | Quarantine, cannot determine order |

### EMPTY-State Specific Rules (Fixes C8CR3-008)

EMPTY is a valid recordState. The table above applies, but EMPTY has special semantics:

| # | Copy A | Copy B | Gen Rel | Result | Reason |
|---|--------|--------|---------|--------|--------|
| 9 | EMPTY(gen=100) | COMMITTED(gen=99) | GEN_NEWER | EMPTY | A is newer → slot is EMPTY |
| 10 | COMMITTED(gen=100) | EMPTY(gen=99) | GEN_OLDER | COMMITTED | A is newer → slot is COMMITTED |
| 11 | EMPTY(gen=100) | EMPTY(gen=99) | GEN_NEWER | EMPTY | Both EMPTY, A newer |
| 12 | EMPTY(gen=100) | EMPTY(gen=100) | GEN_EQUAL + payload eq | EMPTY | Both EMPTY, identical |
| 13 | EMPTY(gen=100) | COMMITTED(gen=100) | GEN_EQUAL + payload divergent | CORRUPTED | Same gen, divergent → corrupt |
| 14 | EMPTY(gen=100) | EMPTY(gen=100) | GEN_EQUAL + payload divergent | CORRUPTED | Impossible if both truly EMPTY (no payload), but if requestIdLen differs → corrupt |

**Rule**: EMPTY with higher generation always wins (slot is free). EMPTY with same generation but divergent payload → CORRUPTED (logical corruption).

### Recovery Action Semantics

**REPAIR** (cases 2, 3):
- Read valid copy
- Write its record to invalid copy (same generation, same payload)
- Verify (re-read + CRC)
- If verify fails → CORRUPTED
- If verify succeeds → I1 now satisfied, ready for mutation

**NEW=A or NEW=B** (cases 4, 5):
- Load the newer copy into RAM
- The older copy will be overwritten on next mutation (COW)
- No immediate action needed — slot is usable

**VALID** (case 6):
- Both copies valid and identical
- Load either into RAM
- Slot is fully healthy

**CORRUPTED** (cases 1, 7, 8, 13, 14):
- Slot is quarantined
- No mutations allowed
- Operator must use `recoverCorruptedEntry()`
- Forensic record written before any erase (I4)

---

## 6. Revised Invariants (I1 expanded to I1a-I1f)

### I1a — Structural Validity (Copy A)
```
Copy A passes CRC, magic, schema version checks.
```

### I1b — Structural Validity (Copy B)
```
Copy B passes CRC, magic, schema version checks.
```

### I1c — Mutual Consistency
```
If genA == genB:
    canonicalPayload(A) == canonicalPayload(B)
    
If genA != genB:
    One is strictly newer (GEN_NEWER or GEN_OLDER, not GEN_AMBIGUOUS)
```

### I1d — Generation Relationship Valid
```
Generation comparison is not GEN_AMBIGUOUS.
(isAmbiguous(genA, genB) == false)
```

### I1e — Same-Generation Payload Equality
```
If genA == genB:
    Every field (recordState, requestId, commandHash, channelId,
    desiredState, previousKnownState, attempt, timestamp, ackJson)
    MUST be identical between A and B.
    
    If ANY field differs → COPY_DIVERGENT → CORRUPTED.
```

### I1f — Different-Generation Strict Ordering
```
If genA != genB:
    Exactly one copy is newer (GEN_NEWER).
    The newer copy is the active copy.
    The older copy is stale (will be overwritten on next mutation).
    
    GEN_AMBIGUOUS is NOT allowed → CORRUPTED.
```

### Full I1 (Composite)
```
I1 is satisfied if and only if ALL of:
    I1a: Copy A structurally valid
    I1b: Copy B structurally valid
    I1c: Mutual consistency (same-gen → equal payload; diff-gen → strict order)
    I1d: No generation ambiguity
    I1e: Same-gen payload equality
    I1f: Diff-gen strict ordering

If I1 is NOT satisfied:
    Attempt repair (if one copy invalid)
    If repair fails OR divergence detected → CORRUPTED
    No mutation may proceed.
```

### I2 — Generation Ordering (unchanged from Rev3)
```
Use serial-number arithmetic:
    isNewer(a, b) = (int32_t)(a - b) > 0
    isAmbiguous(a, b) = (a - b) == 0x80000000
Never use plain uint32 > comparison.
```

### I3 — COW Safety (unchanged from Rev3, but now with I1 enforcement)
```
Never overwrite active copy before verified alternate exists.
Mutation protocol:
    1. Verify I1 is satisfied (both copies valid + consistent)
    2. Identify active copy (higher generation, or either if equal)
    3. Write to inactive copy
    4. Verify (re-read + CRC)
    5. If verify fails → abort (old active still valid)
    6. If verify passes → inactive is now newer (active)
```

### I4 — Recovery Evidence (unchanged from Rev3, with durability clarification)
```
CORRUPTED recovery MUST write forensic record BEFORE erasing.
Forensic record stored in LittleFS (/journal_audit.log).
Forensic log durability depends on LittleFS power-loss contract (§8).
Never auto-deleted. GC is operator-initiated only.
```

### I5 — ACK Durability (REVISED — fixes C8CR3-002)

```
ACK delivery has THREE stages, NOT two:

    ACK_PUBLISH_ACCEPTED:
        mqtt.publish() returned true.
        Meaning: MQTT client library accepted the publish.
        Does NOT mean: broker received, PWA received.
    
    ACK_SENT_TO_BROKER:
        Broker acknowledged receipt (QoS 1 PUBACK received).
        Meaning: Broker has the message.
        Does NOT mean: PWA received.
    
    ACK_PWA_RECEIVED:
        PWA sent application-level acknowledgement.
        Meaning: PWA processed the ACK.
        This is the ONLY stage that guarantees delivery.

EVICTION CONTRACT (revised):
    Eviction of COMMITTED entry requires ALL:
        1. Entry state is COMMITTED or COMMITTED_UNKNOWN
        2. ackJson is durably persisted in journal record (CRC valid)
        3. ACK is in durable queue (NVS tj_ackq) AND one of:
           a. ACK_PUBLISH_ACCEPTED (publish succeeded, queue is backup)
           b. ACK_SENT_TO_BROKER (QoS 1 PUBACK received)
           c. ACK_PWA_RECEIVED (application-level ack from PWA)
    
    "mqtt.publish() == true" alone is NOT sufficient.
    The ACK MUST remain in durable queue until one of (b) or (c) is confirmed,
    OR until retention policy explicitly allows eviction (accepting that PWA
    may not have received the ACK).

HONEST CONTRACT:
    The journal CANNOT prove ACK_PWA_RECEIVED without application-level ack.
    Current design does NOT implement application-level ack.
    Therefore: eviction always carries risk that PWA never received ACK.
    This risk is ACCEPTED for COMMITTED entries (physical state already changed,
    PWA can query status to learn current state).
    This risk is NOT ACCEPTED for non-idempotent commands (future: precharge).
```

---

## 7. ACK Queue Durability Model (Fixes C8CR3-003)

### The Inconsistency in Rev3

Rev3 used dual-copy for journal records (distrusting NVS atomicity) but single-copy NVS for ACK queue (`tj_ackq` blob). This is inconsistent.

### Rev4 Resolution: Explicit NVS Contract Binding

```
NVS CRASH CONSISTENCY CONTRACT (per Espressif documentation):

NVS is designed to be power-loss resistant:
    - Existing data is NOT corrupted by power loss during write.
    - New write MAY be lost (incomplete write is discarded on boot).
    - NVS uses internal journaling within flash pages.

This means:
    - Single-copy NVS write is "crash-safe" in the sense that:
        OLD data preserved (if write didn't complete)
        NEW data preserved (if write completed)
        No partial/corrupt data (NVS internal CRC detects)
    - This is DIFFERENT from raw flash putBytes (which CAN be torn).

WHY JOURNAL USES DUAL-COPY BUT ACK QUEUE USES SINGLE-COPY:

Journal records:
    - Must survive corruption of one copy (dual-copy protects)
    - Contain transaction evidence (high value, must not lose)
    - Accept higher storage cost for reliability

ACK queue:
    - Is a DELIVERY optimization, not transaction evidence
    - If ACK queue is lost, transactions are still durable in journal
    - PWA can re-query status (status topic publishes current state)
    - Single-copy NVS is sufficient: if tj_ackq is lost, ACKs are re-queued
      from journal COMMITTED entries on boot

CONTRACT:
    ACK queue uses NVS single-copy (putBytes to tj_ackq).
    This relies on NVS's internal power-loss protection.
    If NVS itself is corrupt (page failure), ACK queue is lost,
    but journal records (dual-copy) survive.
    On boot: ACK queue is rebuilt from journal COMMITTED entries.

This is EXPLICIT and CONSISTENT:
    - Journal: dual-copy (protects against record corruption)
    - ACK queue: single-copy NVS (relies on NVS internal protection)
    - Forensic log: LittleFS (relies on LittleFS power-loss protection)
    Each storage tier has its durability contract explicitly stated.
```

### ACK Queue Rebuild on Boot

```
On boot, after journal is loaded:
    1. Read tj_ackq from NVS (single-copy)
    2. If tj_ackq is valid → load into RAM queue
    3. If tj_ackq is invalid/missing → rebuild queue:
        For each journal slot with state=COMMITTED or COMMITTED_UNKNOWN:
            If ackJson is non-empty → queueAck(requestId, ackJson)
    4. Persist rebuilt queue to tj_ackq
```

This ensures ACK delivery survives even total ACK queue loss.

---

## 8. Forensic Log Durability Model (Fixes C8CR3-004)

### LittleFS Power-Loss Contract

```
LittleFS is designed to be power-loss resistant:
    - Uses COW for metadata (inode updates)
    - Data writes are preceded by metadata updates
    - On power loss: either old state or new state, not partial
    
However:
    - LittleFS power-loss resistance depends on flash driver
    - ESP32 LittleFS implementation uses wear-leveling
    - fsync() flushes to flash (not just buffer)
    - Append may still be partial if power loss during data write

FORENSIC LOG DURABILITY CONTRACT:
    - Forensic log uses LittleFS file: /journal_audit.log
    - Writes are append + fsync + re-read verify
    - If verify fails → abort recovery (slot stays CORRUPTED)
    - LittleFS power-loss protection is TRUSTED for this use case
    
HONEST LIMITATION:
    - If LittleFS itself is corrupt (flash page failure), forensic log may be lost
    - This is the same limitation as NVS (physical flash failure)
    - For true forensic durability, separate flash chip would be needed
    - ACCEPTED for Rev4: forensic log is "best-effort durable" via LittleFS
    
CONTRACT:
    Forensic log durability = LittleFS power-loss protection.
    Not dual-copy, not HMAC-protected.
    Sufficient for audit trail (operator can verify physical relay state separately).
    NOT sufficient for tamper-evidence (attacker can modify LittleFS).
```

---

## 9. NVS Wear Calculation (Fixes C8CR3-005)

### Rev3 Error

Rev3 stated "383 years" as lifetime. This was based on:
- Logical bytes written / partition size
- Did not account for NVS internal overhead

### Rev4 Honest Calculation

```
RECORD SIZE:
    Header: 8 bytes (magic + version + state + generation)
    CRC: 4 bytes
    requestId: 1 + 64 = 65 bytes (max)
    commandHash: 1 + 64 = 65 bytes (max)
    channelId: 1 byte
    desiredState: 1 byte
    previousKnownState: 1 byte
    attempt: 1 byte
    timestamp: 4 bytes
    ackLen: 2 bytes
    ackJson: 0..1024 bytes
    Padding to: 1200 bytes (BLOB_SIZE)

STORAGE (32 slots, dual-copy):
    32 × 2 × 1200 = 76,800 bytes = 75 KB (raw)
    Recommended partition: 128 KB (with NVS overhead headroom)

WRITE AMPLIFICATION:
    Per transaction (relay ON/OFF):
        storeIntent: 1 COW write = 1200 bytes
        markExecuting: 1 COW write = 1200 bytes
        commitTransaction: 1 COW write = 1200 bytes (with ackJson)
        Total: 3 × 1200 = 3600 bytes per transaction

    Per day (estimated 50 transactions):
        50 × 3600 = 180,000 bytes/day = 176 KB/day

NVS INTERNAL OVERHEAD (per Espressif docs):
    - NVS entry: 32 bytes per key-value pair header
    - For 1200-byte blob: ~4 flash pages (4KB each) may be involved
    - NVS garbage collection moves data between pages
    - Actual flash erase cycles >> logical bytes written

ENDURANCE ESTIMATE (honest, no "383 years" claim):
    Flash endurance: 100,000 erase cycles per 4KB sector
    NVS partition: 128 KB = 32 sectors
    
    WORST CASE (all writes to same sector):
        32 sectors × 100,000 cycles = 3,200,000 total sector erases
        At 176 KB/day with ~4 sectors per write cycle:
        ~44 write cycles/day → ~44 sector erases/day (if evenly distributed)
        3,200,000 / 44 = ~72,727 days = ~199 years (theoretical, evenly distributed)
    
    REALISTIC (NVS wear-leveling is opaque):
        Actual lifetime DEPENDS ON NVS INTERNAL WEAR LEVELING.
        NVS is optimized for small key-value pairs, NOT large blobs.
        Espressif recommends filesystem (LittleFS/SPIFFS) for frequent large updates.
        
    CONCLUSION:
        "383 years" is REMOVED.
        Actual lifetime MUST be established experimentally.
        If wear is a concern: migrate journal to LittleFS (different wear-leveling).
        For Rev4: NVS is acceptable for 32-slot journal, but must be tested.
```

### Recommendation

```
For Rev4 implementation:
    - Start with 32 slots (75 KB raw, 128 KB partition)
    - Use NVS (simpler API, built-in wear-leveling)
    - Monitor flash wear in testing (ESP32 has flash size register)
    - If wear is excessive: migrate to LittleFS in future cycle
    - Espressif explicitly recommends LittleFS for frequent large updates
    - But: LittleFS has higher latency, different corruption modes
    - Decision deferred to testing phase
```

---

## 10. Durable Semantics of CORRUPTED (Fixes C8CR3-009)

### Problem in Rev3

CORRUPTED was a RAM state. If device rebooted before operator recovery, the slot would be re-evaluated from NVS (both copies invalid → re-detected as CORRUPTED). But the design didn't formalize this.

### Rev4 Formalization

```
CORRUPTED is NOT a stored state in the record.
CORRUPTED is an INTERPRETATION of evidence:
    Both copies INVALID → slot is CORRUPTED
    Same-generation divergent → slot is CORRUPTED
    Ambiguous generation → slot is CORRUPTED

On every boot:
    _loadFromNVS() re-evaluates each slot using the recovery decision table (§5).
    CORRUPTED is re-derived from the evidence (invalid/divergent copies).
    It is NOT read from a stored field.

This means:
    - CORRUPTED cannot be "written" to a copy.
    - CORRUPTED is the absence of valid consistent data.
    - Operator recovery (recoverCorruptedEntry) writes EMPTY to both copies.
    - After recovery: slot has valid EMPTY records → no longer CORRUPTED.

CONTRACT:
    CORRUPTED is a derived state, not a stored state.
    The journal never writes "CORRUPTED" as a recordState value.
    CORRUPTED is the conclusion when no valid consistent record exists.
```

### recoverCorruptedEntry() Semantics (Revised)

```
recoverCorruptedEntry(requestId):
    1. Find slot with matching requestId (or CORRUPTED_SLOT_N placeholder)
    2. Read both copies (best-effort extract metadata)
    3. Write forensic record to /journal_audit.log (I4)
    4. Verify forensic record
    5. Write EMPTY to copy A (generation = max readable generation + 1, or 0 if unreadable)
    6. Verify copy A
    7. Write EMPTY to copy B (generation = same as A)
    8. Verify copy B
    9. If all verifies pass → slot is now EMPTY (no longer CORRUPTED)
    10. If any verify fails → slot remains CORRUPTED (forensic record is safe)

Note: recoverCorruptedEntry does NOT write "CORRUPTED" anywhere.
      It writes EMPTY (a valid state) to replace the corrupted data.
      The slot transitions from "no valid data" to "valid EMPTY data".
```

---

## 11. NVS Contract Binding (Fixes C8CR3 note about NVS version)

### Explicit NVS Version Binding

```
This design is bound to:
    ESP-IDF NVS implementation (nvs_flash component)
    As provided with arduino-esp32 core version 2.x.x (ESP-IDF 4.x)
    OR arduino-esp32 core version 3.x.x (ESP-IDF 5.x)

NVS behavior assumed:
    - Key-value store with CRC32 per entry
    - Internal page-level journaling (4KB pages)
    - Power-loss: existing data preserved, new write may be lost
    - Blob support via multi-entry storage

NVS limitations acknowledged:
    - Optimized for small key-value pairs (not large blobs)
    - Espressif recommends LittleFS/SPIFFS for frequent large updates
    - Internal wear-leveling is opaque (implementation detail)
    - Page-level corruption affects all keys on that page

If ESP-IDF version changes:
    - NVS behavior may change
    - This design's assumptions must be re-validated
    - Particularly: power-loss protection and blob storage behavior

DOCUMENTATION:
    README must state:
    "This firmware uses ESP-IDF NVS for transaction journal storage.
     NVS is designed for small key-value pairs; large blobs (1.2KB) are
     outside the recommended use case. Wear-leveling is opaque.
     For production deployment, monitor flash wear and consider migrating
     journal to LittleFS if NVS wear becomes excessive."
```

---

## 12. Revised API (Unchanged from Rev3, with invariant names updated)

### Public API (same functions, now with I1a-I1f enforcement)

All mutation functions enforce I1 (composite: I1a through I1f) before proceeding.

### Internal (Private) — New Equivalence Checker

```cpp
// Canonical equivalence check (I1e enforcement)
bool _canonicallyEquivalent(const JournalRecord& A, const JournalRecord& B);

// Full I1 check (I1a through I1f)
bool _checkI1Satisfied(slotIdx);

// Recovery decision (uses decision table §5)
enum RecoveryDecision {
    RECOVERY_CORRUPTED,
    RECOVERY_REPAIR_A,    // A valid, B invalid → copy A to B
    RECOVERY_REPAIR_B,    // B valid, A invalid → copy B to A
    RECOVERY_NEW_A,       // A is newer
    RECOVERY_NEW_B,       // B is newer
    RECOVERY_VALID,       // Both valid, equal, identical
};
RecoveryDecision _evaluateSlot(slotIdx, JournalRecord& outRecord);
```

---

## 13. Crash Matrix (Unchanged from Rev3, but now with I1a-I1f)

The crash matrix from Rev3 (§11) is still valid. The key addition is that **before any mutation**, I1a-I1f are checked. If any sub-invariant fails, mutation is blocked and recovery is attempted.

### Mutation Protocol (Revised with I1a-I1f)

```
1. _checkI1Satisfied(slotIdx)
   - Read both copies
   - Check I1a (A structurally valid)
   - Check I1b (B structurally valid)
   - Check I1c (mutual consistency)
   - Check I1d (no generation ambiguity)
   - Check I1e (same-gen payload equality)
   - Check I1f (diff-gen strict ordering)
   
2. If I1 NOT satisfied:
   - Evaluate using _evaluateSlot() (decision table §5)
   - If REPAIR_A or REPAIR_B → attempt repair
   - If repair fails → CORRUPTED, abort mutation
   - If RECOVERY_CORRUPTED → abort, operator recovery needed
   - If RECOVERY_NEW_A or NEW_B → I1 should pass (re-check)
   
3. If I1 satisfied:
   - Identify active copy (higher generation)
   - Write to inactive copy (COW)
   - Verify (re-read + CRC) — I3 enforcement
   - If verify fails → abort
   - If verify passes → mutation complete
```

---

## 14. Honest Limitations (Unchanged + Refined)

1. **Snapshot reflects safe-OFF, not pre-crash state** — hardware limitation
2. **GPIO output ≠ physical relay contact** — welded/stuck undetectable
3. **Dual-copy is LOGICAL redundancy, NOT physical independence**
4. **CRC32 protects against accident, NOT malicious modification**
5. **NVS endurance is theoretical** — actual lifetime must be tested experimentally
6. **ACK queue is single-copy NVS** — relies on NVS internal power-loss protection
7. **Forensic log is LittleFS single-copy** — relies on LittleFS power-loss protection
8. **ACK_PWA_RECEIVED is NOT implemented** — eviction carries delivery risk
9. **Hardware power-loss testing NOT RUN**

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

## 16. Implementation Plan (After Auditor Approval — NOT YET STARTED)

### Phase 1: Core Data Structure
1. Define `JournalRecord` struct
2. Implement serialize/deserialize with CRC
3. Implement canonical payload extraction
4. Implement `_canonicallyEquivalent()` (I1e)
5. Implement generation algorithm (`_isNewer()`, `_isAmbiguous()`)

### Phase 2: Dual-Copy Operations + I1 Enforcement
6. Implement `_readCopy()`, `_writeCopy()` (with verify)
7. Implement `_evaluateSlot()` (decision table §5)
8. Implement `_checkI1Satisfied()` (I1a-I1f)
9. Implement `_repairSlot()` (REPAIR_A, REPAIR_B cases)

### Phase 3: State Machine
10. Implement `storeIntent()`, `markExecuting()`, `commitTransaction()`
11. Implement `commitTransactionFailed()`
12. Implement `reconcilePendingEntries()`, `reconcileEntry()`
13. Implement `clearEntry()` (write EMPTY, COW)
14. Implement `recoverCorruptedEntry()` (with forensic record — I4)

### Phase 4: Boot Sequence
15. Implement `_loadFromNVS()` with dual-copy recovery + I1 enforcement
16. Implement `captureOutputSnapshot()`
17. Update `firmware_v4.ino` boot sequence

### Phase 5: ACK Durability
18. Implement ACK queue persistence (NVS `tj_ackq`)
19. Implement ACK queue rebuild from journal on boot
20. Implement eviction pre-condition (I5, with honest delivery risk)

### Phase 6: Forensic Log
21. Implement forensic log (LittleFS `/journal_audit.log`)
22. Implement forensic record write + verify

### Phase 7: Integration
23. Update `MqttClient.cpp` for new API
24. Update `RelayEngine.cpp` boot phase guard
25. Update partition table (128 KB NVS for 32 slots)

### Phase 8: Testing
26. Hardware power-loss injection (every crash point)
27. Divergence injection (write different payloads to A and B with same generation)
28. Generation wrap testing
29. Dual-copy repair testing
30. Forensic log verification
31. ACK queue loss + rebuild testing

---

## 17. Auditor Approval Gate

**This design is NOT approved for implementation.**

Auditor must review:
1. Canonical Record Equivalence (§4) — is the definition complete?
2. Full Recovery Decision Table (§5) — are all cases covered?
3. I1a-I1f (§6) — are sub-invariants complete?
4. I5 revision (§6) — is ACK_PUBLISH_ACCEPTED ≠ PWA_RECEIVED clear?
5. ACK queue durability model (§7) — is the NVS contract binding explicit?
6. Forensic log durability (§8) — is LittleFS contract honest?
7. Wear calculation (§9) — is "383 years" removed?
8. CORRUPTED semantics (§10) — is "derived state, not stored" clear?
9. NVS version binding (§11) — is the contract pinned to specific ESP-IDF version?

**After auditor approval, implementation Phase 1-8 may begin. Until then: NO CODE.**

Precharge: BLOCKED
I/O expander: BLOCKED
16-relay: BLOCKED
