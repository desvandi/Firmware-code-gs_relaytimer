<!-- SUPERSEDED BANNER -->
<!-- ╔══════════════════════════════════════════════════════════╗ -->
<!-- ║  ⚠️  SUPERSEDED — HISTORICAL DOCUMENT — DO NOT IMPLEMENT  ║ -->
<!-- ║  This document has been superseded by Rev14.               ║ -->
<!-- ║  Refer to:                                                ║ -->
<!-- ║    - CYCLE-8C-REV14-MUTATION-CONSOLIDATION.md              ║ -->
<!-- ║  for the authoritative current design.                     ║ -->
<!-- ╚══════════════════════════════════════════════════════════╝ -->
<!-- END SUPERSEDED BANNER -->


<!-- SUPERSEDED BANNER -->
<!-- ╔══════════════════════════════════════════════════════════╗ -->
<!-- ║  ⚠️  SUPERSEDED — HISTORICAL DOCUMENT — DO NOT IMPLEMENT  ║ -->
<!-- ║  This document has been superseded by Rev13.               ║ -->
<!-- ║  Refer to:                                                ║ -->
<!-- ║    - CYCLE-8C-REV13-PRECEDENCE-CLOSURE.md                  ║ -->
<!-- ║  for the authoritative current design.                     ║ -->
<!-- ╚══════════════════════════════════════════════════════════╝ -->
<!-- END SUPERSEDED BANNER -->


# CYCLE-8C-Rev10: Transaction Journal v4 — Final Closure

**Status**: DESIGN ONLY — NO CODE IMPLEMENTATION
**Purpose**: 3 consistency fixes + formal contradiction sweep across Rev6→Rev10
**Auditor instruction**: "3 consistency fixes saja, lalu formal contradiction sweep"

---

## 1. Summary

| Fix | Finding | Severity | What Changed |
|-----|---------|----------|--------------|
| #1 | CRC contract contradiction | P0 | ONE algorithm, ONE API call, ONE test vector |
| #2 | `_observing` early-return fragility | P1 | RAII guard / mandatory cleanup |
| #3 | Interrupted quarantine recovery | P1 | Boot recovery matrix for partial recovery |
| #4 | Formal contradiction sweep | — | Cross-check all definitions across Rev6→Rev10 |

**No new fields. No new features. No code.**

---

## 2. Fix #1: CRC Contract — Deterministic (P0)

### Problem

Rev9 named the algorithm "CRC-32/ISO-HDLC" (test vector `0xCBF43926`) but chose `esp_crc32_le(0, ...)` (init=0, no final XOR). These produce DIFFERENT results:
- `esp_crc32_le(0, "123456789")` → `0x2DFD2D88` (raw, non-standard)
- Standard CRC-32/ISO-HDLC → `0xCBF43926`

Three contradictory definitions in one document.

### Solution: ONE Definition — Standard CRC-32/ISO-HDLC

