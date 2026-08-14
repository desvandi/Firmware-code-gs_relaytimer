# CYCLE-8C-Rev7: Transaction Journal v4 — Formal Completeness

**Status**: DESIGN ONLY — NO CODE IMPLEMENTATION
**Purpose**: Address additional Rev5 audit findings not covered by Rev6
**Base**: Builds on Rev6 (commit 11e25b5) which fixed C8CR5-001 (canonical serialization)
**Rule**: No new features, no new metadata. Formalization only.

---

## 1. Context

Rev6 (commit `11e25b5`) addressed:
- C8CR5-001: Single canonical serialization (recordState moved to payload)
- C8BR5-002 (Rev6 numbering): ACK queue record format + durable delivery state
- C8CR5-005: EPOCH_RESET removed (SLOT_RESET by operator)
- C8CR5-006: Eviction matrix (command class × ACK state)
- C8CR5-007: ACK merge recovery
- C8CR5-008: EVICTABLE derived

This Rev7 document addresses ADDITIONAL findings from the Rev5 audit that Rev6 did not fully cover:
- C8CR5-002 (this audit): canonical LENGTH comparison missing
- C8CR5-003 (this audit): I0 enforcement needs machine-checkable assertion
- C8CR5-004 (this audit): ACK_PWA_RECEIVED protocol undefined
- C8CR5-005 (this audit): ACK loss terminology
- C8CR5-006 (this audit): construction vs observation invariant
- C8CR5-007 (this audit): EPOCH breaks global monotonicity

---

## 2. Fix #1: Formal canonicalEqual(A, B) Predicate (C8CR5-001 + C8CR5-002)

### Problem

Rev6 fixed the canonical serialization contradiction (recordState now in payload). But the formal equality predicate was still underspecified:
- Did not explicitly require length comparison before memcmp
- If A.length=100 and B.length=110 but first 100 bytes match → memcmp says "equal" (WRONG)

### Solution: Formal Predicate

```
canonicalEqual(A, B) ≡
    A.canonicalLength == B.canonicalLength
    AND
    memcmp(A.canonicalBytes, B.canonicalBytes, A.canonicalLength) == 0

WHERE:
    canonicalBytes = the byte sequence starting at recordState (byte 11 in record blob)
    canonicalLength = the actual number of bytes from recordState to end of payload
                     (NOT the padded BLOB_SIZE — the actual payload length)

RECORD LAYOUT (from Rev6, confirmed):
    [0..1]   magic (0x54, 0x4A)
    [2]      schemaVersion (4)
    [3..6]   generation (uint32 LE)
    [7..10]  recordCRC (CRC32 over bytes 0..2 + bytes 11..end)
    [11]     recordState          ← CANONICAL PAYLOAD STARTS HERE
    [12]     requestIdLen
    [13..]   requestId
    [..]     commandHashLen
    [..]     commandHash
    [..]     channelId
    [..]     desiredState
    [..]     previousKnownState
    [..]     attempt
    [..]     timestamp (4 bytes)
    [..]     ackLen (2 bytes)
    [..]     ackJson
    [..]     padding (zeros to BLOB_SIZE)

CANONICAL PAYLOAD:
    canonicalBytes = &blob[11]
    canonicalLength = actualPayloadEnd - 11
    (where actualPayloadEnd = offset after ackJson, before padding)

CRC COVERAGE:
    CRC32(bytes 0..6)       // magic + version + generation
    XOR
    CRC32(bytes 11..actualPayloadEnd)  // canonical payload

NOTE: Padding bytes (between actualPayloadEnd and BLOB_SIZE) are NOT part of
canonical payload and are NOT compared. Only actual payload bytes are compared.
```

### Why Length Check Is Necessary

```
Without length check:
    A: [EXECUTING] [reqId=X] [hash=H] ... [ackJson="short"]
    B: [EXECUTING] [reqId=X] [hash=H] ... [ackJson="longer string"]

    memcmp(A+11, B+11, len(A_payload)) → matches first N bytes → "equal" (WRONG!)

With length check:
    A.canonicalLength = 50 (short ackJson)
    B.canonicalLength = 60 (longer ackJson)
    50 != 60 → NOT equal → CORRUPTED (if same generation)
```

---

## 3. Fix #2: I0 Machine-Checkable Enforcement (C8CR5-003)

### Problem

Rev6 stated "single-threaded, no pseudo-mutex" but did not define machine-checkable enforcement. It's an architecture assumption, not an enforced invariant.

### Solution: Debug Assertion

