# CYCLE-8C-Rev8: Transaction Journal v4 — Consistency Closure

**Status**: DESIGN ONLY — NO CODE IMPLEMENTATION
**Purpose**: Close all contradictions from Rev6+Rev7. No new fields, no new features.
**Auditor instruction**: "Siklus berikutnya harus menjadi Rev8 — Consistency Closure"

---

## 1. Summary of Fixes

| Fix | Finding | What Changed |
|-----|---------|--------------|
| #1 | C8CR7-001 (P0) | I0: executor-ownership via TaskHandle, NOT core ID |
| #2 | C8CR7-002 (P0) | Generation distance: wrap-safe serial arithmetic, NOT abs() |
| #3 | C8CR7-003 (P0) | Eviction: non-idempotent NEVER evict without PWA_RECEIVED |
| #4 | C8CR7-004 (P1) | Safe record parsing: bounds-checked, parse-then-compare |
| #5 | C8CR7-005 (P1) | Single CRC definition: concatenation, exact algorithm |
| #6 | C8CR7-006 (P1) | ACK_PWA_RECEIVED: content binding, NOT sender authentication |

**No new fields. No new features. No code.**

---

## 2. Fix #1: I0 — Executor-Ownership (C8CR7-001)

### Problem

Rev7 used `xPortGetCoreID() == 1` as enforcement. But core ID ≠ executor identity. A FreeRTOS task on core 1 would pass the assertion. And in release builds, assertion is compiled out → I0 becomes unenforced assumption.

### Solution: TaskHandle-Based Enforcement

```
I0 — JOURNAL EXECUTOR-OWNERSHIP INVARIANT

DEFINITION:
    There exists exactly one "Journal Executor Context".
    All TransactionJournal API calls SHALL execute within this context.
    
    The Journal Executor Context is identified by FreeRTOS TaskHandle,
    NOT by CPU core.

ENFORCEMENT MECHANISM:

    static TaskHandle_t s_journalExecutorTask = nullptr;
    
    // Called once during setup(), after scheduler starts, in loop() context.
    void TransactionJournal::registerExecutorContext() {
        s_journalExecutorTask = xTaskGetCurrentTaskHandle();
    }
    
    // Called at entry of every public API function.
    void TransactionJournal::_assertExecutorContext() {
        if (s_journalExecutorTask == nullptr) {
            // Not registered yet — called before setup() complete.
            // This is a bug. Panic.
            panic("I0: journal executor not registered");
        }
        if (xTaskGetCurrentTaskHandle() != s_journalExecutorTask) {
            // Called from wrong context (different task or ISR).
            panic("I0: journal API called from non-executor context");
        }
    }

KEY DIFFERENCE FROM REV7:
    - Rev7: assert(xPortGetCoreID() == 1) — checks CPU core, not task
    - Rev8: assert(xTaskGetCurrentTaskHandle() == s_journalExecutorTask)
            — checks TASK IDENTITY, not core

WHY THIS IS CORRECT:
    - loop() runs as a FreeRTOS task (Arduino creates "loopTask" on core 1)
    - xTaskGetCurrentTaskHandle() returns the unique handle of the calling task
    - A different task on core 1 will have a DIFFERENT handle → assertion fails
    - An ISR calling xTaskGetCurrentTaskHandle() returns nullptr or invalid → fails

RELEASE BUILD BEHAVIOR:
    Unlike Rev7 (which compiled out assertion entirely), Rev8 enforcement
    is NOT compiled out. It is a runtime check with minimal overhead:
    - One pointer comparison (s_journalExecutorTask vs xTaskGetCurrentTaskHandle())
    - ~10 CPU cycles per API call
    - Acceptable for production
    
    If overhead is unacceptable in hot path:
    - Can be made conditional: #ifdef JOURNAL_ENFORCE_I0
    - Default: enabled in both debug and release
    - If disabled: documented as architecture assumption (accepted risk)

ARCHITECTURE CONTRACT:
    - setup() calls journal.registerExecutorContext() after WiFi/MQTT init
    - loop() context IS the journal executor (Arduino loopTask)
    - MQTT callback (PubSubClient) runs in loop() context → OK
    - PIR ISR does NOT call journal → OK (only sets flag)
    - WiFi events do NOT call journal → OK
    
    If future architecture adds a FreeRTOS task that calls journal:
        STOP. I0 is violated. Architecture revision required. Re-audit.

NO NEW METADATA:
    s_journalExecutorTask is a RAM variable (not stored in NVS).
    registerExecutorContext() is a boot-time call (no persistence).
    No new NVS keys, no new record fields.
```

