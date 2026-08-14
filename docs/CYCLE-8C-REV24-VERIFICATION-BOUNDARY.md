<!-- SUPERSEDED BANNER -->
<!-- ╔══════════════════════════════════════════════════════════╗ -->
<!-- ║  ⚠️  SUPERSEDED — HISTORICAL DOCUMENT — DO NOT IMPLEMENT  ║ -->
<!-- ║  Superseded by Rev25 for: MQTT5 claim removal, terminology split,  ║ -->
<!-- ║  future sender-auth requirements, current implementation status.  ║ -->
<!-- ║  Refer to:                                                ║ -->
<!-- ║    - CYCLE-8C-REV25-AUTH-EVIDENCE-NORMALIZATION.md         ║ -->
<!-- ║  for the authoritative supplement.                          ║ -->
<!-- ║  Rev24 remains authoritative for cross-product I2 alignment and AUTHENTICATED not firmware-verifiable.  ║ -->
<!-- ╚══════════════════════════════════════════════════════════╝ -->
<!-- END SUPERSEDED BANNER -->


# CYCLE-8C-Rev24: Transaction Journal v4 — Verification Boundary Closure

**Status**: DESIGN ONLY — NO CODE IMPLEMENTATION
**Purpose**: Close cross-product I2 contradiction (P1) + AUTHENTICATED verifiability gap (P1).
**Rule**: No new fields, no new features, no architecture changes. Closure only.

---

## 1. Summary

| Fix | Finding | Severity | What Changed |
|-----|---------|----------|-------------|
| #1 | C8CR23-001: Cross-product contradicts I2 (BROKER_CONFIRMED → eviction YES) | P1 | BROKER_CONFIRMED → eviction NO; only post-transition PWA_RECEIVED → YES |
| #2 | C8CR23-002: AUTHENTICATED not verifiable by device | P1 | AUTHENTICATED is deployment trust boundary, NOT firmware-verifiable → non-idempotent NEVER evictable until sender-auth implemented |

**No new fields. No new features. No architecture changes. No code.**

---

## 2. Fix #1: Cross-Product — BROKER_CONFIRMED → Eviction NO (C8CR23-001)

### Problem

Rev23 cross-product row:
> NON_IDEMPOTENT + ACK_BROKER_CONFIRMED + AUTHENTICATED → eviction YES

This violates Rev14 I2:
> NON_IDEMPOTENT + ACK_BROKER_CONFIRMED → NO (journal eviction blocked).

The cross-product evaluated eviction at the PRE-transition state (BROKER_CONFIRMED), not the POST-transition state (PWA_RECEIVED). An implementer could evict at BROKER_CONFIRMED, bypassing I2.

### Solution: Eviction Only at POST-Transition State

```
CROSS-PRODUCT CORRECTION (NORMATIVE — REV24):

PRINCIPLE:
    Journal eviction is evaluated based on the CURRENT durable deliveryState.
    A transition (BROKER_CONFIRMED → PWA_RECEIVED) does NOT retroactively
    make the pre-transition state evictable.

    If deliveryState == ACK_BROKER_CONFIRMED:
        → Journal eviction is NO (per I2, for non-idempotent).
        → This is true REGARDLESS of authEvidence.
        → An ack_confirm may trigger STEP 7 (transition to PWA_RECEIVED).
        → AFTER the transition completes and is persisted:
            → deliveryState is now ACK_PWA_RECEIVED.
            → Eviction is re-evaluated with new deliveryState.
            → Now eviction may be YES (if authEvidence == AUTHENTICATED, per §3).

CORRECTED CROSS-PRODUCT (REV24):

| Command       | deliveryState (CURRENT) | authEvidence    | Journal Eviction? | Notes |
|---------------|--------------------------|-----------------|-------------------|-------|
| NON_IDEMPOTENT | ACK_BROKER_CONFIRMED    | ANY             | NO                | I2: only PWA_RECEIVED allows eviction. BROKER_CONFIRMED → ack_confirm triggers STEP 7 transition, but eviction is evaluated BEFORE transition. |
| NON_IDEMPOTENT | ACK_PWA_RECEIVED        | AUTHENTICATED   | YES (if achievable per §3) | Post-transition. I2: PWA_RECEIVED → YES. Auth gate per §3. |
| NON_IDEMPOTENT | ACK_PWA_RECEIVED        | ACKDIGEST       | NO                | Auth not sufficient. |
| NON_IDEMPOTENT | ACK_PWA_RECEIVED        | UNAVAILABLE     | NO                | Evidence lost → RETAIN. |
| NON_IDEMPOTENT | ACK_PUBLISH_ACCEPTED    | ANY             | NO                | I2. |
| NON_IDEMPOTENT | ACK_NOT_SENT            | ANY             | NO                | I2. |
| NON_IDEMPOTENT | ACK_FAILED_EXHAUSTED    | ANY             | NO                | I2. |
| IDEMPOTENT    | ACK_BROKER_CONFIRMED    | N/A             | YES               | I2 (idempotent). |
| IDEMPOTENT    | ACK_PWA_RECEIVED        | N/A             | YES               | I2. |
| IDEMPOTENT    | ACK_PUBLISH_ACCEPTED     | N/A             | YES (if durable queue) | I2. |
| IDEMPOTENT    | ACK_NOT_SENT            | N/A             | NO                | I2. |
| IDEMPOTENT    | ACK_FAILED_EXHAUSTED    | N/A             | YES               | I2. |
| UNKNOWN       | ANY                     | ANY             | NO                | Default retain. |

KEY CORRECTION:
    Rev23: BROKER_CONFIRMED + AUTHENTICATED → eviction YES (WRONG — pre-transition).
    Rev24: BROKER_CONFIRMED + ANY → eviction NO (correct — I2).
           Only PWA_RECEIVED + AUTHENTICATED → eviction YES (post-transition).

FLOW:
    1. deliveryState = BROKER_CONFIRMED → eviction NO.
    2. ack_confirm received → STEP 7 → transition to PWA_RECEIVED → persist.
    3. deliveryState now = PWA_RECEIVED → eviction re-evaluated.
    4. If authEvidence == AUTHENTICATED → eviction YES.
    5. If authEvidence != AUTHENTICATED → eviction NO (RETAIN).

CONSISTENCY WITH I2:
    I2: NON_IDEMPOTENT + BROKER_CONFIRMED → NO. Rev24: ✅ aligned.
    I2: NON_IDEMPOTENT + PWA_RECEIVED → YES. Rev24: ✅ aligned (post-transition).
    I2: NON_IDEMPOTENT + other → NO. Rev24: ✅ aligned.
```

