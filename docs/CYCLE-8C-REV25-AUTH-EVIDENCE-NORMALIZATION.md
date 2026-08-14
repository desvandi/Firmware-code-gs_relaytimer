# CYCLE-8C-Rev25: Transaction Journal v4 — Auth Evidence Normalization

**Status**: DESIGN ONLY — NO CODE IMPLEMENTATION
**Purpose**: Remove MQTT 5 false claim, separate deployment config from firmware evidence, define future sender-auth requirements.
**Rule**: No new fields, no new features, no architecture changes. Semantic normalization only.

---

## 1. Summary

| Fix | Finding | Severity | What Changed |
|-----|---------|----------|-------------|
| #1 | C8CR24-001: MQTT 5 Enhanced Auth false claim | P1 | Removed. Future sender-auth must be independently verifiable, not assumed from MQTT 5. |
| #2 | C8CR24-002: AUTHENTICATED conflates deployment config with firmware evidence | P1 | Split: DEPLOYMENT_AUTH_CONFIGURED (broker assertion) vs AUTH_EVIDENCE_AUTHENTICATED (firmware-verifiable). Only latter is eviction authority. |
| #3 | Future sender-auth requirements | P1 | Explicit requirements: sender identity binding, device authorization, event-bound, ESP32-verifiable. |
| #4 | Current implementation status | P1 | AUTH_EVIDENCE_AUTHENTICATED = NEVER. Non-idempotent eviction = NEVER. |

**No new fields. No new features. No architecture changes. No code.**

---

## 2. Fix #1: Remove MQTT 5 Enhanced Authentication Claim (C8CR24-001)

### Problem

Rev24 §3 listed "MQTT 5.0 enhanced authentication" as a potential future mechanism that provides "publisher identity to subscriber via message metadata." This is incorrect. MQTT 5 Enhanced Authentication is about the client-server authentication exchange (CONNECT/AUTH packets), NOT about automatically injecting publisher identity into PUBLISH messages delivered to subscribers.

### Solution: Remove, Replace with Honest Requirements

```
FUTURE SENDER-AUTH MECHANISM (NORMATIVE — REV25):

MQTT 5 Enhanced Authentication ALONE is NOT sufficient to provide
firmware-verifiable sender identity for ack_confirm events.

Removed from Rev24 §3:
    "MQTT 5.0 enhanced authentication: broker provides publisher identity
     to subscriber via message metadata. This is firmware-verifiable."

REV25 REPLACEMENT:
    A future sender-authentication mechanism MUST provide:
        1. Sender identity that is cryptographically bound to the specific
           ack_confirm event (not just to the MQTT connection).
        2. Device authorization binding (sender is authorized for THIS device).
        3. Independent verification by the ESP32 (firmware can verify without
           trusting broker assertions).

    Examples of mechanisms that COULD meet these requirements (if properly designed):
        - HMAC-SHA256 signature over ack_confirm payload using a per-device secret.
          (Device verifies using NVS-stored secret. Future cycle must define
          key distribution and threat model — see §4.)
        - JWT or Ed25519 signature over ack_confirm payload.
          (Device verifies using trusted public key. Future cycle must define
          key infrastructure.)

    Examples of mechanisms that do NOT meet these requirements:
        - MQTT 5 Enhanced Authentication alone (does not provide publisher
          identity to subscriber per-message).
        - Broker ACL configuration alone (broker assertion, not firmware-verifiable).
        - TLS client certificate alone (proves connection identity, not
          per-message event binding).

CONTRACT:
    The future sender-auth mechanism is NOT specified in Rev25.
    It is a FUTURE CYCLE design task that must define:
        - Cryptographic mechanism (HMAC, JWT, signature).
        - Key distribution (how device obtains secret/public key).
        - Threat model (what attacks are prevented).
        - Payload format (how auth proof is carried in ack_confirm).
        - Verification protocol (how device verifies).

    Until this future cycle is completed and audited:
        - AUTH_EVIDENCE_AUTHENTICATED is NEVER achieved.
        - Non-idempotent entries are NEVER evictable.
```

