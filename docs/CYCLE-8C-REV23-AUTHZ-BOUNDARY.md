<!-- SUPERSEDED BANNER -->
<!-- ╔══════════════════════════════════════════════════════════╗ -->
<!-- ║  ⚠️  SUPERSEDED — HISTORICAL DOCUMENT — DO NOT IMPLEMENT  ║ -->
<!-- ║  Superseded by Rev24 for: cross-product I2 alignment, AUTHENTICATED verifiability.  ║ -->
<!-- ║  Refer to:                                                ║ -->
<!-- ║    - CYCLE-8C-REV24-VERIFICATION-BOUNDARY.md               ║ -->
<!-- ║  for the authoritative supplement.                          ║ -->
<!-- ║  Rev23 remains authoritative for step ordering and evidence lifetime.  ║ -->
<!-- ╚══════════════════════════════════════════════════════════╝ -->
<!-- END SUPERSEDED BANNER -->


# CYCLE-8C-Rev23: Transaction Journal v4 — Authorization Boundary & Step Consistency

**Status**: DESIGN ONLY — NO CODE IMPLEMENTATION
**Purpose**: Close auth≠authorization gap (P1), Step 5/7 contradiction (P1), evidence lifetime (P2).
**Rule**: No new fields, no new features, no architecture changes. Closure only.

---

## 1. Summary

| Fix | Finding | Severity | What Changed |
|-----|---------|----------|-------------|
| #1 | P1: Authentication ≠ authorization for transaction | P1 | AUTHENTICATED requires device-scoped ACL, not just connection-level login |
| #2 | P1: Step 5/Step 7 contradiction | P1 | Auth evaluation moved after state eligibility check (Option A) |
| #3 | P2: Evidence lifetime after journal eviction | P2 | Evidence unusable after journal entry no longer exists |

**No new fields. No new features. No architecture changes. No code.**

---

## 2. Fix #1: Authentication Authorization Boundary (P1)

### Problem

Rev22 defined `EVIDENCE_AUTHENTICATED` as "MQTT connection has ACL enforcement." But ACL authentication proves the **connection/client is authenticated**, NOT that the client is **authorized for this specific transaction/device**.

An authenticated MQTT client that knows the ACK payload (requestId + commandHash + ackDigest) could send `ack_confirm` for a transaction it doesn't own, gaining `AUTHENTICATED` evidence and enabling non-idempotent eviction.

### Solution: Device-Scoped ACL Authorization

```
AUTHENTICATION AUTHORIZATION BOUNDARY (NORMATIVE — REV23):

DEFINITION:
    EVIDENCE_AUTHENTICATED requires BOTH:
        1. MQTT connection is authenticated (client has valid credentials)
        2. MQTT ACL authorizes this client to publish to
           timer12/<device_mac>/ack_confirm for THIS device

    "Authenticated" (connection-level login) ≠ "Authorized" (device-scoped ACL).

    An authenticated client without device-scoped ACL:
        → EVIDENCE_ACKDIGEST (content verified, sender NOT authorized for this device).

DEVICE-SCOPED ACL REQUIREMENT:
    The MQTT broker ACL MUST restrict publish to
    timer12/<device_mac>/ack_confirm to ONLY the authorized PWA client
    for that specific device.

    Example ACL (Mosquitto):
        user pwa-user-for-device-A4CF12345678
        topic write timer12/A4CF12345678/ack_confirm

    This ensures:
        - Client is authenticated (broker login).
        - Client is authorized for THIS device's ack_confirm topic.
        - A different authenticated client (for another device) cannot
          publish to this device's ack_confirm topic.

EVALUATION (revised for Rev23 §3):
    When evaluating auth evidence for a ack_confirm on device <MAC>:
        - Check if MQTT connection is authenticated (client has valid login).
        - Check if MQTT ACL allows this client to publish to
          timer12/<MAC>/ack_confirm.
        - If BOTH conditions are met → EVIDENCE_AUTHENTICATED.
        - If only connection authenticated (no device-scoped ACL) → EVIDENCE_ACKDIGEST.
        - If connection not authenticated → EVIDENCE_ACKDIGEST (or reject).

OPERATIONAL CONTRACT:
    For production deployment:
        - Broker MUST be configured with per-device ACL.
        - Each PWA user is authorized for exactly ONE device's ack_confirm topic.
        - Cross-device ack_confirm publishing is blocked by ACL.
        - Without this ACL configuration: EVIDENCE_AUTHENTICATED is never achieved.
        - Non-idempotent entries are NEVER evictable (consistent with "not implemented").

FORBIDDEN:
    - Treating "MQTT connection authenticated" as sufficient for EVIDENCE_AUTHENTICATED
      without verifying device-scoped ACL authorization.
    - Allowing an authenticated client for device B to publish ack_confirm
      for device A.

NO NEW METADATA:
    This is a normative contract about what EVIDENCE_AUTHENTICATED means.
    It does not add a new field. It clarifies the requirement for the
    existing MQTT ACL to be device-scoped (which is a broker configuration,
    not a firmware field).
```

