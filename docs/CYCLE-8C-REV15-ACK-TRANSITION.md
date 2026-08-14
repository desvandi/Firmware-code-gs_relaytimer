<!-- SUPERSEDED BANNER -->
<!-- ╔══════════════════════════════════════════════════════════╗ -->
<!-- ║  ⚠️  SUPERSEDED — HISTORICAL DOCUMENT — DO NOT IMPLEMENT  ║ -->
<!-- ║  This document has been superseded by Rev16 for ACK state machine + retention.  ║ -->
<!-- ║  Refer to:                                                ║ -->
<!-- ║    - CYCLE-8C-REV16-ACK-STATE-MACHINE-CLOSURE.md           ║ -->
<!-- ║  for the authoritative supplement.                          ║ -->
<!-- ║  Rev15 remains authoritative for mutation boundary, terminology, CRC gate.  ║ -->
<!-- ╚══════════════════════════════════════════════════════════╝ -->
<!-- END SUPERSEDED BANNER -->


# CYCLE-8C-Rev15: Transaction Journal v4 — ACK Transition & Mutation Boundary Closure

**Status**: DESIGN ONLY — NO CODE IMPLEMENTATION
**Purpose**: 4 closure items. Rev15 supplements Rev14 (which remains the base consolidated document).
**Rule**: No new fields, no new features, no architecture changes. Closure only.

---

## 1. Summary

| Fix | Finding | Severity | What Changed |
|-----|---------|----------|-------------|
| #1 | C8CR14-001: ACK_PWA_RECEIVED transition missing | P1 | Normative transition + persistence + duplicate idempotency |
| #2 | C8CR14-002: Mutation entry-point contract ambiguous | P1 | Explicit model: public checks, helpers inherit context |
| #3 | C8CR14-003: assert() terminology residue | P2 | All assert() → panic() in normative sections |
| #4 | C8CR14-004: CRC target-vector gate | P2 | Explicit Phase-1 implementation gate |

**No new fields. No new features. No architecture changes. No code.**

---

## 2. Fix #1: ACK_PWA_RECEIVED Normative Transition (P1 — C8CR14-001)

### Problem

Rev14 defined the ACK_PWA_RECEIVED protocol (PWA sends confirmation, device verifies) but did not specify the normative state transition: what happens after verification succeeds? How is it persisted? What about duplicate confirmations?

### Solution: Complete Transition Specification

```
ACK_PWA_RECEIVED TRANSITION (NORMATIVE — REV15):

TRIGGER:
    Device receives MQTT message on topic: timer12/<mac>/ack_confirm
    Payload: { "requestId": "<UUID>", "commandHash": "<hash>", "ackDigest": "<16 hex chars>" }

VERIFICATION (all MUST pass):
    1. requestId matches an entry in the ACK queue (tj_ackq)
    2. commandHash matches the journal entry for that requestId
    3. ackDigest == SHA-256(ackJson)[0:16] (first 16 hex chars of SHA-256 hex string)
    4. ACK record exists (deliveryState is one of ACK_NOT_SENT..ACK_BROKER_CONFIRMED)

IF ALL VERIFICATIONS PASS:
    IF deliveryState is already ACK_PWA_RECEIVED:
        → No-op (duplicate confirmation is idempotent)
        → Log: "duplicate ACK_PWA_RECEIVED confirmation for <requestId> (ignored)"
    ELSE:
        → deliveryState SHALL transition to ACK_PWA_RECEIVED
        → Updated ACK queue record SHALL be durably persisted to tj_ackq
            (putBytes + verify, same as all ACK queue mutations)
        → Eviction eligibility MAY be recomputed for the associated journal slot
        → Log: "ACK_PWA_RECEIVED for <requestId>"

IF ANY VERIFICATION FAILS:
    → deliveryState SHALL NOT change
    → Log: "ACK_PWA_RECEIVED verification FAILED for <requestId> (reason: <detail>)"
    → Confirmation message is IGNORED (not an error, PWA may retry)

DURABILITY CONTRACT:
    The transition to ACK_PWA_RECEIVED is DURABLE — it survives reboot.
    The updated deliveryState is stored in tj_ackq (NVS).
    If device crashes after verification but before persistence:
        → On boot: deliveryState is still previous value (NOT_SENT/PUBLISH_ACCEPTED/BROKER_CONFIRMED)
        → PWA must re-send confirmation (acceptable — PWA initiated the confirmation)

IDEMPOTENCY:
    Duplicate ACK_PWA_RECEIVED confirmations are SAFE (no-op).
    This allows PWA to retry confirmation without risk of side effects.

AUTHENTICATION BOUNDARY (restated from Rev14):
    ackDigest is CONTENT BINDING, NOT sender authentication.
    Anyone who knows ackJson can compute ackDigest.
    Sender authentication requires MQTT ACL or HMAC (future cycle).
    ACK_PWA_RECEIVED with ackDigest alone is sufficient for IDEMPOTENT eviction.
    ACK_PWA_RECEIVED with ackDigest alone is NOT sufficient for NON_IDEMPOTENT eviction
    (requires additional sender authentication — future cycle).

NOT IMPLEMENTED:
    This protocol is DEFINED but NOT IMPLEMENTED in Rev15.
    Implementation deferred to future cycle after journal v4 is proven.
    Until implemented: ACK_PWA_RECEIVED never transitions to true.
    Non-idempotent entries are NEVER evictable (consistent with eviction matrix).
```

