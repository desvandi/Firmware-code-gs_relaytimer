<!-- SUPERSEDED BANNER -->
<!-- ╔══════════════════════════════════════════════════════════╗ -->
<!-- ║  ⚠️  SUPERSEDED — HISTORICAL DOCUMENT — DO NOT IMPLEMENT  ║ -->
<!-- ║  Superseded by Rev20 for: auth gate restoration, cross-product correction,  ║ -->
<!-- ║  lastAttemptTs deterministic timing, expanded cross-product sweep.  ║ -->
<!-- ║  Refer to:                                                ║ -->
<!-- ║    - CYCLE-8C-REV20-AUTH-REGRESSION-CLOSURE.md             ║ -->
<!-- ║  for the authoritative supplement.                          ║ -->
<!-- ║  Rev19 remains authoritative for cleanup≠eviction and publish≠PUBACK.  ║ -->
<!-- ╚══════════════════════════════════════════════════════════╝ -->
<!-- END SUPERSEDED BANNER -->


# CYCLE-8C-Rev19: Transaction Journal v4 — Semantic Boundary Closure

**Status**: DESIGN ONLY — NO CODE IMPLEMENTATION
**Purpose**: Close 3 findings + semantic state-machine cross-product sweep.
**Rule**: No new fields, no new features, no architecture changes. Closure only.

---

## 1. Summary

| Fix | Finding | Severity | What Changed |
|-----|---------|----------|-------------|
| #1 | C8CR18-001: ACK cleanup vs journal eviction boundary | P1 | Explicit: cleanup = ACK queue deletion only, NEVER journal eviction |
| #2 | C8CR18-002: publish()==true ≠ BROKER_CONFIRMED | P1 | Only PUBACK event triggers BROKER_CONFIRMED transition |
| #3 | C8CR18-003: lastAttemptTs initial condition | P2 | lastAttemptTs=0 at queueAck, first attempt immediately eligible |
| #4 | Semantic state-machine cross-product sweep | — | Command×Journal×ACK×operation matrix verified |

**No new fields. No new features. No architecture changes. No code.**

---

## 2. Fix #1: ACK Cleanup ≠ Journal Eviction (C8CR18-001)

### Problem

Rev18 used "cleanup" ambiguously — could mean ACK queue deletion OR journal eviction. Rev14 I2 says `NON_IDEMPOTENT + FAILED_EXHAUSTED → NO` (journal eviction blocked), but Rev18 says `FAILED_EXHAUSTED → YES` (cleanup allowed). Implementer may conflate the two.

### Solution: Explicit Boundary

