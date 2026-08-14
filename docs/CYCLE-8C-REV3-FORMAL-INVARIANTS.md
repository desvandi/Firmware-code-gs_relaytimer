# CYCLE-8C-Rev3: Transaction Journal v4 — Formal Invariants Design

**Status**: DESIGN ONLY — NO CODE IMPLEMENTATION
**Purpose**: Define formal invariants FIRST, then protocol that enforces them
**Auditor instruction**: "Buat Rev3 design-only dengan fokus sempit: formal storage invariants"

---

## 1. Root Cause Analysis (Why Rev2 Failed)

### C8CR2-002 (P0): Generation wrap comparison — mathematical error

Rev2 stated:
> "if genA > genB → pick A"
> "if genA=0 and genB=0xFFFFFFFF → pick B"

This is WRONG. `0 > 0xFFFFFFFF` is false in unsigned comparison, so the algorithm picks B. But generation 0 is NEWER than 0xFFFFFFFF (it wrapped). The algorithm picks the OLDER copy.

**The bug**: Plain `uint32_t >` comparison does not work for wrap-around sequence numbers. This is a well-known problem in distributed systems (TCP sequence numbers, Lamport clocks).

**The fix**: Serial-number arithmetic with signed difference:
```cpp
bool isNewer(uint32_t a, uint32_t b) {
    return (int32_t)(a - b) > 0;
}
```

This works because:
- `a=1, b=0xFFFFFFFF`: `(int32_t)(1 - 0xFFFFFFFF) = (int32_t)(2) = 2 > 0` → a is newer ✅
- `a=0xFFFFFFFF, b=0`: `(int32_t)(0xFFFFFFFF - 0) = -1 < 0` → b is newer ✅
- `a=5, b=3`: `(int32_t)(5-3) = 2 > 0` → a is newer ✅

**Invariant**: Two generations being compared must not differ by ≥ 2³¹ (otherwise comparison is ambiguous). For a journal with 64 slots and ~100 writes/day, this limit is reached after ~58 million years per slot. Acceptable.

### C8CR2-003 (P0): Two-valid-copy invariant missing

Rev2 crash matrix showed pre-state `A=VALID, B=INVALID` as acceptable for mutation. Then if A tears during write → both INVALID → CORRUPTED.

**The bug**: Dual-copy only provides protection when BOTH copies are valid before mutation. Rev2 allowed mutation with only one valid copy, defeating the purpose.

**The fix**: Formal invariant I1 (below) — no mutation may begin unless both copies are valid. If one is invalid, repair must happen first.

### C8CR2-001 (P0): Logical dual-copy ≠ physical independence

Rev2 assumed `tj_ra_N` and `tj_rb_N` are physically independent. But NVS manages page allocation internally — both keys may land on the same flash page. If that page fails, both copies are lost.

**The bug**: Logical redundancy (two keys) does not guarantee physical redundancy (two flash pages/chips).

**The fix**: Honest contract (below) — dual-copy protects against record-level corruption, NOT against NVS page/partition/chip failure. For physical independence, hardware redundancy is needed (out of scope).

### Pattern Across All Cycles

| Cycle | Failure Mode | Root Cause |
|-------|-------------|------------|
| 7 | No durable intent | Missing storeIntent |
| 8A | Boot contamination | Wrong boot order |
| 8B | State reset on commit | Reused function |
| 8B-Rev1 | Corruption → free slot | No CORRUPTED state |
| 8C | commit=0+COMMITTED valid | Loose invariant |
| 8C-Rev1 | putBytes assumed atomic | False assumption |
| 8C-Rev2 | Wrap comparison + missing 2-copy invariant | No formal invariants |

**The lesson**: Every cycle failed because invariants were implicit, not formalized. Rev3 puts invariants FIRST.

---

## 2. Five Formal Invariants

These are the core contract. The protocol (§3-§8) exists to enforce these. If any invariant is violated, the system is in an undefined state and must halt.

### I1 — Two-Valid-Copy Durability Invariant

```
BEFORE any normal mutation (storeIntent, markExecuting, commit, clearEntry):
    copy A is VALID (CRC passes)
    AND
    copy B is VALID (CRC passes)

If either copy is INVALID:
    NO mutation may begin.
    Repair must be performed first (§5).
    Until repair completes, slot is in RECOVERY mode.

Exception:
    recoverCorruptedEntry() may operate on INVALID+INVALID (both corrupt).
    This is an explicit recovery operation, not a normal mutation.
```

