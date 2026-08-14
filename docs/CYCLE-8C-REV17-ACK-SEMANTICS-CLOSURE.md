<!-- SUPERSEDED BANNER -->
<!-- ╔══════════════════════════════════════════════════════════╗ -->
<!-- ║  ⚠️  SUPERSEDED — HISTORICAL DOCUMENT — DO NOT IMPLEMENT  ║ -->
<!-- ║  Superseded by Rev18 for: ACK cleanup predicate, retryCount definition,  ║ -->
<!-- ║  ACK re-publication invariant.                                            ║ -->
<!-- ║  Refer to:                                                ║ -->
<!-- ║    - CYCLE-8C-REV18-CLEANUP-BOUNDARY-SWEEP.md              ║ -->
<!-- ║  for the authoritative supplement.                          ║ -->
<!-- ║  Rev17 remains authoritative for verification ordering and retry phases.  ║ -->
<!-- ╚══════════════════════════════════════════════════════════╝ -->
<!-- END SUPERSEDED BANNER -->


# CYCLE-8C-Rev17: Transaction Journal v4 — ACK Semantics Closure

**Status**: DESIGN ONLY — NO CODE IMPLEMENTATION
**Purpose**: Close ACK state machine semantics contradictions. No new fields, no features.
**Rule**: Consistency closure only. No new metadata, no new states, no new features.

---

## 1. Summary

| Fix | Finding | Severity | What Changed |
|-----|---------|----------|-------------|
| #1 | C8CR16-001: Duplicate PWA confirmation contradiction | P1 | Explicit verification ordering — PWA_RECEIVED checked first (no-op) |
| #2 | C8CR16-002: BROKER_CONFIRMED → FAILED_EXHAUSTED semantics | P1 | Two retry phases with distinct counters |
| #3 | Retry/timeout semantics without new metadata | P1 | Defined using existing retryCount field + phase semantics |
| #4 | Operator ACK cleanup safety boundary | P2 | Non-idempotent ACK with unresolved delivery: cleanup blocked |
| #5 | Complete state × transition × retention matrix | — | Exhaustive table |

**No new fields. No new states. No new features. No architecture changes. No code.**

---

## 2. Fix #1: Duplicate PWA Confirmation — Explicit Verification Ordering (C8CR16-001)

### Problem

Rev16 §2 had two contradictory behaviors for ACK_PWA_RECEIVED + duplicate confirmation:
- "Duplicate: if already ACK_PWA_RECEIVED → no-op (idempotent)"
- "IF ANY verification fails OR deliveryState != ACK_BROKER_CONFIRMED → IGNORE"

When `deliveryState == ACK_PWA_RECEIVED`, the second rule says "IGNORE" (because `!= ACK_BROKER_CONFIRMED`), but the first says "no-op". Contradiction.

### Solution: Explicit Verification Ordering

```
ACK_CONFIRM VERIFICATION (NORMATIVE — REV17, SOLE AUTHORITY):

When ack_confirm message is received, execute in EXACTLY this order:

    STEP 1: Find ACK record by requestId in tj_ackq.
        If NOT found → log warning, return. (No state change.)

    STEP 2: Verify requestId matches ACK record.
        If mismatch → log warning, return. (No state change.)

    STEP 3: Check if deliveryState == ACK_PWA_RECEIVED.
        If YES → NO-OP. Log "duplicate PWA confirmation (ignored)".
                 Return. (No state change. This is idempotent success, NOT failure.)
        If NO → continue to step 4.

    STEP 4: Verify commandHash matches journal entry for that requestId.
        If mismatch → log "commandHash mismatch", return. (No state change.)

    STEP 5: Verify ackDigest == SHA-256(ackJson)[0:16].
        If mismatch → log "ackDigest mismatch", return. (No state change.)

    STEP 6: Require deliveryState == ACK_BROKER_CONFIRMED.
        If deliveryState != ACK_BROKER_CONFIRMED → log "not in BROKER_CONFIRMED state",
            return. (No state change. Not a failure — PWA sent confirmation too early.)

    STEP 7: All verifications passed AND deliveryState == ACK_BROKER_CONFIRMED.
        → Transition: deliveryState → ACK_PWA_RECEIVED.
        → Persist to tj_ackq (putBytes + verify).
        → Log "ACK_PWA_RECEIVED for <requestId>".
        → Return.

KEY CHANGE FROM REV16:
    Rev16 checked "deliveryState != ACK_BROKER_CONFIRMED" BEFORE checking if already PWA_RECEIVED.
    Rev17 checks "already PWA_RECEIVED" FIRST (step 3), before the BROKER_CONFIRMED gate (step 6).

    Result:
        ACK_PWA_RECEIVED + duplicate confirmation → NO-OP (step 3 returns early).
        ACK_BROKER_CONFIRMED + valid confirmation → transition (step 7).
        Any other state + confirmation → rejected (step 6), NOT treated as duplicate.
        Any verification mismatch → rejected (steps 2, 4, 5).

NO CONTRADICTION:
    Step 3 handles the "already received" case explicitly and returns before
    step 6 (which would reject for "not in BROKER_CONFIRMED state").
    This makes duplicate confirmation a clean no-op, not a rejection.
```

