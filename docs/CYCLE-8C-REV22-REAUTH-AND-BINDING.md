# CYCLE-8C-Rev22: Transaction Journal v4 — Re-Authentication & Evidence Binding

**Status**: DESIGN ONLY — NO CODE IMPLEMENTATION
**Purpose**: Close re-authentication dead-end (P1) + evidence binding to requestId (P2).
**Rule**: No new fields, no new features, no architecture changes. Closure only.

---

## 1. Summary

| Fix | Finding | Severity | What Changed |
|-----|---------|----------|-------------|
| #1 | P1: Re-authentication dead-end after reboot | P1 | Duplicate ack_confirm on PWA_RECEIVED re-evaluates auth (not unconditional NO-OP) |
| #2 | P2: authenticationEvidence not bound to requestId | P2 | Evidence is per-requestId map (RAM-only), with explicit binding + lifetime |

**No new fields. No new features. No architecture changes. No code.**

---

## 2. Fix #1: Re-Authentication After Reboot (P1)

### Problem

Rev17 verification ordering (step 3) says:
> If deliveryState == ACK_PWA_RECEIVED → NO-OP and return.

Rev21 says:
> After reboot, PWA can re-send ack_confirm to restore evidence.

But step 3 returns BEFORE auth evaluation. So after reboot:
- `deliveryState = ACK_PWA_RECEIVED`
- `authEvidence = UNAVAILABLE`
- PWA sends `ack_confirm`
- Step 3: `deliveryState == PWA_RECEIVED` → NO-OP → return
- Auth evidence never re-evaluated → permanently UNAVAILABLE → permanently RETAIN

This is a **protocol dead-end**: evidence can never be restored after reboot.

### Solution: Re-Evaluation of Auth on Duplicate ack_confirm (Option A)