---

## 3. Fix #2: Separate Deployment Config from Firmware Evidence (C8CR24-002)

### Problem

Rev24 used "EVIDENCE_AUTHENTICATED" for two different concepts:
- **Concept A**: Deployment configuration says client is authenticated (broker ACL configured). NOT verifiable by firmware.
- **Concept B**: Firmware independently verified sender identity and authorization for this specific event. Verifiable by firmware.

Using the same name for both is dangerous — an implementer could set `AUTHENTICATED = true` based on broker configuration (Concept A) and use it for eviction, bypassing the security requirement (Concept B).

### Solution: Separate Terminology

```
AUTH EVIDENCE TERMINOLOGY (NORMATIVE — REV25):

TWO DISTINCT CONCEPTS:

1. DEPLOYMENT_AUTH_CONFIGURED (NOT eviction evidence):
    Meaning: Broker/operator has configured ACL for this device's ack_confirm topic.
    Who knows it: Operator, broker. NOT the ESP32 firmware.
    Firmware-verifiable: NO.
    Eviction authority: NONE. This is a deployment property, not event evidence.
    The ESP32 cannot observe or verify this.

2. AUTH_EVIDENCE_AUTHENTICATED (eviction evidence):
    Meaning: ESP32 has independently verified that the sender of THIS
    ack_confirm event is authenticated AND authorized for THIS device.
    Who knows it: ESP32 firmware (after cryptographic verification).
    Firmware-verifiable: YES (via HMAC/JWT/signature verification).
    Eviction authority: YES (for non-idempotent eviction, when combined with
        deliveryState == ACK_PWA_RECEIVED and all other I2 conditions met).

CURRENT IMPLEMENTATION STATUS:
    DEPLOYMENT_AUTH_CONFIGURED: Unknown to firmware. May or may not be
        configured by operator. Firmware does not check or assume this.
    AUTH_EVIDENCE_AUTHENTICATED: NEVER achieved. No sender-auth mechanism
        implemented. authEvidenceMap[requestId] is always ACKDIGEST or
        UNAVAILABLE.

FORBIDDEN:
    - Using DEPLOYMENT_AUTH_CONFIGURED as eviction evidence.
    - Assuming AUTH_EVIDENCE_AUTHENTICATED based on broker ACL configuration.
    - Setting authEvidenceMap[requestId] = AUTH_EVIDENCE_AUTHENTICATED without
      firmware-verifiable cryptographic proof bound to this specific event.

EVIDENCE VALUES (REV25 — RENAMED):
    authEvidenceMap[requestId] values:
        EVIDENCE_UNAVAILABLE = 0  (default, after reboot, or no ack_confirm received)
        EVIDENCE_ACKDIGEST = 1    (ack_confirm verified, content binding only)
        EVIDENCE_AUTHENTICATED = 2 (firmware-verifiable sender auth — NEVER in current impl)

    NOTE: Value 2 (AUTH_EVIDENCE_AUTHENTICATED) requires a future sender-auth
    mechanism (§3). Until then, value 2 is NEVER set. The value exists in the
    enum for forward compatibility, but no code path sets it.

NO NEW METADATA:
    - authEvidenceMap is RAM-only (same as Rev22/Rev23).
    - DEPLOYMENT_AUTH_CONFIGURED is not stored anywhere (it's a deployment concept).
    - AUTH_EVIDENCE_AUTHENTICATED is the renamed value 2 in the existing enum.
    - No new NVS keys, no new record fields.
```

---

## 4. Fix #3: Future Sender-Auth Requirements (P1)

### Problem

Rev24 mentioned HMAC and JWT as examples but did not define requirements. Auditor noted that HMAC with PWA shared secret is not automatically secure (PWA is a client environment, secrets can be extracted).

### Solution: Explicit Requirements + Threat Model Note