```
ACK CLEANUP BOUNDARY (NORMATIVE — REV19):

DEFINITION:
    ACK cleanup ≡ deletion of the ACK queue record (tj_ackq) only.

    ACK cleanup MUST NOT:
        - Evict the journal entry (must NOT call clearEntry or eviction logic)
        - Clear the journal slot (must NOT write EMPTY to journal)
        - Call recoverCorruptedEntry()
        - Modify any journal record state

    ACK cleanup and journal eviction are INDEPENDENT operations:
        - ACK cleanup operates on tj_ackq (ACK queue).
        - Journal eviction operates on tj_ra_N / tj_rb_N (journal records).
        - Neither operation triggers the other.

FAILED_EXHAUSTED BEHAVIOR (clarified):

    ACK_FAILED_EXHAUSTED + ACK queue cleanup:
        → ACK queue record MAY be removed from tj_ackq.
        → This does NOT affect the journal entry.
        → Journal entry remains COMMITTED until independently evicted.

    NON_IDEMPOTENT + ACK_FAILED_EXHAUSTED + journal eviction:
        → Journal eviction is BLOCKED (per Rev14 I2).
        → "operator must investigate" — journal evidence retained.
        → ACK queue record may be cleaned up independently.
        → But: journal entry is NOT evictable.

CONSISTENCY WITH REV14 I2:
    Rev14 I2 eviction matrix (authoritative for journal eviction):
        NON_IDEMPOTENT + ACK_FAILED_EXHAUSTED → NO (journal eviction blocked).
    
    Rev19 ACK cleanup matrix (authoritative for ACK queue deletion):
        ACK_FAILED_EXHAUSTED → YES (ACK queue record may be deleted).
    
    These are CONSISTENT:
        - ACK queue cleanup = remove from tj_ackq. Allowed.
        - Journal eviction = write EMPTY to journal slot. Blocked.
        - Different operations, different storages, different rules.
        - Neither triggers the other.

REVISED CLEANUP MATRIX (REV19 — clarifies "cleanup = ACK queue only"):

| deliveryState          | Command class   | Journal state    | ACK queue cleanup? | Journal eviction? |
|------------------------|-----------------|------------------|--------------------|-------------------|
| ACK_PWA_RECEIVED       | ANY             | ANY              | YES                | Per I2 matrix     |
| ACK_FAILED_EXHAUSTED   | ANY             | ANY              | YES                | Per I2 matrix     |
| ACK_NOT_SENT           | IDEMPOTENT      | ANY              | YES                | Per I2 matrix     |
| ACK_NOT_SENT           | NON_IDEMPOTENT  | COMMITTED        | NO                 | Per I2 matrix     |
| ACK_NOT_SENT           | NON_IDEMPOTENT  | EVICTED          | YES                | N/A (already EMPTY)|
| ACK_PUBLISH_ACCEPTED   | IDEMPOTENT      | ANY              | YES                | Per I2 matrix     |
| ACK_PUBLISH_ACCEPTED   | NON_IDEMPOTENT  | COMMITTED        | NO                 | Per I2 matrix     |
| ACK_PUBLISH_ACCEPTED   | NON_IDEMPOTENT  | EVICTED          | YES                | N/A (already EMPTY)|
| ACK_BROKER_CONFIRMED   | IDEMPOTENT      | ANY              | YES                | Per I2 matrix     |
| ACK_BROKER_CONFIRMED   | NON_IDEMPOTENT  | COMMITTED        | NO                 | Per I2 matrix     |
| ACK_BROKER_CONFIRMED   | NON_IDEMPOTENT  | EVICTED          | YES                | N/A (already EMPTY)|

NOTE: "Per I2 matrix" means the journal eviction decision is made by
Rev14 I2 eviction matrix (which considers command class × ACK state).
ACK queue cleanup is a SEPARATE decision shown in the "ACK queue cleanup?" column.
```

---

## 3. Fix #2: mqtt.publish() ≠ BROKER_CONFIRMED (C8CR18-002)

### Problem

Rev18 algorithm uses `mqtt.publish()==true` to transition `ACK_NOT_SENT → ACK_PUBLISH_ACCEPTED`. But Rev17 defines `PUBLISH_ACCEPTED → BROKER_CONFIRMED` as requiring PUBACK. Without explicit statement that `publish()==true` is local acceptance only (not broker confirmation), implementer could treat it as broker confirmation.

### Solution: Explicit Semantics

```
MQTT PUBLISH SEMANTICS (NORMATIVE — REV19):

DEFINITION:
    mqtt.publish() returns true:
        → The local MQTT client library has accepted the publication for
          transmission to the broker.
        → The bytes have been handed to the TCP stack.
        → It does NOT mean:
            - Broker received the message
            - Broker acknowledged the message (PUBACK)
            - PWA received the message

STATE TRANSITION RULES (EXPLICIT):

    mqtt.publish() == true:
        → ACK_NOT_SENT → ACK_PUBLISH_ACCEPTED
        (Local acceptance only. Broker has NOT confirmed.)

    QoS 1 PUBACK received (future, not implemented):
        → ACK_PUBLISH_ACCEPTED → ACK_BROKER_CONFIRMED
        (Broker has confirmed receipt. This is the ONLY way to enter
         BROKER_CONFIRMED. No other trigger can cause this transition.)

    ack_confirm message verified (future, not implemented):
        → ACK_BROKER_CONFIRMED → ACK_PWA_RECEIVED
        (PWA has confirmed receipt. This is the ONLY way to enter
         PWA_RECEIVED. Requires BROKER_CONFIRMED as precondition.)

FORBIDDEN TRANSITIONS (explicit):
    ACK_NOT_SENT → ACK_BROKER_CONFIRMED
        (Must go through PUBLISH_ACCEPTED first.)
    
    ACK_PUBLISH_ACCEPTED → ACK_PWA_RECEIVED
        (Must go through BROKER_CONFIRMED first.)
    
    mqtt.publish() == true → ACK_BROKER_CONFIRMED
        (publish() is local acceptance, NOT broker confirmation.
         Only PUBACK causes BROKER_CONFIRMED transition.)

IMPLEMENTATION CONTRACT:
    The firmware MUST NOT interpret mqtt.publish() return value as
    broker confirmation. The return value is local acceptance only.
    PUBACK handling (when implemented) is the sole trigger for
    ACK_BROKER_CONFIRMED.

    Code review MUST verify:
        1. mqtt.publish() return value only triggers NOT_SENT → PUBLISH_ACCEPTED.
        2. No code path transitions to BROKER_CONFIRMED without PUBACK event.
        3. PUBACK handler (when implemented) is the sole caller of the
           PUBLISH_ACCEPTED → BROKER_CONFIRMED transition.
```