---

## 3. Fix #2: Mutation Entry-Point Enforcement Model (P1 — C8CR14-002)

### Problem

Rev14 listed 10 functions that must call `_assertMutationAllowed()`, mixing public APIs, private helpers, and low-level NVS functions. No explicit model for which layer enforces what.

### Solution: Explicit Layered Enforcement

```
MUTATION ENTRY-POINT CONTRACT (NORMATIVE — REV15):

MODEL: Public-API enforcement, helpers inherit context.

LAYER 1 — PUBLIC MUTATION APIs:
    Every public mutation API function MUST call BOTH:
        _assertExecutorContext()     (I0: correct task)
        _assertMutationAllowed()     (I0a: not during observation)
    at entry, BEFORE any read or write operation.

    Public mutation APIs (exhaustive list):
        storeIntent()
        markExecuting()
        commitTransaction()
        commitTransactionFailed()
        clearEntry()
        recoverCorruptedEntry()

LAYER 2 — PRIVATE MUTATION HELPERS:
    Private helpers that mutate journal state (NVS or RAM) MUST NOT be
    callable independently from outside the journal module.

    They MAY rely on the mutation context established by the calling
    public API (which has already called _assertExecutorContext and
    _assertMutationAllowed).

    Private mutation helpers (not independently callable):
        _repairSlot()
        _writeCopy()
        _commitExecutingEntryNVS()
        _createPendingEntryNVS()
        _markCorruptedNVS()

    These functions do NOT need to call _assertMutationAllowed() themselves
    because they are ONLY called from Layer 1 functions (which already checked).

    However, they MUST be declared private (not accessible from outside
    TransactionJournal class). Code review verifies no external caller.

LAYER 3 — LOW-LEVEL NVS HELPERS:
    Low-level NVS operations (Preferences::putBytes, putUChar, remove) are
    implementation details of Layer 2 helpers.

    They MUST NOT establish or bypass journal mutation authority.
    They are NOT callable from outside the journal module.
    They do NOT check _assertMutationAllowed() (Layer 2 is responsible).

    Low-level helpers:
        _eraseBlobNVS()
        _clearSlotNVS()
        _writeTombstoneNVS() [removed — tombstone eliminated, listed for historical reference]
        _setTransactionStateNVS()

CONTRACT SUMMARY:
    Only Layer 1 (public APIs) enforce I0 + I0a.
    Layer 2 and Layer 3 inherit the context from Layer 1.
    No Layer 2 or Layer 3 function is publicly accessible.
    Code review MUST verify: no external code calls Layer 2/3 directly.

    If future code adds a new public mutation API:
        It MUST call _assertExecutorContext() and _assertMutationAllowed() at entry.
        This is a design rule, not an implementation detail.

DEFENSE-IN-DEPTH (OPTIONAL):
    If implementation wants additional safety, Layer 2 helpers MAY also call
    _assertMutationAllowed(). This is OPTIONAL, not required.
    The normative contract is: Layer 1 enforces, Layer 2/3 inherit.

NO NEW METADATA:
    No new NVS keys, no new record fields.
    Enforcement is via function calls (no storage).
```

---

## 4. Fix #3: Remove All assert() Terminology (P2 — C8CR14-003)

### Problem

Rev14 correctly stated "panic(), NOT assert()" in §2, but the recovery contract still said "Runtime assertion: assert(!validA && !validB)."