---

## 3. Fix #2: Step 5/Step 7 Consistency — Auth After State Eligibility (P1)

### Problem

Rev22 Step 5 evaluates auth and writes to `authEvidenceMap[requestId]` BEFORE Step 7 checks `deliveryState == ACK_BROKER_CONFIRMED`. If state is not BROKER_CONFIRMED (e.g., PUBLISH_ACCEPTED), Step 7 returns with "no auth update" — but auth was already updated in Step 5. Contradiction.

### Solution: Option A — Auth Evaluation After State Eligibility

```
REVISED VERIFICATION ORDERING (NORMATIVE — REV23, SUPERSEDES REV22):

When ack_confirm message is received, execute in EXACTLY this order:

    STEP 1: Find ACK record by requestId in tj_ackq.
        If NOT found → log warning, return. (No state change, no auth update.)

    STEP 2: Verify requestId matches ACK record.
        If mismatch → log warning, return. (No state change, no auth update.)

    STEP 3: Verify commandHash matches journal entry for that requestId.
        If mismatch → log "commandHash mismatch", return. (No state change, no auth update.)

    STEP 4: Verify ackDigest == SHA-256(ackJson)[0:16].
        If mismatch → log "ackDigest mismatch", return. (No state change, no auth update.)

    STEP 5: Classify deliveryState.
        Determine which branch applies:
            a. deliveryState == ACK_PWA_RECEIVED → go to STEP 6 (re-auth path).
            b. deliveryState == ACK_BROKER_CONFIRMED → go to STEP 7 (first-confirm path).
            c. Any other state → REJECT.
               Log "ack_confirm received in wrong state (<state>)".
               Return. (No state change, no auth update.)

    STEP 6 (re-auth path — deliveryState == ACK_PWA_RECEIVED):
        Evaluate authentication evidence per §2:
            - Check MQTT connection authenticated AND device-scoped ACL authorized.
            - If both: newEvidence = EVIDENCE_AUTHENTICATED.
            - If connection auth only (no ACL): newEvidence = EVIDENCE_ACKDIGEST.
        Update authEvidenceMap[requestId] = newEvidence.
        deliveryState SHALL NOT change (stays ACK_PWA_RECEIVED).
        Log: "ack_confirm re-evaluated for <requestId> (deliveryState unchanged,
              authEvidence updated to <value>)".
        Return. (Auth updated, deliveryState preserved — NOT a no-op.)

    STEP 7 (first-confirm path — deliveryState == ACK_BROKER_CONFIRMED):
        Evaluate authentication evidence per §2:
            - Check MQTT connection authenticated AND device-scoped ACL authorized.
            - If both: newEvidence = EVIDENCE_AUTHENTICATED.
            - If connection auth only (no ACL): newEvidence = EVIDENCE_ACKDIGEST.
        Transition: deliveryState → ACK_PWA_RECEIVED.
        Record authEvidenceMap[requestId] = newEvidence.
        Persist deliveryState to tj_ackq (authEvidence is RAM-only, not persisted).
        Log: "ACK_PWA_RECEIVED for <requestId> (authEvidence = <value>)".
        Return.

    STEP 8: (unreachable — all cases handled in STEP 5-7)

KEY CHANGES FROM REV22:
    Rev22: STEP 5 evaluated auth for ALL ack_confirm (even if state was wrong).
           STEP 7 said "no auth update" — but STEP 5 already wrote to map. Contradiction.
    Rev23: STEP 5 classifies state. Auth evaluation happens ONLY in STEP 6 or STEP 7
           (after confirming state is PWA_RECEIVED or BROKER_CONFIRMED).
           If state is wrong (NOT_SENT, PUBLISH_ACCEPTED, FAILED_EXHAUSTED):
               → REJECT at STEP 5c. No auth evaluation. No map update. No contradiction.

CONSISTENCY:
    - Auth update happens ONLY when deliveryState is PWA_RECEIVED or BROKER_CONFIRMED.
    - For all other states: no auth update, no state change. Clean rejection.
    - No "auth updated but then rejected" contradiction.

DUPLICATE HANDLING:
    - ack_confirm while PWA_RECEIVED → STEP 6: auth refreshed, deliveryState preserved.
    - ack_confirm while BROKER_CONFIRMED → STEP 7: auth set, deliveryState transitions.
    - ack_confirm while any other state → STEP 5c: rejected, no change.

NO NEW METADATA:
    - Reordering of steps is algorithm change, not field.
    - authEvidenceMap is RAM-only (same as Rev22).
    - No new NVS keys, no new record fields.
```