```
FUTURE SENDER-AUTH REQUIREMENTS (NORMATIVE — REV25):

A future sender-authentication mechanism MUST provide ALL of:

1. SENDER IDENTITY BINDING:
    The mechanism must bind sender identity to the specific ack_confirm event.
    It must NOT be possible to replay the auth proof for a different event.
    Example: HMAC over {requestId, commandHash, ackDigest, timestamp, nonce}.

2. DEVICE AUTHORIZATION BINDING:
    The mechanism must prove the sender is authorized for THIS specific device.
    It must NOT be possible to use auth proof from device A for device B.
    Example: Per-device HMAC secret, or JWT with device-scoped claims.

3. EVENT-BOUND:
    The auth proof must be bound to the specific ack_confirm message content.
    It must NOT be possible to transfer the proof to a different ack_confirm.
    Example: Signature over the ack_confirm payload (not just a session token).

4. ESP32-INDEPENDENTLY-VERIFIABLE:
    The ESP32 must be able to verify the proof WITHOUT contacting the broker
    or any external service.
    The verification must use only:
        - Data in the ack_confirm message.
        - Data already on the device (NVS secret, public key, etc.).
    Example: HMAC verification using NVS-stored per-device secret.

5. THREAT MODEL:
    The future cycle MUST define:
        - What attacks are prevented (replay, forgery, cross-device, etc.).
        - What attacks are NOT prevented (e.g., if PWA client is compromised).
        - Key distribution mechanism (how secret/key reaches device and PWA).
        - Key rotation mechanism (how to change keys without downtime).
        - Compromise response (what happens if key is leaked).

HMAC SHARED SECRET CAVEAT:
    If the future mechanism uses HMAC with a shared secret between PWA and ESP32:
        - The secret MUST be per-device (not fleet-wide).
        - The secret MUST be stored in NVS (not in source code).
        - The secret MUST be provisioned via a secure provisioning process.
        - The PWA client environment MUST be assessed for secret extraction risk.
        - If PWA is a browser app: the secret may be extractable from
          localStorage/service worker. This is a KNOWN LIMITATION that must be
          documented in the threat model.
        - For higher security: use a backend proxy (PWA → server → MQTT)
          so the secret stays server-side. This is a future architecture decision.

NOT IN SCOPE FOR REV25:
    - Implementing the sender-auth mechanism.
    - Designing key distribution.
    - Threat model analysis.
    These are FUTURE CYCLE tasks. Rev25 only defines REQUIREMENTS.
```

---

## 5. Current Implementation Status (Explicit)

```
CURRENT IMPLEMENTATION STATUS (NORMATIVE — REV25):

deliveryState states:
    ACK_NOT_SENT = 0           — IMPLEMENTED (queueAck creates record)
    ACK_PUBLISH_ACCEPTED = 1  — IMPLEMENTED (mqtt.publish()==true)
    ACK_BROKER_CONFIRMED = 2  — NOT IMPLEMENTED (future QoS 1 PUBACK)
    ACK_PWA_RECEIVED = 3      — NOT IMPLEMENTED (future ack_confirm)
    ACK_FAILED_EXHAUSTED = 4  — IMPLEMENTED (max retries reached)

authEvidenceMap values:
    EVIDENCE_UNAVAILABLE = 0  — DEFAULT (after reboot, before ack_confirm)
    EVIDENCE_ACKDIGEST = 1    — achievable if ack_confirm received (future)
    EVIDENCE_AUTHENTICATED = 2 — NEVER (no sender-auth mechanism)

CURRENT EVICTION MATRIX (for implementation):

| Command       | deliveryState (achievable now) | Eviction? |
|---------------|-------------------------------|-----------|
| IDEMPOTENT    | ACK_PUBLISH_ACCEPTED + queue  | YES       |
| IDEMPOTENT    | ACK_FAILED_EXHAUSTED          | YES       |
| IDEMPOTENT    | ACK_NOT_SENT                  | NO        |
| NON_IDEMPOTENT| ANY achievable                | NO        |
| UNKNOWN       | ANY                           | NO        |

NON_IDEMPOTENT entries are NEVER evictable in current implementation.
This is because:
    - BROKER_CONFIRMED: not implemented (no PUBACK handling).
    - PWA_RECEIVED: not implemented (no ack_confirm handling).
    - AUTH_EVIDENCE_AUTHENTICATED: not achievable (no sender-auth mechanism).
    - Even if PWA_RECEIVED were implemented, AUTH_EVIDENCE_AUTHENTICATED
      would still not be achievable without sender-auth.

OPERATIONAL CONSEQUENCE:
    Non-idempotent entries (OTA, factory reset, future precharge) accumulate
    in journal until:
        - Operator clears via recoverCorruptedEntry (manual).
        - Journal fills → JOURNAL_FULL → operator intervention.
    This is ACCEPTED: non-idempotent commands are rare (OTA = months apart).
    Journal capacity (32 slots) is sufficient for idempotent daily commands.
```