**Enforcement**: Every mutation function checks both copies at entry. If either is INVALID, mutation returns false and triggers repair.

### I2 — Generation Ordering Invariant

```
Generation comparison MUST use serial-number arithmetic:

    bool isNewer(uint32_t a, uint32_t b) {
        return (int32_t)(a - b) > 0;
    }

    bool isOlder(uint32_t a, uint32_t b) {
        return (int32_t)(a - b) < 0;
    }

    bool sameGeneration(uint32_t a, uint32_t b) {
        return a == b;
    }

Ambiguity invariant:
    Two generations being compared MUST NOT differ by ≥ 2^31.
    If they do, comparison is AMBIGUOUS → treat as CORRUPTED (cannot determine order).

    bool isAmbiguous(uint32_t a, uint32_t b) {
        uint32_t diff = a - b;
        return (diff == 0x80000000);  // exactly 2^31 apart
    }

    If isAmbiguous → CORRUPTED (cannot safely select).
```

**Enforcement**: Recovery selector uses `isNewer()`, never plain `>`. If `isAmbiguous()`, slot is marked CORRUPTED.

### I3 — Copy-on-Write Safety Invariant

```
The ACTIVE valid copy is NEVER intentionally overwritten
before a VERIFIED alternate copy exists.

Mutation protocol:
    1. Identify active copy (highest valid generation)
    2. Identify inactive copy (the other one)
    3. Write new record to inactive copy
    4. Re-read inactive copy, verify CRC
    5. If verify FAILS → abort (old active copy still valid)
    6. If verify PASSES → inactive copy is now active (higher generation)

NEVER:
    - Overwrite active copy directly
    - Skip verification step
    - Assume write succeeded without re-read
```

**Enforcement**: Mutation functions write to inactive copy only. Verification (re-read + CRC check) is mandatory before considering write successful.

### I4 — Recovery Evidence Invariant

```
CORRUPTED entry recovery MUST NOT silently erase forensic evidence.

recoverCorruptedEntry() protocol:
    1. Read both copies (best-effort extract requestId, generation, state)
    2. Write forensic record to durable audit log (separate from journal slots)
    3. Write EMPTY to copy A (generation++)
    4. Verify copy A
    5. Write EMPTY to copy B (generation = copy A generation)
    6. Verify copy B
    7. Only after both copies are EMPTY+valid → slot is free

Forensic record (durable audit log, separate NVS namespace or LittleFS file):
    - timestamp
    - slot index
    - best-effort requestId (if extractable)
    - best-effort generation
    - best-effort state
    - recovery reason (BOTH_CORRUPT, OPERATOR_INITIATED, etc.)
    - recovery action (EMPTIED)

The forensic record is NEVER automatically deleted.
It grows unboundedly (acceptable for audit trail).
GC of forensic records is operator-initiated only.
```

**Enforcement**: recoverCorruptedEntry() writes forensic record BEFORE erasing slot. If forensic write fails, recovery aborts (slot stays CORRUPTED).

### I5 — ACK Durability Invariant

```
Eviction of a COMMITTED entry requires:

    1. Entry state is COMMITTED or COMMITTED_UNKNOWN
    AND
    2. ackJson is durably persisted in the journal record (CRC valid)
    AND
    3. ACK has been delivered to PWA (mqtt.publish returned true)
       OR ACK is in the durable ACK queue (separate from RAM)

"ACK queued in RAM" is NOT sufficient for eviction.

ACK queue durability:
    The ACK queue must be persisted to NVS or LittleFS.
    If ACK queue is RAM-only, eviction is BLOCKED until ACK is delivered.

Contract:
    Transaction durability (journal record) and ACK delivery durability
    are SEPARATE concerns. Both must be satisfied for safe eviction.
```

**Enforcement**: Eviction function checks ACK delivery status. If ACK is RAM-only (not delivered, not durably queued), eviction returns false.

---

## 3. NVS Physical-Failure Model (Honest, Layered)

### Failure Levels