---

## 3. Fix #2: AUTHENTICATED Is Deployment Trust Boundary, NOT Firmware-Verifiable (C8CR23-002)

### Problem

Rev23 requires the device to "check if MQTT ACL authorizes this client to publish to timer12/<mac>/ack_confirm." But the ESP32 (MQTT subscriber) does not receive publisher identity or ACL evaluation results from the broker. It only receives the message payload. There is no MQTT protocol mechanism for the subscriber to verify which client published the message or whether the broker's ACL authorized that client.

### Solution: Honest Deployment Trust Boundary

```
AUTHENTICATED VERIFIABILITY (NORMATIVE — REV24):

HONEST ASSESSMENT:
    The ESP32 (MQTT subscriber) CANNOT verify:
        - Which MQTT client published the ack_confirm message.
        - Whether the broker's ACL authorized that client for this topic.
        - Whether the connection was authenticated (TLS client cert, username/password).

    The ESP32 receives ONLY:
        - The message payload (requestId, commandHash, ackDigest).
        - The topic the message was published to.

    MQTT protocol does NOT provide subscriber-side visibility into:
        - Publisher identity.
        - Broker ACL evaluation result.
        - Connection authentication status of the publisher.

CONSEQUENCE:
    EVIDENCE_AUTHENTICATED cannot be achieved by firmware alone.
    It requires a sender-authentication mechanism that the device can verify.

    Available mechanisms (all require future implementation):
        1. HMAC-SHA256 signature in ack_confirm payload:
           PWA signs {requestId, commandHash, ackDigest} with a shared secret.
           Device verifies signature using per-device secret from NVS.
           This is FIRMWARE-VERIFIABLE (device has the secret).
           BUT: requires HMAC key management (future cycle).

        2. JWT in ack_confirm payload:
           PWA includes a short-lived JWT signed by a trusted issuer.
           Device verifies JWT signature using issuer's public key.
           This is FIRMWARE-VERIFIABLE (device has the public key).
           BUT: requires JWT infrastructure (future cycle).

        3. MQTT 5.0 enhanced authentication:
           Broker provides publisher identity to subscriber via message metadata.
           This is FIRMWARE-VERIFIABLE (if MQTT 5.0 is used).
           BUT: PubSubClient library uses MQTT 3.1.1 (no publisher identity).

REV24 NORMATIVE CONTRACT:

    EVIDENCE_AUTHENTICATED is a DEPLOYMENT TRUST BOUNDARY, not a firmware-verifiable property.

    Until a sender-authentication mechanism is implemented:
        - EVIDENCE_AUTHENTICATED is NEVER achieved.
        - authEvidence is always ACKDIGEST or UNAVAILABLE.
        - NON_IDEMPOTENT entries are NEVER evictable.
        - This is CONSISTENT with all prior design: "not implemented → never evictable."

    When a sender-authentication mechanism is implemented (future cycle):
        - The mechanism MUST be firmware-verifiable (device can independently verify).
        - The mechanism MUST bind sender identity to the specific ack_confirm event.
        - Examples: HMAC signature, JWT, MQTT 5.0 publisher identity.
        - Then: EVIDENCE_AUTHENTICATED may be achieved → non-idempotent eviction possible.

ACKDIGEST REMAINS VALID:
    EVIDENCE_ACKDIGEST is firmware-verifiable:
        - Device computes SHA-256(ackJson)[0:16] and compares with ackDigest.
        - This proves the sender KNEW the ackJson content (content binding).
        - This does NOT prove sender identity or authorization.
        - ACKDIGEST is sufficient for IDEMPOTENT eviction (re-execution safe).
        - ACKDIGEST is NOT sufficient for NON_IDEMPOTENT eviction (Rev15 auth boundary).

SUMMARY:
    EVIDENCE_AUTHENTICATED:
        - Concept: sender is authenticated AND authorized for this device.
        - Firmware-verifiable: NO (until sender-auth mechanism implemented).
        - Current status: NEVER achieved.
        - Non-idempotent eviction: NEVER allowed.

    EVIDENCE_ACKDIGEST:
        - Concept: sender knows ACK content (content binding).
        - Firmware-verifiable: YES (device computes and compares hash).
        - Current status: achievable when ack_confirm is received.
        - Non-idempotent eviction: NOT allowed (Rev15 auth boundary).

    EVIDENCE_UNAVAILABLE:
        - Concept: no ack_confirm received, or evidence lost (reboot).
        - Firmware-verifiable: YES (default state).
        - Current status: default after reboot, before any ack_confirm.
        - Non-idempotent eviction: NOT allowed (RETAIN).

NO NEW METADATA:
    This is a normative contract about what firmware can and cannot verify.
    No new NVS keys, no new record fields, no new stored states.
    The future sender-auth mechanism (HMAC/JWT/MQTT5) is a future feature cycle.
```