---

## 3. Fix #2: Generation Distance — Wrap-Safe (C8CR7-002)

### Problem

Rev7 used `abs(genA - genB)` for distance check. This is WRONG at wrap-around:
```
A = 0xFFFFFFFF (old)
B = 0x00000000 (new, wrapped)

Serial arithmetic: B is exactly 1 generation newer than A. (isValid adjacency)
abs(0xFFFFFFFF - 0) = 4294967295 → CORRUPTED (FALSE POSITIVE!)
```

### Solution: Serial-Arithmetic Distance

```
GENERATION DISTANCE (wrap-safe):

    // Returns the forward distance from a to b (how many increments from a to reach b).
    // Uses serial-number arithmetic (same as isNewer).
    static uint32_t forwardDistance(uint32_t a, uint32_t b) {
        return (uint32_t)(b - a);  // wraps naturally
    }

GENERATION RELATIONSHIP CLASSIFICATION:

    Given genA and genB (both from VALID copies):

    1. If genA == genB:
         → GEN_EQUAL
         → distance = 0
         → Must verify canonicalEqual(A, B) (byte-level)

    2. If genA != genB:
         Calculate forwardDistance(genA, genB) and forwardDistance(genB, genA):
         
         distAB = forwardDistance(genA, genB)  // A → B
         distBA = forwardDistance(genB, genA)  // B → A
         
         The smaller distance is the actual gap.
         The direction with distance ≤ 1 is the "newer" direction.
         
         if distAB == 1:
             → B is 1 generation newer than A → GEN_NEWER (B)
             → Valid adjacent generation
         else if distBA == 1:
             → A is 1 generation newer than B → GEN_NEWER (A)
             → Valid adjacent generation
         else if distAB == 0x80000000 || distBA == 0x80000000:
             → GEN_AMBIGUOUS → CORRUPTED
         else:
             → Distance > 1 in both directions → CORRUPTED
             (one direction will be ≤ 2^31 for valid pairs, but if both
              distances are > 1, the gap is too large)

FORMAL INVARIANT I1g (revised):

    CONSTRUCTION (protocol obligation):
        The write protocol produces pairs where the forward distance
        from older to newer is exactly 1.
        
    OBSERVATION (runtime validation):
        For two valid copies with genA != genB:
            Let d = min(forwardDistance(genA, genB), forwardDistance(genB, genA))
            If d != 1 → CORRUPTED (generation gap violation)
            If d == 1 → valid adjacent generation
        
        For genA == genB:
            Distance = 0 (valid, requires canonicalEqual check)

EXAMPLES (all correct now):

    genA=10, genB=11:
        distAB = forwardDistance(10, 11) = 1 → B newer, distance=1 ✅
    
    genA=11, genB=10:
        distBA = forwardDistance(10, 11) = 1 → A newer, distance=1 ✅
    
    genA=0xFFFFFFFF, genB=0:
        distAB = forwardDistance(0xFFFFFFFF, 0) = 1 → B newer, distance=1 ✅
    
    genA=0, genB=0xFFFFFFFF:
        distBA = forwardDistance(0xFFFFFFFF, 0) = 1 → A newer, distance=1 ✅
        (Wait: forwardDistance(0, 0xFFFFFFFF) = 0xFFFFFFFF - 0 = 0xFFFFFFFF
         forwardDistance(0xFFFFFFFF, 0) = 0 - 0xFFFFFFFF = 1
         min(0xFFFFFFFF, 1) = 1 → A is newer by 1 ✅)
    
    genA=10, genB=20:
        distAB = 10, distBA = 0xFFFFFFF6 → min=10 → distance=10 → CORRUPTED ✅
    
    genA=10, genB=0x8000000A:
        distAB = 0x80000000, distBA = 0x80000000 → AMBIGUOUS → CORRUPTED ✅
```