---

## 3. Fix #2: BROKER_CONFIRMED → FAILED_EXHAUSTED — Two Retry Phases (C8CR16-002)

### Problem

Rev16 used `MAX_ACK_RETRIES` for all transitions to `ACK_FAILED_EXHAUSTED`, including `ACK_BROKER_CONFIRMED → ACK_FAILED_EXHAUSTED`. But after broker confirmed, the retry obligation changes from "publish delivery" to "PWA confirmation". Same counter for different semantics.

### Solution: Two Retry Phases Using Existing Fields

```
ACK RETRY SEMANTICS (NORMATIVE — REV17):

The existing `retryCount` field in AckRecord is reused with phase-specific semantics.
No new fields are added. The phase is determined by `deliveryState`.

PHASE 1 — PUBLISH DELIVERY (deliveryState: NOT_SENT or PUBLISH_ACCEPTED):
    Goal: Get ACK published and accepted by broker.
    Retry action: mqtt.publish() (re-send ACK to MQTT broker).
    retryCount: incremented on each publish attempt.
    Max retries: MAX_ACK_RETRIES (10, existing constant).
    
    Transitions:
        NOT_SENT → PUBLISH_ACCEPTED (publish succeeded)
        NOT_SENT → FAILED_EXHAUSTED (retryCount >= MAX_ACK_RETRIES)
        PUBLISH_ACCEPTED → BROKER_CONFIRMED (PUBACK received, future)
        PUBLISH_ACCEPTED → FAILED_EXHAUSTED (retryCount >= MAX_ACK_RETRIES,
                                              no PUBACK within timeout)

PHASE 2 — PWA CONFIRMATION (deliveryState: BROKER_CONFIRMED):
    Goal: Get PWA to confirm receipt via ack_confirm.
    Retry action: RE-PUBLISH ACK (mqtt.publish() again, PWA may have missed it).
    retryCount: RESET to 0 when entering Phase 2 (BROKER_CONFIRMED).
    Max retries: MAX_ACK_RETRIES (10, same constant, fresh counter).
    
    Transitions:
        BROKER_CONFIRMED → PWA_RECEIVED (ack_confirm + verification passes)
        BROKER_CONFIRMED → FAILED_EXHAUSTED (retryCount >= MAX_ACK_RETRIES,
                                               no PWA confirmation received)

PHASE 2 RESET:
    When deliveryState transitions from PUBLISH_ACCEPTED to BROKER_CONFIRMED:
        retryCount SHALL be reset to 0.
        This is NOT a new field — retryCount already exists.
        The reset is a state-transition side effect, not metadata addition.
    
    Rationale: Phase 2 has a different retry obligation (waiting for PWA, not
    waiting for broker). A fresh counter is appropriate because the failure mode
    is different (PWA may be offline, not a publish failure).

IMPLEMENTATION:
    When processPendingAcks() processes a BROKER_CONFIRMED entry:
        If retryCount < MAX_ACK_RETRIES:
            Re-publish ACK via mqtt.publish().
            Increment retryCount.
            Update lastAttemptTs.
            Persist to tj_ackq.
        If retryCount >= MAX_ACK_RETRIES:
            Transition to ACK_FAILED_EXHAUSTED.
            Persist to tj_ackq.

NO NEW METADATA:
    retryCount: existing field in AckRecord (byte offset in record, already defined).
    Phase is derived from deliveryState (NOT stored separately).
    No new enum, no new field, no new NVS key.

WHY THIS IS CORRECT:
    ACK_BROKER_CONFIRMED means: broker has the ACK.
    But PWA may not have received it (broker delivered to subscriber, PWA was offline).
    Re-publishing the ACK gives PWA another chance to receive it.
    After MAX_ACK_RETRIES re-publishes without PWA confirmation: give up.
    The ACK is retained in queue as FAILED_EXHAUSTED (not deleted immediately).
    PWA can still re-query /status to learn transaction result.
```