---

## 4. Fix #3: lastAttemptTs Initial Condition (C8CR18-003)

### Problem

Rev18 did not define the initial value of `lastAttemptTs` when `queueAck()` creates a new ACK record. Two possible behaviors:
- A: `lastAttemptTs = now` → first attempt delayed by ACK_RETRY_INTERVAL_MS
- B: `lastAttemptTs = 0` → first attempt immediately eligible

### Solution: Immediate First Attempt

```
LASTATTEMPTTS INITIAL CONDITION (NORMATIVE — REV19):

    When queueAck(requestId, ackJson) creates a new ACK record:
        retryCount = 0
        lastAttemptTs = 0
        deliveryState = ACK_NOT_SENT

    When processPendingAcks() evaluates the record:
        if (now - lastAttemptTs) < ACK_RETRY_INTERVAL_MS:
            skip (too soon)
        
        With lastAttemptTs = 0:
            now - 0 = now (large value, typically >> ACK_RETRY_INTERVAL_MS)
            → first attempt is IMMEDIATELY eligible.
            → No artificial delay on first ACK delivery.

    After first attempt:
        lastAttemptTs = now (updated after publish attempt)
        Subsequent attempts are spaced by ACK_RETRY_INTERVAL_MS.

RATIONALE:
    First ACK delivery should be immediate (no delay). The device has just
    committed a transaction; PWA should receive the ACK as soon as possible.
    Retries (if first attempt fails) are spaced by ACK_RETRY_INTERVAL_MS.

    If lastAttemptTs were set to `now` at queueAck time, the first attempt
    would be delayed by ACK_RETRY_INTERVAL_MS (2 seconds). This adds
    unnecessary latency to the common case (first publish succeeds).

NO NEW METADATA:
    lastAttemptTs is an existing field (4 bytes in AckRecord).
    Initial value 0 is a convention, not a new field.
```

---

## 5. Fix #4: Semantic State-Machine Cross-Product Sweep

### Method

Instead of keyword search, perform a **semantic cross-product** verification:
- Command class × Journal state × ACK delivery state × ACK operation × Journal operation
- mqtt.publish() return × PUBACK event × deliveryState transition

### Cross-Product 1: Command × Journal × ACK × Operations

For each combination of (command class, journal state, ACK delivery state), verify that:
1. ACK queue cleanup decision is consistent
2. Journal eviction decision is consistent
3. The two decisions are independent (neither triggers the other)

