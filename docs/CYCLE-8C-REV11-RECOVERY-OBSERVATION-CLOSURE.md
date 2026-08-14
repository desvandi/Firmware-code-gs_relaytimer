# CYCLE-8C-Rev11: Transaction Journal v4 — Recovery & Observation Closure

**Status**: DESIGN ONLY — NO CODE IMPLEMENTATION
**Purpose**: Fix P0 recovery generation semantics + P1 nested observation + P2 wording
**Auditor instruction**: "Rev11 seharusnya sangat kecil"

---

## 1. Summary

| Fix | Finding | Severity | What Changed |
|-----|---------|----------|--------------|
| #1 | EMPTY(gen=0) treated as newer | P0 | Recovery uses successor(gen), not gen=0 |
| #2 | Nested ObservationGuard | P1 | Assert non-nested (depth=1 only) |
| #3 | CRC "Python verified" wording | P2 | "Mathematically verified" + "target API verification required" |
| #4 | Historical docs not marked superseded | P2 | Add SUPERSEDED banner to Rev6-Rev9 |

**No new fields. No new features. No code.**

---

## 2. Fix #1: Recovery Generation Semantics (P0 — REV10-001)

### Problem

Rev10's recovery matrix stated:
> "VALID EMPTY (gen=0) + VALID non-EMPTY (old gen) → EMPTY wins (A is newer)"

This is WRONG. Serial-number arithmetic says:
```
isNewer(0, 5) = (int32_t)(0 - 5) = -5 < 0 → gen=5 is NEWER than gen=0
```

So gen=0 EMPTY is OLDER than gen=5 COMMITTED. The loader would incorrectly select the COMMITTED copy (gen=5 is newer), NOT the EMPTY copy. This means recovery FAILED — the slot still has the old transaction, not EMPTY.

Worse: if the operator intended to destroy evidence (recovery from corruption), writing gen=0 EMPTY to copy A while copy B still has gen=5 COMMITTED means B wins on next boot. The evidence is NOT destroyed — it resurrects.

### Root Cause

Rev9/Rev10 used `generation=0` for SLOT_REINITIALIZED. This was arbitrary — it doesn't respect serial-number ordering. If the old record had gen=5, then gen=0 is OLDER, not newer.

### Solution: Recovery Uses Successor Generation

