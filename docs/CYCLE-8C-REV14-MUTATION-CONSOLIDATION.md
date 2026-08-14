<!-- SUPERSEDED BANNER -->
<!-- ╔══════════════════════════════════════════════════════════╗ -->
<!-- ║  ⚠️  SUPERSEDED — HISTORICAL DOCUMENT — DO NOT IMPLEMENT  ║ -->
<!-- ║  This document has been superseded by Rev15 for 4 specific fixes.  ║ -->
<!-- ║  Refer to:                                                ║ -->
<!-- ║    - CYCLE-8C-REV15-ACK-TRANSITION.md                      ║ -->
<!-- ║  for the authoritative supplement.                          ║ -->
<!-- ║  Rev14 remains base for topics not changed by Rev15.        ║ -->
<!-- ╚══════════════════════════════════════════════════════════╝ -->
<!-- END SUPERSEDED BANNER -->


# CYCLE-8C-Rev14: Transaction Journal v4 — Mutation Enforcement & Full Consolidation

**Status**: DESIGN ONLY — NO CODE IMPLEMENTATION
**Purpose**: Final consolidation. Rev14 is the SINGLE normative document. All previous docs SUPERSEDED.
**Auditor instruction**: "Cukup: formalize mutation forbidden, consolidate Rev10, fix terminology, normalize classifier, clean stale text."

---

## 1. Summary

| Fix | Finding | Severity | What Changed |
|-----|---------|----------|-------------|
| #1 | C8CR13-001: Mutation not forbidden during observation | P0 | `_assertMutationAllowed()` at every mutation entry point |
| #2 | C8CR13-002: Rev10 still authoritative for non-recovery | P1 | ALL definitions consolidated INTO Rev14. Rev10 → SUPERSEDED. |
| #3 | C8CR13-003: Runtime check described as compile-time | P1 | "Runtime enforcement via panic(), NOT compile-time, NOT assert()" |
| #4 | Generation classifier ordering not normative | P2 | Formal if-else chain specified |
| #5 | Stale text (Rev12 authority, init=0 claim) | P2 | Cleaned |

**No new fields. No new features. No architecture changes. No code.**

---

## 2. Fix #1: Mutation Forbidden During Observation (P0 — C8CR13-001)

### Problem

Rev13's `ObservationGuard` sets `_observing=true` during observation and asserts non-nesting. But mutation API functions do NOT check `_observing`. A mutation could execute between `read A` and `read B`, changing B → unstable observation.

### Solution: Mutation Assertion

```
I0a — STABLE OBSERVATION (REV14 — SOLE AUTHORITY):

INVARIANT:
    While _observing == true:
        NO journal mutation SHALL execute.

ENFORCEMENT:

    Every journal mutation API function MUST call _assertMutationAllowed()
    at entry, BEFORE any read or write operation.

    void _assertMutationAllowed() {
        if (_observing) {
            panic("I0a: mutation attempted during observation");
        }
    }

    Mutation API functions (all MUST call _assertMutationAllowed):
        storeIntent()
        markExecuting()
        commitTransaction()
        commitTransactionFailed()
        clearEntry()
        recoverCorruptedEntry()
        _repairSlot()
        _writeCopy()
        _eraseBlobNVS()
        _clearSlotNVS()

    Observation functions (all use ObservationGuard):
        _checkI1Satisfied()
        _evaluateSlot()
        reconcilePendingEntries()
        _loadFromNVS() (read phase only)
        _readCopy() (when called for evaluation, not mutation)

OBSERVATIONGUARD (CONSOLIDATED — REV14):

    class ObservationGuard {
        bool& _flag;
    public:
        ObservationGuard(bool& flag) : _flag(flag) {
            // RUNTIME check (NOT compile-time):
            // Panics if nested observation is attempted.
            if (_flag) {
                panic("I0a: nested observation detected (depth > 1)");
            }
            _flag = true;
        }
        ~ObservationGuard() {
            _flag = false;
        }
    };

    // Mutation assertion (RUNTIME, NOT compile-time):
    void _assertMutationAllowed() {
        if (_observing) {
            panic("I0a: mutation attempted during active observation");
        }
    }

COMPLETE CONTRACT:
    I0:  Journal API only from executor task (TaskHandle check, runtime).
    I0a: Observation and mutation are MUTUALLY EXCLUSIVE (runtime enforced).
         - ObservationGuard prevents nested observation (constructor panic).
         - _assertMutationAllowed() prevents mutation during observation.
         - Together: read A + read B + compare is STABLE (no mutation can interleave).

TERMINOLOGY (CORRECTED — C8CR13-003):
    - "Runtime enforcement" (NOT "compile-time check")
    - "panic()" (NOT "assert()" — assert() can be compiled out with NDEBUG)
    - Constructor check is RUNTIME: the value of _flag is evaluated at program execution time.
    - _assertMutationAllowed() is RUNTIME: same reason.
    - Neither check can be done at compile time (the value depends on execution state).

NO NEW METADATA:
    _observing is RAM-only boolean.
    _assertMutationAllowed is a function call (no storage).
    No new NVS keys, no new record fields.
```