### Solution: Replace ALL assert() with panic()

```
REV15 TERMINOLOGY RULE:

    The word "assert" and the function "assert()" SHALL NOT appear
    in any normative contract section of Rev14 or Rev15.

    All runtime enforcement uses panic():
        if (condition_violated) {
            panic("descriptive message");
        }

    panic() is NOT compilable out (unlike assert() which can be disabled
    with NDEBUG). It always executes in both debug and release builds.

SPECIFIC REPLACEMENTS:

    Rev14 recovery contract:
        OLD: "Runtime assertion: assert(!validA && !validB) at entry (panic if violated)."
        NEW: "Runtime precondition check at entry:
              if (validA || validB) {
                  panic('recoverCorruptedEntry requires both copies INVALID');
              }"

    Rev14 ObservationGuard constructor:
        Already uses panic() — no change needed.

    Rev14 _assertMutationAllowed():
        Already uses panic() — no change needed.
        (Function name contains "assert" but implementation uses panic().
         This is acceptable — the function NAME is an implementation detail.
         The normative CONTRACT states panic(), not assert().)

    Rev14 _assertExecutorContext():
        Already uses panic() — no change needed.
        (Same rationale as above.)

    Rev14 I1g observation:
        OLD: "loader validates, marks CORRUPTED if violated"
        NEW: "loader validates at runtime; if violated, marks slot CORRUPTED
              (via _markCorruptedNVS which uses putUChar, not assert/panic)"

    The function names _assertExecutorContext() and _assertMutationAllowed()
    are implementation names, NOT normative terminology.
    The normative contract says "runtime enforcement via panic()."
```

---

## 5. Fix #4: CRC Target-Vector as Explicit Phase-1 Gate (P2 — C8CR14-004)

### Problem

Rev14 stated "target-API verification required during Phase 1" but did not make it an explicit implementation gate (a pass/fail criterion that blocks implementation if it fails).

### Solution: Explicit Gate

```
CRC IMPLEMENTATION GATE (NORMATIVE — REV15):

    The CRC contract is normative and fixed (CRC-32/ISO-HDLC, test vector 0xCBF43926).

    Phase 1 implementation MUST include the following gate:

    GATE: CRC_TARGET_VECTOR_VERIFY

    Procedure:
        1. Build firmware for target ESP32 with the exact ESP-IDF version
           specified in platformio.ini.
        2. Flash to device (or run in simulator if ESP-IDF supports it).
        3. Execute:
               uint8_t test[] = "123456789";
               uint32_t result = ~esp_crc32_le(0xFFFFFFFF, test, 9) & 0xFFFFFFFF;
        4. Verify: result == 0xCBF43926

    PASS:
        result == 0xCBF43926
        → CRC contract is verified against target API.
        → Implementation may proceed to Phase 2.

    FAIL:
        result != 0xCBF43926
        → ESP-IDF version mismatch or API behavior differs.
        → STOP implementation.
        → Investigate: check ESP-IDF version, check esp_crc32_le() signature,
          check if function is rom_ or esp_ prefixed.
        → Update design document if API has changed.
        → Re-audit updated design before proceeding.

    This gate is NON-NEGOTIABLE.
    Implementation MUST NOT proceed past Phase 1 without passing this gate.

    The gate is a BUILD-TIME test (not a runtime invariant).
    It is executed once during Phase 1 and documented in the implementation log.
    It does NOT need to run on every boot (CRC is a write-time computation,
    not a boot-time check).
```

---

## 6. Consolidated ACK Lifecycle (Updated with Transition)