```
I0 — SINGLE-THREADED ARCHITECTURE (Machine-Checkable)

INVARIANT:
    All TransactionJournal API calls execute on the journal executor context.
    The journal executor context is defined as: loop() on core 1.

ENFORCEMENT (debug builds only):
    #define JOURNAL_EXECUTOR_CORE 1
    
    void TransactionJournal::_assertExecutorContext() {
    #ifdef DEBUG
        BaseType_t core = xPortGetCoreID();
        if (core != JOURNAL_EXECUTOR_CORE) {
            Serial.printf("[Journal] FATAL: I0 violation — called from core %d (expected %d)\n",
                          core, JOURNAL_EXECUTOR_CORE);
            panic("I0 invariant violated: journal called from wrong context");
        }
    #endif
    }
    
    Every public API function calls _assertExecutorContext() at entry.
    In release builds: assertion is compiled out (no overhead).
    In debug builds: assertion triggers panic if called from wrong context.

PROHIBITED (architecture contract):
    - ISR calling journal API → would trigger assertion in debug
    - FreeRTOS task calling journal API → would trigger assertion in debug
    - MQTT callback on different core → would trigger assertion in debug

VERIFICATION:
    During testing phase: run with DEBUG builds to verify no I0 violations.
    If assertion triggers: architecture defect, STOP and fix.

PRODUCTION:
    Release builds compile out the assertion (zero overhead).
    But: the architecture contract still holds (verified during testing).
    If future code changes introduce concurrent access:
        Debug build will catch it during testing.
        Release build will NOT catch it (but testing should have).
```

---

## 4. Fix #3: ACK_PWA_RECEIVED Protocol Definition (C8CR5-004)

### Problem

Rev6 acknowledged PWA_RECEIVED is not implemented but did not define what the protocol WOULD be when implemented. Without protocol definition, the state is unverifiable.

### Solution: Protocol Specification (For Future Implementation)

```
ACK_PWA_RECEIVED PROTOCOL (defined now, implemented in future cycle)

MECHANISM:
    PWA sends an application-level acknowledgement to the device via MQTT:
    
    Topic: timer12/<mac>/ack_confirm
    Payload: {
        "requestId": "<UUID>",
        "commandHash": "<hash>",
        "ackDigest": "<sha256(ackJson) first 16 hex chars>"
    }

VERIFICATION:
    Device receives ack_confirm message.
    Device looks up requestId in ACK queue (tj_ackq).
    Device verifies:
        1. requestId matches
        2. commandHash matches journal entry
        3. ackDigest == sha256(ackJson).substring(0, 16)
    If ALL match → set deliveryState = ACK_PWA_RECEIVED, persist tj_ackq.
    If ANY mismatch → ignore (possible replay/forgery), log warning.

BINDING:
    ackDigest binds the confirmation to the SPECIFIC ACK content.
    Prevents false "received" claims without proof of ACK content.

NOT IMPLEMENTED IN REV7:
    This protocol is DEFINED but NOT IMPLEMENTED.
    Implementation deferred to future cycle (after journal v4 is proven).
    Until implemented: ACK_PWA_RECEIVED never transitions to true.
    Eviction for non-idempotent commands remains BLOCKED.
```

---

## 5. Fix #4: ACK Durability Terminology (C8CR5-005)

### Problem

Rev6 called ACK queue "durable" but accepted that if both journal AND ACK queue are lost, ACK cannot be reconstructed. This is not full durability.

### Solution: Honest Terminology

```
ACK QUEUE DURABILITY CONTRACT:

The ACK queue (tj_ackq) provides:
    "best-effort durable delivery assistance"

NOT:
    "durable ACK delivery"

MEANING:
    1. ACK queue is persisted to NVS (survives reboot under normal conditions)
    2. ACK queue can be rebuilt from journal COMMITTED entries (if tj_ackq lost)
    3. If BOTH journal entry AND ACK queue are lost:
       - ACK is IRRETRIEVABLY LOST
       - PWA must re-query /status to learn transaction result
       - Physical state is preserved (relay state is queryable)
    
    This is ACCEPTED for idempotent commands (relay ON/OFF).
    This is NOT ACCEPTED for non-idempotent commands (precharge, OTA).
    
    For non-idempotent commands:
        ACK queue alone is insufficient.
        Additional measures needed (future cycle):
        - Application-level PWA confirmation (Fix #3)
        - OR: Never evict non-idempotent transaction records
        - OR: Separate durable notification log

CONTRACT:
    Idempotent commands:
        ACK delivery = best-effort (may be lost, PWA can re-query)
        Eviction = allowed (if ACK in durable queue + publish accepted)
    
    Non-idempotent commands:
        ACK delivery = MUST be confirmed (PWA_RECEIVED or equivalent)
        Eviction = BLOCKED until confirmed
        (Currently: non-idempotent commands are never evicted — retained forever
         or until operator clears)
```