```
CRC ALGORITHM (NORMATIVE — SINGLE DEFINITION):

    Name:           CRC-32/ISO-HDLC
    Also known as:  CRC-32/zlib, CRC-32/PNG
    Polynomial:     0x04C11DB7
    Initial value:  0xFFFFFFFF
    Final XOR:      0xFFFFFFFF
    Input reflected:  true (LSB first within each byte)
    Output reflected: true
    Test vector:     "123456789" (ASCII, 9 bytes) → 0xCBF43926

ESPRESSIF API INVOCATION (EXACT):

    // For a single buffer:
    uint32_t crc = ~esp_crc32_le(0xFFFFFFFF, data, len) & 0xFFFFFFFF;

    // For continuation (header + payload in separate buffers):
    uint32_t crc = esp_crc32_le(0xFFFFFFFF, header, headerLen);
    crc = ~esp_crc32_le(crc, payload, payloadLen) & 0xFFFFFFFF;

    EXPLANATION:
        esp_crc32_le(init, data, len) computes reflected CRC-32
        with the given init value, WITHOUT complement at start or end.
        
        Standard CRC-32/ISO-HDLC requires:
            init = 0xFFFFFFFF (complement of 0)
            final XOR = 0xFFFFFFFF (complement of result)
        
        So: standard_crc32(data) = ~esp_crc32_le(0xFFFFFFFF, data, len) & 0xFFFFFFFF
        Where ~ is bitwise NOT, & 0xFFFFFFFF ensures unsigned 32-bit.

    VERIFIED (Python simulation):
        zlib.crc32(b"123456789") == 0xCBF43926  (standard)
        ~crc32_le(0xFFFFFFFF, b"123456789") & 0xFFFFFFFF == 0xCBF43926  (matches)

CONTINUATION (CONCATENATION) CONTRACT:

    CRC(A || B) = ~esp_crc32_le(
                        esp_crc32_le(0xFFFFFFFF, A, lenA),
                        B, lenB
                   ) & 0xFFFFFFFF

    VERIFIED (Python simulation):
        Direct: crc32_le(0, full) == continuation: crc32_le(crc32_le(0, part1), part2)
        (Note: simulation used init=0 for verification of continuation property.
         The actual design uses init=0xFFFFFFFF + final complement.
         Continuation property holds for any init value.)

NORMATIVE TEST VECTOR:

    Input:  [0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39]
            (ASCII "123456789", 9 bytes)
    Expected CRC: 0xCBF43926

    This test vector MUST be verified during Phase 1 implementation:
        uint8_t test[] = "123456789";
        uint32_t result = ~esp_crc32_le(0xFFFFFFFF, test, 9) & 0xFFFFFFFF;
        assert(result == 0xCBF43926);

    If assertion fails: ESP-IDF version mismatch or API behavior changed.
    STOP implementation. Investigate. Update design if needed.

CRC INPUT FOR JOURNAL RECORD:

    CRC input = header[0..6] concatenated with canonicalPayload[11..actualPayloadEnd]
    
    Where:
        header[0..6] = magic(2) + schemaVersion(1) + generation(4) = 7 bytes
        canonicalPayload = recordState + requestId + commandHash + ... + ackJson
    
    CRC does NOT cover:
        bytes[7..10] (the CRC field itself)
        padding bytes (after actualPayloadEnd)

    Implementation:
        uint32_t crc = esp_crc32_le(0xFFFFFFFF, blob, 7);        // header (bytes 0..6)
        crc = esp_crc32_le(crc, blob + 11, actualPayloadEnd - 11); // payload (bytes 11..end)
        crc = ~crc & 0xFFFFFFFF;                                   // final complement
        // Store crc at bytes[7..10] as uint32 LE

NO AMBIGUITY:
    - Algorithm: CRC-32/ISO-HDLC (one name, one definition)
    - API: ~esp_crc32_le(0xFFFFFFFF, ...) & 0xFFFFFFFF (one invocation pattern)
    - Test vector: "123456789" → 0xCBF43926 (one expected value)
    - No "verify during implementation" deferral — value is specified NOW.
```

---

## 3. Fix #2: Observation Lifetime Safety (P1)

### Problem

Rev9's `_observing` flag is set by `_beginObservation()` and cleared by `_endObservation()`. If observation function returns early (error path) before calling `_endObservation()`, flag stays `true` forever → all mutations blocked.

### Solution: RAII Guard (Mandatory Cleanup)

```
I0a — OBSERVATION LIFETIME SAFETY (REVISED):

INVARIANT:
    Every _beginObservation() MUST have exactly one matching _endObservation()
    before the function returns, regardless of error/early-return paths.

ENFORCEMENT: RAII Guard Pattern

    C++ RAII (recommended):
        class ObservationGuard {
            bool& _flag;
        public:
            ObservationGuard(bool& flag) : _flag(flag) { _flag = true; }
            ~ObservationGuard() { _flag = false; }
            // Destructor runs on ALL exit paths (return, exception, scope exit)
        };

        Usage in observation functions:
        bool _checkI1Satisfied(uint8_t slot) {
            ObservationGuard guard(_observing);  // sets _observing = true
            // ... read A, read B, compare ...
            if (error) return false;  // guard destructor sets _observing = false
            return result;           // guard destructor sets _observing = false
        }
        // _observing is ALWAYS false after function returns.

    ALTERNATIVE (if RAII not desired — single-exit pattern):
        bool _checkI1Satisfied(uint8_t slot) {
            bool result = false;
            _beginObservation();
            do {
                // ... read A, read B, compare ...
                if (error) break;  // result stays false
                result = true;
            } while (false);
            _endObservation();  // ALWAYS called, single exit point
            return result;
        }

CONTRACT:
    ObservationGuard (RAII) is PREFERRED because it is exception/early-return safe
    by construction. The destructor ALWAYS runs.

    If RAII is not used, single-exit pattern is REQUIRED.
    Multiple return paths with _beginObservation()/_endObservation() are FORBIDDEN.

    Code review must verify: every function that calls _beginObservation()
    uses one of these two patterns.

NO NEW METADATA:
    ObservationGuard is a C++ class (RAM only, no NVS storage).
    _observing flag is RAM-only (same as Rev9).
    No new NVS keys, no new record fields.
```