| Level | Description | Dual-Copy Protection? | Mitigation |
|-------|-------------|----------------------|------------|
| **Record corruption** | Single key's blob partially written (torn write) | ✅ YES — other copy intact | CRC detects, dual-copy recovers |
| **NVS page corruption** | Flash page (4KB) fails — multiple keys lost | ⚠️ PARTIAL — if both copies on same page, both lost | Cannot prevent; document as risk |
| **NVS metadata corruption** | NVS internal page table/index corrupt | ❌ NO — NVS itself is broken | NVS has its own CRC, but if metadata page fails, namespace may be inaccessible |
| **Partition corruption** | Entire NVS partition unusable | ❌ NO — all data lost | Factory reset, re-provision |
| **Flash chip failure** | Physical flash chip dies | ❌ NO — all data lost | Hardware redundancy (separate flash) — out of scope |

### Honest Contract (MUST be in README and code comments)

```
DUAL-COPY PROTECTION SCOPE:

The dual-copy architecture protects against:
    ✅ Torn writes to a single record (CRC detects, other copy recovers)
    ✅ Single-record NVS key corruption (CRC detects, other copy recovers)

The dual-copy architecture does NOT protect against:
    ❌ NVS page failure (both copies may be on same page)
    ❌ NVS partition corruption (entire namespace lost)
    ❌ Flash chip failure (all data lost)

For physical independence, the following would be needed:
    - Separate flash chips for copy A and copy B (hardware revision)
    - OR: Use LittleFS for copy B (different filesystem, different wear-leveling)

This design does NOT implement physical independence.
It is a LOGICAL redundancy, not PHYSICAL redundancy.

For 220V deployment, operator must understand:
    - If NVS page failure occurs, journal may lose both copies
    - System will detect this (CORRUPTED) but cannot recover automatically
    - Operator must re-provision device and verify physical relay state manually
```

### Why Not Use LittleFS for Copy B?

Considered but deferred:
- LittleFS has different wear-leveling semantics (good for independence)
- But: LittleFS write latency is higher (file open/write/close)
- And: LittleFS has its own corruption modes (power loss during file metadata update)
- Decision: Keep both copies in NVS for simplicity, document the limitation honestly
- Future: If NVS page failure becomes a real problem, migrate copy B to LittleFS

---

## 4. Formal Generation Algorithm

### Definitions

```cpp
// Serial-number arithmetic for generation comparison.
// Based on RFC 1982 (Serial Number Arithmetic) with 32-bit space.

static const uint32_t GENERATION_SPACE = 0x100000000ULL;  // 2^32

// Returns true if generation 'a' is NEWER than generation 'b'.
// Uses signed difference to handle wrap-around.
bool isNewer(uint32_t a, uint32_t b) {
    int32_t diff = (int32_t)(a - b);
    return diff > 0;
}

// Returns true if generation 'a' is OLDER than generation 'b'.
bool isOlder(uint32_t a, uint32_t b) {
    int32_t diff = (int32_t)(a - b);
    return diff < 0;
}

// Returns true if generations are equal.
bool sameGeneration(uint32_t a, uint32_t b) {
    return a == b;
}

// Returns true if comparison is AMBIGUOUS (generations differ by exactly 2^31).
// In this case, we cannot determine which is newer.
// Per RFC 1982, the "serial space" is 2^31, so differences ≥ 2^31 are ambiguous.
bool isAmbiguous(uint32_t a, uint32_t b) {
    if (a == b) return false;
    uint32_t diff = a - b;
    // Ambiguous if diff is exactly 2^31 (0x80000000)
    return (diff == 0x80000000);
}
```

### Generation Increment

```cpp
// Increment generation with wrap.
uint32_t nextGeneration(uint32_t current) {
    return current + 1;  // wraps naturally at uint32 max
}
```

### Recovery Selector

```cpp
// Given two valid copies with generations genA and genB:
// Returns 0 if A is newer, 1 if B is newer, -1 if ambiguous.
int selectActiveCopy(uint32_t genA, uint32_t genB) {
    if (sameGeneration(genA, genB)) {
        // Same generation — both should be identical. Pick A (arbitrary).
        return 0;
    }
    if (isAmbiguous(genA, genB)) {
        // Ambiguous — cannot determine. Caller should mark CORRUPTED.
        return -1;
    }
    if (isNewer(genA, genB)) {
        return 0;  // A is newer
    }
    return 1;  // B is newer
}
```

### Ambiguity Probability