---

## 4. Updated Cross-Product (Final — Rev24)

```
CROSS-PRODUCT (REV24 — FINAL, ALIGNED WITH I2 + VERIFIABILITY):

| Command       | deliveryState        | authEvidence    | Journal Eviction? | ACK Cleanup? | Reason |
|---------------|----------------------|-----------------|-------------------|--------------|--------|
| NON_IDEMPOTENT | ACK_NOT_SENT        | N/A             | NO                | NO (unresolved) | I2 |
| NON_IDEMPOTENT | ACK_PUBLISH_ACCEPTED | N/A             | NO                | NO (unresolved) | I2 |
| NON_IDEMPOTENT | ACK_BROKER_CONFIRMED | N/A             | NO                | NO (unresolved) | I2: only PWA_RECEIVED |
| NON_IDEMPOTENT | ACK_PWA_RECEIVED    | AUTHENTICATED   | YES (if achievable) | YES | Post-transition + auth. See §3: NOT achievable until sender-auth implemented. |
| NON_IDEMPOTENT | ACK_PWA_RECEIVED    | ACKDIGEST       | NO                | NO (unresolved) | Rev15: ackDigest ≠ auth |
| NON_IDEMPOTENT | ACK_PWA_RECEIVED    | UNAVAILABLE     | NO                | NO (unresolved) | Evidence lost → RETAIN |
| NON_IDEMPOTENT | ACK_FAILED_EXHAUSTED | N/A             | NO                | YES           | I2: operator investigate |
| NON_IDEMPOTENT | (EVICTED)           | ANY             | N/A               | YES (if PWA/FAILED) | Journal gone |
| IDEMPOTENT    | ACK_NOT_SENT         | N/A             | NO                | YES           | I2 |
| IDEMPOTENT    | ACK_PUBLISH_ACCEPTED | N/A             | YES (if durable queue) | YES | I2 |
| IDEMPOTENT    | ACK_BROKER_CONFIRMED | N/A             | YES               | YES           | I2 |
| IDEMPOTENT    | ACK_PWA_RECEIVED    | ANY             | YES               | YES           | I2 |
| IDEMPOTENT    | ACK_FAILED_EXHAUSTED | N/A             | YES               | YES           | I2 |
| UNKNOWN       | ANY                  | ANY             | NO                | NO            | Default retain |

CURRENT IMPLEMENTATION STATUS:
    - ACK_BROKER_CONFIRMED: NOT IMPLEMENTED (future QoS 1).
    - ACK_PWA_RECEIVED: NOT IMPLEMENTED (future ack_confirm).
    - EVIDENCE_AUTHENTICATED: NOT ACHIEVABLE (no sender-auth mechanism).
    - Therefore: NON_IDEMPOTENT entries are NEVER evictable.
    - IDEMPOTENT entries: evictable at PUBLISH_ACCEPTED + durable queue, BROKER_CONFIRMED, PWA_RECEIVED, FAILED_EXHAUSTED.

CONSISTENCY:
    - I2: BROKER_CONFIRMED → NO for non-idempotent. ✅
    - I2: PWA_RECEIVED → YES for non-idempotent. ✅ (but AUTHENTICATED not achievable)
    - Rev15: ackDigest ≠ auth. ✅ (ACKDIGEST → NO for non-idempotent)
    - Rev21: evidence loss → RETAIN. ✅
    - Rev22: re-auth possible (STEP 6). ✅ (but AUTHENTICATED not achievable)
    - Rev23: device-scoped ACL concept. ✅ (but not firmware-verifiable)
    - Rev24: AUTHENTICATED is deployment boundary, not firmware-verifiable. ✅
```

