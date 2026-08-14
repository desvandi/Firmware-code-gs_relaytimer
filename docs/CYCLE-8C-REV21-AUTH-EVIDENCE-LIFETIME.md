# CYCLE-8C-Rev21: Transaction Journal v4 — Authentication Evidence Lifetime

**Status**: DESIGN ONLY — NO CODE IMPLEMENTATION
**Purpose**: Close authentication evidence durability + reboot semantics + terminology.
**Rule**: No new fields, no new features, no architecture changes. Closure only.
**Auditor recommendation**: Option A — evidence loss → RETAIN (conservative default).

---

## 1. Summary

| Fix | Finding | Severity | What Changed |
|-----|---------|----------|-------------|
| #1 | P1: Auth evidence not durable (RAM lost on reboot) | P1 | Evidence loss → RETAIN (never permission gain) |
| #2 | P2: Event-time vs decision-time semantics | P2 | Auth is property of ack_confirm event, NOT eviction-time connection |
| #3 | P2: Terminology — deliveryState vs authenticationEvidence | P2 | Explicit separation, no durable enum created |
| #4 | P1: Cross-product NON_IDEMPOTENT × PWA × auth × reboot | P1 | 4 cases verified |

**No new fields. No new features. No architecture changes. No code.**

---

## 2. Fix #1: Authentication Evidence Reboot Semantics (P1)

### Problem

Rev20 defined PWA_RECEIVED_AUTHENTICATED as "computed at eviction-decision time" from "MQTT connection ACL state at the time of ack_confirm receipt." But this is tracked in RAM only. After reboot:
- `deliveryState` in tj_ackq = `ACK_PWA_RECEIVED` (durable)
- Authentication evidence = GONE (RAM lost)

System cannot distinguish "PWA_RECEIVED + authenticated" from "PWA_RECEIVED + unauthenticated" after reboot. This creates a permission-gain risk if the system assumes authenticated by default.

### Solution: Option A — Evidence Loss → RETAIN

```
AUTHENTICATION EVIDENCE LIFETIME (NORMATIVE — REV21):

PRINCIPLE:
    Evidence loss MUST result in more conservative behavior, NOT permission gain.
    (Consistent with QUARANTINED philosophy: loss of evidence → retain, not free.)

RULE:

    ACK_PWA_RECEIVED + authentication evidence available + authenticated:
        → NON_IDEMPOTENT eviction MAY proceed.

    ACK_PWA_RECEIVED + authentication evidence NOT available (e.g., after reboot):
        → NON_IDEMPOTENT eviction BLOCKED.
        → Journal entry RETAINED (default = RETAIN, per I2e).

    ACK_PWA_RECEIVED + authentication evidence available + NOT authenticated:
        → NON_IDEMPOTENT eviction BLOCKED.

DETECTION OF "EVIDENCE AVAILABLE":
    Authentication evidence is available when the ack_confirm was received
    during the CURRENT boot session AND the MQTT connection at that time
    had ACL enforcement.

    After reboot: the MQTT connection that delivered the ack_confirm no
    longer exists. Its ACL state cannot be verified retroactively.
    Therefore: authentication evidence is UNAVAILABLE after reboot.

IMPLICATION:
    NON_IDEMPOTENT entries with ACK_PWA_RECEIVED:
        - During same boot session (before reboot): eviction MAY proceed
          IF authentication was confirmed at ack_confirm receipt time.
        - After reboot: eviction BLOCKED (evidence unavailable → RETAIN).

    This is ACCEPTED and CONSISTENT:
        - I2e: "Default = RETAIN (if any check is uncertain → NO eviction)."
        - Evidence unavailable = uncertain = RETAIN.
        - No permission gain from evidence loss.

WHY OPTION B (durable auth evidence) IS REJECTED:
    Option B would require storing authentication status in tj_ackq,
    which is new metadata. This violates the "zero new metadata" constraint.
    Option A achieves the same safety without metadata:
        - Same boot: RAM evidence is available.
        - After reboot: evidence unavailable → RETAIN (conservative).
        - The journal entry is still COMMITTED (durable).
        - The transaction result is still known.
        - The physical relay state is still queryable via /status.
        - Only eviction (reuse of slot) is blocked until evidence is restored
          (which requires PWA to re-send ack_confirm on an authenticated connection).

OPERATIONAL CONSEQUENCE:
    After reboot, non-idempotent entries with PWA_RECEIVED are NOT evictable
    until PWA re-sends ack_confirm on an authenticated MQTT connection.
    This is acceptable: PWA can re-confirm after reboot if needed.
    If PWA never re-confirms: slot is retained (reduces journal capacity).
    If journal fills: JOURNAL_FULL → operator intervention (same as QUARANTINED).

NO NEW METADATA:
    Authentication evidence is RAM-only (same as Rev20).
    The rule "evidence unavailable → RETAIN" is a normative contract, not a field.
    No new NVS keys, no new record fields, no new stored states.
```