```
ACK LIFECYCLE (NORMATIVE — REV15, SUPERSEDES REV14 I3):

STATES (durable, stored in tj_ackq):
    ACK_NOT_SENT           = 0  — ACK not yet attempted
    ACK_PUBLISH_ACCEPTED   = 1  — mqtt.publish() returned true
    ACK_BROKER_CONFIRMED   = 2  — QoS 1 PUBACK received (future)
    ACK_PWA_RECEIVED       = 3  — application-level confirmation received
    ACK_FAILED_EXHAUSTED   = 4  — max retries reached, give up

TRANSITIONS (normative):

    ACK_NOT_SENT → ACK_PUBLISH_ACCEPTED:
        Trigger: mqtt.publish() returns true
        Action: update deliveryState, persist tj_ackq

    ACK_PUBLISH_ACCEPTED → ACK_BROKER_CONFIRMED:
        Trigger: QoS 1 PUBACK received from broker (future, not implemented)
        Action: update deliveryState, persist tj_ackq

    ACK_BROKER_CONFIRMED → ACK_PWA_RECEIVED:
        Trigger: ack_confirm message received, all verifications pass (§2)
        Action: update deliveryState, persist tj_ackq
        Duplicate: if already ACK_PWA_RECEIVED → no-op (idempotent)

    ACK_NOT_SENT → ACK_FAILED_EXHAUSTED:
        Trigger: max retries (MAX_ACK_RETRIES) reached without publish success
        Action: update deliveryState, persist tj_ackq

    ACK_PUBLISH_ACCEPTED → ACK_FAILED_EXHAUSTED:
        Trigger: max retries reached without broker confirmation
        Action: update deliveryState, persist tj_ackq

    Any state → (dequeued):
        Trigger: clearEntry() / eviction / recoverCorruptedEntry()
        Action: remove from tj_ackq, persist

VERIFICATION FAILURES:
    If ack_confirm verification fails (requestId mismatch, commandHash mismatch,
    ackDigest mismatch, or ACK record not found):
        → deliveryState SHALL NOT change
        → Confirmation message is IGNORED
        → PWA may retry

DURABILITY:
    Every state transition is DURABLE (persisted to tj_ackq via putBytes + verify).
    If device crashes after transition but before persistence:
        → Previous state is retained (transition not yet durable)
        → Triggering event must be re-sent (acceptable — PWA or broker initiates)
```

---

## 7. Updated Authoritative Document Stack

```
NORMATIVE DOCUMENTS (REV15):

    1. CYCLE-8C-REV14-MUTATION-CONSOLIDATION.md
       — Base consolidated document (ALL definitions: I0-I3, CRC, recovery, etc.)
       — SUPERSEDED banner applied (points to Rev15 for the 4 fixes)

    2. CYCLE-8C-REV15-ACK-TRANSITION.md (THIS DOCUMENT)
       — Supplement with 4 closure fixes:
         ACK_PWA_RECEIVED transition, mutation boundary, assert→panic, CRC gate

PRECEDENCE:
    If Rev14 and Rev15 conflict on any topic: Rev15 WINS.
    Rev15 supersedes Rev14 for:
        - ACK_PWA_RECEIVED transition semantics (§2, §6)
        - Mutation entry-point enforcement model (§3)
        - assert()/panic() terminology (§4)
        - CRC implementation gate (§5)
    All other topics: Rev14 remains authoritative.

ALL OTHER DOCUMENTS:
    Rev6-Rev13: SUPERSEDED (banners applied)
    Rev14: SUPERSEDED by Rev15 for the 4 specific fixes above.
           Otherwise remains base consolidated document.
```

---

## 8. Rule Compliance

| Item | New Field? | New Feature? |
|------|-----------|-------------|
| ACK_PWA_RECEIVED transition | NO (uses existing deliveryState field) | NO (protocol defined, not implemented) |
| Mutation enforcement model | NO (function call organization) | NO |
| assert→panic terminology | NO (documentation) | NO |
| CRC gate | NO (test only) | NO |

**Zero new metadata. Zero new features. Zero architecture changes.**

---

## 9. Honest Limitations (Unchanged from Rev14)

1-12: Same as Rev14.

**Added**: ACK_PWA_RECEIVED protocol is fully defined (transition + persistence + idempotency) but NOT IMPLEMENTED. Implementation deferred to future cycle.

---

## 10. What This Design Does NOT Solve

(Same as Rev14 — no changes)

---

## 11. Auditor Approval Gate

**This design is NOT approved for implementation.**

Auditor must verify:
1. ACK_PWA_RECEIVED transition (§2): Normative? Durable? Idempotent? Verification failure = no-op?
2. Mutation entry-point model (§3): Layer 1 checks, Layer 2/3 inherit? Exhaustive public API list?
3. assert→panic (§4): No "assert()" in normative sections? All use panic()?
4. CRC gate (§5): Explicit pass/fail criterion? Non-negotiable? Blocks Phase 2 if failed?
5. ACK lifecycle (§6): All transitions specified? Durable? Duplicate handling?
6. Rule compliance (§8): Zero new metadata, zero new features?

**After auditor approval, implementation Phase 1-8 may begin. Until then: NO CODE.**

Precharge: BLOCKED
I/O expander: BLOCKED
16-relay: BLOCKED