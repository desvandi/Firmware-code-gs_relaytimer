<!-- SUPERSEDED BANNER -->
<!-- ╔══════════════════════════════════════════════════════════╗ -->
<!-- ║  ⚠️  SUPERSEDED — HISTORICAL DOCUMENT — DO NOT IMPLEMENT  ║ -->
<!-- ║  Superseded by Rev19 for: cleanup≠eviction boundary, publish()≠PUBACK,  ║ -->
<!-- ║  lastAttemptTs initial value, semantic cross-product sweep.  ║ -->
<!-- ║  Refer to:                                                ║ -->
<!-- ║    - CYCLE-8C-REV19-SEMANTIC-BOUNDARY-CLOSURE.md            ║ -->
<!-- ║  for the authoritative supplement.                          ║ -->
<!-- ║  Rev18 remains authoritative for cleanup predicate and retryCount definition.  ║ -->
<!-- ╚══════════════════════════════════════════════════════════╝ -->
<!-- END SUPERSEDED BANNER -->


# CYCLE-8C-Rev18: Transaction Journal v4 — Cleanup Boundary & Final Sweep

**Status**: DESIGN ONLY — NO CODE IMPLEMENTATION
**Purpose**: Close 3 remaining findings + full contradiction sweep Rev14→Rev18.
**Rule**: No new fields, no new features, no architecture changes. Closure only.

---

## 1. Summary

| Fix | Finding | Severity | What Changed |
|-----|---------|----------|-------------|
| #1 | C8CR17-001: ACK_NOT_SENT cleanup contradiction | P1 | Safety predicate applies uniformly to ALL unresolved states |
| #2 | C8CR17-002: retryCount boundary | P2 | Mathematical definition + normative algorithm |
| #3 | C8CR17-003: ACK re-publication invariant | P2 | Explicit: delivery retry only, not command replay |
| #4 | Full contradiction sweep Rev14→Rev18 | — | Exhaustive cross-check |

**No new fields. No new features. No architecture changes. No code.**

---

## 2. Fix #1: ACK_NOT_SENT Cleanup — Uniform Safety Predicate (C8CR17-001)

### Problem

Rev17 §5 had two contradictory rules for `ACK_NOT_SENT`:
- "ACK_NOT_SENT → MAY be cleaned up" (state-based, no safety check)
- "NON_IDEMPOTENT + unresolved + COMMITTED → BLOCKED" (predicate-based, includes NOT_SENT)

The matrix §6 also said "Operator cleanup + NOT_SENT → YES" — contradicting the safety predicate.

### Solution: Uniform Safety Predicate

```
OPERATOR ACK CLEANUP SAFETY (NORMATIVE — REV18, SOLE AUTHORITY):

The safety predicate applies uniformly to ALL unresolved delivery states.

UNRESOLVED DELIVERY STATES:
    ACK_NOT_SENT
    ACK_PUBLISH_ACCEPTED
    ACK_BROKER_CONFIRMED

SAFETY PREDICATE (applies to ALL three states above):

    IF command is NON_IDEMPOTENT
       AND journal entry is still COMMITTED (not evicted)
       AND deliveryState is one of {ACK_NOT_SENT, ACK_PUBLISH_ACCEPTED, ACK_BROKER_CONFIRMED}
    THEN:
        Operator cleanup is BLOCKED.

    This means: for non-idempotent commands that have been executed (COMMITTED)
    but whose ACK has not been confirmed received by PWA (deliveryState is
    any pre-PWA_RECEIVED state), the ACK queue entry MUST NOT be deleted.

    Rationale: ACK_NOT_SENT does NOT mean "no obligation." It means:
    "command is durable/executed, but ACK has not yet been sent."
    Deleting this evidence destroys the only delivery record before PWA
    receives the result.

CLEANUP RULES (UNIFORM — REV18, REPLACES REV17 §5):

| deliveryState            | Command class   | Journal state  | Cleanup allowed? |
|--------------------------|-----------------|----------------|------------------|
| ACK_PWA_RECEIVED         | ANY             | ANY            | YES              |
| ACK_FAILED_EXHAUSTED     | ANY             | ANY            | YES              |
| ACK_NOT_SENT             | IDEMPOTENT      | ANY            | YES              |
| ACK_NOT_SENT             | NON_IDEMPOTENT  | COMMITTED      | NO (blocked)     |
| ACK_NOT_SENT             | NON_IDEMPOTENT  | EVICTED (EMPTY)| YES              |
| ACK_PUBLISH_ACCEPTED     | IDEMPOTENT      | ANY            | YES              |
| ACK_PUBLISH_ACCEPTED     | NON_IDEMPOTENT  | COMMITTED      | NO (blocked)     |
| ACK_PUBLISH_ACCEPTED     | NON_IDEMPOTENT  | EVICTED (EMPTY)| YES              |
| ACK_BROKER_CONFIRMED     | IDEMPOTENT      | ANY            | YES              |
| ACK_BROKER_CONFIRMED     | NON_IDEMPOTENT  | COMMITTED      | NO (blocked)     |
| ACK_BROKER_CONFIRMED     | NON_IDEMPOTENT  | EVICTED (EMPTY)| YES              |

RULE SUMMARY:
    - ACK_PWA_RECEIVED or ACK_FAILED_EXHAUSTED → always cleanable (terminal).
    - IDEMPOTENT + any pre-terminal state → cleanable (PWA can re-query /status).
    - NON_IDEMPOTENT + pre-terminal + journal COMMITTED → BLOCKED.
    - NON_IDEMPOTENT + pre-terminal + journal EVICTED → cleanable (evidence gone from journal anyway).

KEY CHANGE FROM REV17:
    Rev17: "ACK_NOT_SENT → MAY be cleaned up" (no command-class check).
    Rev18: "ACK_NOT_SENT + NON_IDEMPOTENT + COMMITTED → BLOCKED."
    
    The state-based simple rule is REMOVED.
    The safety predicate is the SOLE cleanup authority.
    Matrix §6 is updated to match.
```