---

## 3. Fix #2: Event-Time vs Decision-Time Semantics (P2)

### Problem

Rev20 mixed two temporal semantics:
- Authentication as property of the **ack_confirm receipt event** (when PWA sent confirmation)
- Authentication checked at **eviction-decision time** (when system decides to evict)

After reboot, the MQTT connection at eviction time is NOT the same connection that delivered ack_confirm. Using the current connection's ACL state as evidence for a past event is "temporal identity substitution."

### Solution: Auth Is Event Property

```
AUTHENTICATION TEMPORAL SEMANTICS (NORMATIVE — REV21):

DEFINITION:
    Authentication evidence is a property of the ack_confirm RECEIPT EVENT,
    NOT a property of the MQTT connection at eviction-decision time.

    When ack_confirm is received:
        - The system records (in RAM) whether the MQTT connection that
          delivered this message had ACL enforcement.
        - This RAM record is the authentication evidence for THIS event.
        - It is NOT updated by subsequent MQTT reconnections.

    When eviction is evaluated:
        - The system checks the RAM record for the ack_confirm event.
        - If the RAM record exists and says "authenticated": PWA_RECEIVED_AUTHENTICATED.
        - If the RAM record exists and says "not authenticated": PWA_RECEIVED_ACKDIGEST.
        - If the RAM record does NOT exist (e.g., after reboot): evidence UNAVAILABLE.

FORBIDDEN:
    - Using the CURRENT MQTT connection's ACL state as evidence for a
      PAST ack_confirm event (temporal identity substitution).
    - Assuming "authenticated now" implies "authenticated when ack_confirm was received."

CONTRACT:
    Authentication evidence is bound to the ack_confirm event, not to the
    eviction decision. Once the event's RAM record is lost (reboot),
    the evidence is UNAVAILABLE, and eviction defaults to RETAIN (Option A, §2).
```

---

## 4. Fix #3: Terminology — deliveryState vs authenticationEvidence (P2)

### Problem

Rev20 used "PWA_RECEIVED_ACKDIGEST" and "PWA_RECEIVED_AUTHENTICATED" as if they were ACK states, potentially confusing implementers into creating new durable enum values.

### Solution: Explicit Dimension Separation

```
TERMINOLOGY (NORMATIVE — REV21):

TWO SEPARATE DIMENSIONS:

1. deliveryState (DURABLE, stored in tj_ackq):
    ACK_NOT_SENT = 0
    ACK_PUBLISH_ACCEPTED = 1
    ACK_BROKER_CONFIRMED = 2
    ACK_PWA_RECEIVED = 3
    ACK_FAILED_EXHAUSTED = 4

    These are the ONLY stored delivery states.
    There is NO stored "PWA_RECEIVED_ACKDIGEST" or "PWA_RECEIVED_AUTHENTICATED".

2. authenticationEvidence (RAM-ONLY, not stored):
    EVIDENCE_UNAVAILABLE = 0  (e.g., after reboot, or ack_confirm not yet received)
    EVIDENCE_ACKDIGEST = 1    (ack_confirm verified, sender NOT authenticated)
    EVIDENCE_AUTHENTICATED = 2 (ack_confirm verified, sender authenticated)

    This is a RUNTIME property, not a stored field.
    It is recomputed (or marked unavailable) on each boot.

COMPUTATION AT EVICTION TIME:

    To determine non-idempotent eviction eligibility:

    IF deliveryState == ACK_PWA_RECEIVED:
        IF authenticationEvidence == EVIDENCE_AUTHENTICATED:
            → eviction eligible (for non-idempotent)
        ELSE (ACKDIGEST or UNAVAILABLE):
            → eviction BLOCKED (for non-idempotent)
            → RETAIN (per I2e: uncertain → no eviction)

    For idempotent commands:
        authenticationEvidence is NOT consulted.
        deliveryState == ACK_PWA_RECEIVED → eviction eligible (regardless of auth).

CROSS-PRODUCT DIMENSIONS:
    The cross-product uses these dimensions:
        - Command class (IDEMPOTENT / NON_IDEMPOTENT / UNKNOWN)
        - Journal state (COMMITTED / EVICTED)
        - deliveryState (ACK_NOT_SENT..ACK_FAILED_EXHAUSTED)
        - authenticationEvidence (UNAVAILABLE / ACKDIGEST / AUTHENTICATED)
        - Boot session (same boot / after reboot)

    deliveryState and authenticationEvidence are SEPARATE columns.
    There is no combined "PWA_RECEIVED_AUTH" state.
```