For a 64-slot journal with 100 writes/day per slot:
- 6,400 writes/day total
- 2^31 = 2,147,483,648
- Time to ambiguity: 2,147,483,648 / 6,400 = 335,544 days ≈ 919 years

**Acceptable**: Ambiguity will never occur in practice.

---

## 5. Two-Valid-Copy Repair Protocol

### When Repair Is Needed

If any mutation is attempted and one copy is INVALID (CRC fail or missing):

```
State: A=VALID, B=INVALID (or vice versa)
Action: REPAIR before any mutation

Repair protocol:
    1. Read valid copy (the one with passing CRC)
    2. Determine its generation G and recordState S
    3. Serialize same record (generation G, state S) to inactive copy
    4. Verify inactive copy (re-read + CRC check)
    5. If verify FAILS → abort, slot remains in RECOVERY mode
    6. If verify PASSES → both copies now VALID with same generation G
    7. Slot is now ready for mutation (I1 satisfied)
```

### Recovery Mode

If repair fails (verify fails repeatedly):
- Slot enters RECOVERY mode
- Slot is marked CORRUPTED in RAM (but NOT freed)
- Operator must use `recoverCorruptedEntry()` to resolve
- No mutations allowed until resolved

### Boot-Time Repair

On boot, `_loadFromNVS()` for each slot:
```
1. Read copy A → result_A (VALID with genA, or INVALID)
2. Read copy B → result_B (VALID with genB, or INVALID)
3. Cases:
   a. Both VALID:
      - selectActiveCopy(genA, genB)
      - If ambiguous → CORRUPTED
      - Else: load newer copy, mark older for overwrite on next mutation
   b. A VALID, B INVALID:
      - Load A
      - Trigger repair (write A's record to B, verify)
      - If repair succeeds → ready for mutation
      - If repair fails → CORRUPTED
   c. A INVALID, B VALID:
      - Same as (b) but reversed
   d. Both INVALID:
      - CORRUPTED (operator recovery required)
```

### Mutation Pre-Condition Check

Every mutation function begins with:
```cpp
bool mutationPreConditionCheck(slotIdx) {
    bool aValid = verifyCopy(slotIdx, COPY_A);
    bool bValid = verifyCopy(slotIdx, COPY_B);
    if (!aValid || !bValid) {
        // I1 violated — attempt repair
        if (!repairSlot(slotIdx)) {
            // Repair failed — slot is CORRUPTED
            return false;
        }
        // Repair succeeded — re-check
        aValid = verifyCopy(slotIdx, COPY_A);
        bValid = verifyCopy(slotIdx, COPY_B);
        if (!aValid || !bValid) {
            return false;  // still broken
        }
    }
    return true;  // I1 satisfied
}
```

---

## 6. Recovery Audit Trail (Fixes C8CR2-005)

### Forensic Log Storage

Separate from journal slots. Uses LittleFS file: `/journal_audit.log`

### Forensic Record Format

```
[timestamp: 4 bytes]          — unix seconds (RTC)
[slotIdx: 1 byte]            — which slot was recovered
[requestIdLen: 1 byte]       — 0 if unreadable
[requestId: 0..64 bytes]     — best-effort (may be corrupt)
[generationA: 4 bytes]       — generation of copy A (0 if unreadable)
[generationB: 4 bytes]       — generation of copy B (0 if unreadable)
[stateA: 1 byte]             — state of copy A (0xFF if unreadable)
[stateB: 1 byte]             — state of copy B (0xFF if unreadable)
[reason: 1 byte]             — BOTH_CORRUPT / OPERATOR_INITIATED / REPAIR_FAILED
[action: 1 byte]             — EMPTIED / QUARANTINED
[recordCRC: 4 bytes]         — CRC over above fields
```

### recoverCorruptedEntry() Protocol (Revised)

```
1. Read both copies (best-effort)
2. Extract whatever metadata is readable (requestId, generation, state)
3. Write forensic record to /journal_audit.log (append, fsync)
4. Verify forensic record written (re-read)
5. If forensic write FAILED → ABORT (slot stays CORRUPTED, do NOT erase)
6. Write EMPTY to copy A (generation = max(genA, genB) + 1)
7. Verify copy A
8. Write EMPTY to copy B (generation = copy A generation)
9. Verify copy B
10. If any verify fails → slot stays CORRUPTED, but forensic record is safe
11. Only after both copies EMPTY+valid → slot is free
```

