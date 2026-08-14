# CYCLE-8C-Rev20: Transaction Journal v4 — Auth Regression & Cross-Product Correction

**Status**: DESIGN ONLY — NO CODE IMPLEMENTATION
**Purpose**: Restore auth gate, fix cross-product contradictions, deterministic timing, expand sweep.
**Rule**: No new fields, no new features, no architecture changes. Closure only.

---

## 1. Summary

| Fix | Finding | Severity | What Changed |
|-----|---------|----------|-------------|
| #1 | P1: NON_IDEMPOTENT + PWA_RECEIVED lost auth gate | P1 | Eviction requires authenticated confirmation, not just ackDigest |
| #2 | P1: Cross-Product #1 contradicts I2 (NOT_SENT) | P1 | Corrected: IDEMPOTENT+COMMITTED+NOT_SENT → eviction NO |
| #3 | P2: lastAttemptTs=0 "immediately" not deterministic | P2 | Removed "immediately" claim, stated mathematical condition |
| #4 | P2: Cross-product incomplete (missing auth dimension) | P2 | Expanded with authentication status dimension |
| #5 | P1: Regression check I0/I0a, I1, I2, I3, ACK | P1 | Full regression sweep against Rev14-Rev19 |

**No new fields. No new features. No architecture changes. No code.**

---

## 2. Fix #1: Restore Non-Idempotent Authentication Gate (P1)

### Problem

Rev15 §7 established:
> ackDigest is content binding, NOT sender authentication.
> ACK_PWA_RECEIVED with ackDigest alone is NOT sufficient for NON_IDEMPOTENT eviction.

But Rev19 cross-product row:
> NON_IDEMPOTENT + COMMITTED + PWA_RECEIVED → Journal eviction: YES

This dropped the authentication prerequisite. A regression.

### Solution: Two-Tier PWA_RECEIVED

```
NON_IDEMPOTENT EVICTION AUTHENTICATION GATE (NORMATIVE — REV20):

ACK_PWA_RECEIVED has two sub-levels:

    PWA_RECEIVED_ACKDIGEST:
        ack_confirm received, requestId + commandHash + ackDigest all match.
        ackDigest = SHA-256(ackJson)[0:16] = content binding ONLY.
        Sender identity NOT verified.
        → Sufficient for IDEMPOTENT eviction.
        → NOT sufficient for NON_IDEMPOTENT eviction.

    PWA_RECEIVED_AUTHENTICATED:
        ack_confirm received AND verified (PWA_RECEIVED_ACKDIGEST)
        AND sender authentication confirmed (MQTT ACL or HMAC, future).
        Sender identity verified.
        → Sufficient for NON_IDEMPOTENT eviction.

REVISED EVICTION MATRIX (REV20 — restores auth gate):

| Command Class   | ACK State                      | Journal Eviction? |
|-----------------|-------------------------------|-------------------|
| IDEMPOTENT      | ACK_PUBLISH_ACCEPTED + queue  | YES               |
| IDEMPOTENT      | ACK_BROKER_CONFIRMED           | YES               |
| IDEMPOTENT      | PWA_RECEIVED_ACKDIGEST         | YES               |
| IDEMPOTENT      | PWA_RECEIVED_AUTHENTICATED      | YES               |
| IDEMPOTENT      | ACK_FAILED_EXHAUSTED           | YES               |
| IDEMPOTENT      | ACK_NOT_SENT                   | NO                |
| NON_IDEMPOTENT  | PWA_RECEIVED_ACKDIGEST         | NO (auth required)|
| NON_IDEMPOTENT  | PWA_RECEIVED_AUTHENTICATED     | YES               |
| NON_IDEMPOTENT  | anything else                  | NO                |
| UNKNOWN         | ANY                            | NO                |

KEY CHANGE FROM REV19:
    Rev19: NON_IDEMPOTENT + PWA_RECEIVED → YES (dropped auth gate).
    Rev20: NON_IDEMPOTENT + PWA_RECEIVED_ACKDIGEST → NO (auth not confirmed).
    Rev20: NON_IDEMPOTENT + PWA_RECEIVED_AUTHENTICATED → YES (auth confirmed).

NOT IMPLEMENTED:
    PWA_RECEIVED_AUTHENTICATED requires MQTT ACL or HMAC (future cycle).
    Until implemented: NON_IDEMPOTENT entries are NEVER evictable.
    (Consistent with all previous design — auth gate was always required.)

NO NEW METADATA:
    PWA_RECEIVED_ACKDIGEST and PWA_RECEIVED_AUTHENTICATED are NOT stored states.
    They are COMPUTED at eviction-decision time:
        - deliveryState in tj_ackq is ACK_PWA_RECEIVED (value 3).
        - At eviction time, the system checks whether sender authentication
          was confirmed for this ack_confirm.
        - Sender authentication status is tracked in RAM (not persisted) because
          it is derived from the MQTT connection's ACL state at the time of
          ack_confirm receipt.
        - If MQTT ACL is configured: PWA_RECEIVED_AUTHENTICATED.
        - If MQTT ACL is NOT configured (public broker): PWA_RECEIVED_ACKDIGEST only.
        - This is a runtime check, not a stored field.
```

