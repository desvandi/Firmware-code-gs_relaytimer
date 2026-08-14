# CYCLE-8C-Rev12: Transaction Journal v4 — Final Consistency

**Status**: DESIGN ONLY — NO CODE IMPLEMENTATION
**Purpose**: 5 cleanup items. No new fields, no new features, no architecture changes.
**Auditor instruction**: "Rev12 seharusnya hanya: hapus max()+1, bersihkan contradictory prose, ganti wording, tambah SUPERSEDED banners, final contradiction sweep."

---

## 1. Summary

| Fix | Finding | What Changed |
|-----|---------|--------------|
| #1 | C8CR11-001: max(readable gen)+1 branch | REMOVED — recovery only when both INVALID, gen=0 always |
| #2 | C8CR11-002: Contradictory gen=0/0xFFFFFFFF prose | CLEANED — one canonical calculation, formal test vector table |
| #3 | C8CR11-003: "ALL EDGE CASES VERIFIED" | REPLACED — "specified and manually derived, implementation must reproduce" |
| #4 | Historical SUPERSEDED banners | APPLIED to Rev6-Rev9 docs (in this commit) |
| #5 | Final contradiction sweep | Generation/recovery focus, results below |

---

## 2. Fix #1: Remove max(readable generation)+1 (C8CR11-001)

### Problem

Rev11 §2 had two branches:
```
if ANY generation is readable → newGen = max(readable generation) + 1
if NO generation is readable → newGen = 0
```

The first branch is problematic:
- `max()` is not serial-number arithmetic — it doesn't handle wrap
- Example: genA=0xFFFFFFFF, genB=0 → max=0xFFFFFFFF, max+1=0 (correct by coincidence, but reasoning is wrong)
- More importantly: Rev11 also states recovery is ONLY called when both copies are INVALID
- So the "any generation readable" branch is unreachable in correct operation
- If it IS reached, it means a bug (recovery called on non-quarantined slot)

### Solution: Remove the Branch Entirely

```
RECOVERY CONTRACT (REV12 — AUTHORITATIVE):

recoverCorruptedEntry() SHALL be callable ONLY when:
    Copy A == INVALID (CRC fails)
    AND
    Copy B == INVALID (CRC fails)

THEREFORE:
    No trusted generation exists.
    (Invalid copies may have readable bytes, but generation is UNTRUSTED.)

Recovery SHALL write:
    EMPTY(gen=0) → Copy A
    Verify A
    EMPTY(gen=0) → Copy B
    Verify B

NO successor calculation.
NO max(readable generation).
NO conditional generation assignment.

gen=0 is used UNCONDITIONALLY because:
    1. Both copies were INVALID (no valid generation to conflict with)
    2. Both copies now have gen=0 (GEN_EQUAL, canonicalEqual)
    3. On boot: case #6 → slot is EMPTY
    4. Next mutation writes gen=1 to inactive copy (normal COW)

IF recoverCorruptedEntry() is called on a slot where one copy is still VALID:
    This is a BUG (precondition violated).
    The function MUST check: assert(!copyA_valid && !copyB_valid).
    If assertion fails: panic ("recovery called on non-quarantined slot").
```

---

## 3. Fix #2: Clean Contradictory Prose + Formal Test Vector Table (C8CR11-002)

### Problem

Rev11 §6 contained a chain of reasoning with errors and self-corrections:
```
"A=EMPTY(gen=0), B=COMMITTED(gen=0xFFFFFFFF): distAB=1 → case #5 → load B"
...
"Wait: ..."
...
"distBA = 1 → GEN_NEWER_A → Load A (gen=0, which IS newer). ✅ Correct!"
```

Two different answers for the same test case in the same document. The second answer is correct, but the first should not exist.

### Solution: Remove All Prose, Replace With Formal Table

Rev11's §6 edge case verification section (lines 389-427) is REPLACED with:

```
GENERATION TEST VECTOR TABLE (NORMATIVE):

| genA       | genB       | distAB           | distBA           | Classification    | Load  |
|------------|------------|-------------------|-------------------|-------------------|-------|
| 0          | 0          | 0                 | 0                 | GEN_EQUAL         | either (check canonicalEqual) |
| 0          | 1          | 1                 | 0xFFFFFFFF        | GEN_NEWER_B       | B     |
| 1          | 0          | 0xFFFFFFFF        | 1                 | GEN_NEWER_A       | A     |
| 0          | 0xFFFFFFFF | 0xFFFFFFFF        | 1                 | GEN_NEWER_A       | A     |
| 0xFFFFFFFF | 0          | 1                 | 0xFFFFFFFF        | GEN_NEWER_B       | B     |
| 0          | 5          | 5                 | 0xFFFFFFFB        | GEN_INVALID       | CORRUPTED |
| 5          | 0          | 0xFFFFFFFB        | 5                 | GEN_INVALID       | CORRUPTED |
| 10         | 20         | 10                | 0xFFFFFFF6        | GEN_INVALID       | CORRUPTED |
| 10         | 0x8000000A | 0x80000000        | 0x80000000        | GEN_AMBIGUOUS     | CORRUPTED |

WHERE:
    distAB = (uint32_t)(genB - genA)
    distBA = (uint32_t)(genA - genB)
    GEN_NEWER_A = distBA == 1 (A is 1 generation newer than B)
    GEN_NEWER_B = distAB == 1 (B is 1 generation newer than A)
    GEN_EQUAL = genA == genB
    GEN_AMBIGUOUS = distAB == 0x80000000
    GEN_INVALID = neither distAB nor distBA is 0, 1, or 0x80000000

NOTE:
    For GEN_EQUAL, the loader MUST also verify canonicalEqual(A, B).
    If canonicalEqual fails → CORRUPTED (divergent payload, case #7).

DERIVATION (for each row):
    Row "0, 0xFFFFFFFF": A=0, B=0xFFFFFFFF
        distAB = (uint32_t)(0xFFFFFFFF - 0) = 0xFFFFFFFF
        distBA = (uint32_t)(0 - 0xFFFFFFFF) = 1
        distBA == 1 → GEN_NEWER_A → Load A (gen=0 is 1 newer than gen=0xFFFFFFFF, wrapped)

    Row "0xFFFFFFFF, 0": A=0xFFFFFFFF, B=0
        distAB = (uint32_t)(0 - 0xFFFFFFFF) = 1
        distBA = (uint32_t)(0xFFFFFFFF - 0) = 0xFFFFFFFF
        distAB == 1 → GEN_NEWER_B → Load B (gen=0 is 1 newer than gen=0xFFFFFFFF, wrapped)
```

NO self-correcting prose. NO "wait" corrections. ONE answer per test case.

---

## 4. Fix #3: Wording Correction (C8CR11-003)

### Problem

Rev11 stated "ALL EDGE CASES VERIFIED CORRECT" — but the document itself contained an error in one edge case before self-correcting. The word "VERIFIED" implies runtime proof, which doesn't exist yet (no code).

### Solution

```
Rev11: "ALL EDGE CASES VERIFIED CORRECT"
Rev12: "EDGE CASES SPECIFIED AND MANUALLY DERIVED.
        Implementation MUST reproduce these expected outcomes
        with automated unit tests before journal integration."
```

---

## 5. Fix #4: SUPERSEDED Banners Applied

Applied in THIS commit to:
- `docs/CYCLE-8C-REV6-CONSISTENCY-CLEANUP.md`
- `docs/CYCLE-8C-REV7-FORMAL-COMPLETENESS.md`
- `docs/CYCLE-8C-REV8-CONSISTENCY-CLOSURE.md`
- `docs/CYCLE-8C-REV9-QUARANTINE-OBSERVATION.md`