---

## 3. Fix #2: retryCount Mathematical Definition (C8CR17-002)

### Problem

Rev17 used "retryCount" and "publish attempt" interchangeably without defining whether `retryCount=0` means "0 attempts done" or "0 retries after initial attempt." This creates off-by-one ambiguity at `MAX_ACK_RETRIES=10`.

### Solution: Mathematical Definition + Normative Algorithm

```
RETRYCOUNT DEFINITION (NORMATIVE — REV18):

DEFINITION:
    retryCount = number of publish attempts already performed in the current phase.
    
    retryCount = 0  → 0 attempts have been performed.
    retryCount = 1  → 1 attempt has been performed.
    retryCount = N  → N attempts have been performed.

MAX_ACK_RETRIES = 10 (existing constant, unchanged).

NORMATIVE ALGORITHM (per phase):

    When processing an ACK record in processPendingAcks():

        if (now - lastAttemptTs) < ACK_RETRY_INTERVAL_MS:
            skip (too soon to retry)

        if retryCount >= MAX_ACK_RETRIES:
            → transition to ACK_FAILED_EXHAUSTED
            → persist tj_ackq
            → (MAX_ACK_RETRIES attempts have been performed, no more)
        else:
            → perform mqtt.publish(ackJson)
            → retryCount++ (now retryCount reflects total attempts including this one)
            → lastAttemptTs = now
            → if publish succeeded:
                → if Phase 1 (NOT_SENT): transition to PUBLISH_ACCEPTED
                → (Phase 2: no state transition on publish success, waiting for ack_confirm)
            → persist tj_ackq

BOUNDARY BEHAVIOR:
    retryCount=0  → attempt #1 → retryCount becomes 1
    retryCount=1  → attempt #2 → retryCount becomes 2
    ...
    retryCount=9  → attempt #10 → retryCount becomes 10
    retryCount=10 → FAILED_EXHAUSTED (check happens before attempt, no attempt #11)

    Total attempts per phase: exactly MAX_ACK_RETRIES (10).
    After 10 unsuccessful attempts: transition to FAILED_EXHAUSTED.

PHASE TRANSITION RESET:
    When deliveryState transitions from PUBLISH_ACCEPTED to BROKER_CONFIRMED:
        retryCount = 0 (reset for Phase 2).
        Phase 2 starts with 0 attempts, allows up to 10 more.

NO NEW METADATA:
    retryCount is an existing field (1 byte in AckRecord).
    The definition is mathematical (no storage change).
    The algorithm is normative (no new constants).
```

---

## 4. Fix #3: ACK Re-Publication Invariant (C8CR17-003)

### Problem

Rev17 Phase 2 retries by re-publishing ACK via `mqtt.publish()`. But Rev17 did not explicitly state that re-publication is delivery-only, not command execution. An implementer could mistake the re-published ACK for a command/event that triggers journal execution.

### Solution: Explicit Invariant