---

## 3. Fix #2: Cross-Product #1 Corrected — No I2 Contradiction (P1)

### Problem

Rev19 cross-product row:
> IDEMPOTENT | COMMITTED | NOT_SENT | Journal eviction: Per I2 (YES if ACK in durable queue)

But Rev14 I2 explicitly says:
> IDEMPOTENT + ACK_NOT_SENT → NO

Direct contradiction.

### Solution: Corrected Cross-Product

```
CROSS-PRODUCT #1 (CORRECTED — REV20, ALIGNED WITH I2):

For journal eviction, the decision is made by Rev14 I2 eviction matrix:
    IDEMPOTENT + NOT_SENT → NO (ACK not yet sent, not in durable queue)
    IDEMPOTENT + PUBLISH_ACCEPTED + durable queue → YES
    IDEMPOTENT + BROKER_CONFIRMED → YES
    IDEMPOTENT + PWA_RECEIVED → YES
    IDEMPOTENT + FAILED_EXHAUSTED → YES

| Command       | Journal    | ACK State          | ACK cleanup? | Journal eviction? | Source     |
|---------------|------------|--------------------|--------------|-------------------- |------------|
| IDEMPOTENT    | COMMITTED  | NOT_SENT           | YES          | NO                 | I2: not sent |
| IDEMPOTENT    | COMMITTED  | PUBLISH_ACCEPTED   | YES          | YES (if durable queue) | I2 |
| IDEMPOTENT    | COMMITTED  | BROKER_CONFIRMED   | YES          | YES                | I2         |
| IDEMPOTENT    | COMMITTED  | PWA_RECEIVED_ACKDIGEST | YES      | YES                | I2 + Rev20 |
| IDEMPOTENT    | COMMITTED  | PWA_RECEIVED_AUTH   | YES          | YES                | I2 + Rev20 |
| IDEMPOTENT    | COMMITTED  | FAILED_EXHAUSTED   | YES          | YES                | I2         |
| IDEMPOTENT    | EVICTED    | ANY                | YES          | N/A                | —          |
| NON_IDEMPOTENT| COMMITTED  | NOT_SENT           | NO           | NO                 | I2         |
| NON_IDEMPOTENT| COMMITTED  | PUBLISH_ACCEPTED   | NO           | NO                 | I2         |
| NON_IDEMPOTENT| COMMITTED  | BROKER_CONFIRMED   | NO           | NO                 | I2         |
| NON_IDEMPOTENT| COMMITTED  | PWA_RECEIVED_ACKDIGEST | NO      | NO (auth required)  | Rev20 §2   |
| NON_IDEMPOTENT| COMMITTED  | PWA_RECEIVED_AUTH   | YES         | YES                | Rev20 §2   |
| NON_IDEMPOTENT| COMMITTED  | FAILED_EXHAUSTED   | YES          | NO                 | I2         |
| NON_IDEMPOTENT| EVICTED    | ANY pre-terminal     | YES         | N/A                | —          |
| NON_IDEMPOTENT| EVICTED    | PWA_RECEIVED        | YES          | N/A                | —          |
| UNKNOWN       | ANY        | ANY                | NO           | NO                 | I2         |

KEY CORRECTIONS FROM REV19:
    1. IDEMPOTENT + COMMITTED + NOT_SENT → eviction NO (was "YES if durable queue" — WRONG).
       Reason: ACK has not been sent. "In durable queue" means the ACK record
       exists in tj_ackq, but deliveryState=NOT_SENT means publish hasn't happened.
       I2 says: "ACK_NOT_SENT → NO" for ALL command classes. No exceptions.

    2. NON_IDEMPOTENT + COMMITTED + PWA_RECEIVED_ACKDIGEST → eviction NO (was YES — WRONG).
       Reason: ackDigest is content binding, NOT sender auth. Rev15 auth gate restored.

CONSISTENCY WITH I2:
    Every "Journal eviction?" value in the cross-product is taken DIRECTLY
    from Rev14 I2 eviction matrix. No interpretation, no "per I2 (YES if...)".
    The value is either YES or NO, matching I2 exactly.
```