### Audit Log Lifecycle

- **Never** automatically deleted
- Grows unboundedly (acceptable — small records, ~80 bytes each)
- At 1 recovery/day for 10 years = 3,650 records × 80 bytes = 292KB
- If size becomes a concern: operator can archive (download + factory reset)
- GC is operator-initiated ONLY (never automatic)

---

## 7. ACK Durability Separation (Fixes C8CR2-006)

### Two Separate Durability Concerns

```
1. Transaction durability:
   The journal record (PENDING/EXECUTING/COMMITTED) is durable.
   This means: physical side effect evidence is preserved.

2. ACK delivery durability:
   The ACK has been received by PWA, OR is in a durable queue.
   This means: PWA will eventually know the transaction result.
```

### ACK Queue Persistence

Current state (v3): ACK queue is RAM-only (`_pendingAcks[]` array).
Problem: If device reboots, queued ACKs are lost.

**Rev3 design**: ACK queue is persisted to NVS.

```
NVS key: tj_ackq (blob, max 1024 bytes)
Contains: array of {requestId, ackJson} pairs, up to MAX_PENDING_ACKS (8)

On queueAck():
    1. Update RAM array
    2. Serialize RAM array to blob
    3. Write blob to tj_ackq (putBytes)
    4. Verify (re-read)

On processPendingAcks():
    1. For each ACK in queue:
       a. Publish via MQTT
       b. If publish succeeds → remove from queue → persist queue
       c. If publish fails → keep in queue, retry later

On boot:
    1. Read tj_ackq blob
    2. Deserialize into RAM array
    3. Queue is restored — ACKs will be re-delivered
```

### Eviction Pre-Condition (Revised)

```
Eviction of COMMITTED entry requires ALL:
    1. Entry state is COMMITTED or COMMITTED_UNKNOWN
    2. ackJson is durably persisted in journal record (CRC valid)
    3. ACK delivery status is one of:
       a. mqtt.publish() returned true (ACK sent to broker)
       b. ACK is in durable queue (tj_ackq, CRC valid)
    4. Retention policy permits eviction (LRU, slot needed)

If ACK is RAM-only (not delivered, not in durable queue):
    Eviction is BLOCKED.
    Wait for ACK delivery or durable queue persistence.
```

---

## 8. Wear/Capacity Analysis (Fixes C8CR2-007)

### Record Size

```
Header: 8 bytes (magic + version + state + generation)
CRC: 4 bytes
requestId: 1 + 64 = 65 bytes (max)
commandHash: 1 + 64 = 65 bytes (max)
channelId: 1 byte
desiredState: 1 byte
previousKnownState: 1 byte
attempt: 1 byte
timestamp: 4 bytes
ackLen: 2 bytes
ackJson: 0..1024 bytes

Total max: 8 + 4 + 65 + 65 + 1 + 1 + 1 + 1 + 4 + 2 + 1024 = 1176 bytes
Padded to: 1200 bytes (BLOB_SIZE)
```

### Storage Requirements

```
64 slots × 2 copies × 1200 bytes = 153,600 bytes = 150 KB (raw payload)

NVS overhead (Espressif documentation):
    - NVS uses 4KB pages
    - Each page has header + entry table + data
    - Effective capacity ≈ 75-80% of partition size
    - NVS is optimized for small key-value pairs, not large blobs

For 150 KB raw payload:
    Required partition size ≈ 150 / 0.75 = 200 KB minimum
    Recommended (with headroom): 256 KB

But: 64 slots × 2 copies × 1.2KB is a LOT of large blobs for NVS.
    NVS may have performance issues with many large blobs.
    Wear-leveling may be suboptimal.
```

### Write Amplification