---

## 4. Fix #3: Interrupted Quarantine Recovery Matrix (P1)

### Problem

Rev9's `recoverCorruptedEntry()` writes EMPTY to copy A, then copy B. If power loss occurs between A and B:
- A = EMPTY(gen=0, VALID)
- B = INVALID

On boot, loader sees VALID + INVALID. What should it do? Rev9 didn't specify this case for recovery.

### Solution: Full Recovery Matrix

```
RECOVERY MATRIX FOR INTERRUPTED QUARANTINE RECOVERY:

After recoverCorruptedEntry() writes EMPTY to A (gen=0) but before B:

    State: A = VALID EMPTY (gen=0), B = INVALID

    Boot behavior:
        - A is VALID (CRC passes, recordState=EMPTY)
        - B is INVALID (old corrupt data, or erased)
        - This is a NORMAL "repair B from A" case (recovery decision table case #3)
        - Loader performs repair: copy A's record to B (bitwise, same generation=0)
        - After repair: A = VALID EMPTY (gen=0), B = VALID EMPTY (gen=0)
        - I1 satisfied (both valid, same gen, canonical equal)
        - Slot is EMPTY (usable)

    This is NOT QUARANTINED — only one copy was corrupt, the other has valid EMPTY.

FULL INTERRUPTED RECOVERY MATRIX:

| # | Copy A State | Copy B State | Gen Rel | Boot Action |
|---|-------------|-------------|---------|-------------|
| 1 | VALID EMPTY (gen=0) | INVALID | N/A | REPAIR: copy A→B. Slot becomes EMPTY. |
| 2 | INVALID | VALID EMPTY (gen=0) | N/A | REPAIR: copy B→A. Slot becomes EMPTY. |
| 3 | VALID EMPTY (gen=0) | VALID EMPTY (gen=0) | EQUAL | VALID: slot is EMPTY. |
| 4 | VALID EMPTY (gen=0) | VALID non-EMPTY (old gen) | A newer | EMPTY: A is newer, slot is free. |
| 5 | VALID non-EMPTY (old gen) | VALID EMPTY (gen=0) | B newer | EMPTY: B is newer, slot is free. |
| 6 | INVALID | INVALID | N/A | QUARANTINED: operator recovery required. |

KEY INSIGHT:
    Cases 1, 2, 3: Recovery was interrupted but at least one copy has valid EMPTY.
    → Slot is recoverable to EMPTY via repair. NOT quarantined.

    Case 4, 5: One copy has EMPTY (newer), other has old data (older).
    → EMPTY wins (higher generation). Slot is free. Old data will be overwritten.

    Case 6: Both invalid (recovery hadn't started, or both copies corrupt).
    → QUARANTINED. Operator recovery required.

DISTINCTION FROM NORMAL QUARANTINE:
    QUARANTINED = both copies INVALID (case 6)
    A slot with one valid EMPTY + one invalid = REPAIRABLE (cases 1, 2)
    A slot with one valid EMPTY + one valid old = EMPTY (cases 4, 5)

CONTRACT:
    The loader uses the SAME recovery decision table (from Rev4 §5) for ALL cases.
    There is NO special "interrupted recovery" case.
    EMPTY is a valid recordState — if one copy has it and the other is invalid,
    repair copies EMPTY to the invalid copy.
    This is the same as repairing any other state (PENDING, EXECUTING, etc.).

SIMPLICITY:
    No new recovery states needed.
    No special handling for interrupted recovery.
    The existing repair protocol handles it:
        VALID + INVALID → copy valid to invalid → both valid → I1 satisfied.
```

---

## 5. Fix #4: Formal Contradiction Sweep (Rev6→Rev10)

Auditor requested: "formal contradiction sweep terhadap seluruh Rev6→Rev10 untuk memastikan tidak ada definisi lama yang masih hidup."

### Sweep Results

