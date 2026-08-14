# CYCLE-8C-Rev26: Transaction Journal v4 — Final Eviction Predicate

**Status**: DESIGN ONLY — NO CODE IMPLEMENTATION
**Purpose**: Close enum≠evidence (P1) + I2 authority + complete eviction predicate (P1).
**Rule**: No new fields, no new features, no architecture changes. Closure only.

---

## 1. Summary

| Fix | Finding | Severity | What Changed |
|-----|---------|----------|-------------|
| #1 | P1-1: AUTH_EVIDENCE_AUTHENTICATED enum ≠ evidence | P1 | Enum existence is FORBIDDEN to constitute evidence. No code path may produce it. |
| #2 | P1-2: I2 authority + auth as additional gate | P1 | I2 remains authoritative. Auth is ADDITIONAL mandatory gate, not replacement. |
| #3 | P1: Complete eviction predicate | P1 | Single normative predicate combining I2a-I2e + auth gate. |

**No new fields. No new features. No architecture changes. No code.**

---

## 2. Fix #1: AUTH_EVIDENCE_AUTHENTICATED Enum ≠ Evidence (P1-1)

### Problem

Rev25 §3 stated `AUTH_EVIDENCE_AUTHENTICATED → firmware-verifiable: YES`, which could be misread as "the enum value itself constitutes evidence." An implementer could set `authEvidenceMap[requestId] = 2` based on some assumption and claim evidence exists.

### Solution: Enum Existence Is Not Evidence

```
AUTH_EVIDENCE_AUTHENTICATED DEFINITION (NORMATIVE — REV26):

DEFINITION:
    AUTH_EVIDENCE_AUTHENTICATED is a future evidence class representing
    successful firmware verification of sender identity + authorization
    for a specific ack_confirm event.

CURRENT IMPLEMENTATION STATUS:
    UNACHIEVABLE. FORBIDDEN.

    No current code path may produce AUTH_EVIDENCE_AUTHENTICATED.
    The enum value 2 exists in the type definition for forward compatibility.
    Its existence does NOT constitute evidence.
    Setting authEvidenceMap[requestId] = 2 without a firmware-verifiable
    sender-auth mechanism is a SECURITY VIOLATION.

    Only a future audited sender-auth implementation (HMAC/JWT/signature,
    per Rev25 §4 requirements) may produce this value.

FORBIDDEN:
    - Assuming AUTH_EVIDENCE_AUTHENTICATED based on:
        * MQTT connection authenticated (deployment config).
        * Broker ACL configured (deployment config).
        * Message received on ACL-protected topic (not subscriber-verifiable).
        * TLS connection established (connection-level, not event-bound).
    - Setting authEvidenceMap[requestId] = 2 without cryptographic proof
      that is independently verifiable by ESP32 for THIS specific event.

ALLOWED (future, after audited implementation):
    - Setting authEvidenceMap[requestId] = 2 ONLY after:
        1. ack_confirm payload contains cryptographic proof (HMAC/JWT/signature).
        2. ESP32 independently verifies the proof using NVS-stored secret/key.
        3. Proof is bound to this specific ack_confirm event (requestId + content).
        4. Proof confirms sender is authorized for THIS device.

CONTRACT:
    enum value 2 in authEvidenceMap ≠ evidence.
    Evidence requires a verified cryptographic proof for this event.
    Until sender-auth mechanism is implemented and audited: value 2 is FORBIDDEN.
```

---

## 3. Fix #2: I2 Authority + Auth as Additional Gate (P1-2)

### Problem

Rev25 cross-product showed `PWA_RECEIVED + AUTH_EVIDENCE_AUTHENTICATED → YES (future)`, which could be read as Rev25 changing I2 from `PWA_RECEIVED → YES` to `PWA_RECEIVED + AUTH → YES`. Rev25 does not have authority to change I2.

### Solution: I2 Authoritative, Auth Is Additional Gate