---

## 4. Fix #3: lastAttemptTs Deterministic (P2)

### Problem

Rev19 said "first attempt is IMMEDIATELY eligible" with `lastAttemptTs=0`. But if `now=500ms` and `ACK_RETRY_INTERVAL_MS=2000ms`, then `now - 0 = 500 < 2000` → SKIP. "Immediately" is not guaranteed.

### Solution: Mathematical Condition (No "Immediately" Claim)

```
LASTATTEMPTTS TIMING (NORMATIVE — REV20):

    When queueAck() creates a new ACK record:
        retryCount = 0
        lastAttemptTs = 0
        deliveryState = ACK_NOT_SENT

    When processPendingAcks() evaluates the record:
        if (now - lastAttemptTs) < ACK_RETRY_INTERVAL_MS:
            skip (too soon)

        With lastAttemptTs = 0:
            Eligibility condition: (now - 0) >= ACK_RETRY_INTERVAL_MS
            Simplified: now >= ACK_RETRY_INTERVAL_MS

            This means: first attempt is eligible when uptime (now) has reached
            at least ACK_RETRY_INTERVAL_MS (2000ms = 2 seconds since boot).

            If queueAck() is called before 2 seconds of uptime:
                First attempt waits until uptime >= 2 seconds.
            
            If queueAck() is called after 2 seconds of uptime:
                First attempt is eligible immediately (at next processPendingAcks call).

    This is DETERMINISTIC. The claim "immediately eligible" from Rev19 is REMOVED.
    
    The actual behavior is: first attempt is eligible when
    (now - lastAttemptTs) >= ACK_RETRY_INTERVAL_MS, which with lastAttemptTs=0
    means now >= ACK_RETRY_INTERVAL_MS.

    For typical operation (device has been booted for >2 seconds before
    first transaction): first attempt IS effectively immediate.
    For edge case (transaction within first 2 seconds of boot): first attempt
    waits until 2 seconds have elapsed.

NO NEW METADATA:
    lastAttemptTs is existing field. Initial value 0 is a convention.
    The mathematical condition is stated, not the word "immediately".
```

---

## 5. Fix #4: Expanded Cross-Product with Authentication Dimension (P2)

### Problem