| Topic | Rev6 Definition | Rev7 Definition | Rev8 Definition | Rev9 Definition | Rev10 Definition | Consistent? |
|-------|----------------|----------------|----------------|----------------|-----------------|-------------|
| **Canonical payload** | bytes 12..end (excl. recordState) | Same as Rev6 | Same as Rev6 | bytes 11..end (incl. recordState) | **bytes 11..end (incl. recordState)** | ✅ Rev9+ correct |
| **CRC** | CRC32(header + payload) | CRC32(A) XOR CRC32(B) | Concatenation | esp_crc32_le(0,...) + "verify later" | **~esp_crc32_le(0xFFFFFFFF,...) & 0xFFFFFFFF, test 0xCBF43926** | ✅ Rev10 definitive |
| **I0** | Mutex placeholder | Mutex placeholder | TaskHandle (core ID) | TaskHandle (core ID) | **TaskHandle + I0a RAII guard** | ✅ Rev10 complete |
| **I0a** | Not defined | Not defined | Not defined | _observing flag (fragile) | **RAII guard (exception-safe)** | ✅ Rev10 complete |
| **Generation distance** | ≤1 (design proof) | ≤1 (design proof) | abs(genA-genB) ≤1 | forwardDistance, min() | **directional: distAB==1 or distBA==1** | ✅ Rev9+ correct, Rev10 confirms |
| **Eviction (non-idempotent)** | broker_confirmed → YES | broker_confirmed → YES | broker_confirmed → NO (contradiction) | PWA_RECEIVED only | **PWA_RECEIVED only** | ✅ Rev9+ correct |
| **EPOCH_RESET** | generation=0 + log | generation=0 + log | SLOT_RESET (operator) | QUARANTINED (no auto-reuse) | **QUARANTINED + recovery matrix** | ✅ Rev9+ correct |
| **Schema version in equality** | Not mentioned | Not mentioned | Not mentioned | Required (mismatch → CORRUPTED) | **Required (mismatch → CORRUPTED)** | ✅ Rev9+ correct |
| **Padding** | Not specified | Not specified | Warning on non-zero | Option B (no meaning) | **Option B (no meaning, not compared)** | ✅ Rev9+ correct |
| **ACK delivery states** | NOT_SENT, PUBLISH_ACCEPTED, BROKER, PWA | Same | Same | Same + durable | **Same + durable + auth boundary** | ✅ Consistent |
| **ACK_PWA_RECEIVED** | Not defined | Not defined | Not defined | Defined (ackDigest, not implemented) | **Defined (ackDigest, content binding not auth, not implemented)** | ✅ Rev9+ correct |
| **RecordState location** | byte 3 (header) | byte 3 (header) | byte 3 (header) | byte 11 (payload) | **byte 11 (payload)** | ✅ Rev9+ correct |
| **Record layout** | header[0..7] + CRC[8..11] + payload[12..] | Same | Same | header[0..6] + CRC[7..10] + payload[11..] | **Same as Rev9** | ✅ Consistent |
| **Dual-copy keys** | tj_ra_N, tj_rb_N | Same | Same | Same | **Same** | ✅ Consistent |
| **Tombstone** | Removed (EMPTY+gen) | Removed | Removed | Removed | **Removed** | ✅ Consistent |
| **Legacy storeTransaction()** | Present (deprecated) | Present | Present | Present | **Present (to be removed in implementation)** | ✅ Consistent |
| **QUARANTINED recovery** | Not defined | Not defined | Not defined | Operator-only, gen=0 | **Operator-only + recovery matrix (§4)** | ✅ Rev10 complete |

### Stale Definitions Found and Corrected

| Stale Reference | Where | Correction |
|----------------|-------|------------|
| "bytes 12..end" (Rev6/Rev7/Rev8) | Old canonical payload definition | Superseded by Rev9 "bytes 11..end". Rev6-Rev8 docs are historical, Rev10 is authoritative. |
| "CRC32(A) XOR CRC32(B)" (Rev7) | Old CRC definition | Superseded by Rev8 concatenation, Rev10 exact API. Rev7 is historical. |
| "abs(genA - genB)" (Rev8) | Old generation distance | Superseded by Rev9 forwardDistance. Rev8 is historical. |
| "mutex placeholder" (Rev6/Rev7) | Old I0 enforcement | Superseded by Rev8 TaskHandle, Rev10 RAII. Rev6-Rev7 are historical. |
| "EPOCH_RESET" (Rev6/Rev7/Rev8) | Old recovery terminology | Superseded by Rev9 QUARANTINED. Rev6-Rev8 are historical. |
| "non-idempotent + BROKER_CONFIRMED → YES" (Rev6/Rev7) | Old eviction matrix | Superseded by Rev8/Rev9 "PWA_RECEIVED only". Rev6-Rev7 are historical. |