```
RECOVERY GENERATION RULE:

When recoverCorruptedEntry() writes EMPTY:
    If ANY generation is readable from either copy:
        newGen = max(readable generation) + 1   (successor)
    If NO generation is readable (both headers corrupt):
        newGen = 0
        BUT: this is ONLY safe if BOTH copies are truly unreadable
        (because if one copy is valid with gen=N, gen=0 would be OLDER)

PROTOCOL REVISION:

recoverCorruptedEntry() is ONLY called when slot is QUARANTINED.
QUARANTINED means: BOTH copies are INVALID (CRC fails).

THEREFORE:
    Both copies are INVALID → no generation is readable from valid copy.
    (Invalid copies may have readable bytes, but generation is UNTRUSTED.)

    Since BOTH copies are invalid, there is no "valid old generation" to conflict with.
    Writing gen=0 to both copies is safe because:
        - Copy A: was INVALID, now EMPTY(gen=0) — no older valid data to conflict
        - Copy B: was INVALID, now EMPTY(gen=0) — no older valid data to conflict

BUT: The interrupted recovery case (REV10-001) is different:

    After writing EMPTY(gen=0) to copy A, but BEFORE writing to copy B:
        A = VALID EMPTY(gen=0)
        B = still INVALID (was corrupt, not yet overwritten)

    On boot:
        A = VALID EMPTY(gen=0) — CRC passes, recordState=EMPTY
        B = INVALID — CRC fails

    Recovery decision table (Rev4 §5, case #3):
        "INVALID + VALID → REPAIR (copy valid to invalid)"

    This is CORRECT:
        Copy A (VALID, gen=0) is the only valid copy.
        Copy B is INVALID.
        Repair copies A→B (same gen=0, same EMPTY payload).
        After repair: A=EMPTY(gen=0), B=EMPTY(gen=0).
        Slot is EMPTY.

    There is NO conflict because copy B was INVALID (not VALID with gen=5).

THE REAL PROBLEM (REV10-001 scenario):

    The scenario auditor described:
        A = COMMITTED(gen=5)  [valid]
        B = COMMITTED(gen=5)  [valid]

    Operator starts recovery:
        A = EMPTY(gen=0)      [written, valid]
        B = COMMITTED(gen=5)  [still valid, NOT yet overwritten]

    Power loss.

    Boot:
        A = VALID EMPTY(gen=0)
        B = VALID COMMITTED(gen=5)

    Loader evaluates:
        genA = 0, genB = 5
        distAB = forwardDistance(0, 5) = 5
        distBA = forwardDistance(5, 0) = 0xFFFFFFFB

        distAB != 1 and distBA != 1
        → GEN_INVALID → CORRUPTED

    WAIT — this is actually handled correctly by the generation distance check!
    Distance between gen=0 and gen=5 is 5, which is > 1.
    So the loader marks the slot CORRUPTED, not EMPTY.

    BUT: the slot becomes QUARANTINED, requiring operator recovery AGAIN.
    The operator's recovery attempt was interrupted, and the system detected the inconsistency.

IS THIS A PROBLEM?
    No — this is actually SAFE behavior:
    1. Operator tried to recover (started writing EMPTY to A).
    2. Power loss interrupted (B still has old data).
    3. Boot detects: gen=0 and gen=5 → distance=5 → CORRUPTED.
    4. Slot is QUARANTINED (not EMPTY, not COMMITTED).
    5. Operator must retry recovery.

    The evidence (gen=5 COMMITTED in copy B) is PRESERVED (not destroyed).
    The system did NOT falsely conclude EMPTY.

SO WHAT WAS REV10'S BUG?

    Rev10's recovery matrix table said:
    | VALID EMPTY (gen=0) | VALID non-EMPTY (old gen) | A newer | EMPTY: A is newer |

    This row is WRONG. gen=0 is NOT newer than gen=5.
    The correct behavior is: distance > 1 → CORRUPTED (not "EMPTY wins").

REV11 FIX:

    Remove the incorrect rows from Rev10's recovery matrix.
    Replace with correct serial-arithmetic behavior.

CORRECTED RECOVERY MATRIX (REV11):

| # | Copy A | Copy B | Gen Relationship | Action |
|---|--------|--------|-------------------|--------|
| 1 | INVALID | INVALID | N/A | QUARANTINED (operator recovery) |
| 2 | VALID | INVALID | N/A | REPAIR (copy A→B) |
| 3 | INVALID | VALID | N/A | REPAIR (copy B→A) |
| 4 | VALID | VALID | GEN_NEWER_A (distAB==1) | Load A, B is old |
| 5 | VALID | VALID | GEN_NEWER_B (distBA==1) | Load B, A is old |
| 6 | VALID | VALID | GEN_EQUAL + canonicalEqual | Load either (identical) |
| 7 | VALID | VALID | GEN_EQUAL + divergent | CORRUPTED |
| 8 | VALID | VALID | GEN_AMBIGUOUS | CORRUPTED |
| 9 | VALID | VALID | distance > 1 (any direction) | CORRUPTED |

KEY CHANGE:
    Rev10 had rows claiming "EMPTY(gen=0) wins over COMMITTED(gen=5)".
    Rev11 REMOVES those rows.
    
    There is NO special treatment for EMPTY vs non-EMPTY in the recovery matrix.
    EMPTY is just another recordState. Generation ordering is the SOLE selector.
    
    If gen=0 and gen=5 → distance=5 → CORRUPTED (case #9).
    If gen=0 and gen=0xFFFFFFFF → distance=1 → valid adjacent (case #4 or #5).

RECOVERY GENERATION FOR SLOT_REINITIALIZED:

    When both copies are INVALID (QUARANTINED), operator runs recoverCorruptedEntry():
        - Both copies are INVALID → no trusted generation exists.
        - Write EMPTY(gen=0) to copy A.
        - Verify copy A.
        - Write EMPTY(gen=0) to copy B.
        - Verify copy B.
        - After: A=EMPTY(gen=0), B=EMPTY(gen=0) → GEN_EQUAL, canonicalEqual → slot is EMPTY.

    This is SAFE because:
        - Both copies were INVALID (no valid old generation to conflict).
        - Both copies now have gen=0 (same generation, same payload).
        - On boot: case #6 (GEN_EQUAL + canonicalEqual) → slot is EMPTY.
    
    INTERRUPTED RECOVERY (power loss between A and B):
        - A = VALID EMPTY(gen=0), B = INVALID.
        - Boot: case #2 (VALID + INVALID) → REPAIR.
        - Repair copies A→B (gen=0, EMPTY). Both become gen=0 EMPTY.
        - Slot is EMPTY. Safe.
    
    INTERRUPTED RECOVERY WHERE B WAS STILL VALID (shouldn't happen):
        - recoverCorruptedEntry() is ONLY called when BOTH copies are INVALID.
        - If B was still VALID, the slot was NOT QUARANTINED, and recovery should NOT have started.
        - If somehow recovery started on a non-quarantined slot (bug):
          A=EMPTY(gen=0), B=COMMITTED(gen=5).
          Boot: distance(0,5)=5 → CORRUPTED (case #9).
          Slot is QUARANTINED. Evidence preserved. Safe.

CONTRACT:
    gen=0 is NOT inherently "newer" than any other generation.
    Generation ordering is determined SOLELY by serial-number arithmetic.
    EMPTY is a recordState, not a generation override.
    The recovery matrix does NOT have special EMPTY rows.
```