---

## 3. Fix #2: Full Consolidation — Rev14 is SINGLE Normative Document (P1 — C8CR13-002)

### Problem

Rev13 claimed "sole normative" but topic table pointed to Rev10 for CRC, I0, I1, I2, I3, etc. Two models of authority coexisted.

### Solution: Consolidate ALL Definitions INTO Rev14

Rev14 now contains ALL normative definitions. Rev10 is fully SUPERSEDED.

Below is the complete consolidated design, organized by invariant.

---

## 4. CONSOLIDATED INVARIANTS (I0-I3) — REV14 SOLE AUTHORITY

### I0 — Journal Executor-Ownership

```
I0: Journal API calls execute ONLY in the journal executor context.

DEFINITION:
    The journal executor context is identified by FreeRTOS TaskHandle.
    During setup(), the executor registers its TaskHandle:
        s_journalExecutorTask = xTaskGetCurrentTaskHandle();

ENFORCEMENT (RUNTIME, NOT COMPILED OUT):
    void _assertExecutorContext() {
        if (s_journalExecutorTask == nullptr) {
            panic("I0: executor not registered");
        }
        if (xTaskGetCurrentTaskHandle() != s_journalExecutorTask) {
            panic("I0: journal API called from non-executor context");
        }
    }

    Every public API function calls _assertExecutorContext() at entry.
    This check is NOT compiled out in release builds (overhead: ~10 cycles).

ARCHITECTURE CONTRACT:
    - loop() is the journal executor (Arduino creates "loopTask")
    - MQTT callback (PubSubClient) runs in loop() context → OK
    - PIR ISR does NOT call journal → OK (only sets flag)
    - WiFi events do NOT call journal → OK

    If future architecture adds a FreeRTOS task that calls journal:
        STOP. I0 is violated. Architecture revision required. Re-audit.
```

### I0a — Stable Observation (mutation forbidden)

```
I0a: While _observing == true, NO mutation SHALL execute.

ENFORCEMENT:
    ObservationGuard (RAII): sets _observing=true, destructor resets to false.
    Constructor panics if _observing already true (prevents nesting).
    _assertMutationAllowed(): called at entry of every mutation function.
    Panics if _observing is true.

    Together: observation and mutation are mutually exclusive (runtime enforced).
```

### I1 — Canonical Equivalence + Recovery