```
ACK RE-PUBLICATION INVARIANT (NORMATIVE — REV18):

INVARIANT:
    Re-publication of an existing ACK is a delivery retry only.
    It MUST NOT cause:
        - Journal execution
        - Command replay
        - A new transaction
        - Any side effect on relay state, schedule, or configuration

CONTRACT:
    The ACK payload (ackJson) is the RESULT of a transaction that has already
    been committed to the journal. Re-publishing this payload to the MQTT broker
    is equivalent to re-sending a letter that was already written — the letter
    contains information about a completed action, it does not cause a new action.

    The MQTT topic for ACK delivery is: timer12/<mac>/ack
    This topic is SUBSCRIBED by PWA, not by the device itself.
    The device publishes to /ack, it does not subscribe to /ack.
    Therefore: re-publishing ACK cannot trigger any device-side processing.

    The MQTT topic for commands is: timer12/<mac>/command
    This topic is SUBSCRIBED by the device.
    ACK re-publication goes to /ack, NOT /command.
    Therefore: ACK re-publication is structurally incapable of triggering
    command execution (different topic, different subscriber).

VERIFICATION:
    Code review MUST verify:
        1. ACK publish always goes to /ack topic (never /command).
        2. Device never subscribes to its own /ack topic.
        3. processPendingAcks() only calls mqtt.publish(_topicAck, ...).
        4. No code path connects ACK re-publication to journal mutation.

NO NEW METADATA:
    This is a structural invariant enforced by topic separation.
    No new fields, no new constants, no new states.
```

---

## 5. Full Contradiction Sweep (Rev14→Rev18)

### Method

Search all normative documents (Rev14, Rev15, Rev16, Rev17, Rev18) for contradictions on each topic.

### Results

| Topic | Rev14 | Rev15 | Rev16 | Rev17 | Rev18 | Consistent? |
|-------|-------|-------|-------|-------|-------|-------------|
| **CRC algorithm** | CRC-32/ISO-HDLC | — | — | — | — | ✅ (Rev14, unchanged) |
| **CRC API** | ~esp_crc32_le(0xFFFFFFFF,...) | — | — | — | — | ✅ |
| **CRC test vector** | 0xCBF43926 | gate defined | — | — | — | ✅ |
| **I0 executor** | TaskHandle | — | — | — | — | ✅ |
| **I0a observation** | RAII + non-nested | mutation boundary | — | — | — | ✅ |
| **I1 canonical** | schemaVer + length + memcmp | — | — | — | — | ✅ |
| **I1f generation** | directional (distAB/distBA) | — | — | — | — | ✅ |
| **I1g distance** | construction + observation | — | — | — | — | ✅ |
| **Recovery table** | 9 uniform rows | — | — | — | — | ✅ |
| **Recovery contract** | gen=0 unconditional | — | — | — | — | ✅ |
| **QUARANTINED** | no auto-reuse | — | — | — | — | ✅ |
| **EMPTY** | not special generation | — | — | — | — | ✅ |
| **Eviction matrix** | non-idempotent PWA only | — | — | — | — | ✅ |
| **ACK states** | 5 states | — | PWA from BROKER only | retry phases | cleanup uniform | ✅ |
| **ACK transitions** | — | — | graph defined | ordering + phases | retryCount boundary | ✅ |
| **ACK retention** | eviction ≠ deletion | — | eviction ≠ deletion | operator safety | NOT_SENT safety | ✅ (Rev18 fixes Rev17 contradiction) |
| **ACK cleanup** | — | — | — | NOT_SENT contradiction | **FIXED: uniform predicate** | ✅ |
| **retryCount** | existing field | — | — | phase reset | **mathematical definition** | ✅ |
| **ACK re-publish** | — | — | — | re-publish as retry | **invariant: delivery only** | ✅ |
| **Padding** | Option B (no meaning) | — | — | — | — | ✅ |
| **Forensic log** | LittleFS, format defined | — | — | — | — | ✅ |
| **NVS model** | logical redundancy | — | — | — | — | ✅ |
| **Partition** | empirical verification | — | — | — | — | ✅ |

### Stale Terms Check

| Keyword | In Rev18? | Status |
|---------|----------|--------|
| "assert()" (normative) | NO | Removed in Rev15 |
| "init=0" (normative CRC) | NO | Removed in Rev15 |
| "abs(genA-genB)" | NO | Removed in Rev9 |
| "EPOCH_RESET" | NO | Removed in Rev9 |
| "EMPTY wins" | NO | Removed in Rev11 |
| "max(readable gen)+1" | NO | Removed in Rev12 |
| "tombstone" | NO | Removed in Rev6 |
| "bytes 12..end" | NO | Removed in Rev9 (now byte 11) |
| "ACK_NOT_SENT → MAY cleanup" (without predicate) | NO | **Removed in Rev18** (replaced by uniform predicate) |
| "eviction → dequeue" | NO | Removed in Rev16 |

**All stale terms removed from normative documents. No contradictions found.**

---

## 6. Updated ACK Cleanup Matrix (Rev18 — Sole Authority)

This REPLACES Rev17 §5 and Rev17 §6 deletion rules.

