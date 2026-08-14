<!-- SUPERSEDED BANNER -->
<!-- ╔══════════════════════════════════════════════════════════╗ -->
<!-- ║  ⚠️  SUPERSEDED — HISTORICAL DOCUMENT — DO NOT IMPLEMENT  ║ -->
<!-- ║  This document has been superseded by Rev17 for ACK verification ordering,  ║ -->
<!-- ║  retry phase semantics, operator cleanup safety, and state matrix.  ║ -->
<!-- ║  Refer to:                                                ║ -->
<!-- ║    - CYCLE-8C-REV17-ACK-SEMANTICS-CLOSURE.md               ║ -->
<!-- ║  for the authoritative supplement.                          ║ -->
<!-- ║  Rev16 remains authoritative for transition graph (PWA from BROKER only).  ║ -->
<!-- ╚══════════════════════════════════════════════════════════╝ -->
<!-- END SUPERSEDED BANNER -->


# CYCLE-8C-Rev16: Transaction Journal v4 — ACK State Machine & Retention Closure

**Status**: DESIGN ONLY — NO CODE IMPLEMENTATION
**Purpose**: Fix ACK transition graph consistency + ACK queue retention contract.
**Rule**: No new fields, no new features, no architecture changes. Consistency closure only.

---

## 1. Summary

| Fix | Finding | Severity | What Changed |
|-----|---------|----------|-------------|
| #1 | C8CR15-001: ACK transition graph inconsistent with verification contract | P1 | PWA_RECEIVED only from BROKER_CONFIRMED; §2 and §6 aligned |
| #2 | C8CR15-002: ACK queue deletion vs eviction contradiction | P1 | Eviction ≠ ACK deletion; recoverCorruptedEntry ≠ ACK deletion |
| #3 | Cross-check: §2 eligibility == §6 transition graph | — | Verified consistent |

**No new fields. No new features. No architecture changes. No code.**

---

## 2. Fix #1: ACK Transition Graph — Single State Machine (C8CR15-001)

### Problem

Rev15 §2 allowed PWA confirmation when `deliveryState` is one of `ACK_NOT_SENT..ACK_BROKER_CONFIRMED` (i.e., including `ACK_PUBLISH_ACCEPTED`). But Rev15 §6 only defined transition `ACK_BROKER_CONFIRMED → ACK_PWA_RECEIVED`. No `ACK_PUBLISH_ACCEPTED → ACK_PWA_RECEIVED` transition exists. Two contradictory contracts.

### Solution: PWA_RECEIVED Only From BROKER_CONFIRMED