```
RECORD LAYOUT (NORMATIVE — REV14):

    Offset  Field              Size
    ------  ----------------   ----
    0       magic              2     0x54, 0x4A ("TJ")
    2       schemaVersion      1     4
    3       generation         4     uint32 LE
    7       recordCRC          4     CRC-32/ISO-HDLC over bytes 0..2 + bytes 11..end
    --- CANONICAL PAYLOAD (byte 11 onward) ---
    11      recordState        1
    12      requestIdLen       1     0..64
    13..   requestId          var
    ..      commandHashLen     1     0..64
    ..      commandHash        var
    ..      channelId          1     0=N/A, 1..NUM_CHANNELS
    ..      desiredState       1     0=OFF, 1=ON, 0xFF=N/A
    ..      previousKnownState 1     0=OFF, 1=ON
    ..      attempt            1
    ..      timestamp          4     uint32 LE
    ..      ackLen             2     uint16 LE, 0..1024
    ..      ackJson            var
    ..      (padding to 1200 bytes, zeros, NO semantic meaning)

CRC (NORMATIVE — REV14):

    Algorithm: CRC-32/ISO-HDLC
    Polynomial: 0x04C11DB7
    Init: 0xFFFFFFFF
    Final XOR: 0xFFFFFFFF
    Reflected: true (input and output)
    Test vector: "123456789" → 0xCBF43926 (mathematically verified, target-API verification required during Phase 1)

    API:
        // Direct (single buffer):
        uint32_t crc = ~esp_crc32_le(0xFFFFFFFF, data, len) & 0xFFFFFFFF;

        // Continuation (two buffers = concatenation):
        uint32_t state = esp_crc32_le(0xFFFFFFFF, bufA, lenA);
        state = esp_crc32_le(state, bufB, lenB);
        uint32_t crc = ~state & 0xFFFFFFFF;

    CRC INPUT: bytes[0..6] (header) concatenated with bytes[11..actualPayloadEnd] (canonical payload)
    CRC does NOT cover: bytes[7..10] (CRC field), padding bytes.

SAFE RECORD PARSING (NORMATIVE — REV14):

    parseRecord(blob, blobLen) → PARSE_VALID or PARSE_INVALID.
    Every variable-length field is bounds-checked before advancing cursor.
    canonicalLength is derived ONLY from a successfully validated parse.
    If any bounds check fails → PARSE_INVALID → copy is INVALID.

    Parsing order:
        raw bytes → structural validation → safe parse → canonical representation → canonicalEqual

CANONICAL EQUIVALENCE (NORMATIVE — REV14):

    canonicalEqual(A, B) ≡
        A.schemaVersion == B.schemaVersion
        AND
        A.canonicalLength == B.canonicalLength
        AND
        memcmp(A.canonicalBytes, B.canonicalBytes, A.canonicalLength) == 0

    WHERE:
        canonicalBytes = bytes starting at recordState (byte 11)
        canonicalLength = actual payload length (from safe parse, excluding padding)
        schemaVersion is at byte 2 (in header, checked separately before payload comparison)

    If schemaVersion differs → CORRUPTED (incompatible records).
    If canonicalLength differs → NOT equivalent.
    If memcmp differs → NOT equivalent.

PADDING SEMANTICS:
    Padding has NO semantic meaning.
    NOT compared during canonicalEqual.
    NOT covered by CRC.
    NOT validated by parser (parser stops at actualPayloadEnd).
    May contain arbitrary bytes.

GENERATION ORDERING (NORMATIVE — REV14):

    distAB = (uint32_t)(genB - genA)   // forward distance A→B
    distBA = (uint32_t)(genA - genB)   // forward distance B→A

    CLASSIFIER (normative ordering — check in this exact sequence):

    if genA == genB:
        → GEN_EQUAL
        → Must verify canonicalEqual(A, B). If fails → CORRUPTED.

    else if distAB == 1:
        → GEN_NEWER_B (B is 1 generation newer than A)

    else if distBA == 1:
        → GEN_NEWER_A (A is 1 generation newer than B)

    else if distAB == 0x80000000:
        → GEN_AMBIGUOUS → CORRUPTED

    else:
        → GEN_INVALID → CORRUPTED

    NOTE: distAB == 0x80000000 implies distBA == 0x80000000 (symmetric).
    The classifier checks distAB first for ambiguity.
    If neither direction has distance 0, 1, or 0x80000000, it's GEN_INVALID.

GENERATION DISTANCE (I1g):
    CONSTRUCTION: protocol produces distance 0 (same/repair) or 1 (adjacent mutation).
    OBSERVATION: loader validates distance is 0 or 1, else CORRUPTED.
    These are separate: construction prevents, observation catches.

GENERATION TEST VECTORS (NORMATIVE):

| genA       | genB       | distAB           | distBA           | Result        | Load  |
|------------|------------|-------------------|-------------------|---------------|-------|
| 0          | 0          | 0                 | 0                 | GEN_EQUAL     | check canonicalEqual |
| 0          | 1          | 1                 | 0xFFFFFFFF        | GEN_NEWER_B   | B     |
| 1          | 0          | 0xFFFFFFFF        | 1                 | GEN_NEWER_A   | A     |
| 0          | 0xFFFFFFFF | 0xFFFFFFFF        | 1                 | GEN_NEWER_A   | A     |
| 0xFFFFFFFF | 0          | 1                 | 0xFFFFFFFF        | GEN_NEWER_B   | B     |
| 0          | 5          | 5                 | 0xFFFFFFFB        | GEN_INVALID   | CORRUPTED |
| 5          | 0          | 0xFFFFFFFB        | 5                 | GEN_INVALID   | CORRUPTED |
| 10         | 20         | 10                | 0xFFFFFFF6        | GEN_INVALID   | CORRUPTED |
| 10         | 0x8000000A | 0x80000000        | 0x80000000        | GEN_AMBIGUOUS | CORRUPTED |

EDGE CASES SPECIFIED AND MANUALLY DERIVED.
Implementation MUST reproduce these expected outcomes
with automated unit tests before journal integration.
```