```
I2 AUTHORITY (NORMATIVE — REV26):

I2 (Rev14) remains AUTHORITATIVE and UNCHANGED for journal eviction.

I2 establishes the BASELINE eviction permission:
    I2a: Retention policy permits (journal full, slot needed)
    I2b: Command class is IDEMPOTENT or NON_IDEMPOTENT (not UNKNOWN)
    I2c: ACK condition met (per I2 eviction matrix)
    I2d: No unresolved recovery (not CORRUPTED/QUARANTINED)
    I2e: Default = RETAIN (uncertain → no eviction)

For NON_IDEMPOTENT + ACK_PWA_RECEIVED:
    I2 permits eviction (I2c = YES for this combination).
    This is the BASELINE permission. I2 is NOT changed by Rev25/Rev26.

REV26 ADDS A MANDATORY SECURITY GATE:
    I2 permission ≠ automatic eviction permission.

    For NON_IDEMPOTENT commands, an ADDITIONAL mandatory gate applies:
        AUTH_EVIDENCE_AUTHENTICATED must exist for this exact
        requestId/event, produced by a firmware-verifiable sender-auth
        mechanism (currently UNACHIEVABLE, per §2).

    Therefore:
        I2 says: PWA_RECEIVED → eviction PERMITTED.
        Rev26 says: PWA_RECEIVED → eviction permitted ONLY IF
                    I2a-I2e ALL satisfied AND
                    AUTH_EVIDENCE_AUTHENTICATED exists.

    This is an ADDITIONAL gate, NOT a replacement of I2.
    I2 remains the authority for the baseline matrix.
    Rev26 adds a security gate on TOP of I2, specifically for non-idempotent.

FOR IDEMPOTENT commands:
    No additional auth gate. I2 alone is sufficient.
    (Idempotent re-execution is safe; auth not required.)

FOR UNKNOWN commands:
    I2e: Default = RETAIN. No eviction regardless of auth.
```

---

## 4. Fix #3: Complete Eviction Predicate (P1)

### Problem

Rev25 cross-product showed individual rows but did not provide a single normative predicate that combines all conditions. Implementer could miss a condition.

### Solution: Single Normative Predicate

```
COMPLETE EVICTION PREDICATE (NORMATIVE — REV26, SOLE AUTHORITY):

    journal_eviction_permitted(slotIdx) ≡

        // I2a: Retention policy
        journal_is_full
        AND slot_is_needed_for_new_transaction

        // I2b: Command class
        AND command_class(slotIdx) ∈ {IDEMPOTENT, NON_IDEMPOTENT}
        // (UNKNOWN → RETAIN)

        // I2c: ACK condition (from I2 eviction matrix)
        AND (
            (command_class == IDEMPOTENT
             AND deliveryState ∈ {
                 ACK_PUBLISH_ACCEPTED (if ACK in durable queue),
                 ACK_BROKER_CONFIRMED,
                 ACK_PWA_RECEIVED,
                 ACK_FAILED_EXHAUSTED
             })
            OR
            (command_class == NON_IDEMPOTENT
             AND deliveryState == ACK_PWA_RECEIVED
             AND authEvidenceMap.get(requestId, EVIDENCE_UNAVAILABLE)
                 == AUTH_EVIDENCE_AUTHENTICATED)
        )

        // I2d: No unresolved recovery
        AND journal_state(slotIdx) == COMMITTED
        // (not CORRUPTED, not QUARANTINED)

        // I2e: Default = RETAIN (implicit: if any condition is uncertain → NO)

        // Additional mandatory gate for NON_IDEMPOTENT:
        AND (
            command_class == IDEMPOTENT
            OR
            (command_class == NON_IDEMPOTENT
             AND deliveryState == ACK_PWA_RECEIVED
             AND authEvidenceMap.get(requestId, EVIDENCE_UNAVAILABLE)
                 == AUTH_EVIDENCE_AUTHENTICATED
             AND journal_entry_exists(requestId)
             AND ack_queue_entry_exists(requestId))
        )

    WHERE:
        AUTH_EVIDENCE_AUTHENTICATED is currently UNACHIEVABLE (per §2).
        Therefore: NON_IDEMPOTENT eviction is currently NEVER permitted.
        This predicate is normative for ALL future implementations.
        If AUTH_EVIDENCE_AUTHENTICATED becomes achievable (future sender-auth):
            The predicate still applies. Auth gate is not bypassed.

CURRENT IMPLEMENTATION (achievable states only):

    journal_eviction_permitted(slotIdx) for current implementation ≡

        I2a (journal full, slot needed)
        AND I2b (command_class ∈ {IDEMPOTENT, NON_IDEMPOTENT})
        AND I2c (
            (IDEMPOTENT
             AND deliveryState ∈ {
                 ACK_PUBLISH_ACCEPTED (if durable queue),
                 ACK_FAILED_EXHAUSTED
             })
            // NON_IDEMPOTENT: no achievable deliveryState permits eviction
            // because PWA_RECEIVED not implemented, AUTH not achievable
        )
        AND I2d (COMMITTED)
        AND I2e (default RETAIN)

    RESULT:
        IDEMPOTENT + PUBLISH_ACCEPTED + durable queue → YES.
        IDEMPOTENT + FAILED_EXHAUSTED → YES.
        All other → NO.
        NON_IDEMPOTENT → ALWAYS NO (in current implementation).
```