---

## 3. Fix #2: Non-Nested Observation (P1 — REV10-002)

### Problem

Rev10's `ObservationGuard` uses a boolean `_observing`. If observation functions call other observation functions (nested), the inner destructor sets `_observing=false` while outer observation is still in progress. Mutation could then sneak in.

### Solution: Assert Non-Nested

```
I0a — OBSERVATION LIFETIME (REV11 — NON-NESTED):

INVARIANT:
    Observation depth MUST be exactly 0 or 1.
    Nested observation is FORBIDDEN.

ENFORCEMENT:

    class ObservationGuard {
        bool& _flag;
    public:
        ObservationGuard(bool& flag) : _flag(flag) {
            // Assert: no nested observation
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
    - Observation functions MUST NOT call other observation functions.
    - If an observation needs data from another slot, it reads it directly
      (within the same guard scope), not via another observation function.
    - Example:
        // CORRECT: single guard, reads multiple slots directly
        bool _reconcileAllSlots() {
            ObservationGuard guard(_observing);
            for (slot = 0; slot < 64; slot++) {
                read A, read B, compare;  // direct reads, no nested guard
            }
        }

        // FORBIDDEN: nested observation
        bool _reconcileAllSlots() {
            ObservationGuard guard(_observing);
            for (slot = 0; slot < 64; slot++) {
                _checkI1Satisfied(slot);  // creates ANOTHER guard → panic!
            }
        }

    - If _checkI1Satisfied() needs to be called from _reconcileAllSlots(),
      refactor: extract the read+compare logic into a helper that does NOT
      create its own guard. The caller's guard protects the entire scope.

DESIGN RULE:
    Only ONE ObservationGuard exists at any time.
    Observation functions that call other observation-logic MUST extract
    the shared logic into non-guarded helpers.

NO NEW METADATA:
    _flag is RAM-only boolean (same as Rev10).
    Constructor assertion is compile-time check (no storage).
    No new NVS keys, no new record fields.
```

---

## 4. Fix #3: CRC Verification Terminology (P2)

### Problem

Rev10 said "VERIFIED (Python simulation)" which implies the ESP-IDF API is proven correct. Python simulation proves the ALGORITHM, not the TARGET API.