| Command | Journal | ACK State | ACK cleanup? | Journal eviction? | Independent? |
|---------|---------|-----------|-------------|-------------------|-------------|
| IDEMPOTENT | COMMITTED | NOT_SENT | YES | Per I2 (YES if ACK in durable queue) | ✅ Independent |
| IDEMPOTENT | COMMITTED | PUBLISH_ACCEPTED | YES | YES | ✅ |
| IDEMPOTENT | COMMITTED | BROKER_CONFIRMED | YES | YES | ✅ |
| IDEMPOTENT | COMMITTED | PWA_RECEIVED | YES | YES | ✅ |
| IDEMPOTENT | COMMITTED | FAILED_EXHAUSTED | YES | YES | ✅ |
| IDEMPOTENT | EVICTED | NOT_SENT | YES | N/A | ✅ |
| IDEMPOTENT | EVICTED | PUBLISH_ACCEPTED | YES | N/A | ✅ |
| IDEMPOTENT | EVICTED | FAILED_EXHAUSTED | YES | N/A | ✅ |
| NON_IDEMPOTENT | COMMITTED | NOT_SENT | NO | NO | ✅ |
| NON_IDEMPOTENT | COMMITTED | PUBLISH_ACCEPTED | NO | NO | ✅ |
| NON_IDEMPOTENT | COMMITTED | BROKER_CONFIRMED | NO | NO | ✅ |
| NON_IDEMPOTENT | COMMITTED | PWA_RECEIVED | YES | YES | ✅ |
| NON_IDEMPOTENT | COMMITTED | FAILED_EXHAUSTED | YES | NO | ✅ (cleanup ≠ eviction) |
| NON_IDEMPOTENT | EVICTED | NOT_SENT | YES | N/A | ✅ |
| NON_IDEMPOTENT | EVICTED | PUBLISH_ACCEPTED | YES | N/A | ✅ |
| NON_IDEMPOTENT | EVICTED | FAILED_EXHAUSTED | YES | N/A | ✅ |
| UNKNOWN | ANY | ANY | NO | NO | ✅ |

**Key verification**: `NON_IDEMPOTENT + COMMITTED + FAILED_EXHAUSTED`:
- ACK cleanup: YES (Rev19 §2 — cleanup = ACK queue deletion only)
- Journal eviction: NO (Rev14 I2 — "operator must investigate")
- Independent: ✅ (ACK queue deletion does NOT trigger journal eviction)
- **This is the case Rev18 was ambiguous about. Rev19 makes it explicit.**

### Cross-Product 2: mqtt.publish() × PUBACK × State Transition

| Event | deliveryState before | deliveryState after | Correct? |
|-------|----------------------|--------------------| | 
| mqtt.publish()==true | ACK_NOT_SENT | ACK_PUBLISH_ACCEPTED | ✅ (local acceptance) |
| mqtt.publish()==true | ACK_PUBLISH_ACCEPTED | (no change) | ✅ (already accepted) |
| mqtt.publish()==true | ACK_BROKER_CONFIRMED | (no change) | ✅ (already broker-confirmed) |
| mqtt.publish()==true | ACK_PWA_RECEIVED | (no change) | ✅ (terminal) |
| mqtt.publish()==true | ACK_FAILED_EXHAUSTED | (no change) | ✅ (terminal) |
| PUBACK received | ACK_NOT_SENT | (no change) | ✅ (must be PUBLISH_ACCEPTED first) |
| PUBACK received | ACK_PUBLISH_ACCEPTED | ACK_BROKER_CONFIRMED | ✅ (sole trigger) |
| PUBACK received | ACK_BROKER_CONFIRMED | (no change) | ✅ (already confirmed) |
| PUBACK received | ACK_PWA_RECEIVED | (no change) | ✅ (terminal) |
| ack_confirm verified | ACK_BROKER_CONFIRMED | ACK_PWA_RECEIVED | ✅ (sole trigger) |
| ack_confirm verified | ACK_PWA_RECEIVED | (no change, no-op) | ✅ (idempotent, step 3) |
| ack_confirm verified | ACK_PUBLISH_ACCEPTED | (no change) | ✅ (must be BROKER_CONFIRMED, step 6) |
| ack_confirm verified | ACK_NOT_SENT | (no change) | ✅ (must be BROKER_CONFIRMED, step 6) |
| ack_confirm verified | ACK_FAILED_EXHAUSTED | (no change) | ✅ (terminal) |
| retryCount >= MAX (Phase 1) | ACK_NOT_SENT | ACK_FAILED_EXHAUSTED | ✅ |
| retryCount >= MAX (Phase 1) | ACK_PUBLISH_ACCEPTED | ACK_FAILED_EXHAUSTED | ✅ |
| retryCount >= MAX (Phase 2) | ACK_BROKER_CONFIRMED | ACK_FAILED_EXHAUSTED | ✅ |