### RECOVERY DECISION TABLE (NORMATIVE — REV14 SOLE AUTHORITY)

```
| # | Copy A   | Copy B   | Gen Relationship              | Action      |
|---|----------|----------|-------------------------------|-------------|
| 1 | INVALID  | INVALID  | N/A                           | QUARANTINED |
| 2 | VALID    | INVALID  | N/A                           | REPAIR A→B |
| 3 | INVALID  | VALID    | N/A                           | REPAIR B→A |
| 4 | VALID    | VALID    | GEN_NEWER_A (distBA == 1)     | Load A      |
| 5 | VALID    | VALID    | GEN_NEWER_B (distAB == 1)     | Load B      |
| 6 | VALID    | VALID    | GEN_EQUAL + canonicalEqual    | Load either |
| 7 | VALID    | VALID    | GEN_EQUAL + divergent         | CORRUPTED   |
| 8 | VALID    | VALID    | GEN_AMBIGUOUS (dist==2^31)    | CORRUPTED   |
| 9 | VALID    | VALID    | GEN_INVALID (distance > 1)    | CORRUPTED   |

WHERE:
    distAB = (uint32_t)(genB - genA)
    distBA = (uint32_t)(genA - genB)
    GEN_NEWER_A = distBA == 1  (A is 1 newer than B)
    GEN_NEWER_B = distAB == 1  (B is 1 newer than A)
    GEN_EQUAL = genA == genB
    GEN_AMBIGUOUS = distAB == 0x80000000
    GEN_INVALID = neither distAB nor distBA is 0, 1, or 0x80000000

NOTES:
    EMPTY is a recordState, NOT a special generation.
    Generation ordering is the SOLE selector.
    No special EMPTY rows in recovery table.

RECOVERY CONTRACT:
    recoverCorruptedEntry():
        PRECONDITION: Copy A == INVALID AND Copy B == INVALID
        Runtime assertion: assert(!validA && !validB) at entry (panic if violated).
        Write EMPTY(gen=0) to Copy A. Verify A.
        Write EMPTY(gen=0) to Copy B. Verify B.
        gen=0 is unconditional. No max(). No successor calculation.

QUARANTINED SEMANTICS:
    QUARANTINED = both copies INVALID (derived state, not stored).
    No auto-reuse. No auto-recovery.
    Slot occupies space (reduces journal capacity).
    Operator must use recoverCorruptedEntry().
    If journal fills with QUARANTINED slots → JOURNAL_FULL → device halts.

EMPTY SEMANTICS:
    EMPTY is a recordState, NOT a special generation.
    gen=0 is NOT inherently "newer" than any other generation.
    No special EMPTY treatment in recovery table.

INTERRUPTED RECOVERY:
    A = VALID EMPTY(gen=0), B = INVALID → case #2 → REPAIR A→B. Safe.
    A = INVALID, B = VALID EMPTY(gen=0) → case #3 → REPAIR B→A. Safe.
    A = VALID EMPTY(gen=0), B = VALID EMPTY(gen=0) → case #6 → slot EMPTY. Safe.
    A = VALID EMPTY(gen=0), B = VALID COMMITTED(gen=5) → distance=5 → case #9 → CORRUPTED. Safe.

REPAIR SEMANTICS:
    Repair is BITWISE RESTORATION, not state transition.
    REPAIR(B) when A=VALID, B=INVALID:
        Read A's full record (including generation).
        Write IDENTICAL record to B (same generation, same payload).
        Verify B (re-read + CRC + byte-compare with A).
    Repair does NOT increment generation.
    Repair does NOT change recordState or any field.
```