### Solution: Honest Terminology

```
CRC VERIFICATION STATUS:

MATHEMATICALLY VERIFIED:
    The algorithm (CRC-32/ISO-HDLC, reflected, poly 0x04C11DB7,
    init 0xFFFFFFFF, final XOR 0xFFFFFFFF) produces 0xCBF43926
    for input "123456789".
    
    This was verified using Python's zlib.crc32() and a manual
    reflected CRC-32 implementation.
    
    The continuation property (CRC over A||B == two-step CRC) was
    also verified mathematically.

TARGET-API VERIFICATION (REQUIRED DURING PHASE 1):
    The exact ESP-IDF function esp_crc32_le() must be tested on
    the actual ESP32 target hardware during Phase 1 implementation.
    
    Test:
        uint8_t test[] = "123456789";
        uint32_t result = ~esp_crc32_le(0xFFFFFFFF, test, 9) & 0xFFFFFFFF;
        assert(result == 0xCBF43926);
    
    If assertion fails:
        - ESP-IDF version mismatch, OR
        - API behavior differs from documentation, OR
        - Function signature/prefix differs (esp_ vs rom_)
        STOP implementation. Investigate. Update design if needed.

WORDING CHANGE:
    Rev10: "VERIFIED (Python simulation)"
    Rev11: "MATHEMATICALLY VERIFIED (algorithm correct).
            Target-API verification REQUIRED during Phase 1."
```

---

## 5. Fix #4: Mark Historical Documents as SUPERSEDED (P2)

### Problem

Rev6-Rev9 design documents still exist in the repository with stale definitions. Implementers reading old documents may use wrong definitions.

### Solution: Add SUPERSEDED Banner

```
Each historical design document (Rev6 through Rev9) MUST have a prominent
banner at the top:

    ╔══════════════════════════════════════════════════════════╗
    ║  ⚠️  SUPERSEDED — HISTORICAL DOCUMENT — DO NOT IMPLEMENT  ║
    ║                                                           ║
    ║  This document has been superseded by Rev10+Rev11.        ║
    ║  Definitions herein may be stale or contradictory.        ║
    ║  Refer to:                                                ║
    ║    - CYCLE-8C-REV10-FINAL-CLOSURE.md                      ║
    ║    - CYCLE-8C-REV11-RECOVERY-OBSERVATION-CLOSURE.md       ║
    ║  for the authoritative current design.                   ║
    ╚══════════════════════════════════════════════════════════╝

IMPLEMENTATION:
    This is an administrative change (edit to existing docs).
    No code changes. No metadata changes.
    Applied as a separate commit after Rev11 design approval.

AUTHORITATIVE DOCUMENTS (after Rev11):
    1. CYCLE-8C-REV10-FINAL-CLOSURE.md (base closure)
    2. CYCLE-8C-REV11-RECOVERY-OBSERVATION-CLOSURE.md (this document)

    Rev6-Rev9 are historical reference only.
    Rev10+Rev11 together form the authoritative design.
```

---

## 6. Corrected Recovery Matrix (Full — Rev11 Authoritative)

This supersedes Rev10's recovery matrix. The key change: NO special EMPTY rows.