---

## 4. Fix #3: Evidence Lifetime After Journal Eviction (P2)

### Problem

Rev22 said authEvidenceMap entry is "deleted when ACK queue entry is dequeued." But Rev19 established "journal eviction ≠ ACK queue deletion." So if journal is evicted but ACK queue entry remains, what happens to the evidence map entry? Can it be used for eviction decisions after the journal entry is gone?

### Solution: Evidence Inseparable From Journal Entry

```
EVIDENCE LIFETIME AFTER JOURNAL EVICTION (NORMATIVE — REV23):

CONTRACT:
    Authentication evidence MUST NOT be usable for an eviction decision
    after its associated journal entry no longer exists.

IMPLEMENTATION:
    authEvidenceMap[requestId] can only be consulted for eviction if:
        1. A journal entry for requestId EXISTS and is COMMITTED (not EMPTY).
        2. An ACK queue entry for requestId EXISTS.

    If the journal entry has been evicted (EMPTY):
        → authEvidenceMap[requestId] is STALE.
        → It MUST NOT be used for any eviction decision.
        → It MAY remain in RAM (for ACK delivery purposes) but has no
          eviction authority.
        → It is removed when the ACK queue entry is dequeued (per Rev22 lifetime).

    If the journal entry still exists but ACK queue entry was cleaned up:
        → authEvidenceMap[requestId] is STALE (no ACK record to match).
        → It MUST NOT be used for eviction.

EVICTABILITY CHECK (REVISED):
    To determine non-idempotent eviction eligibility for requestId R:

        IF journal_entry_exists(R) AND journal_entry(R).state == COMMITTED:
            IF ack_queue_entry_exists(R):
                IF deliveryState(R) == ACK_PWA_RECEIVED:
                    evidence = authEvidenceMap.get(R, EVIDENCE_UNAVAILABLE)
                    IF evidence == EVIDENCE_AUTHENTICATED:
                        → eviction eligible
                    ELSE:
                        → eviction BLOCKED (RETAIN)
                ELSE:
                    → eviction BLOCKED (per I2 matrix)
            ELSE:
                → eviction BLOCKED (no ACK record, cannot verify delivery)
        ELSE:
            → N/A (journal entry already EMPTY or doesn't exist)

KEY PRINCIPLE:
    Evidence has no eviction authority without BOTH:
        - A live journal entry (COMMITTED).
        - A live ACK queue entry (with deliveryState).
    If either is missing: eviction is BLOCKED (conservative default).

NO NEW METADATA:
    - authEvidenceMap is RAM-only (same as Rev22).
    - The "must not be usable" rule is a normative contract.
    - The evictability check is an algorithm, not a field.
    - No new NVS keys, no new record fields.
```