### I2 — Eviction Safety

```
I2a: Retention policy permits (journal full, slot needed for new transaction)
I2b: Command class is IDEMPOTENT or NON_IDEMPOTENT (not UNKNOWN)
I2c: ACK condition met (from eviction matrix below)
I2d: No unresolved recovery (slot is not CORRUPTED/QUARANTINED)
I2e: Default = RETAIN (if any check is uncertain → NO eviction)

EVICTION MATRIX (NORMATIVE — REV14):

| Command Class   | ACK State                  | Eviction? |
|-----------------|---------------------------|-----------|
| IDEMPOTENT      | ACK_NOT_SENT               | NO        |
| IDEMPOTENT      | ACK_PUBLISH_ACCEPTED       | YES*      | (*if ACK in durable queue)
| IDEMPOTENT      | ACK_BROKER_CONFIRMED       | YES       |
| IDEMPOTENT      | ACK_PWA_RECEIVED           | YES       |
| IDEMPOTENT      | ACK_FAILED_EXHAUSTED       | YES       | (PWA can re-query /status)
| NON_IDEMPOTENT  | ACK_NOT_SENT               | NO        |
| NON_IDEMPOTENT  | ACK_PUBLISH_ACCEPTED       | NO        |
| NON_IDEMPOTENT  | ACK_BROKER_CONFIRMED       | NO        |
| NON_IDEMPOTENT  | ACK_PWA_RECEIVED           | YES       | (ONLY this)
| NON_IDEMPOTENT  | ACK_FAILED_EXHAUSTED       | NO        | (operator must investigate)
| UNKNOWN         | ANY                        | NO        | (retain until classified)

EVICTABLE is COMPUTED from I2a-I2e, NEVER stored.

COMMAND CLASSIFICATION:
    IDEMPOTENT: relay ON, relay OFF, set_mode, schedule upsert, schedule delete,
                 PIR config, channel rename, time set, config set
    NON_IDEMPOTENT: OTA update, factory reset, future precharge
    UNKNOWN: any command type not yet classified (default: treat as non-idempotent)

EXACTLY-ONCE GUARANTEE:
    Applies ONLY within durable requestId retention window (64 slots, LRU).
    After eviction: requestId is NO LONGER tracked.
    Non-idempotent commands MUST NOT rely on journal retention alone.
```

### I3 — ACK Lifecycle Separation