---

## 5. Rule Compliance

| Item | New Field? | New Feature? |
|------|-----------|-------------|
| Cross-product correction | NO (verification) | NO |
| AUTHENTICATED verifiability | NO (normative contract) | NO |

**Zero new metadata. Zero new features. Zero architecture changes.**

---

## 6. Regression Check

```
REGRESSION CHECK (REV24):

I0/I0a: Unchanged. ✅
I1: Unchanged. ✅
I2: BROKER_CONFIRMED → NO (non-idempotent). Rev24 aligned. ✅
I3: Unchanged. ✅
ACK state machine: Unchanged. ✅
Recovery: Unchanged. ✅
CRC: Unchanged. ✅
Auth boundary (Rev15): ackDigest ≠ auth. Rev24 maintains. ✅
Re-auth (Rev22): STEP 6 mechanism intact. AUTHENTICATED not achievable but path exists. ✅
Evidence binding (Rev22): per-requestId map. ✅
Evidence lifetime (Rev23): stale after eviction. ✅
Cleanup ≠ eviction (Rev19): maintained. ✅
publish() ≠ PUBACK (Rev19): maintained. ✅

CONCLUSION: No regressions. Rev24 corrects cross-product and makes AUTHENTICATED
honestly unachievable (consistent with "not implemented → never evictable").
```

---

## 7. Authoritative Document Stack (Rev24)

```
NORMATIVE DOCUMENTS:

    Rev14 (base) + Rev15 + Rev16 + Rev17 (partial) + Rev18 + Rev19 + Rev20
    + Rev21 + Rev22 + Rev23 + Rev24

PRECEDENCE:
    For cross-product eviction decisions: Rev24 WINS (BROKER_CONFIRMED → NO).
    For AUTHENTICATED verifiability: Rev24 WINS (deployment boundary, not firmware-verifiable).
    For all other topics: Rev14 (as supplemented) remains authoritative.

ALL OTHER DOCUMENTS: SUPERSEDED (banners applied)
```

---

## 8. Honest Limitations (Updated)

1-27: Same as Rev14+Rev18+Rev20+Rev21+Rev22+Rev23.

**Updated/Critical**:
28. EVIDENCE_AUTHENTICATED is NOT firmware-verifiable. It is a deployment trust boundary.
    The ESP32 cannot verify publisher identity or broker ACL evaluation from a received MQTT message.
    Until a sender-authentication mechanism (HMAC/JWT/MQTT5) is implemented:
        - AUTHENTICATED is NEVER achieved.
        - Non-idempotent entries are NEVER evictable.
29. Cross-product: BROKER_CONFIRMED → eviction NO (pre-transition state, per I2).
    Only PWA_RECEIVED (post-transition) → eviction YES (if AUTHENTICATED achievable).
30. Current implementation: non-idempotent entries accumulate until operator clears or
    journal fills → JOURNAL_FULL → operator intervention.

---

## 9. Auditor Approval Gate

**This design is NOT approved for implementation.**

Auditor must verify:
1. Cross-product (§2): BROKER_CONFIRMED → eviction NO? Only PWA_RECEIVED → YES?
2. AUTHENTICATED (§3): Deployment boundary, not firmware-verifiable? Not achievable until sender-auth?
3. Cross-product (§4): All rows aligned with I2? No pre-transition eviction?
4. Non-idempotent NEVER evictable (§4): Until sender-auth implemented (future)?
5. Regression (§6): No regressions?
6. Rule compliance (§5): Zero new metadata?

**After auditor approval, implementation Phase 1-8 may begin. Until then: NO CODE.**

Precharge: BLOCKED
I/O expander: BLOCKED
16-relay: BLOCKED