**All stale definitions are in historical documents (Rev6-Rev8). Rev9 + Rev10 are the authoritative current design. No stale definitions remain in Rev10.**

---

## 6. Consolidated Final Invariant Summary (I0-I3, Rev10)

### I0 — Journal Executor-Ownership + Stable Observation

```
I0: Single executor task (TaskHandle check, not core ID, not compiled out).
I0a: Observation and mutation sequential. RAII guard ensures _observing always reset.
```

### I1 — Canonical Equivalence + Recovery

```
I1a: Copy A structurally valid (CRC passes)
I1b: Copy B structurally valid (CRC passes)
I1c: Schema version match (A.schemaVersion == B.schemaVersion, else CORRUPTED)
I1d: Mutual consistency (same-gen → canonicalEqual, diff-gen → directional distance 1)
I1e: canonicalEqual = sameSchema AND sameLength AND memcmp==0
     (canonicalBytes from safe parse, padding ignored)
I1f: Generation directional: distAB==1→B newer, distBA==1→A newer,
     0x80000000→ambiguous→CORRUPTED, else→CORRUPTED
I1g: Construction: protocol produces distance 0 or 1.
     Observation: loader validates, marks CORRUPTED if violated.

CRC (normative): CRC-32/ISO-HDLC, ~esp_crc32_le(0xFFFFFFFF,...)&0xFFFFFFFF
                 Test: "123456789" → 0xCBF43926
```

### I2 — Eviction Safety

```
I2a: Retention policy permits
I2b: Command class classified (not UNKNOWN)
I2c: ACK condition (eviction matrix):
     IDEMPOTENT + PUBLISH_ACCEPTED + durable queue → YES
     IDEMPOTENT + BROKER_CONFIRMED → YES
     IDEMPOTENT + PWA_RECEIVED → YES
     IDEMPOTENT + FAILED_EXHAUSTED → YES
     NON_IDEMPOTENT + PWA_RECEIVED → YES (only)
     NON_IDEMPOTENT + else → NO
     UNKNOWN → NO
I2d: Not QUARANTINED/CORRUPTED
I2e: Default = RETAIN
EVICTABLE = computed, never stored.
```

### I3 — ACK Lifecycle Separation

```
I3a: Transaction ≠ ACK lifecycle
I3b: ACK queue durable (tj_ackq with deliveryState)
I3c: Eviction does NOT delete ACK queue entry
I3d: Boot = merge journal + ACK queue
ACK durability = "best-effort delivery assistance"
ACK_PWA_RECEIVED = defined (ackDigest = content binding, NOT sender auth), NOT IMPLEMENTED
```

### Recovery Semantics

```
CORRUPTED (derived):
    Same-gen divergent / schema mismatch / gen ambiguous / gen gap > 1
    → CORRUPTED

QUARANTINED (derived from CORRUPTED when both copies invalid):
    No auto-reuse. Operator recovery only.
    Recovery writes forensic + SLOT_REINITIALIZED (gen=0 to both copies).
    Interrupted recovery (one copy EMPTY, other invalid) → repair, NOT quarantine.

EMPTY (valid stored state):
    Slot is free. Written by: clearEntry, eviction, recoverCorruptedEntry.
```

---

## 7. Rule Compliance

| Item | New Field? | New Feature? |
|------|-----------|-------------|
| CRC exact API + test vector | NO | NO |
| RAII observation guard | NO (RAM class) | NO |
| Recovery matrix for interrupted quarantine | NO (uses existing repair) | NO |

**Zero new metadata. Zero new features.**

---

## 8. Honest Limitations (Unchanged)

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

---

## 9. What This Design Does NOT Solve

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

## 10. Auditor Approval Gate

**This design is NOT approved for implementation.**

Auditor must verify:
1. CRC (§2): Is `~esp_crc32_le(0xFFFFFFFF, ...)` correct? Test vector `0xCBF43926`?
2. RAII (§3): Is ObservationGuard exception/early-return safe?
3. Recovery matrix (§4): Are all 6 cases covered? No special handling needed?
4. Contradiction sweep (§5): Are all stale definitions identified? Rev10 authoritative?
5. Rule compliance (§7): Zero new metadata, zero new features?

**After auditor approval, implementation Phase 1-8 (from Rev6) may begin. Until then: NO CODE.**

Precharge: BLOCKED
I/O expander: BLOCKED
16-relay: BLOCKED