Each document now has at the top:
```
╔══════════════════════════════════════════════════════════╗
║  ⚠️  SUPERSEDED — HISTORICAL DOCUMENT — DO NOT IMPLEMENT  ║
║                                                           ║
║  This document has been superseded by Rev10+Rev11+Rev12.  ║
║  Definitions herein may be stale or contradictory.        ║
║  Refer to:                                                ║
║    - CYCLE-8C-REV10-FINAL-CLOSURE.md                      ║
║    - CYCLE-8C-REV12-FINAL-CONSISTENCY.md                  ║
║  for the authoritative current design.                   ║
╚══════════════════════════════════════════════════════════╝
```

Note: Rev10 and Rev11 are NOT marked superseded — Rev10 remains authoritative base, Rev11 is superseded by Rev12 for the specific fixes, but Rev12 references Rev10 as base. To avoid confusion:

- Rev6-Rev9: SUPERSEDED (historical only)
- Rev10: AUTHORITATIVE BASE (CRC, ObservationGuard, contradiction sweep)
- Rev11: SUPERSEDED by Rev12 (recovery matrix, prose cleanup)
- Rev12: AUTHORITATIVE SUPPLEMENT (recovery contract, test vectors, wording)

---

## 6. Fix #5: Final Contradiction Sweep (Generation/Recovery Focus)

| Topic | Rev10 | Rev11 | Rev12 | Consistent? |
|-------|-------|-------|-------|-------------|
| Generation ordering | serial arithmetic (distAB/distBA) | same + directional | same + formal test vector table | ✅ |
| EMPTY special treatment | WRONG (gen=0 wins over gen=5) | FIXED (no special rows) | Confirmed: no special rows | ✅ |
| Recovery generation | gen=0 (Rev10) | gen=0 + max()+1 branch | gen=0 ONLY (max branch removed) | ✅ |
| Recovery precondition | Not explicit | "only when both INVALID" | "assert(!validA && !validB)" | ✅ |
| Interrupted recovery | matrix with wrong EMPTY rows | matrix corrected (9 uniform rows) | Confirmed: 9 uniform rows | ✅ |
| QUARANTINED semantics | Not defined | Both INVALID → QUARANTINED, no auto-reuse | Same, confirmed | ✅ |
| gen=0 vs gen=0xFFFFFFFF | Not addressed | Two contradictory calculations | ONE calculation (test vector table) | ✅ |
| "verified" wording | "VERIFIED (Python)" | "MATHEMATICALLY VERIFIED" | "SPECIFIED AND MANUALLY DERIVED" | ✅ |
| max(readable gen)+1 | Not present | Present (unreachable branch) | REMOVED | ✅ |

**All generation/recovery topics are now consistent across Rev10+Rev12.**
Rev11 is superseded by Rev12 for the specific fixes (recovery matrix, prose, wording).
Rev10 remains authoritative for CRC, ObservationGuard, and contradiction sweep.

---

## 7. Authoritative Document Stack (Final)