---

## 5. Updated Cross-Product (with Authz Boundary + Step Consistency + Lifetime)

```
CROSS-PRODUCT (REV23 — WITH AUTHORIZATION BOUNDARY):

| Command       | deliveryState        | State        | Auth status                     | Eviction? | Reason |
|---------------|----------------------|-------------|---------------------------------|-----------|--------|
| NON_IDEMPOTENT | ACK_PWA_RECEIVED    | COMMITTED   | AUTHENTICATED (auth+ACL)       | YES       | Both auth + ACL verified |
| NON_IDEMPOTENT | ACK_PWA_RECEIVED    | COMMITTED   | ACKDIGEST (auth, no ACL)       | NO        | Not device-authorized |
| NON_IDEMPOTENT | ACK_PWA_RECEIVED    | COMMITTED   | UNAVAILABLE (no ack_confirm)  | NO        | No evidence |
| NON_IDEMPOTENT | ACK_PWA_RECEIVED    | COMMITTED   | UNAVAILABLE (after reboot)     | NO        | Evidence lost → RETAIN |
| NON_IDEMPOTENT | ACK_PWA_RECEIVED    | COMMITTED   | AUTHENTICATED (re-auth post-reboot) | YES  | Fresh ack_confirm on auth+ACL connection |
| NON_IDEMPOTENT | ACK_PWA_RECEIVED    | COMMITTED   | ACKDIGEST (downgrade from AUTH)| NO        | Latest event not ACL-authorized |
| NON_IDEMPOTENT | ACK_PWA_RECEIVED    | EVICTED     | ANY                             | N/A       | Journal entry gone → evidence stale |
| NON_IDEMPOTENT | ACK_PUBLISH_ACCEPTED | COMMITTED   | N/A (STEP 5c rejects)         | NO        | Wrong state for ack_confirm |
| NON_IDEMPOTENT | ACK_BROKER_CONFIRMED | COMMITTED   | AUTHENTICATED                  | YES       | STEP 7: transitions to PWA_RECEIVED + auth |
| NON_IDEMPOTENT | ACK_BROKER_CONFIRMED | COMMITTED   | ACKDIGEST                      | NO        | STEP 7: transitions but auth not ACL |
| IDEMPOTENT    | ACK_PWA_RECEIVED    | COMMITTED   | ANY                             | YES       | Idempotent, auth not required |
| IDEMPOTENT    | ACK_PUBLISH_ACCEPTED | COMMITTED   | N/A                             | YES (if queue) | Per I2 |
| IDEMPOTENT    | ACK_NOT_SENT         | COMMITTED   | N/A                             | NO        | Not sent |
| UNKNOWN       | ANY                  | ANY         | ANY                             | NO        | Default retain |

KEY VERIFICATIONS:

    Authenticated client for WRONG device + valid ack_confirm:
        → ACL blocks publish to this device's ack_confirm topic.
        → ack_confirm never received by device.
        → No evidence update. ✅ Safe.

    Authenticated client for RIGHT device + valid ack_confirm:
        → ACL allows publish to this device's ack_confirm topic.
        → STEP 6 or 7 evaluates: auth=true, ACL=true → AUTHENTICATED.
        → Eviction eligible (if non-idempotent). ✅ Correct.

    Authenticated client with NO device-scoped ACL + valid ack_confirm:
        → Connection authenticated, but no ACL restriction.
        → STEP 6 or 7 evaluates: auth=true, ACL=false → ACKDIGEST.
        → Eviction BLOCKED. ✅ Conservative.

    ack_confirm while PUBLISH_ACCEPTED (wrong state):
        → STEP 5c: REJECT. No auth evaluation. No map update. ✅ No contradiction.

    Journal entry evicted but ACK queue entry still exists:
        → Evictability check: journal_entry_exists(R) = false.
        → Eviction BLOCKED (N/A). Evidence stale. ✅ Safe.
```