---

## 4. Fix #3: Retry/Timeout Semantics (P1)

### Problem

Rev16 did not define when retries occur, how timeout is calculated, or what triggers the transition to FAILED_EXHAUSTED.

### Solution: Explicit Retry Protocol

```
ACK RETRY PROTOCOL (NORMATIVE — REV17):

EXISTING CONSTANTS (from Rev14, no change):
    MAX_ACK_RETRIES = 10
    ACK_RETRY_INTERVAL_MS = 2000 (2 seconds)

PROCESSING (in loop(), via processPendingAcks()):

    For each ACK record in tj_ackq (iterating from oldest to newest):

        If deliveryState == ACK_PWA_RECEIVED:
            → Skip (terminal, waiting for explicit dequeue).

        If deliveryState == ACK_FAILED_EXHAUSTED:
            → Skip (terminal, waiting for retention period or operator cleanup).

        If deliveryState == ACK_NOT_SENT or ACK_PUBLISH_ACCEPTED:
            → PHASE 1: publish delivery retry.
            If (now - lastAttemptTs) >= ACK_RETRY_INTERVAL_MS:
                If retryCount >= MAX_ACK_RETRIES:
                    → Transition to ACK_FAILED_EXHAUSTED.
                    → Persist tj_ackq.
                Else:
                    → mqtt.publish(ackJson).
                    → If publish returns true:
                        → If deliveryState == ACK_NOT_SENT:
                            → Transition to ACK_PUBLISH_ACCEPTED.
                        → Increment retryCount.
                        → Update lastAttemptTs.
                        → Persist tj_ackq.
                    → If publish returns false:
                        → Increment retryCount.
                        → Update lastAttemptTs.
                        → Persist tj_ackq.
                        → (Will retry again next interval.)

        If deliveryState == ACK_BROKER_CONFIRMED:
            → PHASE 2: PWA confirmation retry.
            If (now - lastAttemptTs) >= ACK_RETRY_INTERVAL_MS:
                If retryCount >= MAX_ACK_RETRIES:
                    → Transition to ACK_FAILED_EXHAUSTED.
                    → Persist tj_ackq.
                Else:
                    → Re-publish ACK via mqtt.publish(ackJson).
                    → Increment retryCount.
                    → Update lastAttemptTs.
                    → Persist tj_ackq.

PHASE TRANSITION:
    When ACK_PUBLISH_ACCEPTED → ACK_BROKER_CONFIRMED:
        retryCount = 0 (reset for Phase 2).
        lastAttemptTs = current time.
        Persist tj_ackq.

NOT IMPLEMENTED:
    ACK_BROKER_CONFIRMED transition (QoS 1 PUBACK): future cycle.
    ACK_PWA_RECEIVED transition (ack_confirm): future cycle.
    
    Currently implemented:
        NOT_SENT → PUBLISH_ACCEPTED → (FAILED_EXHAUSTED or stuck at PUBLISH_ACCEPTED)
    
    Future implementation will add:
        PUBLISH_ACCEPTED → BROKER_CONFIRMED → PWA_RECEIVED or FAILED_EXHAUSTED

NO NEW METADATA:
    retryCount: existing field, reused with phase-specific semantics.
    lastAttemptTs: existing field, reused.
    Phase: derived from deliveryState (not stored).
    No new fields, no new constants.
```

---

## 5. Fix #4: Operator ACK Cleanup Safety Boundary (P2)

### Problem

Rev16 listed "operator cleanup" as one of three ACK queue deletion mechanisms, but did not define safety boundaries. Operator could delete non-idempotent ACK with unresolved delivery obligation.

### Solution: Safety Boundary