```
REVISED VERIFICATION ORDERING (NORMATIVE — REV22, SUPERSEDES REV17 STEP 3):

When ack_confirm message is received, execute in EXACTLY this order:

    STEP 1: Find ACK record by requestId in tj_ackq.
        If NOT found → log warning, return. (No state change.)

    STEP 2: Verify requestId matches ACK record.
        If mismatch → log warning, return. (No state change.)

    STEP 3: Verify commandHash matches journal entry for that requestId.
        If mismatch → log "commandHash mismatch", return. (No state change.)

    STEP 4: Verify ackDigest == SHA-256(ackJson)[0:16].
        If mismatch → log "ackDigest mismatch", return. (No state change.)

    STEP 5: Evaluate authentication evidence for THIS ack_confirm event.
        Record (in RAM, bound to requestId per §3):
            - If MQTT connection has ACL enforcement: EVIDENCE_AUTHENTICATED.
            - If MQTT connection has NO ACL: EVIDENCE_ACKDIGEST.

    STEP 6: Check if deliveryState == ACK_PWA_RECEIVED.
        If YES (duplicate confirmation):
            → deliveryState SHALL NOT change (stays ACK_PWA_RECEIVED).
            → BUT: authenticationEvidence for this requestId SHALL be UPDATED
              to the value determined in STEP 5.
            → This is NOT a "no-op" — it is "delivery state preserved,
              auth evidence refreshed."
            → Log: "ack_confirm re-evaluated for <requestId> (deliveryState
              unchanged, authEvidence updated to <value>)".
            → Return.
        If NO → continue to step 7.

    STEP 7: Require deliveryState == ACK_BROKER_CONFIRMED.
        If deliveryState != ACK_BROKER_CONFIRMED → log "not in BROKER_CONFIRMED",
            return. (No state change, no auth update — preconditions not met.)

    STEP 8: All verifications passed AND deliveryState == ACK_BROKER_CONFIRMED.
        → Transition: deliveryState → ACK_PWA_RECEIVED.
        → Record authenticationEvidence (from STEP 5) for this requestId.
        → Persist deliveryState to tj_ackq (authEvidence is RAM-only, not persisted).
        → Log "ACK_PWA_RECEIVED for <requestId>".
        → Return.

KEY CHANGE FROM REV17:
    Rev17 step 3: PWA_RECEIVED → unconditional NO-OP (return before auth check).
    Rev22 step 6: PWA_RECEIVED → deliveryState preserved, BUT auth evidence
                  is refreshed (STEP 5 runs before STEP 6).

    This allows post-reboot re-authentication:
        - After reboot: authEvidence = UNAVAILABLE.
        - PWA sends ack_confirm on authenticated connection.
        - STEP 5 evaluates auth → EVIDENCE_AUTHENTICATED.
        - STEP 6: deliveryState stays PWA_RECEIVED (no duplicate transition).
        - BUT authEvidence is now EVIDENCE_AUTHENTICATED.
        - Eviction eligibility is recomputed → may now evict (if non-idempotent).

DUPLICATE HANDLING:
    - ack_confirm while deliveryState == ACK_PWA_RECEIVED is NOT an error.
    - It is a re-confirmation that may refresh auth evidence.
    - deliveryState does NOT change (no duplicate transition).
    - authEvidence MAY change (UNAVAILABLE → AUTHENTICATED, etc.).
    - This is idempotent for deliveryState, but NOT for authEvidence.
    - authEvidence is always set to the value from the LATEST ack_confirm event.

FORBIDDEN:
    - Using current MQTT connection ACL as evidence for a PAST event (Rev21, maintained).
    - BUT: if PWA RE-SENDS ack_confirm NOW, on the CURRENT connection, the
      CURRENT connection's ACL IS the evidence for THIS NEW event.
      This is NOT temporal substitution — it is a new event with fresh evidence.
    - The evidence is for the NEW ack_confirm event, not the old one.
    - After re-evaluation: evidence reflects the latest ack_confirm.

IDEMPOTENCY OF deliveryState:
    - Multiple ack_confirm messages do not cause multiple transitions to PWA_RECEIVED.
    - First ack_confirm (from BROKER_CONFIRMED) → transitions to PWA_RECEIVED.
    - Subsequent ack_confirm (while PWA_RECEIVED) → deliveryState unchanged, auth refreshed.
    - This is safe: deliveryState is monotonic (never goes backward).

NO NEW METADATA:
    - authEvidence is still RAM-only (not persisted).
    - STEP 5 evaluation is a runtime computation.
    - The ordering change (STEP 5 before STEP 6) is an algorithm change, not a field.
    - No new NVS keys, no new record fields, no new stored states.
```

---

## 3. Fix #2: authenticationEvidence Binding to requestId (P2)

### Problem

Rev21 defined `authenticationEvidence` as a RAM-only property but did not bind it to a specific requestId. If two transactions both have `ACK_PWA_RECEIVED`, a single `authEvidence = AUTHENTICATED` flag could apply to the wrong transaction.

### Solution: Per-RequestId Evidence Map