```
IMPLEMENTATION MUST FOLLOW THESE DOCUMENTS ONLY:

1. CYCLE-8C-REV10-FINAL-CLOSURE.md
   - CRC contract (algorithm, API, test vector, continuation)
   - ObservationGuard (RAII, early-return safe)
   - Contradiction sweep (Rev6→Rev10)
   - Interrupted quarantine recovery matrix (6 cases)
   - Rule compliance (zero new metadata)

2. CYCLE-8C-REV12-FINAL-CONSISTENCY.md (THIS DOCUMENT)
   - Recovery contract (gen=0 unconditional, no max() branch)
   - Generation test vector table (normative, 9 rows)
   - Wording correction ("specified and manually derived")
   - SUPERSEDED banners applied
   - Final contradiction sweep (generation/recovery)

SUPERSEDED (HISTORICAL — DO NOT IMPLEMENT):
   - CYCLE-8C-REV6-CONSISTENCY-CLEANUP.md
   - CYCLE-8C-REV7-FORMAL-COMPLETENESS.md
   - CYCLE-8C-REV8-CONSISTENCY-CLOSURE.md
   - CYCLE-8C-REV9-QUARANTINE-OBSERVATION.md
   - CYCLE-8C-REV11-RECOVERY-OBSERVATION-CLOSURE.md

NOTE: Rev11 is SUPERSEDED by Rev12 for:
   - Recovery matrix (Rev12 §2: removed max() branch)
   - Edge case prose (Rev12 §3: formal test vector table)
   - Wording (Rev12 §4: "specified and manually derived")
   
   Rev11's contributions that REMAIN AUTHORITATIVE (via Rev10):
   - Non-nested ObservationGuard (Rev11 §3 → Rev10 §3 base)
   - CRC terminology (Rev11 §4 → Rev10 §2 base)
   - SUPERSEDED banners concept (Rev11 §5 → Rev12 §5 applied)
```

---

## 8. Consolidated Recovery Decision Table (Rev12 — Authoritative, Final)

This is the SINGLE authoritative recovery table. No other document's table is normative.

```
RECOVERY DECISION TABLE (REV12 — AUTHORITATIVE):

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

| genA       | genB       | Result           | Load  |
|------------|------------|------------------|-------|
| 0          | 0          | GEN_EQUAL        | check canonicalEqual |
| 0          | 1          | GEN_NEWER_B      | B     |
| 1          | 0          | GEN_NEWER_A      | A     |
| 0          | 0xFFFFFFFF | GEN_NEWER_A      | A     |
| 0xFFFFFFFF | 0          | GEN_NEWER_B      | B     |
| 0          | 5          | GEN_INVALID      | CORRUPTED |
| 5          | 0          | GEN_INVALID      | CORRUPTED |
| 10         | 20         | GEN_INVALID      | CORRUPTED |
| 10         | 0x8000000A | GEN_AMBIGUOUS    | CORRUPTED |

RECOVERY CONTRACT:
    recoverCorruptedEntry(): ONLY when both copies INVALID.
    Writes EMPTY(gen=0) to both copies unconditionally.
    No max(readable generation)+1.
    Assert: !validA && !validB at entry.

NO SPECIAL EMPTY TREATMENT:
    EMPTY is a recordState.
    Generation ordering is the SOLE selector.
    gen=0 is NOT inherently "newer" than any other generation.
```

---

## 9. Rule Compliance

| Item | New Field? | New Feature? |
|------|-----------|-------------|
| Removed max()+1 branch | NO (removed code path) | NO |
| Formal test vector table | NO (documentation) | NO |
| Wording correction | NO (documentation) | NO |
| SUPERSEDED banners | NO (administrative) | NO |
| Contradiction sweep | NO (verification) | NO |

**Zero new metadata. Zero new features. Zero architecture changes.**

---

## 10. Honest Limitations (Unchanged from Rev11)

1-12: Same as Rev11 (see §9 of Rev11 or §9 of Rev10).

**Added**: CRC test vector is mathematically verified but NOT target-API verified (Phase 1 requirement).

---

## 11. Auditor Approval Gate

**This design is NOT approved for implementation.**

Auditor must verify:
1. max()+1 branch removed (§2)? Recovery is gen=0 unconditional?
2. Contradictory prose removed (§3)? Formal test vector table is sole reference?
3. Wording corrected (§4)? "Specified and manually derived"?
4. SUPERSEDED banners applied (§5)? Which docs?
5. Contradiction sweep (§6)? All generation/recovery topics consistent?
6. Recovery table (§8)? 9 uniform rows + 9 test vectors?
7. Rule compliance (§9)? Zero new metadata, zero new features?

**After auditor approval, implementation Phase 1-8 may begin. Until then: NO CODE.**

Precharge: BLOCKED
I/O expander: BLOCKED
16-relay: BLOCKED