---

## 6. Cross-Product (Final — Rev25)

```
CROSS-PRODUCT (REV25 — FINAL, ALL ALIGNED):

| Command       | deliveryState        | authEvidence        | Eviction? | Notes |
|---------------|----------------------|---------------------|-----------|-------|
| NON_IDEMPOTENT | ACK_NOT_SENT        | N/A                 | NO        | I2 |
| NON_IDEMPOTENT | ACK_PUBLISH_ACCEPTED | N/A                | NO        | I2 |
| NON_IDEMPOTENT | ACK_BROKER_CONFIRMED | N/A                | NO        | I2: only PWA |
| NON_IDEMPOTENT | ACK_PWA_RECEIVED    | AUTH_EVIDENCE_AUTH  | YES (future) | Requires sender-auth (not implemented) |
| NON_IDEMPOTENT | ACK_PWA_RECEIVED    | EVIDENCE_ACKDIGEST  | NO        | Rev15: ackDigest ≠ auth |
| NON_IDEMPOTENT | ACK_PWA_RECEIVED    | EVIDENCE_UNAVAILABLE| NO        | Evidence lost → RETAIN |
| NON_IDEMPOTENT | ACK_FAILED_EXHAUSTED | N/A                 | NO        | I2: operator investigate |
| IDEMPOTENT    | ACK_PUBLISH_ACCEPTED | N/A                 | YES (if queue) | I2 |
| IDEMPOTENT    | ACK_BROKER_CONFIRMED | N/A                 | YES       | I2 |
| IDEMPOTENT    | ACK_PWA_RECEIVED    | ANY                 | YES       | I2 |
| IDEMPOTENT    | ACK_FAILED_EXHAUSTED | N/A                 | YES       | I2 |
| IDEMPOTENT    | ACK_NOT_SENT         | N/A                 | NO        | I2 |
| UNKNOWN       | ANY                  | ANY                 | NO        | Default retain |

CURRENT IMPLEMENTATION (achievable states only):
    NON_IDEMPOTENT: NEVER evictable (all achievable deliveryStates → NO).
    IDEMPOTENT: evictable at PUBLISH_ACCEPTED+queue and FAILED_EXHAUSTED.

CONSISTENCY:
    - I2: BROKER_CONFIRMED → NO (non-idempotent). ✅
    - I2: PWA_RECEIVED → YES (non-idempotent, if AUTH). ✅ (but AUTH not achievable)
    - Rev15: ackDigest ≠ auth. ✅
    - Rev21: evidence loss → RETAIN. ✅
    - Rev24: AUTH is deployment boundary, not firmware-verifiable. ✅
    - Rev25: AUTH_EVIDENCE_AUTHENTICATED = NEVER (current). ✅
```

---

## 7. Rule Compliance