```
AUTHENTICATION EVIDENCE BINDING (NORMATIVE — REV22):

DATA STRUCTURE:
    authenticationEvidence is a RAM-only map (not stored in NVS):

        Map<String requestId, AuthEvidence evidence>

    Where AuthEvidence is:
        EVIDENCE_UNAVAILABLE = 0  (default, or after reboot)
        EVIDENCE_ACKDIGEST = 1    (ack_confirm verified, sender NOT authenticated)
        EVIDENCE_AUTHENTICATED = 2 (ack_confirm verified, sender authenticated)

LIFETIME:
    - Entry is CREATED when ack_confirm is first received for a requestId.
    - Entry is UPDATED when a subsequent ack_confirm is received for the same requestId
      (re-evaluation per §2 STEP 5).
    - Entry is DELETED when the ACK queue entry is dequeued (removed from tj_ackq).
    - On reboot: entire map is cleared (all entries → UNAVAILABLE).
      This is consistent with Rev21: "evidence loss → RETAIN."

BINDING CONTRACT:
    - Evidence is bound to requestId (the key in the map).
    - Each requestId has its own evidence value.
    - Eviction decision for requestId A uses ONLY evidence[A].
    - Eviction decision for requestId B uses ONLY evidence[B].
    - Cross-transaction evidence confusion is IMPOSSIBLE (separate map entries).

DEFAULT VALUE:
    - On boot: map is empty. All requestIds have EVIDENCE_UNAVAILABLE.
    - On ack_confirm receipt (STEP 5): map[requestId] = evaluated value.
    - If requestId not in map: evidence = UNAVAILABLE (RETAIN).

EVICTION CHECK:
    To determine non-idempotent eviction eligibility for requestId R:

        IF deliveryState(R) == ACK_PWA_RECEIVED:
            evidence = authEvidenceMap.get(R, EVIDENCE_UNAVAILABLE)
            IF evidence == EVIDENCE_AUTHENTICATED:
                → eviction eligible
            ELSE (ACKDIGEST or UNAVAILABLE):
                → eviction BLOCKED (RETAIN)

NO NEW METADATA:
    - The map is RAM-only (not stored in NVS).
    - The map is cleared on reboot (consistent with Rev21).
    - No new NVS keys, no new record fields.
    - The map is an implementation detail (how RAM evidence is organized).
    - The normative contract is: evidence is per-requestId, RAM-only,
      cleared on reboot, updated on ack_confirm receipt.
```

---

## 4. Updated Cross-Product (with Re-Authentication)

```
CROSS-PRODUCT (REV22 — WITH RE-AUTHENTICATION):

| Command       | deliveryState        | authEvidence (before) | Event               | authEvidence (after) | Boot session  | Eviction? |
|---------------|----------------------|-----------------------|---------------------|-----------------------|---------------|-----------|
| NON_IDEMPOTENT | ACK_BROKER_CONFIRMED | UNAVAILABLE            | ack_confirm (auth)  | AUTHENTICATED         | Same boot     | YES (step 8) |
| NON_IDEMPOTENT | ACK_BROKER_CONFIRMED | UNAVAILABLE            | ack_confirm (no ACL)| ACKDIGEST             | Same boot     | NO (step 8, auth not confirmed) |
| NON_IDEMPOTENT | ACK_PWA_RECEIVED     | AUTHENTICATED          | ack_confirm (auth)  | AUTHENTICATED         | Same boot     | YES (refreshed) |
| NON_IDEMPOTENT | ACK_PWA_RECEIVED     | ACKDIGEST              | ack_confirm (auth)  | AUTHENTICATED         | Same boot     | YES (upgraded) |
| NON_IDEMPOTENT | ACK_PWA_RECEIVED     | UNAVAILABLE            | ack_confirm (auth)  | AUTHENTICATED         | Same boot     | YES (restored) |
| NON_IDEMPOTENT | ACK_PWA_RECEIVED     | UNAVAILABLE            | No ack_confirm      | UNAVAILABLE           | Same boot     | NO |
| NON_IDEMPOTENT | ACK_PWA_RECEIVED     | UNAVAILABLE            | ack_confirm (no ACL)| ACKDIGEST             | Same boot     | NO |
| NON_IDEMPOTENT | ACK_PWA_RECEIVED     | (any)                  | (nothing)           | UNAVAILABLE           | After reboot  | NO (evidence lost) |
| NON_IDEMPOTENT | ACK_PWA_RECEIVED     | UNAVAILABLE            | ack_confirm (auth)  | AUTHENTICATED         | After reboot  | YES (re-auth!) |
| NON_IDEMPOTENT | ACK_PWA_RECEIVED     | UNAVAILABLE            | ack_confirm (no ACL)| ACKDIGEST             | After reboot  | NO |
| NON_IDEMPOTENT | ACK_PWA_RECEIVED     | AUTHENTICATED          | (nothing)           | UNAVAILABLE           | After reboot  | NO (evidence lost → RETAIN) |
| NON_IDEMPOTENT | ACK_PWA_RECEIVED     | AUTHENTICATED          | ack_confirm (auth)  | AUTHENTICATED         | After reboot  | YES (re-confirmed) |
| IDEMPOTENT    | ACK_PWA_RECEIVED     | ANY                    | ANY                 | ANY                   | Any           | YES (auth not required) |

KEY CASE (REV22 FIXES P1):
    NON_IDEMPOTENT + PWA_RECEIVED + UNAVAILABLE + after reboot + ack_confirm (auth):
        → STEP 5 evaluates auth → AUTHENTICATED.
        → STEP 6: deliveryState stays PWA_RECEIVED (no duplicate transition).
        → authEvidence updated to AUTHENTICATED.
        → Eviction NOW eligible. ✅ Dead-end resolved.

KEY CASE (EVIDENCE LOSS STILL SAFE):
    NON_IDEMPOTENT + PWA_RECEIVED + AUTHENTICATED + after reboot (no ack_confirm):
        → Map cleared on boot → UNAVAILABLE.
        → No ack_confirm received → stays UNAVAILABLE.
        → Eviction BLOCKED (RETAIN). ✅ Safe (no permission gain).

KEY CASE (DOWNGRADE):
    NON_IDEMPOTENT + PWA_RECEIVED + AUTHENTICATED + ack_confirm (no ACL):
        → STEP 5 evaluates auth → ACKDIGEST (current connection has no ACL).
        → STEP 6: authEvidence updated to ACKDIGEST.
        → Eviction now BLOCKED.
        → This is a DOWNGRADE (AUTHENTICATED → ACKDIGEST).
        → Is this safe? YES: the latest ack_confirm event was not authenticated.
        → Evidence reflects the LATEST event, not the best historical event.
        → This prevents stale auth from being used when current connection is unauthenticated.
        → However: PWA can re-send on authenticated connection to restore AUTHENTICATED.

DOWNGRADE RATIONALE:
    If PWA sends ack_confirm on an unauthenticated connection (e.g., public WiFi,
    public broker without ACL), the evidence should NOT be upgraded to AUTHENTICATED.
    The evidence reflects the CURRENT event's auth status.
    If the latest event is unauthenticated → evidence is ACKDIGEST.
    PWA must re-send on authenticated connection to restore AUTHENTICATED.
    This is conservative and correct.
```