```
ACK STATE MACHINE (NORMATIVE — REV16, SOLE AUTHORITY):

STATES (durable, stored in tj_ackq):
    ACK_NOT_SENT           = 0
    ACK_PUBLISH_ACCEPTED   = 1
    ACK_BROKER_CONFIRMED   = 2  (future, not implemented)
    ACK_PWA_RECEIVED       = 3  (future, not implemented)
    ACK_FAILED_EXHAUSTED   = 4

TRANSITIONS (normative — exhaustive, no implicit transitions):

    ACK_NOT_SENT → ACK_PUBLISH_ACCEPTED:
        Trigger: mqtt.publish() returns true.
        Action: update deliveryState, persist tj_ackq.

    ACK_PUBLISH_ACCEPTED → ACK_BROKER_CONFIRMED:
        Trigger: QoS 1 PUBACK received from broker.
        Action: update deliveryState, persist tj_ackq.
        NOTE: NOT IMPLEMENTED in Rev16. Future cycle.

    ACK_BROKER_CONFIRMED → ACK_PWA_RECEIVED:
        Trigger: ack_confirm message received.
        Precondition: deliveryState == ACK_BROKER_CONFIRMED.
        Verification: requestId match AND commandHash match AND ackDigest match.
        Action: update deliveryState, persist tj_ackq.
        NOTE: NOT IMPLEMENTED in Rev16. Future cycle.

    ACK_NOT_SENT → ACK_FAILED_EXHAUSTED:
        Trigger: MAX_ACK_RETRIES reached without publish success.
        Action: update deliveryState, persist tj_ackq.

    ACK_PUBLISH_ACCEPTED → ACK_FAILED_EXHAUSTED:
        Trigger: MAX_ACK_RETRIES reached without broker confirmation.
        Action: update deliveryState, persist tj_ackq.

    ACK_BROKER_CONFIRMED → ACK_FAILED_EXHAUSTED:
        Trigger: MAX_ACK_RETRIES reached without PWA confirmation.
        Action: update deliveryState, persist tj_ackq.

FORBIDDEN TRANSITIONS (explicitly NOT allowed):
    ACK_NOT_SENT → ACK_BROKER_CONFIRMED     (must go through PUBLISH_ACCEPTED)
    ACK_NOT_SENT → ACK_PWA_RECEIVED         (must go through PUBLISH_ACCEPTED → BROKER_CONFIRMED)
    ACK_PUBLISH_ACCEPTED → ACK_PWA_RECEIVED (must go through BROKER_CONFIRMED)
    ACK_PWA_RECEIVED → any other state      (terminal — PWA has confirmed)
    ACK_FAILED_EXHAUSTED → any other state  (terminal — max retries)

ACK_CONFIRM VERIFICATION (REVISED — ALIGNED WITH §6):

    When ack_confirm message is received:
        1. Find ACK record by requestId in tj_ackq.
        2. If no ACK record found → IGNORE (log warning, no state change).
        3. If ACK record found:
            a. Verify: requestId matches ACK record.
            b. Verify: commandHash matches journal entry for that requestId.
            c. Verify: ackDigest == SHA-256(ackJson)[0:16].
            d. Verify: deliveryState == ACK_BROKER_CONFIRMED.
        4. IF ALL verifications pass AND deliveryState == ACK_BROKER_CONFIRMED:
            → Transition to ACK_PWA_RECEIVED.
            → Persist to tj_ackq.
            → Duplicate: if already ACK_PWA_RECEIVED → no-op (idempotent).
        5. IF ANY verification fails OR deliveryState != ACK_BROKER_CONFIRMED:
            → deliveryState SHALL NOT change.
            → Confirmation message is IGNORED.
            → Log: "ACK_PWA_RECEIVED confirmation rejected (state=<state>, reason=<detail>)".

KEY CHANGE FROM REV15:
    Rev15 §2: "deliveryState is one of ACK_NOT_SENT..ACK_BROKER_CONFIRMED"
    Rev16 §2: "deliveryState == ACK_BROKER_CONFIRMED"
    
    Rev15 allowed PWA confirmation from PUBLISH_ACCEPTED (contradicting §6).
    Rev16 requires BROKER_CONFIRMED before PWA confirmation is accepted.
    
    Since BROKER_CONFIRMED is NOT IMPLEMENTED:
        ACK_PWA_RECEIVED is unreachable (no BROKER_CONFIRMED transition exists).
        Non-idempotent entries are NEVER evictable (no PWA_RECEIVED possible).
        This is CONSISTENT with the eviction matrix.

CONSISTENCY GUARANTEE:
    §2 (verification contract) and §6 (transition graph) now describe
    the SAME state machine. There is exactly ONE path to ACK_PWA_RECEIVED:
        ACK_NOT_SENT → ACK_PUBLISH_ACCEPTED → ACK_BROKER_CONFIRMED → ACK_PWA_RECEIVED
    No shortcuts. No alternative paths.
```

---

## 3. Fix #2: ACK Queue Retention — Eviction ≠ ACK Deletion (C8CR15-002)

### Problem

Rev15 §6 listed:
```
Any state → (dequeued):
    Trigger: clearEntry() / eviction / recoverCorruptedEntry()
    Action: remove from tj_ackq
```

This contradicts Rev14 I3c: "Eviction does NOT delete ACK queue entry." Eviction of journal record should NOT remove the ACK queue entry — the ACK may still need to be delivered.

Similarly, `recoverCorruptedEntry()` should NOT automatically delete ACK queue evidence without proving delivery obligation is satisfied.

### Solution: Separate Journal Eviction from ACK Queue Deletion