---

## 6. Fix #5: Construction vs Observation Invariant (C8CR5-006)

### Problem

Rev6 stated generation distance ≤ 1 as both design proof and runtime check, without distinguishing the two roles.

### Solution: Formal Distinction

```
CONSTRUCTION INVARIANT (protocol obligation):
    The write protocol (storeIntent → markExecuting → commit) is DESIGNED
    to produce generation distance ≤ 1 between copies A and B.
    
    This is a property of the PROTOCOL, verified by design review.
    The protocol writes alternately: A, then B, then A, then B.
    Each write increments generation by 1.
    Therefore: max(genA, genB) - min(genA, genB) ≤ 1.
    
    Repair writes SAME generation (bitwise copy). Distance = 0.
    Recovery (SLOT_RESET) writes generation=0 to both. Distance = 0.

OBSERVATION INVARIANT (runtime validation):
    At runtime, when reading both copies, the journal VALIDATES that
    generation distance ≤ 1.
    
    This is a DEFENSE-IN-DEPTH check. If it triggers:
    - The construction invariant was violated (bug or corruption)
    - The slot is marked CORRUPTED
    - The violation is logged for diagnosis
    
    The observation invariant does NOT assume the construction invariant.
    It independently verifies it.

FORMAL STATEMENT:
    Construction: "The write protocol SHALL produce generation distance ≤ 1."
    Observation:  "The loader SHALL validate generation distance ≤ 1 and
                   mark CORRUPTED if violated."
    
    Both are required. Construction prevents the condition.
    Observation catches it if construction fails.
```

---

## 7. Fix #6: Generation Monotonicity Terminology (C8CR5-007)

### Problem

Rev6 used SLOT_RESET (generation=0) which breaks global monotonicity. The terminology was not clear about what "reset" means for ordering.

### Solution: Epoch Terminology

```
GENERATION MONOTONICITY:

Generation is monotonic WITHIN AN EPOCH.

Epoch:
    A logical period of generation counting.
    Each slot has its own epoch (independent).
    
    Normal operation:
        Epoch 0: generation 0, 1, 2, 3, ..., N
        (monotonically increasing)
    
    After SLOT_RESET (operator recovery):
        Epoch 1: generation 0, 1, 2, 3, ...
        (restarts from 0, but epoch has changed)
    
    The epoch ID is NOT stored as a separate field (no new metadata).
    Instead, the forensic log records: "SLOT_RESET at <timestamp>, slot <N>,
    previous generation unknown, new epoch started."
    
    This means:
    - Generation 0 from epoch 0 is DIFFERENT from generation 0 from epoch 1.
    - But: the recovery selector only compares generations within the same epoch
      (both copies are from the same epoch, because SLOT_RESET writes to both).
    - Cross-epoch comparison never happens (SLOT_RESET resets both copies simultaneously).

TERMINOLOGY:
    "generation monotonic within an epoch"
    NOT: "generation is globally monotonic"
    
    The forensic log provides the epoch boundary record for audit purposes.
    The journal itself does not need epochId field (no new metadata).

WHY THIS IS SAFE:
    - SLOT_RESET is operator-initiated only.
    - Both copies are written with same generation (0) during reset.
    - All subsequent writes to this slot continue from 0+1=1.
    - Other slots are unaffected (each has independent epoch).
    - The forensic log preserves the fact that a reset occurred.
    - Cross-epoch comparison is impossible by construction (both copies always
      in same epoch after reset).
```

---

## 8. Consolidated Invariant Summary (I0-I3, Final)

### I0 — Single-Threaded Architecture (Machine-Checkable)
```
All journal API calls execute on loop() context (core 1).
Debug assertion: assert(xPortGetCoreID() == JOURNAL_EXECUTOR_CORE).
No ISR, no FreeRTOS task, no concurrent access.
If architecture changes: STOP, re-audit.
```

### I1 — Canonical Equivalence + Recovery
```
I1a: Copy A structurally valid (CRC passes, magic correct, version correct)
I1b: Copy B structurally valid (CRC passes, magic correct, version correct)
I1c: Mutual consistency:
       - If genA == genB: canonicalEqual(A, B) must be true
       - If genA != genB: exactly one is GEN_NEWER (not GEN_AMBIGUOUS)
I1d: No generation ambiguity (genA - genB != 0x80000000)
I1e: canonicalEqual(A, B) = (canonicalLen(A) == canonicalLen(B))
                            AND memcmp(canonicalBytes(A), canonicalBytes(B),
                                       canonicalLen(A)) == 0
     WHERE canonicalBytes starts at recordState (byte 11), includes ALL payload fields
I1f: Diff-gen strict ordering (serial-number arithmetic: isNewer(a,b) = (int32_t)(a-b) > 0)
I1g: Generation distance ≤ 1 (OBSERVATION INVARIANT, runtime asserted)
     Construction invariant: write protocol produces ≤ 1
     Observation invariant: loader validates ≤ 1, marks CORRUPTED if violated
```