---

## 5. Fix #4: Cross-Product — NON_IDEMPOTENT × PWA × Auth × Reboot (P1)

```
CROSS-PRODUCT (REV21 — AUTH EVIDENCE × REBOOT):

| Command       | deliveryState        | authEvidence        | Boot session  | Eviction? | Reason |
|---------------|----------------------|---------------------|---------------|----------|--------|
| NON_IDEMPOTENT | ACK_PWA_RECEIVED    | EVIDENCE_AUTHENTICATED | Same boot   | YES      | Auth confirmed at receipt event |
| NON_IDEMPOTENT | ACK_PWA_RECEIVED    | EVIDENCE_ACKDIGEST     | Same boot   | NO       | Auth not confirmed (ackDigest only) |
| NON_IDEMPOTENT | ACK_PWA_RECEIVED    | EVIDENCE_UNAVAILABLE   | Same boot   | NO       | No ack_confirm received yet |
| NON_IDEMPOTENT | ACK_PWA_RECEIVED    | EVIDENCE_UNAVAILABLE   | After reboot | NO       | Evidence lost → RETAIN (Option A) |
| NON_IDEMPOTENT | ACK_BROKER_CONFIRMED | N/A                  | Any          | NO       | PWA not received |
| NON_IDEMPOTENT | ACK_PUBLISH_ACCEPTED | N/A                 | Any          | NO       | PWA not received |
| NON_IDEMPOTENT | ACK_NOT_SENT         | N/A                  | Any          | NO       | Not sent |
| NON_IDEMPOTENT | ACK_FAILED_EXHAUSTED | N/A                  | Any          | NO       | Operator investigate |
| IDEMPOTENT    | ACK_PWA_RECEIVED     | ANY                  | Any          | YES      | Idempotent, auth not required |
| IDEMPOTENT    | ACK_PUBLISH_ACCEPTED | N/A                  | Any          | YES (if durable queue) | Per I2 |
| IDEMPOTENT    | ACK_BROKER_CONFIRMED | N/A                  | Any          | YES      | Per I2 |
| IDEMPOTENT    | ACK_FAILED_EXHAUSTED | N/A                  | Any          | YES      | Per I2 |
| IDEMPOTENT    | ACK_NOT_SENT         | N/A                  | Any          | NO       | Not sent |
| UNKNOWN       | ANY                  | ANY                  | Any          | NO       | Default retain |

KEY CASES:

    NON_IDEMPOTENT + PWA_RECEIVED + EVIDENCE_AUTHENTICATED + same boot:
        → Eviction YES.
        → Authentication was confirmed when ack_confirm was received.
        → RAM evidence is available and says "authenticated."

    NON_IDEMPOTENT + PWA_RECEIVED + EVIDENCE_UNAVAILABLE + after reboot:
        → Eviction NO.
        → RAM evidence was lost on reboot.
        → Cannot prove authentication → RETAIN.
        → This is the case auditor identified as P1.
        → Rev21 closes it: evidence loss → RETAIN, never permission gain.

    NON_IDEMPOTENT + PWA_RECEIVED + EVIDENCE_ACKDIGEST + same boot:
        → Eviction NO.
        → ack_confirm received but sender not authenticated (public broker, no ACL).
        → ackDigest = content binding only, not sender auth.

CONSISTENCY WITH I2e:
    I2e: "Default = RETAIN (if any check is uncertain → NO eviction)."
    EVIDENCE_UNAVAILABLE = uncertain → RETAIN. ✅
    EVIDENCE_ACKDIGEST = auth not confirmed = uncertain → RETAIN. ✅
    EVIDENCE_AUTHENTICATED = auth confirmed = certain → may evict. ✅
```

---

## 6. Rule Compliance

| Item | New Field? | New Feature? |
|------|-----------|-------------|
| Evidence loss → RETAIN rule | NO (normative contract) | NO |
| Event-time vs decision-time | NO (terminology) | NO |
| deliveryState vs authEvidence separation | NO (dimension clarification) | NO |
| Cross-product with auth × reboot | NO (verification) | NO |