---

## 5. Cross-Product (Final — Rev26, Aligned with Predicate)

```
CROSS-PRODUCT (REV26 — FINAL, ALIGNED WITH §4 PREDICATE):

| Command       | deliveryState        | authEvidence    | I2c? | Auth gate? | Eviction? |
|---------------|----------------------|-----------------|------|------------|-----------|
| IDEMPOTENT    | NOT_SENT            | N/A             | NO   | N/A        | NO        |
| IDEMPOTENT    | PUBLISH_ACCEPTED+Q  | N/A             | YES  | N/A        | YES       |
| IDEMPOTENT    | BROKER_CONFIRMED    | N/A             | YES  | N/A        | YES       |
| IDEMPOTENT    | PWA_RECEIVED        | N/A             | YES  | N/A        | YES       |
| IDEMPOTENT    | FAILED_EXHAUSTED    | N/A             | YES  | N/A        | YES       |
| NON_IDEMPOTENT| NOT_SENT            | N/A             | NO   | N/A        | NO        |
| NON_IDEMPOTENT| PUBLISH_ACCEPTED    | N/A             | NO   | N/A        | NO        |
| NON_IDEMPOTENT| BROKER_CONFIRMED    | N/A             | NO   | N/A        | NO        |
| NON_IDEMPOTENT| PWA_RECEIVED        | AUTHENTICATED   | YES  | YES (future)| YES (future)|
| NON_IDEMPOTENT| PWA_RECEIVED        | ACKDIGEST       | YES  | NO         | NO        |
| NON_IDEMPOTENT| PWA_RECEIVED        | UNAVAILABLE     | YES  | NO         | NO        |
| NON_IDEMPOTENT| FAILED_EXHAUSTED    | N/A             | NO   | N/A        | NO        |
| UNKNOWN       | ANY                  | ANY             | NO   | N/A        | NO        |

CURRENT IMPLEMENTATION (achievable rows only):
    IDEMPOTENT + PUBLISH_ACCEPTED+queue → YES.
    IDEMPOTENT + FAILED_EXHAUSTED → YES.
    All other achievable → NO.
    NON_IDEMPOTENT → ALWAYS NO (AUTH not achievable, PWA not implemented).

CONSISTENCY:
    - I2: I2c values taken DIRECTLY from I2 eviction matrix. ✅
    - I2 authority: I2 is NOT changed. Auth is additional gate. ✅
    - Rev15: ackDigest ≠ auth. ✅ (ACKDIGEST → NO for non-idempotent)
    - Rev24: AUTH not firmware-verifiable. ✅ (AUTHENTICATED → UNACHIEVABLE)
    - Rev25: enum ≠ evidence. ✅ (FORBIDDEN to set value 2 without proof)
    - Rev26: Complete predicate. ✅ (single normative predicate)
```