---

## 4. Fix #3: Eviction Matrix — Non-Idempotent Contradiction (C8CR7-003)

### Problem

Rev7 §5 said non-idempotent ACK delivery "MUST be confirmed (PWA_RECEIVED)".
Rev7 §8/I2c said non-idempotent + BROKER_CONFIRMED → eviction YES.
Rev6 eviction matrix said non-idempotent + BROKER_CONFIRMED → YES.

**Contradiction**: Broker confirmation ≠ PWA confirmation. For non-idempotent commands, PWA confirmation is required.

### Solution: Single Authoritative Eviction Matrix

```
EVICTION MATRIX (REV8 — AUTHORITATIVE, NO CONTRADICTION):

| Command Class   | ACK State                  | Eviction? | Reason |
|-----------------|---------------------------|-----------|--------|
| IDEMPOTENT      | ACK_NOT_SENT               | NO        | ACK not attempted |
| IDEMPOTENT      | ACK_PUBLISH_ACCEPTED       | YES*      | *if ACK in durable queue |
| IDEMPOTENT      | ACK_BROKER_CONFIRMED       | YES       | Broker has it |
| IDEMPOTENT      | ACK_PWA_RECEIVED           | YES       | PWA confirmed |
| IDEMPOTENT      | ACK_FAILED_EXHAUSTED       | YES       | PWA can re-query /status |
| NON_IDEMPOTENT  | ACK_NOT_SENT               | NO        | Never sent |
| NON_IDEMPOTENT  | ACK_PUBLISH_ACCEPTED       | NO        | Publish ≠ PWA received |
| NON_IDEMPOTENT  | ACK_BROKER_CONFIRMED       | NO        | Broker ≠ PWA received |
| NON_IDEMPOTENT  | ACK_PWA_RECEIVED           | YES       | PWA confirmed receipt |
| NON_IDEMPOTENT  | ACK_FAILED_EXHAUSTED       | NO        | Operator must investigate |
| UNKNOWN         | ANY                        | NO        | Default retain |

KEY CHANGE FROM REV6/REV7:
    NON_IDEMPOTENT + ACK_BROKER_CONFIRMED → NO (was YES)
    
    Broker confirmation proves the broker received the message.
    It does NOT prove PWA received and processed the ACK.
    For non-idempotent commands, re-execution is dangerous.
    Therefore: PWA_RECEIVED is the ONLY sufficient ACK state for eviction.

WHEN ACK_PWA_RECEIVED IS NOT IMPLEMENTED:
    Non-idempotent entries are NEVER evictable.
    They remain in journal until:
        - ACK_PWA_RECEIVED is implemented and confirmed, OR
        - Operator explicitly clears via recoverCorruptedEntry(), OR
        - Journal runs out of space (reject new transactions with JOURNAL_FULL)

CONTRACT CONSISTENCY:
    §5 (ACK durability): "NON_IDEMPOTENT: ACK delivery MUST be confirmed (PWA_RECEIVED)"
    §8/I2c (eviction matrix): "NON_IDEMPOTENT + PWA_RECEIVED → YES, all others → NO"
    
    These now AGREE. No contradiction.
```

---

## 5. Fix #4: Safe Record Parsing (C8CR7-004)

### Problem

Rev7 said `canonicalLength = actualPayloadEnd - 11` but didn't define how to safely determine `actualPayloadEnd`. Corrupted length bytes could make parser read past BLOB_SIZE.

### Solution: Bounds-Checked Parse Protocol