**Zero new metadata. Zero new features. Zero architecture changes.**

- `authenticationEvidence` is RAM-only (same as Rev20).
- `EVIDENCE_UNAVAILABLE` after reboot → RETAIN (normative rule, not stored field).
- No new NVS keys, no new record fields, no new stored states.

---

## 7. Regression Check

```
REGRESSION CHECK (REV21):

I0/I0a (executor + observation):
    Rev20: Unchanged. Rev21: No change. ✅

I1 (canonical + recovery):
    Rev20: Unchanged. Rev21: No change. ✅

I2 (eviction safety):
    Rev20: Auth gate restored (PWA_RECEIVED_ACKDIGEST vs AUTH).
    Rev21: Auth evidence lifetime defined. Evidence loss → RETAIN.
    Rev14 I2e: "Default = RETAIN if uncertain." ✅ Consistent.
    Rev15: "ackDigest ≠ sender auth." ✅ Consistent.
    ✅ No regression. Auth gate strengthened (evidence loss → conservative).

I3 (ACK lifecycle):
    Rev20: Unchanged. Rev21: No change to state machine.
    deliveryState still has 5 values (0-4). No new stored states. ✅

ACK state machine:
    NOT_SENT → PUBLISH_ACCEPTED → BROKER_CONFIRMED → PWA_RECEIVED
    Any pre-terminal → FAILED_EXHAUSTED.
    Rev21: No change. authEvidence is separate dimension, not a state. ✅

Recovery:
    9 uniform rows, gen=0 unconditional, QUARANTINED. ✅ No change.

CRC:
    CRC-32/ISO-HDLC, test vector 0xCBF43926. ✅ No change.

CONCLUSION:
    No regressions. Rev21 closes auth evidence lifetime without breaking
    any prior invariant. Evidence loss → RETAIN is consistent with I2e
    and QUARANTINED philosophy.
```

---

## 8. Authoritative Document Stack (Rev21)

```
NORMATIVE DOCUMENTS:

    1. REV14 — base (I0-I3, CRC, canonical, recovery, eviction, ACK lifecycle)
    2. REV15 — supplement (mutation boundary, terminology, CRC gate, auth boundary)
    3. REV16 — supplement (transition graph, retention separation)
    4. REV17 — supplement (verification ordering, retry phases)
    5. REV18 — supplement (cleanup predicate, retryCount definition, re-publish invariant)
    6. REV19 — supplement (cleanup≠eviction, publish≠PUBACK, lastAttemptTs)
    7. REV20 — supplement (auth gate restoration, cross-product correction, timing)
    8. REV21 (THIS DOCUMENT) — supplement (auth evidence lifetime, event vs decision time, terminology)

PRECEDENCE:
    For authentication evidence lifetime and reboot semantics: Rev21 WINS.
    For event-time vs decision-time semantics: Rev21 WINS.
    For deliveryState vs authenticationEvidence terminology: Rev21 WINS.
    For all other topics: Rev14 (as supplemented by Rev15-Rev20) remains authoritative.

ALL OTHER DOCUMENTS: SUPERSEDED (banners applied)
```

---

## 9. Honest Limitations (Unchanged + Added)

1-19: Same as Rev14+Rev18+Rev20.

**Added**:
20. After reboot, NON_IDEMPOTENT entries with ACK_PWA_RECEIVED are NOT evictable
    until PWA re-sends ack_confirm on an authenticated MQTT connection.
21. authenticationEvidence is RAM-only — lost on reboot → RETAIN (conservative default).
22. This means non-idempotent entries may accumulate after reboots if PWA does not
    re-confirm. If journal fills: JOURNAL_FULL → operator intervention.

---

## 10. Auditor Approval Gate

**This design is NOT approved for implementation.**

Auditor must verify:
1. Evidence loss → RETAIN (§2): After reboot, PWA_RECEIVED + evidence unavailable → eviction NO?
2. Event-time vs decision-time (§3): Auth is property of ack_confirm event, not current connection?
3. Terminology (§4): deliveryState (5 values, stored) vs authEvidence (3 values, RAM-only) separated?
4. Cross-product (§5): 4 key cases verified? NON_IDEMPOTENT × PWA × auth × reboot?
5. Regression (§7): No regressions against I0-I3, ACK state machine, recovery, CRC?
6. Rule compliance (§6): Zero new metadata (authEvidence is RAM-only)?

**After auditor approval, implementation Phase 1-8 may begin. Until then: NO CODE.**

Precharge: BLOCKED
I/O expander: BLOCKED
16-relay: BLOCKED