---

## 6. Rule Compliance

| Item | New Field? | New Feature? |
|------|-----------|-------------|
| Device-scoped ACL authorization | NO (broker config, not firmware field) | NO |
| Step ordering (auth after eligibility) | NO (algorithm) | NO |
| Evidence lifetime after eviction | NO (normative contract) | NO |
| Cross-product | NO (verification) | NO |

**Zero new metadata. Zero new features. Zero architecture changes.**

---

## 7. Regression Check

```
REGRESSION CHECK (REV23):

I0/I0a (executor + observation): Unchanged. ✅
I1 (canonical + recovery): Unchanged. ✅

I2 (eviction safety):
    Rev20: auth gate restored.
    Rev21: evidence loss → RETAIN.
    Rev22: re-auth after reboot.
    Rev23: auth requires device-scoped ACL (not just connection auth).
    Rev23: auth evaluation after state eligibility (no contradiction).
    I2e: "uncertain → no eviction." ACKDIGEST/UNAVAILABLE = uncertain. ✅
    ✅ No regression. Auth gate strengthened.

I3 (ACK lifecycle):
    deliveryState transitions unchanged (5 states, same graph).
    STEP 5c rejects ack_confirm in wrong state. ✅
    No new stored states. ✅

ACK state machine: Unchanged. ✅
Recovery: Unchanged. ✅
CRC: Unchanged. ✅
Cleanup ≠ eviction: Maintained. ✅
publish() ≠ PUBACK: Maintained. ✅
authEvidence per-requestId: Maintained. ✅
Evidence cleared on reboot: Maintained. ✅
Re-auth via fresh ack_confirm: Maintained (STEP 6). ✅

CONCLUSION: No regressions. Rev23 closes authz gap + step contradiction + lifetime.
```

---

## 8. Authoritative Document Stack (Rev23)

```
NORMATIVE DOCUMENTS:

    Rev14 (base) + Rev15 + Rev16 + Rev17 (partial) + Rev18 + Rev19 + Rev20
    + Rev21 + Rev22 + Rev23

PRECEDENCE:
    For AUTHENTICATED definition: Rev23 WINS (requires device-scoped ACL).
    For verification ordering: Rev23 WINS (supersedes Rev22 STEP 5/6/7).
    For evidence lifetime after eviction: Rev23 WINS.
    For all other topics: Rev14 (as supplemented by Rev15-Rev22) remains authoritative.

ALL OTHER DOCUMENTS: SUPERSEDED (banners applied)
```

---

## 9. Honest Limitations (Unchanged + Added)

1-24: Same as Rev14+Rev18+Rev20+Rev21+Rev22.

**Added**:
25. EVIDENCE_AUTHENTICATED requires broker ACL configured with per-device topic authorization.
    Without this ACL: EVIDENCE_AUTHENTICATED is never achieved → non-idempotent never evictable.
26. Auth evaluation only occurs when deliveryState is PWA_RECEIVED (STEP 6) or BROKER_CONFIRMED (STEP 7).
    For all other states: ack_confirm is rejected at STEP 5c without auth evaluation.
27. authEvidenceMap entry is stale (unusable for eviction) if journal entry is evicted, even if
    ACK queue entry still exists.

---

## 10. Auditor Approval Gate

**This design is NOT approved for implementation.**

Auditor must verify:
1. Authz boundary (§2): AUTHENTICATED requires auth + device-scoped ACL? Wrong-device client blocked?
2. Step consistency (§3): STEP 5 classifies, auth in STEP 6/7 only? No "auth updated then rejected"?
3. Evidence lifetime (§4): Stale after journal evicted? Evictability check requires both entries?
4. Cross-product (§5): 14 cases? Wrong-device safe? Wrong-state rejected at STEP 5c?
5. Regression (§7): No regressions?
6. Rule compliance (§6): Zero new metadata?

**After auditor approval, implementation Phase 1-8 may begin. Until then: NO CODE.**

Precharge: BLOCKED
I/O expander: BLOCKED
16-relay: BLOCKED