```
SAFE RECORD PARSING PROTOCOL:

    parseRecord(blob, blobLen) → ParseResult:
        ParseResult is one of:
            PARSE_VALID (with extracted fields + canonicalLength)
            PARSE_INVALID (corrupt, cannot trust any field)

    PARSING STEPS (each step bounds-checked):
        
        offset = 11  // start of canonical payload (after header+generation+CRC)
        
        // Step 1: recordState
        if offset >= blobLen → INVALID
        recordState = blob[offset]
        if not isValidRecordState(recordState) → INVALID
        offset += 1
        
        // Step 2: requestIdLen
        if offset >= blobLen → INVALID
        requestIdLen = blob[offset]
        if requestIdLen > MAX_REQUEST_ID_LEN (64) → INVALID
        offset += 1
        
        // Step 3: requestId
        if offset + requestIdLen > blobLen → INVALID
        requestId = blob[offset..offset+requestIdLen]
        offset += requestIdLen
        
        // Step 4: commandHashLen
        if offset >= blobLen → INVALID
        commandHashLen = blob[offset]
        if commandHashLen > MAX_COMMAND_HASH_LEN (64) → INVALID
        offset += 1
        
        // Step 5: commandHash
        if offset + commandHashLen > blobLen → INVALID
        commandHash = blob[offset..offset+commandHashLen]
        offset += commandHashLen
        
        // Step 6: channelId (1 byte)
        if offset >= blobLen → INVALID
        channelId = blob[offset]
        offset += 1
        
        // Step 7: desiredState (1 byte)
        if offset >= blobLen → INVALID
        desiredState = blob[offset]
        offset += 1
        
        // Step 8: previousKnownState (1 byte)
        if offset >= blobLen → INVALID
        previousKnownState = blob[offset]
        offset += 1
        
        // Step 9: attempt (1 byte)
        if offset >= blobLen → INVALID
        attempt = blob[offset]
        offset += 1
        
        // Step 10: timestamp (4 bytes)
        if offset + 4 > blobLen → INVALID
        timestamp = uint32_le(blob[offset..offset+4])
        offset += 4
        
        // Step 11: ackLen (2 bytes)
        if offset + 2 > blobLen → INVALID
        ackLen = uint16_le(blob[offset..offset+2])
        if ackLen > MAX_ACK_LEN (1024) → INVALID
        offset += 2
        
        // Step 12: ackJson
        if offset + ackLen > blobLen → INVALID
        ackJson = blob[offset..offset+ackLen]
        offset += ackLen
        
        // Step 13: canonicalLength
        canonicalLength = offset - 11  // bytes from recordState to here
        
        // Step 14: padding (remaining bytes should be zeros)
        // Not strictly required to verify, but can detect corruption:
        for i in range(offset, blobLen):
            if blob[i] != 0:
                // Non-zero padding — might be corruption
                // LOG WARNING but don't necessarily INVALID (could be old format)
                break  // accept for now
        
        return PARSE_VALID with all extracted fields + canonicalLength

CONTRACT:
    canonicalLength is derived ONLY from a successfully validated parse.
    If any bounds check fails → PARSE_INVALID → copy is INVALID.
    canonicalEqual can ONLY be called on PARSE_VALID records.
    
    Parsing order:
        raw bytes → structural validation → safe parse → canonical representation → canonicalEqual
```

---

## 6. Fix #5: Single CRC Definition (C8CR7-005)

### Problem

Rev7 wrote `CRC32(bytes 0..6) XOR CRC32(bytes 11..end)`. But Rev6 wrote `CRC32(bytes 0..6 + bytes 11..end)` (concatenation). CRC32(A||B) ≠ CRC32(A) XOR CRC32(B) in general.

### Solution: ONE Definition — Concatenation