```
I3a: Transaction lifecycle independent of ACK lifecycle
I3b: ACK queue persists independently (tj_ackq with durable deliveryState)
I3c: Eviction does NOT delete ACK queue entry
I3d: Boot recovery = MERGE journal + ACK queue (not journal-only rebuild)

ACK DELIVERY STATES (DURABLE, stored in tj_ackq):
    ACK_NOT_SENT           = 0
    ACK_PUBLISH_ACCEPTED   = 1
    ACK_BROKER_CONFIRMED  = 2
    ACK_PWA_RECEIVED       = 3
    ACK_FAILED_EXHAUSTED   = 4

ACK QUEUE RECORD FORMAT:
    [ackMagic:2] [ackVersion:1] [deliveryState:1]
    [requestIdLen:1] [requestId:var]
    [commandHashLen:1] [commandHash:var]
    [retryCount:1] [lastAttemptTs:4]
    [ackLen:2] [ackJson:var]
    (padding to 256 bytes per record)

tj_ackq BLOB:
    [count:1] [reserved:3] [AckRecord × 8 = 2048] [queueCRC:4]
    Total: 2056 bytes

ACK DURABILITY:
    "Best-effort durable delivery assistance" (NOT "durable ACK delivery").
    If both journal AND ACK queue are lost: ACK is irretrievable.
    PWA must re-query /status to learn transaction result.
    Accepted for idempotent commands. NOT accepted for non-idempotent.

ACK_PWA_RECEIVED PROTOCOL (DEFINED, NOT IMPLEMENTED):
    PWA sends: {requestId, commandHash, ackDigest} to timer12/<mac>/ack_confirm
    ackDigest = SHA-256(ackJson)[0:16] (first 16 hex chars)
    Device verifies: requestId match + commandHash match + ackDigest == SHA-256(ackJson)[:16]

    ackDigest is CONTENT BINDING, NOT sender authentication.
    Anyone who knows ackJson can compute ackDigest.
    Sender authentication requires MQTT ACL or HMAC (future cycle).

BOOT ACK RECOVERY:
    1. Read tj_ackq from NVS (durable ACK queue with delivery states)
    2. Scan journal for COMMITTED entries with non-empty ackJson
    3. MERGE: keep existing queue entries, add missing from journal
    4. Orphaned ACKs (transaction evicted, ACK not delivered) are RETAINED in queue
    5. Persist merged queue to tj_ackq
```

---

## 5. Physical Failure Model

```
DUAL-COPY PROTECTION SCOPE:

The dual-copy architecture protects against:
    ✅ Torn writes to a single record (CRC detects, other copy recovers)
    ✅ Single-record NVS key corruption (CRC detects, other copy recovers)

The dual-copy architecture does NOT protect against:
    ❌ NVS page failure (both copies may be on same page)
    ❌ NVS partition corruption (entire namespace lost)
    ❌ Flash chip failure (all data lost)

NVS CRASH CONSISTENCY (per Espressif):
    NVS IS designed to be crash-consistent (power-loss resistant).
    NVS is NOT optimized for large blobs (1.2KB is large by NVS standards).
    Espressif recommends LittleFS for frequent large updates.
    Dual-copy is APPLICATION-LEVEL REDUNDANCY, not "fixing unsafe NVS."
    These are COMPLEMENTARY.

CRC32 INTEGRITY BOUNDARY:
    CRC32 protects against ACCIDENTAL corruption (power loss, flash wear).
    CRC32 does NOT protect against MALICIOUS modification.
    For tamper protection: Flash Encryption + Secure Boot (hardware provisioning).
```

---

## 6. NVS Storage Requirements

```
STORAGE (32 slots, dual-copy):
    32 × 2 × 1200 bytes = 76,800 bytes (raw journal data)
    + ACK queue (tj_ackq): ~2KB
    + Other NVS data: ~4KB
    + NVS overhead: ~25%
    = ~108KB minimum

RECOMMENDATION:
    Start with 128KB NVS partition.
    VERIFY during implementation using nvs_get_stats().
    If insufficient: increase to 256KB or reduce to 16 slots.

NVS KEY NAMING:
    tj_ra_N  = 8 chars (record A, slot N) ✅
    tj_rb_N  = 8 chars (record B, slot N) ✅
    tj_ackq  = 7 chars (ACK queue blob) ✅

WEAR ANALYSIS:
    No "383 years" claim. Actual lifetime must be established experimentally.
    Write amplification: 3 COW writes per transaction (storeIntent + markExecuting + commit).
    At 50 transactions/day: ~180KB/day write volume.
    NVS endurance depends on internal wear-leveling (opaque).
    If wear is excessive: migrate to LittleFS in future cycle.
```

---

## 7. Forensic Log

```
STORAGE: LittleFS file: /journal_audit.log

FORENSIC RECORD FORMAT:
    [timestamp:4] [slotIdx:1] [requestIdLen:1] [requestId:var]
    [generationA:4] [generationB:4] [stateA:1] [stateB:1]
    [reason:1] [action:1] [recordCRC:4]

REASONS:
    BOTH_CORRUPT = 1
    OPERATOR_INITIATED = 2
    DIVERGENT_PAYLOAD = 3
    GEN_AMBIGUOUS = 4
    GEN_GAP_EXCEEDED = 5
    SCHEMA_MISMATCH = 6

ACTIONS:
    EMPTIED = 1
    QUARANTINED = 2

DURABILITY:
    LittleFS power-loss protection (best-effort, not dual-copy).
    fsync semantics must be verified during implementation.
    Never auto-deleted. GC is operator-initiated only.
```