Rev19 cross-product did not include authentication status as a dimension. This allowed the auth gate regression (Fix #1) to go undetected.

### Solution: Authentication Dimension Added

```
CROSS-PRODUCT: NON_IDEMPOTENT × PWA_RECEIVED × Authentication Status

| Command       | ACK State                | Auth Status     | Journal eviction? | ACK cleanup? |
|---------------|--------------------------|-----------------|-------------------|--------------|
| NON_IDEMPOTENT | PWA_RECEIVED_ACKDIGEST  | Not authenticated | NO             | NO (unresolved) |
| NON_IDEMPOTENT | PWA_RECEIVED_ACKDIGEST  | Authenticated    | YES (becomes PWA_RECEIVED_AUTH) | YES |
| NON_IDEMPOTENT | PWA_RECEIVED_AUTH        | Authenticated    | YES             | YES          |
| NON_IDEMPOTENT | PWA_RECEIVED_AUTH        | Not authenticated | (impossible — auth is prerequisite) | — |
| IDEMPOTENT    | PWA_RECEIVED_ACKDIGEST  | Any              | YES             | YES          |
| IDEMPOTENT    | PWA_RECEIVED_AUTH        | Any              | YES             | YES          |

NOTE:
    PWA_RECEIVED_ACKDIGEST = ack_confirm verified, sender NOT authenticated.
    PWA_RECEIVED_AUTH = ack_confirm verified AND sender authenticated (MQTT ACL/HMAC).
    
    For IDEMPOTENT: auth status does not affect eviction (idempotent re-execution is safe).
    For NON_IDEMPOTENT: auth status IS the gating factor.
    
    Sender authentication is a runtime property (derived from MQTT connection ACL state).
    It is NOT persisted in tj_ackq (no new metadata).
    At eviction time, the system checks whether the MQTT connection that delivered
    the ack_confirm had ACL enforcement. If yes → PWA_RECEIVED_AUTH. If no → ACKDIGEST only.
```

---

## 6. Fix #5: Regression Check Against I0/I0a, I1, I2, I3, ACK State Machine

```
REGRESSION CHECK (REV20 — EXHAUSTIVE):

I0 (Executor Ownership):
    Rev14: TaskHandle check, not compiled out.
    Rev15-19: Unchanged.
    Rev20: No change. ✅ No regression.

I0a (Stable Observation):
    Rev14: RAII guard, mutation forbidden during observation.
    Rev15: 3-layer mutation enforcement model.
    Rev16-19: Unchanged.
    Rev20: No change. ✅ No regression.

I1 (Canonical Equivalence + Recovery):
    Rev14: Record layout, CRC, safe parse, canonicalEqual, generation ordering.
    Rev9: Generation distance wrap-safe (forwardDistance).
    Rev11: EMPTY not special, 9 uniform recovery rows.
    Rev12: Recovery contract gen=0 unconditional, test vector table.
    Rev15-19: Unchanged.
    Rev20: No change. ✅ No regression.

I2 (Eviction Safety):
    Rev14: Eviction matrix (command class × ACK state).
    Rev15: ackDigest = content binding, NOT sender auth.
    Rev20 Fix #1: Restores auth gate — NON_IDEMPOTENT + PWA_RECEIVED_ACKDIGEST → NO.
    Rev20 Fix #2: Cross-product aligned with I2 (NOT_SENT → NO for all classes).
    
    Regression check:
        Rev14 I2: IDEMPOTENT + NOT_SENT → NO. Rev20: ✅ aligned.
        Rev14 I2: NON_IDEMPOTENT + PWA_RECEIVED → YES. Rev20: ✅ aligned (but PWA_RECEIVED_AUTH required).
        Rev14 I2: NON_IDEMPOTENT + FAILED_EXHAUSTED → NO. Rev20: ✅ aligned.
        Rev15 auth boundary: ackDigest NOT sender auth. Rev20: ✅ restored.
    
    ✅ No regression. Auth gate restored. Cross-product aligned.

I3 (ACK Lifecycle Separation):
    Rev14: Transaction ≠ ACK lifecycle. Eviction ≠ ACK deletion. Merge recovery.
    Rev16: PWA_RECEIVED only from BROKER_CONFIRMED.
    Rev17: Verification ordering (step 3 before step 6). Two retry phases.
    Rev18: Uniform cleanup predicate. retryCount definition. Re-publish invariant.
    Rev19: Cleanup ≠ eviction. publish() ≠ PUBACK. lastAttemptTs.
    Rev20: Auth dimension added to cross-product.
    
    Regression check:
        Eviction ≠ ACK deletion: ✅ (Rev19 §2, Rev20 maintains)
        PWA from BROKER only: ✅ (Rev16, Rev20 maintains)
        Verification ordering: ✅ (Rev17, Rev20 maintains)
        retryCount definition: ✅ (Rev18, Rev20 maintains)
        Re-publish invariant: ✅ (Rev18, Rev20 maintains)
        publish() ≠ PUBACK: ✅ (Rev19, Rev20 maintains)
        Cleanup ≠ eviction: ✅ (Rev19, Rev20 maintains)
    
    ✅ No regression.

ACK STATE MACHINE:
    NOT_SENT → PUBLISH_ACCEPTED → BROKER_CONFIRMED → PWA_RECEIVED
    Any pre-terminal → FAILED_EXHAUSTED
    
    Rev20: No state machine changes. Auth dimension is computed at decision time,
    not stored as a state. ✅ No regression.

RECOVERY:
    9 uniform rows, gen=0 unconditional, QUARANTINED no auto-reuse.
    Rev20: No change. ✅ No regression.

CRC:
    CRC-32/ISO-HDLC, ~esp_crc32_le(0xFFFFFFFF,...), test vector 0xCBF43926.
    Rev20: No change. ✅ No regression.

CONCLUSION:
    No regressions found. Rev20 restores auth gate (Fix #1) and aligns
    cross-product with I2 (Fix #2) without breaking any prior invariant.
```

---

## 7. Rule Compliance

| Item | New Field? | New Feature? |
|------|-----------|-------------|
| Auth gate restoration | NO (runtime check, not stored) | NO |
| Cross-product correction | NO (verification) | NO |
| lastAttemptTs deterministic | NO (removed word "immediately") | NO |
| Auth dimension in cross-product | NO (runtime check, not stored) | NO |
| Regression check | NO (verification) | NO |

**Zero new metadata. Zero new features. Zero architecture changes.**

---

## 8. Authoritative Document Stack (Rev20)

```
NORMATIVE DOCUMENTS:

    1. REV14 — base (I0-I3, CRC, canonical, recovery, eviction, ACK lifecycle)
    2. REV15 — supplement (mutation boundary, terminology, CRC gate, auth boundary)
    3. REV16 — supplement (transition graph, retention separation)
    4. REV17 — supplement (verification ordering, retry phases)
    5. REV18 — supplement (cleanup predicate, retryCount definition, re-publish invariant)
    6. REV19 — supplement (cleanup≠eviction, publish≠PUBACK, lastAttemptTs)
    7. REV20 (THIS DOCUMENT) — supplement (auth gate restoration, cross-product correction, timing, expanded sweep)

PRECEDENCE:
    For NON_IDEMPOTENT eviction with PWA_RECEIVED: Rev20 WINS (auth gate restored).
    For cross-product alignment with I2: Rev20 WINS (NOT_SENT → NO for all classes).
    For lastAttemptTs timing: Rev20 WINS (deterministic, no "immediately" claim).
    For all other topics: Rev14 (as supplemented by Rev15-Rev19) remains authoritative.

ALL OTHER DOCUMENTS: SUPERSEDED (banners applied)
```

---

## 9. Honest Limitations (Unchanged + Added)

1-16: Same as Rev14+Rev18.

**Added**:
17. PWA_RECEIVED_AUTHENTICATED requires MQTT ACL or HMAC (future cycle, not implemented).
18. Until ACL/HMAC implemented: NON_IDEMPOTENT entries are NEVER evictable.
19. Sender authentication is a runtime property (derived from MQTT connection ACL), NOT persisted.

---

## 10. Auditor Approval Gate

**This design is NOT approved for implementation.**

Auditor must verify:
1. Auth gate (§2): NON_IDEMPOTENT + PWA_RECEIVED_ACKDIGEST → eviction NO? PWA_RECEIVED_AUTH → YES?
2. Cross-product correction (§3): IDEMPOTENT+COMMITTED+NOT_SENT → eviction NO (aligned with I2)?
3. lastAttemptTs (§4): Mathematical condition stated? "Immediately" claim removed?
4. Auth dimension (§5): NON_IDEMPOTENT × PWA × Auth → all cases covered?
5. Regression check (§6): I0/I0a, I1, I2, I3, ACK state machine — no regressions?
6. Rule compliance (§7): Zero new metadata (auth is runtime, not stored)?

**After auditor approval, implementation Phase 1-8 may begin. Until then: NO CODE.**

Precharge: BLOCKED
I/O expander: BLOCKED
16-relay: BLOCKED