### I2 — Eviction Safety
```
I2a: Retention policy permits (journal full, slot needed)
I2b: Command class is IDEMPOTENT or NON_IDEMPOTENT (not UNKNOWN)
I2c: ACK condition met (eviction matrix §7 of Rev6):
     - IDEMPOTENT + PUBLISH_ACCEPTED + durable queue → YES
     - NON_IDEMPOTENT + PUBLISH_ACCEPTED → NO
     - NON_IDEMPOTENT + BROKER_CONFIRMED → YES
     - UNKNOWN → NEVER
I2d: No unresolved recovery (not CORRUPTED)
I2e: Default = RETAIN (uncertain → no eviction)
EVICTABLE is COMPUTED from I2a-I2e, never stored.
```

### I3 — ACK Lifecycle Separation
```
I3a: Transaction lifecycle independent of ACK lifecycle
I3b: ACK queue persists independently (tj_ackq with durable deliveryState)
I3c: Eviction does NOT delete ACK queue entry
I3d: Boot recovery = MERGE journal + ACK queue (not journal-only)
ACK durability = "best-effort durable delivery assistance"
    (not full durability — if both journal and queue lost, ACK is irretrievable)
ACK_PWA_RECEIVED protocol defined (§4 above) but NOT IMPLEMENTED.
```

---

## 9. Rule Compliance

**Auditor rule**: "Rev7 tidak boleh menambah metadata baru kecuali diperlukan oleh invariant yang sudah diformalkan."

| Item | New Metadata? | Required By |
|------|--------------|-------------|
| canonicalEqual predicate | NO (formalization of existing I1e) | I1 |
| I0 debug assertion | NO (debug-only code, no storage) | I0 |
| ACK_PWA_RECEIVED protocol | NO (definition only, not implemented) | I3 (future) |
| ACK durability terminology | NO (documentation change) | I3 |
| Construction vs observation | NO (formalization of existing I1g) | I1 |
| Epoch terminology | NO (terminology, no new field) | Recovery |

**No new metadata added.** All changes are formalization of existing invariants.

---

## 10. Honest Limitations (Unchanged)

1. Snapshot reflects safe-OFF, not pre-crash state — hardware limitation
2. GPIO output ≠ physical relay contact — welded/stuck undetectable
3. Dual-copy is LOGICAL redundancy, NOT physical independence
4. CRC32 protects against accident, NOT malicious modification
5. NVS endurance is theoretical — must test empirically
6. ACK durability = "best-effort delivery assistance" (not full durability)
7. ACK_PWA_RECEIVED is DEFINED but NOT IMPLEMENTED
8. Hardware power-loss testing NOT RUN
9. fsync semantics must be verified at implementation time
10. Partition size must be verified empirically

---

## 11. What This Design Does NOT Solve

- Pre-crash GPIO state recovery (hardware revision needed)
- Physical relay contact verification (feedback hardware needed)
- Physical flash independence (separate flash chips needed)
- Tamper protection (Flash Encryption + Secure Boot needed)
- Application-level ACK confirmation implementation (protocol defined, not coded)
- Multi-output transaction model (precharge) — separate cycle needed
- 16-relay / I/O expander — separate cycle needed
- F-008 PWA credential architecture — separate cycle needed

---

## 12. Auditor Approval Gate

**This design is NOT approved for implementation.**

Auditor must verify:
1. canonicalEqual (§2): Is the formal predicate correct? Length check included?
2. I0 enforcement (§3): Is debug assertion acceptable? Machine-checkable?
3. ACK_PWA_RECEIVED (§4): Is protocol definition sufficient? ackDigest binding correct?
4. ACK terminology (§5): Is "best-effort delivery assistance" honest?
5. Construction vs observation (§6): Is the distinction clear?
6. Epoch terminology (§7): Is "monotonic within epoch" correct? No cross-epoch comparison?
7. No new metadata (§9): Rule compliance verified?

**After auditor approval, implementation Phase 1-8 (from Rev6) may begin. Until then: NO CODE.**

Precharge: BLOCKED
I/O expander: BLOCKED
16-relay: BLOCKED