**All transitions verified. No contradictions. No ambiguous paths.**

### Cross-Product 3: retryCount × Phase × Boundary

| Phase | retryCount before | Action | retryCount after | deliveryState | Correct? |
|-------|------------------|--------|-----------------|---------------|----------|
| 1 | 0 | publish → success | 1 | NOT_SENT → PUBLISH_ACCEPTED | ✅ |
| 1 | 0 | publish → fail | 1 | NOT_SENT (stays) | ✅ |
| 1 | 9 | publish → success | 10 | PUBLISH_ACCEPTED | ✅ |
| 1 | 10 | (check, no publish) | 10 | → FAILED_EXHAUSTED | ✅ |
| 1→2 | (reset) | PUBACK received | 0 | → BROKER_CONFIRMED | ✅ |
| 2 | 0 | publish → success | 1 | BROKER_CONFIRMED (stays) | ✅ |
| 2 | 9 | publish → success | 10 | BROKER_CONFIRMED (stays) | ✅ |
| 2 | 10 | (check, no publish) | 10 | → FAILED_EXHAUSTED | ✅ |

**All boundary conditions verified. Off-by-one eliminated.**

---

## 6. Rule Compliance

| Item | New Field? | New Feature? |
|------|-----------|-------------|
| ACK cleanup ≠ eviction boundary | NO (contract) | NO |
| publish() ≠ BROKER_CONFIRMED | NO (contract) | NO |
| lastAttemptTs initial value | NO (convention) | NO |
| Semantic cross-product sweep | NO (verification) | NO |

**Zero new metadata. Zero new features. Zero architecture changes.**

---

## 7. Authoritative Document Stack (Rev19)

```
NORMATIVE DOCUMENTS:

    1. REV14 — base (I0-I3, CRC, canonical, recovery, eviction, ACK lifecycle)
    2. REV15 — supplement (mutation boundary, terminology, CRC gate)
    3. REV16 — supplement (transition graph, retention separation)
    4. REV17 — supplement (verification ordering, retry phases)
    5. REV18 — supplement (cleanup predicate, retryCount definition, re-publish invariant)
    6. REV19 (THIS DOCUMENT) — supplement (cleanup≠eviction, publish≠PUBACK, lastAttemptTs, sweep)

PRECEDENCE:
    For ACK cleanup vs eviction boundary: Rev19 WINS.
    For publish() vs PUBACK semantics: Rev19 WINS.
    For lastAttemptTs initial value: Rev19 WINS.
    For semantic cross-product verification: Rev19 is authoritative.
    For all other topics: Rev14 (as supplemented by Rev15-Rev18) remains authoritative.

ALL OTHER DOCUMENTS: SUPERSEDED (banners applied)
```

---

## 8. Honest Limitations (Unchanged)

Same as Rev14+Rev18. No new limitations.

---

## 9. Auditor Approval Gate

**This design is NOT approved for implementation.**

Auditor must verify:
1. ACK cleanup boundary (§2): cleanup = ACK queue deletion only? NEVER journal eviction?
2. FAILED_EXHAUSTED consistency (§2): ACK cleanup YES, journal eviction NO (for non-idempotent)?
3. publish() semantics (§3): publish()==true → PUBLISH_ACCEPTED only? PUBACK → BROKER_CONFIRMED only?
4. lastAttemptTs (§4): initial value 0? First attempt immediately eligible?
5. Semantic cross-product 1 (§5): 17 rows, all independent? NON_IDEMPOTENT+COMMITTED+FAILED → cleanup YES, eviction NO?
6. Semantic cross-product 2 (§5): 15 rows, all transitions correct? No ambiguous paths?
7. Semantic cross-product 3 (§5): 8 rows, retryCount boundary verified? Off-by-one eliminated?
8. Rule compliance (§6): Zero new metadata, zero new features?

**After auditor approval, implementation Phase 1-8 may begin. Until then: NO CODE.**

Precharge: BLOCKED
I/O expander: BLOCKED
16-relay: BLOCKED