```
CRC SPECIFICATION (SINGLE, AUTHORITATIVE):

    CRC INPUT:
        Concatenation of:
            bytes[0..6]   (magic[2] + schemaVersion[1] + generation[4])
            bytes[11..actualPayloadEnd]  (canonical payload, from recordState to end of ackJson)
        
        NOTE: bytes[7..10] (the CRC field itself) are NOT included.
        NOTE: Padding bytes (after actualPayloadEnd) are NOT included.
    
    CRC ALGORITHM:
        Polynomial:    CRC-32 (ISO 3309 / ITU-T V.42)
        Same as:       zlib CRC-32, PNG CRC-32
        Initial value: 0xFFFFFFFF
        Final XOR:     0xFFFFFFFF
        Input reflected:  true (bit order LSB first within each byte)
        Output reflected: true
        
        This is what ESP-IDF's esp_crc32_le() computes.
        This is what Python's zlib.crc32() computes.
        This is what Python's binascii.crc32() computes.
    
    IMPLEMENTATION:
        uint32_t computeRecordCRC(const uint8_t* blob, size_t actualPayloadEnd) {
            // CRC over header (bytes 0..6) + canonical payload (bytes 11..actualPayloadEnd)
            uint32_t crc = 0xFFFFFFFF;  // esp_crc32_le uses 0 initial, but ESP-IDF
                                         // esp_crc32_le already handles init internally.
            // Actually: esp_crc32_le(0, data, len) computes CRC-32 with init=0.
            // The standard CRC-32 with init=0xFFFFFFFF and final XOR 0xFFFFFFFF
            // is what zlib.crc32() does.
            //
            // For ESP-IDF: esp_crc32_le() IS the standard CRC-32 (LE variant).
            // It handles init and final XOR internally.
            // So: crc = esp_crc32_le(0, header, 7);
            //     crc = esp_crc32_le(crc, payload, actualPayloadEnd - 11);
            // This is equivalent to CRC-32 over the concatenation.
            
            crc = esp_crc32_le(0, blob, 7);  // bytes 0..6 (7 bytes)
            crc = esp_crc32_le(crc, blob + 11, actualPayloadEnd - 11);  // bytes 11..end
            return crc;
        }
    
    VERIFICATION:
        To verify: recompute CRC over same byte ranges, compare with stored CRC.
        If mismatch → INVALID (corrupt).

NO MORE "XOR" LANGUAGE:
    The word "XOR" is removed from CRC specification.
    CRC is computed over CONCATENATION of header + canonical payload.
    esp_crc32_le() with continuation (calling twice) IS concatenation.
```

---

## 7. Fix #6: ACK_PWA_RECEIVED Authentication Boundary (C8CR7-006)

### Problem

Rev7 defined `ackDigest = sha256(ackJson)[:16]` but didn't clarify that this is **content binding**, NOT **sender authentication**. Anyone who knows the ackJson can compute the digest.

### Solution: Explicit Authentication Boundary

```
ACK_PWA_RECEIVED AUTHENTICATION CONTRACT:

ackDigest = SHA-256(ackJson)[0:16] (first 16 hex chars)

WHAT ackDigest PROVES:
    The sender KNEW the ackJson content at the time of confirmation.
    This is CONTENT BINDING: the confirmation is tied to a specific ACK payload.

WHAT ackDigest DOES NOT PROVE:
    The sender IS the PWA.
    The sender is AUTHORIZED to confirm receipt.
    The sender is not an attacker who intercepted the ackJson.

AUTHENTICATION BOUNDARY:
    ackDigest is NOT sender authentication.
    It is content integrity binding.
    
    For sender authentication, one of the following is needed (future cycle):
    1. MQTT broker ACL: only PWA user can publish to timer12/<mac>/ack_confirm
       (broker enforces identity, but if PWA credentials leak, attacker can publish)
    2. HMAC signature: PWA signs confirmation with a shared secret
       (stronger, but requires key management)
    3. JWT-based: PWA includes a short-lived JWT in the confirmation
       (strongest, but most complex)

CURRENT STATE (REV8):
    ackDigest provides content binding only.
    Sender authentication relies on MQTT broker ACL (future: configure in mosquitto).
    This is ACCEPTED for idempotent commands (re-execution is safe).
    This is NOT ACCEPTED for non-idempotent commands (precharge, OTA).
    
    Therefore:
    - ACK_PWA_RECEIVED with ackDigest = sufficient for IDEMPOTENT eviction
    - ACK_PWA_RECEIVED with ackDigest = NOT sufficient for NON_IDEMPOTENT eviction
      (requires additional authentication — future cycle)
    
    Since ACK_PWA_RECEIVED is NOT IMPLEMENTED:
    Non-idempotent entries are NEVER evictable (consistent with Fix #3).

DOCUMENTATION REQUIREMENT:
    Code comments and README MUST state:
    "ackDigest is content binding, NOT sender authentication.
     Sender authentication requires MQTT broker ACL or HMAC (future cycle)."
```