```
OPERATOR ACK CLEANUP SAFETY BOUNDARY (NORMATIVE — REV17):

Operator-initiated ACK queue cleanup SHALL observe the following safety rules:

    1. ACK records with deliveryState == ACK_PWA_RECEIVED:
        → MAY be cleaned up (delivery confirmed, no obligation remains).

    2. ACK records with deliveryState == ACK_FAILED_EXHAUSTED:
        → MAY be cleaned up (max retries reached, delivery will never succeed).

    3. ACK records with deliveryState == ACK_BROKER_CONFIRMED:
        → MAY be cleaned up ONLY IF:
            (a) Associated journal entry has been evicted (EMPTY), AND
            (b) Operator acknowledges that PWA may not have received the ACK.
        → If associated journal entry is NOT evicted (still COMMITTED):
            Cleanup is BLOCKED for NON_IDEMPOTENT commands.
            Cleanup is ALLOWED for IDEMPOTENT commands (PWA can re-query /status).

    4. ACK records with deliveryState == ACK_PUBLISH_ACCEPTED:
        → Same rules as ACK_BROKER_CONFIRMED (above).

    5. ACK records with deliveryState == ACK_NOT_SENT:
        → MAY be cleaned up (ACK was never attempted, no delivery obligation).

CONTRACT:
    Operator cleanup of NON_IDEMPOTENT ACK with unresolved delivery obligation
    (deliveryState is NOT_SENT, PUBLISH_ACCEPTED, or BROKER_CONFIRMED) and
    journal entry is still COMMITTED (not evicted):
        → BLOCKED.
        → Operator must first evict the journal entry (if eligible) or
          wait for ACK_FAILED_EXHAUSTED.

    This prevents silent destruction of delivery evidence for non-idempotent
    commands that have been executed but not yet confirmed by PWA.
```

---

## 6. Fix #5: Complete State × Transition × Retention Matrix

```
COMPLETE ACK STATE × TRANSITION × RETENTION MATRIX (NORMATIVE — REV17):

| State                   | Entry From              | Exit To               | Trigger                          | Retry Phase | Persist? | Retention           |
|-------------------------|-------------------------|-----------------------|----------------------------------|-------------|----------|---------------------|
| ACK_NOT_SENT            | queueAck()              | ACK_PUBLISH_ACCEPTED  | mqtt.publish()==true             | Phase 1     | Yes      | Until terminal       |
| ACK_NOT_SENT            | queueAck()              | ACK_FAILED_EXHAUSTED  | retryCount >= MAX (Phase 1)      | Phase 1     | Yes      | Until retention/operator |
| ACK_PUBLISH_ACCEPTED    | NOT_SENT                | ACK_BROKER_CONFIRMED  | PUBACK received (future)         | Phase 1     | Yes      | Until terminal       |
| ACK_PUBLISH_ACCEPTED    | NOT_SENT                | ACK_FAILED_EXHAUSTED  | retryCount >= MAX (Phase 1)      | Phase 1     | Yes      | Until retention/operator |
| ACK_BROKER_CONFIRMED    | PUBLISH_ACCEPTED        | ACK_PWA_RECEIVED      | ack_confirm + verification (future) | Phase 2 (retryCount reset) | Yes | Until terminal |
| ACK_BROKER_CONFIRMED    | PUBLISH_ACCEPTED        | ACK_FAILED_EXHAUSTED  | retryCount >= MAX (Phase 2)      | Phase 2     | Yes      | Until retention/operator |
| ACK_PWA_RECEIVED        | BROKER_CONFIRMED        | (dequeued)            | dequeueAck()                     | Terminal    | Yes (on dequeue) | N/A (removed) |
| ACK_FAILED_EXHAUSTED    | any pre-terminal        | (dequeued)            | retention period elapsed / operator cleanup | Terminal | N/A (on dequeue) | Until retention/operator |

DUPLICATE HANDLING:
| Current State            | Event                        | Result     |
|--------------------------|------------------------------|------------|
| ACK_PWA_RECEIVED         | ack_confirm (duplicate)      | NO-OP (step 3, idempotent) |
| ACK_BROKER_CONFIRMED     | ack_confirm (valid)          | → PWA_RECEIVED |
| ACK_BROKER_CONFIRMED     | ack_confirm (invalid)        | IGNORED (no state change) |
| ACK_PUBLISH_ACCEPTED     | ack_confirm                  | IGNORED (step 6, not BROKER_CONFIRMED) |
| ACK_NOT_SENT             | ack_confirm                  | IGNORED (step 6) |
| ACK_FAILED_EXHAUSTED     | ack_confirm                  | IGNORED (step 6) |

DELETION RULES:
| Trigger                          | Removes ACK entry? | Safety check |
|----------------------------------|--------------------|--------------|
| ACK_PWA_RECEIVED + dequeueAck() | YES                | None (delivery confirmed) |
| ACK_FAILED_EXHAUSTED + retention | YES (future GC)   | None (max retries reached) |
| Operator cleanup + PWA_RECEIVED  | YES                | Allowed (delivery confirmed) |
| Operator cleanup + FAILED        | YES                | Allowed (max retries) |
| Operator cleanup + NOT_SENT      | YES                | Allowed (never sent) |
| Operator cleanup + PUBLISH_ACCEPTED (idempotent, journal evicted) | YES | Allowed |
| Operator cleanup + PUBLISH_ACCEPTED (non-idempotent, journal COMMITTED) | BLOCKED | Non-idempotent unresolved |
| Operator cleanup + BROKER_CONFIRMED (non-idempotent, journal COMMITTED) | BLOCKED | Non-idempotent unresolved |
| Journal eviction                 | NO                 | N/A (retention independent) |
| recoverCorruptedEntry()          | NO                 | N/A (retention independent) |
| clearEntry()                     | NO                 | N/A (retention independent) |

PHASE TRANSITIONS:
| From                  | To                    | retryCount action |
|-----------------------|-----------------------|--------------------|
| NOT_SENT → PUBLISH_ACCEPTED | (same phase)     | increment          |
| PUBLISH_ACCEPTED → BROKER_CONFIRMED | Phase 1→2  | RESET to 0         |
| BROKER_CONFIRMED → PWA_RECEIVED | (terminal)     | (no further retries) |
| BROKER_CONFIRMED → FAILED_EXHAUSTED | (terminal)  | (no further retries) |
```