---

## 8. Authoritative Document Stack (Final — Rev14)

```
SINGLE NORMATIVE DOCUMENT:

    CYCLE-8C-REV14-MUTATION-CONSOLIDATION.md (THIS DOCUMENT)

This document consolidates ALL authoritative definitions from Rev10, Rev12, and Rev13.
It is the SOLE normative reference for implementation.

ALL PREVIOUS DOCUMENTS ARE SUPERSEDED:
    Rev6, Rev7, Rev8, Rev9, Rev10, Rev11, Rev12, Rev13 → SUPERSEDED

Rev14 supersedes:
    - Rev10 (CRC, I0, I1, I2, I3, canonical, eviction, ACK, NVS model, forensic log)
    - Rev11 (recovery matrix, non-nested observation)
    - Rev12 (recovery contract, test vectors, wording)
    - Rev13 (precedence table, ObservationGuard consolidation, CRC cleanup)

If any previous document conflicts with Rev14: Rev14 WINS.
There is NO other normative document.
```

---

## 9. Rule Compliance

| Item | New Field? | New Feature? |
|------|-----------|-------------|
| _assertMutationAllowed() | NO (function) | NO |
| Consolidated definitions | NO (moved from Rev10) | NO |
| Runtime terminology fix | NO (documentation) | NO |
| Classifier ordering | NO (algorithm) | NO |
| Stale text cleanup | NO (documentation) | NO |

**Zero new metadata. Zero new features. Zero architecture changes.**

---

## 10. Honest Limitations (Unchanged)

1. Snapshot reflects safe-OFF, not pre-crash state — hardware limitation
2. GPIO output ≠ physical relay contact — welded/stuck undetectable
3. Dual-copy is LOGICAL redundancy, NOT physical independence
4. CRC32 protects against accident, NOT malicious modification
5. NVS endurance is theoretical — must test empirically
6. ACK durability = "best-effort delivery assistance"
7. ACK_PWA_RECEIVED is DEFINED but NOT IMPLEMENTED
8. ackDigest is content binding, NOT sender authentication
9. Hardware power-loss testing NOT RUN
10. fsync semantics must be verified at implementation time
11. Partition size must be verified empirically
12. CRC target-API verification required during Phase 1

---

## 11. What This Design Does NOT Solve

- Pre-crash GPIO state recovery (hardware revision)
- Physical relay contact verification (feedback hardware)
- Physical flash independence (separate flash chips)
- Tamper protection (Flash Encryption + Secure Boot)
- ACK_PWA_RECEIVED implementation (protocol defined, not coded)
- Sender authentication for ACK (ACL/HMAC — future)
- Multi-output transaction model (precharge) — separate cycle
- 16-relay / I/O expander — separate cycle
- F-008 PWA credential architecture — separate cycle

---

## 12. Auditor Approval Gate

**This design is NOT approved for implementation.**

Auditor must verify:
1. Mutation assertion (§2): _assertMutationAllowed() at every mutation entry?
2. ObservationGuard (§2): RAII + non-nested + mutation check = stable observation?
3. Full consolidation (§4): ALL definitions in Rev14? Rev10 fully superseded?
4. Runtime terminology (§2): "runtime, not compile-time"? "panic(), not assert()"?
5. Classifier ordering (§4): if-else chain normative?
6. Recovery table (§4): 9 rows, sole authority?
7. Test vectors (§4): 9 rows, all correct?
8. Rule compliance (§9): Zero new metadata, zero new features?
9. Single normative (§8): Rev14 is ONLY document?

**After auditor approval, implementation Phase 1-8 may begin. Until then: NO CODE.**

Precharge: BLOCKED
I/O expander: BLOCKED
16-relay: BLOCKED