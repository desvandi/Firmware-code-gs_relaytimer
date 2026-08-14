# CYCLE-8C-Rev13: Transaction Journal v4 — Precedence Closure

**Status**: DESIGN ONLY — NO CODE IMPLEMENTATION
**Purpose**: Establish topic-level authoritative precedence. No new fields, no features.
**Auditor instruction**: "Tetapkan Rev12 sebagai sole authority untuk seluruh recovery/generation/quarantine semantics."

---

## 1. Summary

| Fix | Finding | What Changed |
|-----|---------|--------------|
| #1 | C8CR12-001: Rev10 recovery still in authoritative scope | Topic-level precedence table: Rev12 sole authority for recovery/generation/quarantine |
| #2 | C8CR12-002: Non-nested attribution wrong | ObservationGuard consolidated in Rev13 (not "via Rev10") |
| #3 | C8CR12-003: CRC continuation uses init=0 | Replaced with init=0xFFFFFFFF only |
| #4 | Document-level keyword sweep | Searched for stale terms, results below |

---

## 2. Topic-Level Authoritative Precedence (C8CR12-001)

### Problem

Rev12 §7 listed Rev10 as "authoritative base" including "interrupted quarantine recovery matrix (6 cases)" — but Rev12 §8 declared Rev12's table as the "SINGLE authoritative recovery table." Two documents claiming authority over recovery.

### Solution: Topic-Level Precedence Table

```
AUTHORITATIVE SOURCE BY TOPIC (REV13 — NORMATIVE):

| Topic                          | Authoritative Source                          |
|--------------------------------|-----------------------------------------------|
| CRC contract                   | Rev10 §2 (algorithm, API, test vector)       |
|                                | Rev13 §4 (continuation example cleanup)      |
| Canonical serialization        | Rev10 §2 (record layout, byte 11 payload)    |
| I0 executor ownership          | Rev10 §2 (TaskHandle, not core ID)            |
| I0a observation lifetime       | Rev13 §3 (consolidated: RAII + non-nested)     |
| I1 canonical equivalence       | Rev10 §2 (canonicalEqual, schema version)      |
| I1f generation ordering        | Rev13 §5 (test vector table, directional)      |
| I1g generation distance        | Rev13 §5 (construction + observation)           |
| Recovery decision table        | Rev13 §5 (9 uniform rows, SOLE authority)      |
| Recovery contract              | Rev13 §3 (gen=0 unconditional, no max())       |
| QUARANTINED semantics          | Rev13 §3 (no auto-reuse, operator only)        |
| Interrupted recovery           | Rev13 §5 (uses universal repair, no special EMPTY) |
| Eviction safety (I2)           | Rev10 §2 (matrix, command class, ACK condition) |
| ACK lifecycle (I3)              | Rev10 §2 (separation, durable queue, merge)     |
| ACK_PWA_RECEIVED protocol      | Rev10 §2 (defined, not implemented)            |
| ACK authentication boundary    | Rev10 §2 (ackDigest = content binding)          |
| Eviction matrix                | Rev10 §2 (idempotent/non-idempotent × ACK state) |
| NVS physical-failure model     | Rev10 §2 (logical redundancy, not physical)    |
| Partition sizing               | Rev10 §2 (empirical verification required)      |
| Schema-version equivalence     | Rev10 §2 (mismatch → CORRUPTED)                 |
| Padding semantics              | Rev10 §2 (Option B, no semantic meaning)       |

PRECEDENCE RULE:
    If any document OTHER than the source listed above contains a definition
    for a topic, that definition is NON-NORMATIVE and MUST NOT be implemented.
    
    Specifically:
    - Rev10's recovery matrix (6 cases with EMPTY rows) is NON-NORMATIVE.
    - Rev11's recovery matrix (9 rows) is superseded by Rev13 §5.
    - Rev6-Rev9 definitions for ANY topic are NON-NORMATIVE (SUPERSEDED banners applied).

SINGLE NORMATIVE DOCUMENT:
    Rev13 is the final consolidating document for Cycle 8C.
    It incorporates all authoritative definitions from Rev10 and Rev12.
    Implementers SHOULD read Rev13 as the sole normative reference.
    
    Rev10 remains as historical base for non-recovery topics,
    but Rev13's topic-level table is the authoritative index.
    If Rev10 and Rev13 conflict on any topic, Rev13 WINS.
```

---

## 3. ObservationGuard — Consolidated in Rev13 (C8CR12-002)

### Problem

Rev12 §7 attributed non-nested ObservationGuard to "Rev11 §3 → Rev10 §3 base" — but Rev10 §3 does NOT contain the non-nesting assertion. Rev11 introduced it.

### Solution: Consolidate in Rev13