---

## 6. Rule Compliance

| Item | New Field? | New Feature? |
|------|-----------|-------------|
| Enum ≠ evidence contract | NO (normative contract) | NO |
| I2 authority declaration | NO (documentation) | NO |
| Complete eviction predicate | NO (algorithm) | NO |

**Zero new metadata. Zero new features. Zero architecture changes.**

---

## 7. Regression Check

```
REGRESSION CHECK (REV26):

I0/I0a: Unchanged. ✅
I1: Unchanged. ✅
I2: AUTHORITATIVE and UNCHANGED. Auth is additional gate, not replacement. ✅
I3: Unchanged. ✅
ACK state machine: Unchanged. ✅
Recovery: Unchanged. ✅
CRC: Unchanged. ✅
Auth boundary (Rev15): ackDigest ≠ auth. ✅
Evidence loss → RETAIN (Rev21): ✅
Re-auth (Rev22): STEP 6 mechanism. AUTH not achievable but path exists. ✅
Evidence binding (Rev22): per-requestId map. ✅
Evidence lifetime (Rev23): stale after eviction. ✅
Cleanup ≠ eviction (Rev19): ✅
publish() ≠ PUBACK (Rev19): ✅
AUTH not firmware-verifiable (Rev24): ✅
MQTT 5 claim removed (Rev25): ✅
Terminology split (Rev25): DEPLOYMENT_AUTH vs AUTH_EVIDENCE. ✅
Enum ≠ evidence (Rev26): FORBIDDEN to set value 2 without proof. ✅

CONCLUSION: No regressions. Rev26 provides the complete eviction predicate
and clarifies I2 authority + auth as additional gate.
```

---

## 8. Authoritative Document Stack (Rev26)

```
NORMATIVE DOCUMENTS:

    Rev14 (base) + Rev15 + Rev16 + Rev17 (partial) + Rev18 + Rev19 + Rev20
    + Rev21 + Rev22 + Rev23 + Rev24 + Rev25 + Rev26

PRECEDENCE:
    For eviction predicate: Rev26 WINS (complete normative predicate).
    For enum ≠ evidence: Rev26 WINS.
    For I2 authority: Rev26 DECLARES I2 authoritative and unchanged.
    For all other topics: Rev14 (as supplemented) remains authoritative.

ALL OTHER DOCUMENTS: SUPERSEDED (banners applied)
```

---

## 9. Honest Limitations (Unchanged)

1-34: Same as Rev14+Rev18+Rev20+Rev21+Rev22+Rev23+Rev24+Rev25.

**Confirmed**:
- AUTH_EVIDENCE_AUTHENTICATED: UNACHIEVABLE. FORBIDDEN. Enum ≠ evidence.
- Non-idempotent eviction: NEVER (current implementation).
- I2: AUTHORITATIVE and UNCHANGED. Auth is additional mandatory gate.

---

## 10. Auditor Approval Gate

**This design is NOT approved for implementation.**

Auditor must verify:
1. Enum ≠ evidence (§2): Value 2 is FORBIDDEN? No code path may produce it?
2. I2 authority (§3): I2 unchanged? Auth is additional gate, not replacement?
3. Complete predicate (§4): Single normative predicate? All conditions combined?
4. Cross-product (§5): Aligned with predicate? Current implementation = non-idempotent NEVER?
5. Regression (§7): No regressions?
6. Rule compliance (§6): Zero new metadata?

**After auditor approval, implementation Phase 1-8 may begin. Until then: NO CODE.**

Precharge: BLOCKED
I/O expander: BLOCKED
16-relay: BLOCKED