---

## 5. Rule Compliance

| Item | New Field? | New Feature? |
|------|-----------|-------------|
| Re-evaluation of auth on duplicate ack_confirm | NO (algorithm ordering change) | NO |
| Per-requestId evidence map | NO (RAM data structure, not stored) | NO |
| Cross-product with re-auth | NO (verification) | NO |

**Zero new metadata. Zero new features. Zero architecture changes.**

- authEvidence map is RAM-only (cleared on reboot).
- STEP 5 before STEP 6 is an ordering change (no storage).
- No new NVS keys, no new record fields, no new stored states.

---

## 6. Regression Check

```
REGRESSION CHECK (REV22):

I0/I0a (executor + observation):
    Unchanged. ✅

I1 (canonical + recovery):
    Unchanged. ✅

I2 (eviction safety):
    Rev21: evidence loss → RETAIN.
    Rev22: evidence can be restored via re-auth (but only via fresh ack_confirm).
    Rev22: evidence loss without re-auth → still RETAIN. ✅
    I2e: "uncertain → no eviction." UNAVAILABLE = uncertain = RETAIN. ✅

I3 (ACK lifecycle):
    deliveryState transitions unchanged (5 states, same graph).
    ack_confirm duplicate: deliveryState preserved, authEvidence refreshed. ✅
    No new stored states. ✅

ACK state machine:
    NOT_SENT → PUBLISH_ACCEPTED → BROKER_CONFIRMED → PWA_RECEIVED
    PWA_RECEIVED duplicate → deliveryState unchanged, authEvidence refreshed. ✅
    No backward transitions. ✅

Recovery:
    9 uniform rows, gen=0 unconditional, QUARANTINED. ✅ Unchanged.

CRC:
    CRC-32/ISO-HDLC, ~esp_crc32_le(0xFFFFFFFF,...), test 0xCBF43926. ✅ Unchanged.

Auth boundary (Rev15):
    ackDigest ≠ sender auth. ✅ Maintained.
    authEvidence is RAM-only, per-requestId, cleared on reboot. ✅

Cleanup ≠ eviction (Rev19):
    Maintained. ✅

publish() ≠ PUBACK (Rev19):
    Maintained. ✅

CONCLUSION:
    No regressions. Rev22 closes re-auth dead-end without breaking any prior invariant.
    Evidence loss → RETAIN is maintained (no permission gain from evidence loss).
    Evidence restoration requires fresh ack_confirm event (not temporal substitution).
```