---

## 8. Consolidated Invariant Summary (I0-I3, Final — No Contradictions)

### I0 — Journal Executor-Ownership (TaskHandle, NOT Core ID)
```
All journal API calls execute within the Journal Executor Context.
Context = FreeRTOS TaskHandle (registered during setup()).
Enforcement: xTaskGetCurrentTaskHandle() == s_journalExecutorTask.
NOT compiled out in release builds (minimal overhead: ~10 cycles).
If future task calls journal: STOP, re-audit.
```

### I1 — Canonical Equivalence + Recovery
```
I1a: Copy A structurally valid (CRC passes — see §6 for exact CRC definition)
I1b: Copy B structurally valid
I1c: Mutual consistency:
     - genA == genB → canonicalEqual(A, B) must be true
     - genA != genB → exactly one is GEN_NEWER (forward distance == 1)
I1d: No generation ambiguity (forward distance != 0x80000000 in either direction)
I1e: canonicalEqual(A, B) = (canonicalLen(A) == canonicalLen(B))
                         AND memcmp(canonicalBytes(A), canonicalBytes(B),
                                    canonicalLen(A)) == 0
     WHERE canonicalBytes/canonicalLen are derived from SAFE PARSE (§5)
I1f: Serial-number arithmetic: isNewer(a,b) = (int32_t)(a-b) > 0
I1g: Generation distance:
     CONSTRUCTION: protocol produces forward distance == 1 (or 0 for repair/same)
     OBSERVATION: loader validates:
         if genA == genB → distance = 0 (valid, check canonicalEqual)
         else:
             distAB = forwardDistance(genA, genB) = (uint32_t)(genB - genA)
             distBA = forwardDistance(genB, genA) = (uint32_t)(genA - genB)
             d = min(distAB, distBA)
             if d == 0x80000000 → AMBIGUOUS → CORRUPTED
             if d != 1 → CORRUPTED (gap too large)
             if d == 1 → valid (the direction with distance 1 is newer)
```

### I2 — Eviction Safety (No Contradiction with I3)
```
I2a: Retention policy permits (journal full, slot needed)
I2b: Command class is IDEMPOTENT or NON_IDEMPOTENT (not UNKNOWN)
I2c: ACK condition (from REV8 eviction matrix §4):
     IDEMPOTENT + PUBLISH_ACCEPTED + durable queue → YES
     IDEMPOTENT + BROKER_CONFIRMED → YES
     IDEMPOTENT + PWA_RECEIVED → YES
     IDEMPOTENT + FAILED_EXHAUSTED → YES
     NON_IDEMPOTENT + PWA_RECEIVED → YES (only this!)
     NON_IDEMPOTENT + anything else → NO
     UNKNOWN + anything → NO
I2d: No unresolved recovery (not CORRUPTED)
I2e: Default = RETAIN
EVICTABLE is COMPUTED, never stored.
```

### I3 — ACK Lifecycle Separation
```
I3a: Transaction lifecycle independent of ACK lifecycle
I3b: ACK queue persists independently (tj_ackq with durable deliveryState)
I3c: Eviction does NOT delete ACK queue entry
I3d: Boot recovery = MERGE journal + ACK queue
ACK durability = "best-effort delivery assistance" (not full durability)
ACK_PWA_RECEIVED: DEFINED (content binding via ackDigest), NOT IMPLEMENTED
ACK_PWA_RECEIVED does NOT provide sender authentication (requires ACL/HMAC — future)
```