```
ACK QUEUE RETENTION CONTRACT (NORMATIVE — REV16):

PRINCIPLE:
    Journal record eviction and ACK queue deletion are SEPARATE operations.
    Evicting a journal record does NOT remove the corresponding ACK queue entry.
    The ACK queue entry persists until:
        (a) ACK is delivered to PWA (ACK_PWA_RECEIVED, then dequeued), OR
        (b) ACK retries are exhausted (ACK_FAILED_EXHAUSTED, then dequeued after retention period), OR
        (c) Operator explicitly clears the ACK queue (manual operation).

JOURNAL EVICTION:
    When a journal record is evicted (clearEntry / LRU eviction):
        - Journal record transitions to EMPTY (generation incremented).
        - ACK queue entry is NOT removed.
        - ACK queue entry remains in tj_ackq with its current deliveryState.
        - If ACK has not been delivered: ACK delivery continues independently.
        - If ACK was already delivered (ACK_PWA_RECEIVED): orphaned ACK entry
          may be cleaned up during periodic ACK queue GC (future cycle).

recoverCorruptedEntry():
    When a journal slot is recovered (both copies INVALID → EMPTY):
        - Journal slot is reinitialized to EMPTY(gen=0).
        - ACK queue entry is NOT automatically removed.
        - The ACK queue entry for the recovered slot's requestId (if known)
          remains in tj_ackq.
        - If the requestId is unknown (both copies were corrupt):
            - The ACK queue may contain an orphaned entry that cannot be matched.
            - This orphaned entry remains until ACK_FAILED_EXHAUSTED or operator cleanup.
            - This is ACCEPTED: better to retain orphaned ACK than to destroy evidence.

ACK QUEUE DELETION (ONLY these cases):
    1. ACK_PWA_RECEIVED + dequeueAck(requestId):
        - PWA has confirmed receipt. ACK is no longer needed.
        - Remove from tj_ackq. Persist.
    
    2. ACK_FAILED_EXHAUSTED + retention period elapsed:
        - Max retries reached. ACK will never be delivered.
        - Remove from tj_ackq after retention period (default: 24 hours from FAILED_EXHAUSTED).
        - This is a FUTURE implementation detail (GC not yet implemented).
    
    3. Operator-initiated ACK queue cleanup:
        - Operator manually clears ACK queue (recovery procedure).
        - This is an explicit operation, NOT automatic.

ACK QUEUE BOOT RECOVERY (REVISED — consistent with retention):
    1. Read tj_ackq from NVS (durable ACK queue with delivery states).
    2. Scan journal for COMMITTED entries with non-empty ackJson.
    3. MERGE:
        - Keep ALL existing tj_ackq entries (including orphaned ones).
        - Add missing entries from journal (COMMITTED with non-empty ackJson
          but no corresponding tj_ackq entry).
    4. Persist merged queue.
    5. Orphaned entries (transaction evicted, ACK not delivered) are RETAINED.

KEY CHANGE FROM REV15:
    Rev15 §6: "Any state → dequeued: Trigger: eviction / recoverCorruptedEntry"
    Rev16 §3: Eviction does NOT dequeue. recoverCorruptedEntry does NOT dequeue.
    
    ACK queue entries are ONLY removed by:
        (a) ACK_PWA_RECEIVED + explicit dequeue, OR
        (b) ACK_FAILED_EXHAUSTED + retention elapsed (future), OR
        (c) Operator cleanup.

CONSISTENCY WITH I3 (ACK LIFECYCLE SEPARATION):
    I3a: Transaction lifecycle independent of ACK lifecycle. ✅
    I3b: ACK queue persists independently (tj_ackq with deliveryState). ✅
    I3c: Eviction does NOT delete ACK queue entry. ✅ (REVISED — was violated by Rev15)
    I3d: Boot recovery = MERGE journal + ACK queue. ✅
```

---

## 4. Cross-Check: §2 Verification Eligibility == §6 Transition Graph

| Verification Precondition (§2) | Transition in §6 | Consistent? |
|-------------------------------|-------------------|-------------|
| deliveryState == ACK_BROKER_CONFIRMED | ACK_BROKER_CONFIRMED → ACK_PWA_RECEIVED | ✅ |
| deliveryState == ACK_PUBLISH_ACCEPTED | (no transition to PWA_RECEIVED) | ✅ (§2 rejects) |
| deliveryState == ACK_NOT_SENT | (no transition to PWA_RECEIVED) | ✅ (§2 rejects) |
| deliveryState == ACK_PWA_RECEIVED | (already PWA_RECEIVED, no-op) | ✅ (idempotent) |
| deliveryState == ACK_FAILED_EXHAUSTED | (terminal, no transition) | ✅ (§2 rejects) |

**§2 and §6 describe the SAME state machine. No contradictions.**

---

## 5. Updated ACK Lifecycle Summary (Rev16 — Sole Authority for ACK Semantics)