```
I0a — OBSERVATION LIFETIME (REV13 — CONSOLIDATED, SOLE AUTHORITY):

INVARIANT:
    Observation depth MUST be exactly 0 or 1.
    Nested observation is FORBIDDEN.

ENFORCEMENT:

    class ObservationGuard {
        bool& _flag;
    public:
        ObservationGuard(bool& flag) : _flag(flag) {
            if (_flag) {
                panic("I0a: nested observation detected (depth > 1)");
            }
            _flag = true;
        }
        ~ObservationGuard() {
            _flag = false;
        }
    };

CONTRACT:
    - RAII ensures _observing is reset on ALL exit paths (from Rev10).
    - Constructor asserts non-nested (from Rev11).
    - Observation functions MUST NOT call other guarded observation functions.
    - Shared logic extracted to non-guarded helpers.
    - Only ONE ObservationGuard exists at any time.

ATTRIBUTION:
    This consolidated rule is authored in Rev13.
    It incorporates RAII lifetime safety (conceptually from Rev10)
    and non-nesting assertion (conceptually from Rev11).
    Rev13 is the SOLE normative source for this invariant.
    Do NOT reference Rev10 or Rev11 for ObservationGuard semantics.

NO NEW METADATA:
    _flag is RAM-only boolean. Constructor check is compile-time.
    No new NVS keys, no new record fields.
```

---

## 4. CRC Continuation — Clean Example (C8CR12-003)

### Problem

Rev10 §2 showed continuation verification using `init=0`:
```
Direct: crc32_le(0, full)
Continuation: crc32_le(crc32_le(0, part1), part2)
```

This uses `init=0`, but the normative API uses `init=0xFFFFFFFF` + final complement. Having `init=0` in the document confuses implementers.

### Solution: Single API Pattern

```
CRC CONTINUATION (REV13 — NORMATIVE, NO init=0 REFERENCE):

The normative CRC uses init=0xFFFFFFFF and final XOR (complement).
The continuation property is verified using the SAME API pattern:

Direct (single buffer):
    uint32_t crc = ~esp_crc32_le(0xFFFFFFFF, fullBuffer, fullLen) & 0xFFFFFFFF;

Continuation (two buffers):
    uint32_t state = esp_crc32_le(0xFFFFFFFF, bufferA, lenA);
    state = esp_crc32_le(state, bufferB, lenB);
    uint32_t crc = ~state & 0xFFFFFFFF;

Expected:
    direct == continuation

The continuation property holds because esp_crc32_le() is designed
for chained buffers: the 'init' parameter carries forward the CRC state.

NO init=0 IN NORMATIVE DOCUMENTS:
    The value 0 as init parameter does NOT appear in Rev13.
    All examples use 0xFFFFFFFF (the standard CRC-32 init value).
```

---

## 5. Consolidated Recovery Decision Table + Test Vectors (Rev13 — Sole Authority)

This table SUPERSEDES all previous recovery tables (Rev10, Rev11, Rev12).
Rev13 is the SOLE normative source for recovery/generation/quarantine semantics.

```
RECOVERY DECISION TABLE (REV13 — SOLE AUTHORITY):

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

WHERE:
    distAB = (uint32_t)(genB - genA)
    distBA = (uint32_t)(genA - genB)
    GEN_NEWER_A = distBA == 1
    GEN_NEWER_B = distAB == 1
    GEN_EQUAL = genA == genB
    GEN_AMBIGUOUS = distAB == 0x80000000
    GEN_INVALID = neither distAB nor distBA is 0, 1, or 0x80000000

RECOVERY CONTRACT (REV13 — SOLE AUTHORITY):

    recoverCorruptedEntry():
        PRECONDITION: Copy A == INVALID AND Copy B == INVALID
        Assert: !validA && !validB at entry.
        Write EMPTY(gen=0) to Copy A. Verify A.
        Write EMPTY(gen=0) to Copy B. Verify B.
        No max(readable generation). No successor calculation.
        gen=0 is unconditional.

QUARANTINED SEMANTICS (REV13 — SOLE AUTHORITY):

    QUARANTINED = both copies INVALID (derived state, not stored).
    No auto-reuse. No auto-recovery to EMPTY.
    Slot occupies space (reduces journal capacity).
    Operator must use recoverCorruptedEntry().
    If journal fills with QUARANTINED slots → JOURNAL_FULL → device halts.

EMPTY SEMANTICS (REV13 — SOLE AUTHORITY):

    EMPTY is a recordState, NOT a special generation.
    Generation ordering is the SOLE selector.
    gen=0 is NOT inherently "newer" than any other generation.
    No special EMPTY rows in recovery table.

INTERRUPTED RECOVERY (REV13 — SOLE AUTHORITY):

    A = VALID EMPTY(gen=0), B = INVALID → case #2 → REPAIR A→B. Safe.
    A = INVALID, B = VALID EMPTY(gen=0) → case #3 → REPAIR B→A. Safe.
    A = VALID EMPTY(gen=0), B = VALID EMPTY(gen=0) → case #6 → slot is EMPTY. Safe.
    A = VALID EMPTY(gen=0), B = VALID COMMITTED(gen=5) → distance=5 → case #9 → CORRUPTED. Safe.
    A = INVALID, B = INVALID → case #1 → QUARANTINED. Operator recovery.

EDGE CASES SPECIFIED AND MANUALLY DERIVED.
Implementation MUST reproduce these expected outcomes
with automated unit tests before journal integration.
```