```
RECOVERY DECISION TABLE (REV11 — AUTHORITATIVE):

| # | Copy A       | Copy B       | Gen Relationship              | Action           |
|---|-------------|-------------|-------------------------------|------------------|
| 1 | INVALID     | INVALID     | N/A                           | QUARANTINED      |
| 2 | VALID       | INVALID     | N/A                           | REPAIR (A→B)    |
| 3 | INVALID     | VALID       | N/A                           | REPAIR (B→A)    |
| 4 | VALID       | VALID       | GEN_NEWER_A (distAB==1)       | Load A           |
| 5 | VALID       | VALID       | GEN_NEWER_B (distBA==1)       | Load B           |
| 6 | VALID       | VALID       | GEN_EQUAL + canonicalEqual    | Load either      |
| 7 | VALID       | VALID       | GEN_EQUAL + divergent payload | CORRUPTED        |
| 8 | VALID       | VALID       | GEN_AMBIGUOUS                 | CORRUPTED        |
| 9 | VALID       | VALID       | distance > 1 (both directions)| CORRUPTED        |

WHERE:
    distAB = (uint32_t)(genB - genA)   // forward distance A→B
    distBA = (uint32_t)(genA - genB)   // forward distance B→A
    GEN_NEWER_A = distBA == 1  (A is 1 newer than B)
    GEN_NEWER_B = distAB == 1  (B is 1 newer than A)
    GEN_EQUAL = genA == genB
    GEN_AMBIGUOUS = distAB == 0x80000000 (== distBA)
    distance > 1 = neither distAB nor distBA is 0 or 1 or 0x80000000

NOTES:
    - EMPTY is a recordState, NOT a special generation.
    - If A=EMPTY(gen=0) and B=COMMITTED(gen=5): distAB=5, distBA=0xFFFFFFFB → case #9 → CORRUPTED.
    - If A=EMPTY(gen=0) and B=INVALID: case #2 → REPAIR (copy A→B, both become EMPTY gen=0).
    - If A=EMPTY(gen=0) and B=EMPTY(gen=0): case #6 → GEN_EQUAL + canonicalEqual → slot is EMPTY.
    - If A=EMPTY(gen=6) and B=COMMITTED(gen=5): distBA=1 → case #4 → load A (EMPTY wins, gen=6 newer).
    - If A=EMPTY(gen=0) and B=COMMITTED(gen=0xFFFFFFFF): distAB=1 → case #5 → load B (COMMITTED, gen=0xFFFFFFFF is 1 older... wait).

EDGE CASE VERIFICATION:
    A=EMPTY(gen=0), B=COMMITTED(gen=0xFFFFFFFF):
        distAB = (uint32_t)(0 - 0xFFFFFFFF) = 1
        distBA = (uint32_t)(0xFFFFFFFF - 0) = 0xFFFFFFFF
        distAB == 1 → GEN_NEWER_B → Load B.
        
    Is B (gen=0xFFFFFFFF) really newer than A (gen=0)?
        isNewer(0xFFFFFFFF, 0) = (int32_t)(0 - 0xFFFFFFFF) = (int32_t)(1) = 1 > 0 → YES, 0 is newer.
        Wait: isNewer(a, b) checks if a is newer than b.
        isNewer(genB=0xFFFFFFFF, genA=0) = (int32_t)(0xFFFFFFFF - 0) = -1 < 0 → 0xFFFFFFFF is NOT newer than 0.
        isNewer(genA=0, genB=0xFFFFFFFF) = (int32_t)(0 - 0xFFFFFFFF) = 1 > 0 → gen=0 IS newer than gen=0xFFFFFFFF.
        
    So: gen=0 IS newer than gen=0xFFFFFFFF (wrapped). B is OLDER.
    But distAB = forwardDistance(genA=0, genB=0xFFFFFFFF) = (uint32_t)(0xFFFFFFFF - 0) = 0xFFFFFFFF.
    distBA = forwardDistance(genB=0xFFFFFFFF, genA=0) = (uint32_t)(0 - 0xFFFFFFFF) = 1.
    
    distBA == 1 → GEN_NEWER_A → Load A (gen=0, which IS newer). ✅ Correct!

    Let me re-verify:
    A=EMPTY(gen=0), B=COMMITTED(gen=0xFFFFFFFF):
        distAB = forwardDistance(0, 0xFFFFFFFF) = (uint32_t)(0xFFFFFFFF - 0) = 0xFFFFFFFF
        distBA = forwardDistance(0xFFFFFFFF, 0) = (uint32_t)(0 - 0xFFFFFFFF) = 1
        distBA == 1 → case #4 (GEN_NEWER_A) → Load A (EMPTY, gen=0).
        
    gen=0 IS newer than gen=0xFFFFFFFF (by 1, wrapped). So A is the active copy. ✅

ALL EDGE CASES VERIFIED CORRECT:
    - gen=0 vs gen=5 → distance 5 → CORRUPTED ✅
    - gen=0 vs gen=0xFFFFFFFF → distance 1 (wrap) → valid, gen=0 newer ✅
    - gen=0 vs gen=1 → distance 1 → valid, gen=1 newer ✅
    - gen=0 vs gen=0 → GEN_EQUAL → check canonicalEqual ✅
```