```
ACK CLEANUP MATRIX (NORMATIVE — REV18):

| deliveryState          | Command class   | Journal state    | Cleanup? | Reason |
|------------------------|-----------------|------------------|----------|--------|
| ACK_PWA_RECEIVED       | ANY             | ANY              | YES      | Delivery confirmed |
| ACK_FAILED_EXHAUSTED   | ANY             | ANY              | YES      | Max retries reached |
| ACK_NOT_SENT           | IDEMPOTENT      | ANY              | YES      | PWA can re-query /status |
| ACK_NOT_SENT           | NON_IDEMPOTENT  | COMMITTED        | NO       | Unresolved delivery obligation |
| ACK_NOT_SENT           | NON_IDEMPOTENT  | EVICTED (EMPTY)  | YES      | Journal evidence already gone |
| ACK_PUBLISH_ACCEPTED   | IDEMPOTENT      | ANY              | YES      | PWA can re-query /status |
| ACK_PUBLISH_ACCEPTED   | NON_IDEMPOTENT  | COMMITTED        | NO       | Unresolved delivery obligation |
| ACK_PUBLISH_ACCEPTED   | NON_IDEMPOTENT  | EVICTED (EMPTY)  | YES      | Journal evidence already gone |
| ACK_BROKER_CONFIRMED   | IDEMPOTENT      | ANY              | YES      | PWA can re-query /status |
| ACK_BROKER_CONFIRMED   | NON_IDEMPOTENT  | COMMITTED        | NO       | Unresolved delivery obligation |
| ACK_BROKER_CONFIRMED   | NON_IDEMPOTENT  | EVICTED (EMPTY)  | YES      | Journal evidence already gone |

RULE: For NON_IDEMPOTENT + pre-terminal state + journal COMMITTED → BLOCKED.
      Everything else → allowed.
```

---

## 7. Rule Compliance

| Item | New Field? | New Feature? |
|------|-----------|-------------|
| Uniform cleanup predicate | NO (contract) | NO |
| retryCount definition | NO (mathematical) | NO |
| ACK re-publish invariant | NO (structural) | NO |
| Contradiction sweep | NO (verification) | NO |

**Zero new metadata. Zero new features. Zero architecture changes.**

---

## 8. Authoritative Document Stack (Rev18)

```
NORMATIVE DOCUMENTS (read in order for full specification):

    1. CYCLE-8C-REV14-MUTATION-CONSOLIDATION.md
       — Base: I0-I3, CRC, canonical, recovery, eviction, ACK lifecycle, NVS model

    2. CYCLE-8C-REV15-ACK-TRANSITION.md
       — Supplement: mutation boundary (3-layer), assert→panic, CRC gate

    3. CYCLE-8C-REV16-ACK-STATE-MACHINE-CLOSURE.md
       — Supplement: transition graph (PWA from BROKER only), eviction ≠ deletion

    4. CYCLE-8C-REV17-ACK-SEMANTICS-CLOSURE.md
       — Supplement: verification ordering, retry phases, operator safety (partially superseded)

    5. CYCLE-8C-REV18-CLEANUP-BOUNDARY-SWEEP.md (THIS DOCUMENT)
       — Supplement: cleanup uniform predicate, retryCount definition, re-publish invariant, sweep

PRECEDENCE:
    For ACK cleanup rules: Rev18 WINS (supersedes Rev17 §5/§6).
    For retryCount definition: Rev18 WINS (supersedes Rev17 informal usage).
    For ACK re-publication: Rev18 WINS (new invariant).
    For all other topics: Rev14 (as supplemented by Rev15/Rev16/Rev17) remains authoritative.

ALL OTHER DOCUMENTS: SUPERSEDED (banners applied)
```

---

## 9. Honest Limitations (Unchanged + Added)

1-16: Same as Rev14+Rev17.

**Added**: Nothing new. All limitations already documented.

---

## 10. What This Design Does NOT Solve

(Same as Rev14 — no changes)

---

## 11. Auditor Approval Gate

**This design is NOT approved for implementation.**

Auditor must verify:
1. Cleanup predicate (§2): Uniform for ALL unresolved states? NOT_SENT included?
2. Cleanup matrix (§6): 11 rows? NON_IDEMPOTENT + pre-terminal + COMMITTED → NO?
3. retryCount (§3): Mathematically defined? Boundary clear (10 attempts, then FAILED)?
4. Re-publish invariant (§4): Delivery only? Not command replay? Topic separation?
5. Contradiction sweep (§5): All topics Rev14→Rev18 consistent? No stale terms?
6. Rule compliance (§7): Zero new metadata, zero new features?

**After auditor approval, implementation Phase 1-8 may begin. Until then: NO CODE.**

Precharge: BLOCKED
I/O expander: BLOCKED
16-relay: BLOCKED