```
Per transaction (relay ON/OFF):
    1. storeIntent() → 1 COW write (1200 bytes)
    2. markExecuting() → 1 COW write (1200 bytes)
    3. commitTransaction() → 1 COW write (1200 bytes, includes ackJson)
    Total: 3 writes × 1200 bytes = 3600 bytes per transaction

Per day (estimated):
    - Relay commands: 50/day (manual + scheduler)
    - Total writes: 50 × 3600 = 180,000 bytes/day = 176 KB/day

NVS flash endurance (ESP32 WROOM-32):
    - Flash: 4MB, typically Winbond W25Q32
    - Endurance: 100,000 erase cycles per sector (4KB)
    - NVS wear-leveling distributes writes across pages

With 256 KB NVS partition:
    - 64 pages (4KB each)
    - 180 KB/day write volume
    - Each page erased roughly every 64 pages × 4KB / 180KB = ~1.4 days
    - 100,000 cycles / (1/1.4) = 140,000 days ≈ 383 years

This is acceptable, BUT:
    - NVS internal wear-leveling is opaque
    - Large blobs (1.2KB) may cause uneven wear
    - Actual endurance depends on NVS implementation details
```

### Alternative: Reduce Journal Size

If wear/capacity is a concern:
```
Option A: 32 slots (instead of 64)
    Storage: 32 × 2 × 1.2KB = 76.8 KB raw
    Partition: 128 KB
    Retention window: halved

Option B: 16 slots
    Storage: 16 × 2 × 1.2KB = 38.4 KB raw
    Partition: 64 KB
    Retention window: quarter

Option C: Reduce ackJson max to 256 bytes
    Record size: ~400 bytes
    64 slots × 2 × 400 = 51.2 KB raw
    Partition: 96 KB
```

**Recommendation**: Start with 32 slots (Option A) for Rev3 implementation. Can expand later if retention window is too short.

### NVS vs LittleFS Decision

```
NVS:
    ✅ Simple API (Preferences library)
    ✅ Built-in wear-leveling
    ✅ Atomic key-value semantics (within NVS's own guarantees)
    ❌ Opaque internals (can't control page placement)
    ❌ Optimized for small values, not 1.2KB blobs
    ❌ Large blobs may cause performance issues

LittleFS:
    ✅ File-based, more flexible
    ✅ Better for larger records
    ✅ Can have separate files for copy A and copy B (different inodes)
    ❌ Higher write latency (file open/write/close)
    ❌ Different corruption modes (metadata vs data)

Decision for Rev3:
    Use NVS for simplicity (both copies in NVS).
    Document that physical independence is NOT guaranteed (§3).
    If NVS page failure becomes a real problem in testing,
    migrate copy B to LittleFS in a future cycle.
```

---

## 9. CRC vs HMAC Integrity Boundary (Fixes C8CR2-004)

### CRC32 — What It Protects Against

```
CRC32 protects against:
    ✅ Accidental corruption (bit flips, partial writes, torn writes)
    ✅ Storage media degradation (flash bit rot)
    ✅ Power-loss-induced write interruption

CRC32 does NOT protect against:
    ❌ Malicious modification (attacker can recompute CRC)
    ❌ Replay attacks (attacker can copy old valid record over new one)
    ❌ Physical access attacks (attacker can read/write flash directly)
```

### Threat Model

```
This design assumes:
    Threat = accidental failure (power loss, flash wear, NVS bugs)

This design does NOT assume:
    Threat = malicious attacker with physical access to flash

If physical attack is in scope:
    - Use HMAC-SHA256 instead of CRC (key stored in eFuse or NVS)
    - Enable Flash Encryption (ESP32 eFuse)
    - Enable Secure Boot
    - These are hardware provisioning steps, not software changes

For Rev3:
    CRC32 is sufficient for accidental failure protection.
    HMAC is deferred to hardware provisioning cycle.
```

### Explicit Documentation (MUST be in code comments and README)

```
// INTEGRITY BOUNDARY:
// This journal uses CRC32 for corruption detection.
// CRC32 protects against ACCIDENTAL corruption (power loss, flash wear).
// CRC32 does NOT protect against MALICIOUS modification.
// For tamper protection, enable Flash Encryption + Secure Boot (hardware provisioning).
```

---

## 10. Revised API (For Implementation Phase — NOT YET IMPLEMENTED)

### Public API (unchanged from Rev2, with invariant enforcement)