| Item | New Field? | New Feature? |
|------|-----------|-------------|
| MQTT 5 claim removed | NO (documentation) | NO |
| Terminology split | NO (renaming, not new field) | NO |
| Future sender-auth requirements | NO (requirements definition) | NO |
| Current implementation status | NO (documentation) | NO |

**Zero new metadata. Zero new features. Zero architecture changes.**

---

## 8. Regression Check

```
REGRESSION CHECK (REV25):

I0/I0a: Unchanged. ✅
I1: Unchanged. ✅
I2: BROKER_CONFIRMED → NO. PWA_RECEIVED → YES (if AUTH achievable). ✅
I3: Unchanged. ✅
ACK state machine: Unchanged. ✅
Recovery: Unchanged. ✅
CRC: Unchanged. ✅
Auth boundary (Rev15): ackDigest ≠ auth. ✅
Evidence loss → RETAIN (Rev21): ✅
Re-auth (Rev22): STEP 6 mechanism intact. AUTH not achievable but path exists. ✅
Evidence binding (Rev22): per-requestId map. ✅
Evidence lifetime (Rev23): stale after eviction. ✅
Cleanup ≠ eviction (Rev19): ✅
publish() ≠ PUBACK (Rev19): ✅
AUTHENTICATED not firmware-verifiable (Rev24): ✅
MQTT 5 false claim removed (Rev25): ✅
Terminology split (Rev25): ✅

CONCLUSION: No regressions. Rev25 normalizes auth evidence terminology
and removes false MQTT 5 claim without breaking any prior invariant.
```

---

## 9. Authoritative Document Stack (Rev25)

```
NORMATIVE DOCUMENTS:

    Rev14 (base) + Rev15 + Rev16 + Rev17 (partial) + Rev18 + Rev19 + Rev20
    + Rev21 + Rev22 + Rev23 + Rev24 + Rev25

PRECEDENCE:
    For future sender-auth mechanism requirements: Rev25 WINS.
    For AUTH_EVIDENCE vs DEPLOYMENT_AUTH terminology: Rev25 WINS.
    For MQTT 5 Enhanced Auth: Rev25 REMOVES this claim.
    For all other topics: Rev14 (as supplemented) remains authoritative.

ALL OTHER DOCUMENTS: SUPERSEDED (banners applied)
```

---

## 10. Honest Limitations (Updated)

1-30: Same as Rev14+Rev18+Rev20+Rev21+Rev22+Rev23+Rev24.

**Updated/Critical**:
31. AUTH_EVIDENCE_AUTHENTICATED is NEVER achieved in current implementation.
    No sender-auth mechanism exists. Non-idempotent entries are NEVER evictable.
32. MQTT 5 Enhanced Authentication does NOT automatically provide publisher identity
    to subscribers. This claim is removed.
33. DEPLOYMENT_AUTH_CONFIGURED (broker ACL) is NOT firmware-verifiable and has
    NO eviction authority.
34. Future HMAC shared secret between PWA and ESP32 has known limitation:
    PWA client environment may allow secret extraction. Threat model must be
    defined in future cycle before implementation.

---

## 11. Auditor Approval Gate

**This design is NOT approved for implementation.**

Auditor must verify:
1. MQTT 5 claim (§2): Removed? Future sender-auth must be independently verifiable?
2. Terminology split (§3): DEPLOYMENT_AUTH_CONFIGURED ≠ AUTH_EVIDENCE_AUTHENTICATED?
3. Future requirements (§4): All 5 requirements defined? Threat model noted? HMAC caveat?
4. Current status (§5): AUTH_EVIDENCE_AUTHENTICATED = NEVER? Non-idempotent NEVER evictable?
5. Cross-product (§6): All rows aligned? Current implementation only has NO for non-idempotent?
6. Regression (§8): No regressions?
7. Rule compliance (§7): Zero new metadata?

**After auditor approval, implementation Phase 1-8 may begin. Until then: NO CODE.**

Precharge: BLOCKED
I/O expander: BLOCKED
16-relay: BLOCKED