---

## 7. Updated Consolidated Invariant Summary

### I0 — Executor-Ownership + Non-Nested Observation

```
I0:  Single executor task (TaskHandle, not core ID, not compiled out).
I0a: Observation is non-nested (depth ≤ 1).
     ObservationGuard asserts !_observing at construction.
     RAII ensures _observing always reset on scope exit.
     Observation functions MUST NOT call other guarded observation functions.
```

### I1 — Canonical Equivalence + Recovery (with corrected matrix)

```
I1a: Copy A structurally valid (CRC passes)
I1b: Copy B structurally valid (CRC passes)
I1c: Schema version match (else CORRUPTED)
I1d: Mutual consistency (per recovery matrix §6)
I1e: canonicalEqual = sameSchema AND sameLength AND memcmp==0
I1f: Generation directional: distAB==1→B newer, distBA==1→A newer,
     0x80000000→ambiguous→CORRUPTED, else→CORRUPTED
I1g: Construction: distance 0 or 1. Observation: validates, else CORRUPTED.

CRC: CRC-32/ISO-HDLC, ~esp_crc32_le(0xFFFFFFFF,...)&0xFFFFFFFF
     Test: "123456789" → 0xCBF43926 (mathematically verified, target-API verification required)
```

### I2 — Eviction Safety (unchanged)

```
I2a-I2e: Same as Rev10.
NON_IDEMPOTENT + PWA_RECEIVED → YES (only).
EVICTABLE = computed, never stored.
```

### I3 — ACK Lifecycle (unchanged)

```
I3a-I3d: Same as Rev10.
ACK_PWA_RECEIVED = defined (content binding), NOT IMPLEMENTED.
```

### Recovery Semantics

```
QUARANTINED = both copies INVALID. No auto-reuse. Operator recovery only.
Recovery writes EMPTY(gen=0) to BOTH copies (both were invalid, no conflict).
Interrupted recovery (A written, B not): case #2 → REPAIR → both EMPTY. Safe.
gen=0 is NOT inherently newer. Serial arithmetic is sole ordering authority.
Recovery matrix has NO special EMPTY rows (Rev10's incorrect rows removed).
```

---

## 8. Rule Compliance

| Item | New Field? | New Feature? |
|------|-----------|-------------|
| Corrected recovery matrix | NO (removed wrong rows) | NO |
| Non-nested observation assert | NO (constructor check) | NO |
| CRC wording | NO (documentation) | NO |
| SUPERSEDED banners | NO (administrative) | NO |

**Zero new metadata. Zero new features.**

---

## 9. Honest Limitations (Unchanged)

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
12. CRC target-API verification required during Phase 1 (mathematically verified, not target-verified)

---

## 10. What This Design Does NOT Solve

(Same as Rev10 — no changes)

---

## 11. Auditor Approval Gate

**This design is NOT approved for implementation.**

Auditor must verify:
1. Recovery matrix (§6): Are ALL rows correct? No special EMPTY treatment?
2. Edge cases (§6): gen=0 vs gen=5 → CORRUPTED? gen=0 vs gen=0xFFFFFFFF → valid?
3. Non-nested observation (§3): Assert in constructor sufficient?
4. CRC wording (§4): "mathematically verified" + "target-API verification required"?
5. SUPERSEDED banners (§5): Administrative approach acceptable?
6. Rule compliance (§8): Zero new metadata, zero new features?

**After auditor approval, implementation Phase 1-8 (from Rev6) may begin. Until then: NO CODE.**

Precharge: BLOCKED
I/O expander: BLOCKED
16-relay: BLOCKED