---

## 7. Authoritative Document Stack (Rev22)

```
NORMATIVE DOCUMENTS:

    1. REV14 — base (I0-I3, CRC, canonical, recovery, eviction, ACK lifecycle)
    2. REV15 — supplement (mutation boundary, terminology, CRC gate, auth boundary)
    3. REV16 — supplement (transition graph, retention separation)
    4. REV17 — supplement (verification ordering — SUPERSEDED by Rev22 §2 for step 3/5/6)
    5. REV18 — supplement (cleanup predicate, retryCount definition, re-publish invariant)
    6. REV19 — supplement (cleanup≠eviction, publish≠PUBACK, lastAttemptTs)
    7. REV20 — supplement (auth gate restoration, cross-product correction, timing)
    8. REV21 — supplement (auth evidence lifetime, event vs decision time, terminology)
    9. REV22 (THIS DOCUMENT) — supplement (re-auth after reboot, evidence binding)

PRECEDENCE:
    For ack_confirm verification ordering: Rev22 WINS (supersedes Rev17 step 3).
    For authenticationEvidence binding: Rev22 WINS.
    For re-authentication after reboot: Rev22 WINS.
    For all other topics: Rev14 (as supplemented by Rev15-Rev21) remains authoritative.

NOTE:
    Rev17's step 3 (PWA_RECEIVED → unconditional NO-OP) is SUPERSEDED by Rev22's
    revised ordering (STEP 5 evaluates auth BEFORE STEP 6 checks PWA_RECEIVED).
    Rev17's steps 1-2 and 4-8 are still valid (renumbered in Rev22).

ALL OTHER DOCUMENTS: SUPERSEDED (banners applied)
```

---

## 8. Honest Limitations (Unchanged + Added)

1-22: Same as Rev14+Rev18+Rev20+Rev21.

**Added**:
23. After reboot, NON_IDEMPOTENT entries with PWA_RECEIVED require a fresh ack_confirm
    on an authenticated MQTT connection to restore authEvidence and become evictable.
24. If PWA sends ack_confirm on an unauthenticated connection after previously being
    authenticated (same boot), authEvidence is DOWNGRADED to ACKDIGEST. This is conservative
    and correct (latest event's auth status prevails).

---

## 9. Auditor Approval Gate

**This design is NOT approved for implementation.**

Auditor must verify:
1. Re-auth after reboot (§2): STEP 5 before STEP 6? PWA_RECEIVED duplicate → deliveryState preserved, auth refreshed?
2. Dead-end resolved (§2): After reboot + UNAVAILABLE + ack_confirm(auth) → AUTHENTICATED → eviction YES?
3. Evidence binding (§3): Per-requestId map? RAM-only? Cleared on reboot?
4. Cross-product (§4): 13 cases? All verified? Downgrade case safe?
5. Regression (§6): No regressions against I0-I3, ACK state machine, recovery, CRC?
6. Rule compliance (§5): Zero new metadata?

**After auditor approval, implementation Phase 1-8 may begin. Until then: NO CODE.**

Precharge: BLOCKED
I/O expander: BLOCKED
16-relay: BLOCKED