---

## 6. Document-Level Keyword Sweep (C8CR12-001 verification)

Searched all documents in `docs/` for stale terms that could be interpreted as normative:

| Keyword | Found In | Status |
|---------|----------|--------|
| "AUTHORITATIVE" (recovery) | Rev10 §4 (recovery matrix), Rev12 §7 (authoritative stack) | Rev10 recovery NON-NORMATIVE per Rev13 §2 precedence table. Rev12 §7 superseded by Rev13 §2. |
| "recovery matrix" | Rev10 §4, Rev11 §6, Rev12 §8 | Rev13 §5 is SOLE authority. All others NON-NORMATIVE. |
| "gen=0" (as "newer") | Rev10 §4 (incorrect rows), Rev11 §2 (corrected), Rev12 §2 (correct) | Rev13 §5 is SOLE authority. Rev10 rows NON-NORMATIVE. |
| "max(" (generation) | Rev11 §2 (removed in Rev12) | NOT in Rev13. Removed. |
| "non-nested" | Rev11 §3, Rev12 §7 (wrong attribution) | Rev13 §3 is SOLE authority. |
| "successor" (generation) | Rev11 §2 (removed in Rev12) | NOT in Rev13. Removed. |
| "EMPTY wins" | Rev10 §4 (incorrect rows) | NOT in Rev13. Rev10 rows NON-NORMATIVE. |
| "EPOCH_RESET" | Rev6-Rev8 (historical) | NOT in Rev10+. SUPERSEDED banners applied. |
| "init=0" (CRC) | Rev10 §2 (verification example) | NOT in Rev13. Replaced with init=0xFFFFFFFF. |
| "abs(" (generation) | Rev8 (historical) | NOT in Rev10+. SUPERSEDED banner applied. |

**Result**: All stale terms in authoritative documents (Rev10) are now explicitly declared NON-NORMATIVE by Rev13's precedence table. Historical documents (Rev6-Rev9, Rev11) have SUPERSEDED banners.

---

## 7. Rule Compliance

| Item | New Field? | New Feature? |
|------|-----------|-------------|
| Topic-level precedence table | NO (documentation) | NO |
| ObservationGuard consolidated | NO (same RAM flag) | NO |
| CRC continuation cleanup | NO (example only) | NO |
| Keyword sweep | NO (verification) | NO |

**Zero new metadata. Zero new features. Zero architecture changes.**

---

## 8. Honest Limitations (Unchanged)

Same as Rev10+Rev12. See §9 of Rev10 or §10 of Rev12.

---

## 9. Authoritative Document Stack (Final — Rev13)

```
SINGLE NORMATIVE DOCUMENT FOR IMPLEMENTATION:

    CYCLE-8C-REV13-PRECEDENCE-CLOSURE.md (THIS DOCUMENT)

This document incorporates and consolidates all authoritative definitions
from Rev10 and Rev12. It is the sole normative reference.

Rev10 remains as historical background for non-recovery topics (CRC, I0,
canonical serialization, eviction, ACK lifecycle). Rev13's precedence table
(§2) specifies which topics Rev10 is authoritative for.

If Rev10 and Rev13 conflict on any topic: Rev13 WINS.

SUPERSEDED (banners applied, historical only):
    Rev6, Rev7, Rev8, Rev9, Rev11, Rev12

NOTE: Rev12 is also SUPERSEDED by Rev13.
    Rev12's contributions are incorporated into Rev13:
    - Recovery contract (gen=0 unconditional) → Rev13 §5
    - Test vector table → Rev13 §5
    - Wording correction → Rev13 §5
    - SUPERSEDED banners → already applied (Rev12 commit)
```

---

## 10. Auditor Approval Gate

**This design is NOT approved for implementation.**

Auditor must verify:
1. Precedence table (§2): Is Rev13 sole authority for recovery/generation/quarantine?
2. ObservationGuard (§3): Consolidated in Rev13, no false attribution?
3. CRC continuation (§4): No init=0 reference? Only 0xFFFFFFFF?
4. Recovery table + test vectors (§5): Sole authority? All edge cases correct?
5. Keyword sweep (§6): All stale terms identified? Non-normative declared?
6. Rule compliance (§7): Zero new metadata, zero new features?
7. Authoritative stack (§9): Rev13 is SINGLE normative document?

**After auditor approval, implementation Phase 1-8 may begin. Until then: NO CODE.**

Precharge: BLOCKED
I/O expander: BLOCKED
16-relay: BLOCKED