```cpp
// Boot
void begin();
void setBootPhase(BootPhase phase);
void captureOutputSnapshot();
uint8_t reconcilePendingEntries();

// Transaction lifecycle (all enforce I1: both copies valid before mutation)
bool storeIntent(requestId, commandHash, channelId, desiredState, previousKnownState);
bool markExecuting(requestId);
bool commitTransaction(requestId, ackJson);
bool commitTransactionFailed(requestId, ackJson, TransactionState failureState);

// Lookup
bool isProcessed(requestId);
bool isCommitted(requestId);
TransactionState getTransactionState(requestId);
String getCommandHash(requestId);
String getAckJson(requestId);
uint8_t getChannelId(requestId);
bool getDesiredState(requestId);

// Recovery (enforces I4: forensic record before erase)
TransactionState reconcileEntry(requestId);
bool clearEntry(requestId);
bool recoverCorruptedEntry(requestId);

// ACK queue (enforces I5: durable queue)
void queueAck(requestId, ackJson);
uint8_t processPendingAcks();
void dequeueAck(requestId);

// Audit trail (NEW — for forensic access)
String getAuditLogText(size_t maxBytes);
```

### Internal (Private) API (with dual-copy awareness)

```cpp
// Dual-copy operations
bool _readCopy(slotIdx, copySelector, record);           // read specific copy
bool _writeCopy(slotIdx, copySelector, record);           // COW write + verify
bool _verifyCopy(slotIdx, copySelector);                   // re-read + CRC check
int  _selectActiveCopy(slotIdx, record);                   // returns copySelector or -1

// Repair (I1 enforcement)
bool _repairSlot(slotIdx);                                // VALID+INVALID → VALID+VALID
bool _mutationPreConditionCheck(slotIdx);                 // I1 check + auto-repair

// Generation (I2 enforcement)
bool _isNewer(uint32_t a, uint32_t b);
bool _isAmbiguous(uint32_t a, uint32_t b);

// Forensic log (I4 enforcement)
bool _writeForensicRecord(slotIdx, reason, action);
bool _readForensicRecords(maxCount, callback);

// ACK queue durability (I5 enforcement)
bool _persistAckQueue();
bool _loadAckQueue();
```

---

## 11. Crash Matrix (Revised for Dual-Copy with Invariants)

### Mutation Protocol (all mutations follow same pattern)

```
Pre-state: A=VALID(gen=N), B=VALID(gen=N) [I1 satisfied]

1. mutationPreConditionCheck() — verifies both valid, repairs if needed
2. Identify active copy (e.g., A has higher generation, or both same → pick A)
3. Identify inactive copy (B)
4. Serialize new record with generation=N+1
5. Write to B (putBytes)
6. Verify B (re-read + CRC) — I3 enforcement
7. If verify FAILS → abort (A still valid, B still old or corrupt)
8. If verify PASSES → B is now active (gen=N+1), A is old (gen=N)

Post-state: A=VALID(gen=N, old), B=VALID(gen=N+1, new) → select B
```

### Crash Scenarios