```
ACK LIFECYCLE (NORMATIVE — REV16):

STATE MACHINE:
    ACK_NOT_SENT
        ↓ mqtt.publish()==true
    ACK_PUBLISH_ACCEPTED
        ↓ QoS 1 PUBACK (future)
    ACK_BROKER_CONFIRMED
        ↓ ack_confirm + verification (future)
    ACK_PWA_RECEIVED (terminal for ACK purposes)

    Any pre-terminal state
        ↓ MAX_ACK_RETRIES exhausted
    ACK_FAILED_EXHAUSTED (terminal)

DELETION RULES:
    ACK queue entry is removed ONLY by:
        1. ACK_PWA_RECEIVED + explicit dequeueAck()
        2. ACK_FAILED_EXHAUSTED + retention period (future GC)
        3. Operator cleanup (explicit)

    Journal eviction does NOT remove ACK queue entry.
    recoverCorruptedEntry() does NOT remove ACK queue entry.
    clearEntry() does NOT remove ACK queue entry.

RETENTION:
    ACK queue entries persist independently of journal records.
    Orphaned entries (transaction evicted, ACK not delivered) are retained.
    Boot recovery MERGES journal + ACK queue (does not replace either).

IMPLEMENTATION STATUS:
    ACK_NOT_SENT → ACK_PUBLISH_ACCEPTED: IMPLEMENTED (mqtt.publish)
    ACK_PUBLISH_ACCEPTED → ACK_BROKER_CONFIRMED: NOT IMPLEMENTED (future QoS 1)
    ACK_BROKER_CONFIRMED → ACK_PWA_RECEIVED: NOT IMPLEMENTED (future ack_confirm)
    → ACK_FAILED_EXHAUSTED: IMPLEMENTED (max retries)
    
    Since BROKER_CONFIRMED is not implemented: ACK_PWA_RECEIVED is unreachable.
    Non-idempotent entries are NEVER evictable (no PWA_RECEIVED possible).
```

---

## 6. Rule Compliance

| Item | New Field? | New Feature? |
|------|-----------|-------------|
| ACK transition graph alignment | NO (removed contradictory transition) | NO |
| ACK queue retention separation | NO (contract clarification) | NO |
| Cross-check verification | NO (verification) | NO |

**Zero new metadata. Zero new features. Zero architecture changes.**

---

## 7. Honest Limitations (Unchanged from Rev14/Rev15)

1-12: Same as Rev14.

**Added**: ACK_PWA_RECEIVED is unreachable until ACK_BROKER_CONFIRMED is implemented (future cycle). Non-idempotent entries are NEVER evictable.

---

## 8. What This Design Does NOT Solve

(Same as Rev14 — no changes)

---

## 9. Authoritative Document Stack (Rev16)

```
NORMATIVE DOCUMENTS:

    1. CYCLE-8C-REV14-MUTATION-CONSOLIDATION.md
       — Base consolidated document (ALL definitions: I0-I3, CRC, recovery, etc.)
       — SUPERSEDED by Rev15 for 4 fixes (ACK transition, mutation model, terminology, CRC gate)
       — SUPERSEDED by Rev16 for ACK state machine + retention

    2. CYCLE-8C-REV15-ACK-TRANSITION.md
       — Supplement: ACK_PWA_RECEIVED transition, mutation boundary, terminology, CRC gate
       — SUPERSEDED by Rev16 for ACK transition graph + retention contract

    3. CYCLE-8C-REV16-ACK-STATE-MACHINE-CLOSURE.md (THIS DOCUMENT)
       — Supplement: ACK state machine closure + ACK queue retention

PRECEDENCE:
    If Rev14/Rev15 and Rev16 conflict on ACK state machine or retention: Rev16 WINS.
    For all other topics: Rev14 (as supplemented by Rev15) remains authoritative.

ALL OTHER DOCUMENTS:
    Rev6-Rev13: SUPERSEDED (banners applied)
    Rev14: SUPERSEDED by Rev15+Rev16 for specific fixes
    Rev15: SUPERSEDED by Rev16 for ACK state machine + retention
```

---

## 10. Auditor Approval Gate

**This design is NOT approved for implementation.**

Auditor must verify:
1. ACK state machine (§2): PWA_RECEIVED only from BROKER_CONFIRMED? §2 and §6 aligned?
2. ACK queue retention (§3): Eviction ≠ ACK deletion? recoverCorruptedEntry ≠ ACK deletion?
3. Cross-check (§4): §2 eligibility == §6 transition graph? All cases verified?
4. Deletion rules (§5): Only 3 cases (PWA_RECEIVED+dequeue, FAILED+retention, operator)?
5. Rule compliance (§6): Zero new metadata, zero new features?

**After auditor approval, implementation Phase 1-8 may begin. Until then: NO CODE.**

Precharge: BLOCKED
I/O expander: BLOCKED
16-relay: BLOCKED