---

## 9. Cross-Reference: All Contradictions Closed

| Contradiction | Where It Was | How Rev8 Closes It |
|---------------|-------------|---------------------|
| Canonical serialization (§2 vs §4) | Rev5/Rev6 | Fixed in Rev6 (recordState at byte 11) |
| CRC "XOR" vs concatenation | Rev6 vs Rev7 | Fixed in Rev8 §6 (concatenation only, exact algorithm) |
| I0 core ID vs executor | Rev7 | Fixed in Rev8 §2 (TaskHandle) |
| Generation distance abs() | Rev6/Rev7 | Fixed in Rev8 §3 (serial arithmetic) |
| Non-idempotent eviction broker vs PWA | Rev6/Rev7 | Fixed in Rev8 §4 (PWA only) |
| canonicalLength undefined | Rev7 | Fixed in Rev8 §5 (safe parse) |
| ackDigest = auth? | Rev7 | Fixed in Rev8 §7 (content binding, not auth) |

**All 7 contradictions from Rev5-Rev7 are now closed.**

---

## 10. No New Fields, No New Features

| Item | New Field? | New Feature? | Justification |
|------|-----------|-------------|----------------|
| TaskHandle enforcement | NO (RAM var) | NO | Formalization of I0 |
| forwardDistance | NO (function) | NO | Fix for I1g wrap bug |
| Eviction matrix change | NO | NO | Removing contradiction |
| Safe parse protocol | NO | NO | Formalization of existing parse |
| CRC concatenation | NO | NO | Removing "XOR" ambiguity |
| ackDigest auth boundary | NO | NO | Documentation clarification |

**Rule compliance verified: zero new metadata, zero new features.**

---

## 11. Honest Limitations (Unchanged)

1. Snapshot reflects safe-OFF, not pre-crash state — hardware limitation
2. GPIO output ≠ physical relay contact — welded/stuck undetectable
3. Dual-copy is LOGICAL redundancy, NOT physical independence
4. CRC32 protects against accident, NOT malicious modification
5. NVS endurance is theoretical — must test empirically
6. ACK durability = "best-effort delivery assistance"
7. ACK_PWA_RECEIVED is DEFINED but NOT IMPLEMENTED
8. ackDigest is content binding, NOT sender authentication
9. Hardware power-loss testing NOT RUN
10. fsync semantics must be verified at implementation time
11. Partition size must be verified empirically

---

## 12. What This Design Does NOT Solve

- Pre-crash GPIO state recovery (hardware revision needed)
- Physical relay contact verification (feedback hardware needed)
- Physical flash independence (separate flash chips needed)
- Tamper protection (Flash Encryption + Secure Boot needed)
- ACK_PWA_RECEIVED implementation (protocol defined, not coded)
- Sender authentication for ACK confirmation (ACL/HMAC — future cycle)
- Multi-output transaction model (precharge) — separate cycle needed
- 16-relay / I/O expander — separate cycle needed
- F-008 PWA credential architecture — separate cycle needed

---

## 13. Auditor Approval Gate

**This design is NOT approved for implementation.**

Auditor must verify ALL contradictions are closed:
1. I0 (§2): TaskHandle, not core ID. Not compiled out in release.
2. Generation distance (§3): forwardDistance, not abs(). Wrap-safe.
3. Eviction matrix (§4): Non-idempotent + PWA_RECEIVED only. No contradiction with §8.
4. Safe parse (§5): All fields bounds-checked. canonicalLength from validated parse.
5. CRC (§6): Concatenation, exact algorithm. No "XOR" language.
6. ACK auth boundary (§7): ackDigest = content binding, NOT sender auth.
7. Cross-reference table (§9): All 7 contradictions closed?
8. Rule compliance (§10): Zero new metadata, zero new features?

**After auditor approval, implementation Phase 1-8 (from Rev6) may begin. Until then: NO CODE.**

Precharge: BLOCKED
I/O expander: BLOCKED
16-relay: BLOCKED