| Step | Crash Point | A State | B State | Recovery |
|------|-------------|---------|---------|----------|
| 1 | Before pre-check | VALID(N) | VALID(N) | Normal load, pick either |
| 2 | During pre-check | VALID(N) | VALID(N) | Normal load |
| 3 | Before write to B | VALID(N) | VALID(N) | Normal load (mutation didn't start) |
| 4 | During write to B (torn) | VALID(N) | INVALID | Pick A (gen=N) → mutation didn't complete |
| 5 | After write to B, before verify | VALID(N) | VALID(N+1) or INVALID | Re-verify B; if valid pick B, if invalid pick A |
| 6 | During verify (re-read) | VALID(N) | VALID(N+1) | Pick B (gen=N+1) |
| 7 | Verify fails (B torn) | VALID(N) | INVALID | Pick A (gen=N) → mutation didn't complete |
| 8 | After verify passes | VALID(N, old) | VALID(N+1, new) | Pick B → mutation completed |

**All scenarios safe**: Either old state preserved (mutation didn't complete) or new state valid (mutation completed). No ambiguous intermediate.

### Repair Scenario

```
Pre-state: A=VALID(gen=N), B=INVALID (from previous torn write)

1. Read A → valid, gen=N
2. Serialize A's record to B with gen=N (same data)
3. Write to B (putBytes)
4. Verify B
5. If verify FAILS → ABORT, slot stays in RECOVERY (I1 not satisfied)
6. If verify PASSES → A=VALID(N), B=VALID(N) → I1 satisfied, ready for mutation
```

### Both-Corrupt Scenario

```
Pre-state: A=INVALID, B=INVALID

1. _loadFromNVS detects both invalid → CORRUPTED
2. No mutation possible
3. recoverCorruptedEntry() required (operator-initiated)
4. Forensic record written first (I4)
5. EMPTY written to both copies (with repair protocol between)
```

---

## 12. Honest Limitations (Unchanged + New)

1. **Snapshot reflects safe-OFF, not pre-crash state** — hardware limitation
2. **GPIO output ≠ physical relay contact** — welded/stuck undetectable
3. **Non-relay commands cannot be reconciled via GPIO** — marked UNKNOWN
4. **Hardware power-loss testing NOT RUN** — designed behavior only
5. **Dual-copy is LOGICAL redundancy, NOT physical independence** (NEW — C8CR2-001)
6. **CRC32 protects against accident, NOT against malicious modification** (NEW — C8CR2-004)
7. **NVS endurance estimates are theoretical** — actual wear depends on NVS internals (NEW — C8CR2-007)

---

## 13. What This Design Does NOT Solve

- Pre-crash GPIO state recovery (hardware revision needed)
- Physical relay contact verification (feedback hardware needed)
- Physical flash independence (separate flash chips needed)
- Tamper protection (Flash Encryption + Secure Boot needed)
- Multi-output transaction model (precharge) — separate cycle needed
- 16-relay / I/O expander — separate cycle needed
- F-008 PWA credential architecture — separate cycle needed

---

## 14. Implementation Plan (After Auditor Approval — NOT YET STARTED)

### Phase 1: Core Data Structure
1. Define `JournalRecord` struct (packed, 1200 bytes)
2. Implement `_serializeRecord()` / `_deserializeRecord()` with CRC
3. Implement generation algorithm (`_isNewer()`, `_isAmbiguous()`)

### Phase 2: Dual-Copy Operations
4. Implement `_readCopy()`, `_writeCopy()` (with verify), `_verifyCopy()`
5. Implement `_selectActiveCopy()` using generation algorithm
6. Implement `_repairSlot()` (VALID+INVALID → VALID+VALID)
7. Implement `_mutationPreConditionCheck()` (I1 enforcement)

### Phase 3: State Machine
8. Implement `storeIntent()` (with validation-before)
9. Implement `markExecuting()` (single COW write)
10. Implement `commitTransaction()` (single COW write)
11. Implement `commitTransactionFailed()`
12. Implement `reconcilePendingEntries()` (snapshot-based)
13. Implement `reconcileEntry()` (always UNKNOWN)
14. Implement `clearEntry()` (write EMPTY, COW)
15. Implement `recoverCorruptedEntry()` (with forensic record — I4)

### Phase 4: Boot Sequence
16. Implement `_loadFromNVS()` with dual-copy recovery + repair
17. Implement `captureOutputSnapshot()`
18. Update `firmware_v4.ino` boot sequence

### Phase 5: ACK Durability
19. Implement ACK queue persistence (NVS blob `tj_ackq`)
20. Implement eviction pre-condition check (I5 enforcement)

### Phase 6: Integration
21. Update `MqttClient.cpp` for new API
22. Update `RelayEngine.cpp` boot phase guard
23. Update partition table (256KB NVS or 128KB with 32 slots)

### Phase 7: Testing
24. Hardware power-loss injection (every crash point in §11)
25. Corruption injection (flip bits in NVS)
26. Generation wrap testing
27. Dual-copy repair testing
28. Forensic log verification

---

## 15. Auditor Approval Gate

**This design is NOT approved for implementation.**

Auditor must review:
1. Five formal invariants (§2) — are they complete and correct?
2. NVS physical-failure model (§3) — is the honest contract acceptable?
3. Generation algorithm (§4) — is serial-number arithmetic correct?
4. Two-valid-copy repair protocol (§5) — does it enforce I1?
5. Recovery audit trail (§6) — does it preserve forensic evidence (I4)?
6. ACK durability separation (§7) — does it enforce I5?
7. Wear/capacity analysis (§8) — is 32-slot recommendation acceptable?
8. CRC vs HMAC boundary (§9) — is the threat model documented?
9. Crash matrix (§11) — are all dual-copy scenarios covered?

**After auditor approval, implementation Phase 1-7 may begin. Until then: NO CODE.**

Precharge: BLOCKED
I/O expander: BLOCKED
16-relay: BLOCKED