---

## 7. Rule Compliance

| Item | New Field? | New Feature? |
|------|-----------|-------------|
| Verification ordering (step 3 before step 6) | NO (algorithm) | NO |
| Two retry phases | NO (existing retryCount, phase derived from state) | NO |
| Retry protocol | NO (existing constants) | NO |
| Operator cleanup safety | NO (contract) | NO |
| State × transition × retention matrix | NO (documentation) | NO |

**Zero new metadata. Zero new features. Zero architecture changes.**

---

## 8. Authoritative Document Stack (Rev17)

```
NORMATIVE DOCUMENTS:

    1. CYCLE-8C-REV14-MUTATION-CONSOLIDATION.md
       — Base consolidated document (ALL definitions: I0-I3, CRC, recovery, etc.)

    2. CYCLE-8C-REV15-ACK-TRANSITION.md
       — Supplement: mutation boundary, terminology, CRC gate

    3. CYCLE-8C-REV16-ACK-STATE-MACHINE-CLOSURE.md
       — Supplement: ACK transition graph (PWA from BROKER only), retention separation

    4. CYCLE-8C-REV17-ACK-SEMANTICS-CLOSURE.md (THIS DOCUMENT)
       — Supplement: verification ordering, retry phases, operator safety, matrix

PRECEDENCE:
    For ACK state machine, transitions, retry, retention: Rev17 WINS.
    For all other topics: Rev14 (as supplemented by Rev15/Rev16) remains authoritative.

ALL OTHER DOCUMENTS: SUPERSEDED (banners applied)
```

---

## 9. Honest Limitations (Unchanged + Added)

1-12: Same as Rev14.

**Added**:
13. ACK_BROKER_CONFIRMED transition (QoS 1 PUBACK) is NOT IMPLEMENTED.
14. ACK_PWA_RECEIVED transition (ack_confirm) is NOT IMPLEMENTED.
15. Until both are implemented: ACK_PWA_RECEIVED is unreachable.
16. Non-idempotent entries are NEVER evictable.

---

## 10. Auditor Approval Gate

**This design is NOT approved for implementation.**

Auditor must verify:
1. Verification ordering (§2): Step 3 (PWA_RECEIVED check) before step 6 (BROKER_CONFIRMED gate)?
2. Two retry phases (§3): Phase 1 (publish) and Phase 2 (PWA confirmation)? retryCount reset on phase transition?
3. Retry protocol (§4): Explicit when/what/how for each phase? No new constants?
4. Operator safety (§5): Non-idempotent + unresolved + COMMITTED → cleanup BLOCKED?
5. State × transition × retention matrix (§6): Exhaustive? All states × all transitions × all deletion rules?
6. Duplicate handling (§6): PWA_RECEIVED + duplicate → NO-OP (not rejection)?
7. Rule compliance (§7): Zero new metadata, zero new features?

**After auditor approval, implementation Phase 1-8 may begin. Until then: NO CODE.**

Precharge: BLOCKED
I/O expander: BLOCKED
16-relay: BLOCKED