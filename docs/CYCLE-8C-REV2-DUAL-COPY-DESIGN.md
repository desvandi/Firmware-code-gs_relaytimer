# CYCLE-8C-Rev2: Transaction Journal v4 — Dual-Copy Copy-on-Write Design

**Status**: DESIGN ONLY — NO CODE IMPLEMENTATION
**Purpose**: Redesign storage model to eliminate atomic-write assumption
**Auditor instruction**: "Redesign storage menjadi dual-copy / copy-on-write durable record"

---

## 1. Root Cause Analysis (Why Rev1 Failed)

### C8CR1-001 (P0): putBytes() assumed atomic

Rev1 design stated:
> "Setiap operasi adalah single putBytes() ke tj_rec_N."
> "Tidak ada intermediate state yang mungkin."

**This is FALSE.** NVS `putBytes()` is a software API call, not a hardware atomic transaction. Flash storage operations involve:

```
Application (putBytes)
    ↓
NVS API (key-value lookup, page allocation)
    ↓
NVS implementation (journal entry, page write)
    ↓
Flash sector/page operation (erase, program)
    ↓
Flash hardware (voltage change, bit flip)
    ↓
Power failure possible at ANY point
```

A single `putBytes()` of a 1.2KB record can result in:
- **OLD valid** (write didn't start)
- **NEW valid** (write completed)
- **TORN / INVALID** (write interrupted — partial new data, old data destroyed)
- **Metadata changed, payload incomplete** (NVS header updated but data not written)
- **Payload changed, metadata incomplete** (data written but header not updated)

CRC detects corruption but does NOT make writes atomic. If the old record is partially overwritten, neither old nor new is recoverable.

### C8CR1-002 (P0): CRC layout contradiction

Rev1 §2 showed:
```
[0..7] header (magic, version, state, generation)
[8..11] CRC (covers bytes 12..end)
[12..] requestId + payload
```

But Rev1 §5 showed a DIFFERENT layout:
```
[magic] [version] [state] [generation]
[requestId]          ← NOT covered by CRC
[CRC]                ← covers everything after
[payload]
```

Two contradictory formats in one document. Engineer implementing Phase 1 would have to guess which is correct.

### C8CR1-003 (P0): Tombstone TTL causes resurrection

Rev1 design: tombstone garbage-collected after 24 hours.
Scenario:
```
T0:    clearEntry() → tombstone written, record erase FAILS
T1:    reboot → tombstone protects (record not resurrected)
T+25h: reboot → tombstone GC'd (age > 24h)
       → record still in NVS (erase failed earlier)
       → record RESURRECTS
```

TTL-based GC is not safe for resurrection prevention.

### C8CR1-004 (P1): Tombstone mismatch ignored

Rev1 said: if tombstone requestId ≠ record requestId → ignore tombstone.
This is dangerous — a mismatch means two durable records disagree about slot lifecycle. Should be anomaly/quarantine, not ignore.

### C8CR1-005 (P1): Eviction semantics incomplete

Rev1 said "evict oldest COMMITTED entry" but didn't define exactly-once contract. After eviction, requestId is unknown → command can be re-executed. For non-idempotent commands (OTA, future precharge), this is unsafe.

### C8CR1-006 (P1): Generation alone doesn't detect torn-write

Rev1 said generation detects torn writes. But if the write is torn, generation itself may be corrupted. Generation is only useful AFTER CRC passes — it doesn't help recover from torn writes.

**Root pattern across all cycles**: Every cycle tried to fix consistency by adding more metadata (commit flag → state byte → tombstone → generation). But the fundamental problem is that **single-copy storage cannot be made crash-safe at the software level**. The solution is **dual-copy copy-on-write**.

---

## 2. Dual-Copy Copy-on-Write Architecture

### Principle

> "Power loss may corrupt the currently written copy; recovery selects the newest VALID durable copy."

This replaces the false assumption:
> ~~"putBytes() is atomic."~~

### Physical Layout

Each logical journal slot N (0-63) has TWO physical copies:

```
tj_ra_N  — Record copy A (slot N)
tj_rb_N  — Record copy B (slot N)
```

Both copies use identical record format. Only one is "active" at any time — determined by which has the highest valid generation number.

### Write Protocol (Copy-on-Write)

```
1. Read both copies (A and B) for slot N
2. Determine active copy = one with highest VALID generation (CRC passes)
3. Determine inactive copy = the other one (or either if one is corrupt)
4. Serialize new record with generation = active_generation + 1
5. Write new record to INACTIVE copy (putBytes)
6. Re-read inactive copy to verify CRC (detect torn write)
7. If verify FAILS → abort, old active copy still valid
8. If verify PASSES → inactive copy becomes new active (higher generation)
```

**Key insight**: We NEVER overwrite the active copy. We always write to the inactive copy first. If the write is torn, the old active copy is still intact and will be selected on recovery.

### Recovery Protocol (On Boot)

For each slot N (0-63):
```
1. Read copy A (tj_ra_N) → verify CRC → result_A (VALID with genA, or INVALID)
2. Read copy B (tj_rb_N) → verify CRC → result_B (VALID with genB, or INVALID)
3. Select active copy:
   - If A valid AND B valid: pick higher generation (A if genA >= genB, else B)
   - If A valid AND B invalid: pick A
   - If A invalid AND B valid: pick B
   - If both invalid: slot is CORRUPTED (quarantine)
4. Load active copy into RAM
```

**This is the standard shadow-paging / copy-on-write approach used by ZFS, btrfs, and journaled filesystems.**

---

## 3. Power-Loss Model for NVS (Honest, No Atomic Assumption)

### What NVS Guarantees (ESP-IDF nvs_flash)

- Key-value store with CRC32 per entry
- Internal journaling within flash pages (4KB each)
- Page-level erase before write (flash erase is ~30ms, vulnerable to power loss)
- Write granularity: 4 bytes minimum (flash program is 4-byte aligned)

### What NVS Does NOT Guarantee

- Atomicity of large `putBytes()` (1.2KB record may span multiple flash operations)
- Atomicity across multiple keys (writing `tj_ra_N` and `tj_rb_N` is NOT atomic)
- Protection against power loss during flash erase cycle

### Possible Outcomes of `putBytes(key, 1.2KB blob)`

| Outcome | Description | Detectable? |
|---------|-------------|--------------|
| OLD valid | Write didn't start, old data intact | Yes (CRC valid, old generation) |
| NEW valid | Write completed, new data intact | Yes (CRC valid, new generation) |
| TORN — partial new | Write interrupted, some bytes new, some old | Yes (CRC fail) |
| TORN — metadata valid, data corrupt | NVS entry header written but blob partially written | Yes (CRC fail) |
| TORN — data valid, metadata corrupt | Blob written but NVS entry header not updated | Yes (getBytesLength returns 0 or wrong size) |
| ERASE corrupt | Flash erase interrupted, page in unknown state | Yes (CRC fail or magic mismatch) |

### Dual-Copy Recovery Under Power Loss

| Copy A State | Copy B State | Recovery Action |
|--------------|--------------|------------------|
| OLD valid (gen N) | OLD valid (gen N) | Pick either (both same) |
| OLD valid (gen N) | NEW valid (gen N+1) | Pick B (higher generation) |
| NEW valid (gen N+1) | OLD valid (gen N) | Pick A (higher generation) |
| NEW valid (gen N+1) | NEW valid (gen N+1) | Pick either (both same, shouldn't happen) |
| VALID (any gen) | INVALID (torn) | Pick valid copy |
| INVALID (torn) | VALID (any gen) | Pick valid copy |
| INVALID (torn) | INVALID (torn) | CORRUPTED — quarantine slot |

**Critical**: The ONLY way to lose data is if BOTH copies are torn simultaneously. Since writes are sequential (write A first, verify, then write B), the probability of both being torn is extremely low — it requires power loss during write to A AND during write to B in the same operation.

---

## 4. Record Layout (Single Format — Fixes C8CR1-002)

### Record Format (requestId INSIDE CRC)

```
Offset  Field              Size  Description
------  ----------------   ----  ------------------------------------------
0       magic              2     0x54, 0x4A ("TJ")
2       schemaVersion      1     4 (journal v4)
3       recordState        1     enum (EMPTY, PENDING, EXECUTING, etc.)
4       generation         4     uint32 LE — incremented on every write to this slot
8       recordCRC          4     CRC32 over bytes 0..7 AND bytes 12..end
                                (covers header + payload, NOT the CRC field itself)
12      requestIdLen       1     0..64 (0 = EMPTY slot)
13      requestId          var   requestIdLen bytes
..      commandHashLen    1     0..64
..      commandHash       var   commandHashLen bytes
..      channelId          1     0=N/A, 1..NUM_CHANNELS
..      desiredState       1     0=OFF, 1=ON, 0xFF=N/A
..      previousKnownState 1     0=OFF, 1=ON
..      attempt            1     retry counter
..      timestamp          4     uint32 LE, unix seconds
..      ackLen              2     uint16 LE, 0..1024
..      ackJson             var   ackLen bytes
..      (padding to BLOB_SIZE)
```

### CRC Coverage

CRC covers:
- Bytes 0..7 (magic, schemaVersion, recordState, generation) — header
- Bytes 12..end (requestId + all payload fields)

CRC does NOT cover:
- Bytes 8..11 (the CRC field itself)

**This is the ONLY format.** There is no second layout. requestId is INSIDE CRC protection (fixes C8CR1-002).

### If CRC Fails

- requestId is **UNTRUSTED** (may be corrupt)
- recordState is **UNTRUSTED**
- The entire record is CORRUPTED
- **Do NOT use untrusted requestId for duplicate matching** (fixes C8CR1-002 recommendation)
- Slot is QUARANTINED (marked CORRUPTED, not freed)
- If BOTH copies fail CRC → slot is CORRUPTED, operator recovery required

### EMPTY Record

When a slot is cleared (clearEntry), we write a record with:
- `recordState = EMPTY`
- `generation = previous_generation + 1`
- `requestIdLen = 0` (no requestId)
- CRC covers the header + empty payload

This is a VALID record (CRC passes) that says "this slot is free." It replaces the tombstone entirely.

---

## 5. Tombstone Removal (Fixes C8CR1-003, C8CR1-004)

### Why Tombstones Are Eliminated

Rev1 used separate tombstone keys (`tj_tomb_N`). This introduced:
- A second source of durable state (record + tombstone) — violates single-source-of-truth
- TTL-based GC → resurrection risk (C8CR1-003)
- Mismatch handling ambiguity (C8CR1-004)

### v4-Rev2 Solution: recordState=EMPTY

There is NO tombstone. The record itself carries the "cleared" state.

**clearEntry() protocol:**
```
1. Read active copy (A or B) for slot N → get current generation G
2. Serialize new record: recordState=EMPTY, generation=G+1, requestIdLen=0
3. Write to INACTIVE copy (copy-on-write)
4. Verify CRC by re-reading
5. If verify passes: EMPTY record is now active (higher generation)
6. If verify fails: abort, old record still active
```

**On reboot:**
```
1. Read both copies for slot N
2. Select highest-generation VALID copy
3. If active copy has recordState=EMPTY → slot is free (not resurrected)
4. If active copy has recordState=PENDING/EXECUTING/etc → slot is occupied
```

**Why this is safe:**
- The EMPTY record has a HIGHER generation than the old record
- Recovery selector picks highest generation → EMPTY wins
- Old record (in the other copy) is stale, not selected
- No TTL, no GC, no resurrection window
- No second source of truth — the record IS the truth

**If both copies are corrupt:**
- Slot is CORRUPTED (quarantined)
- Operator must use `recoverCorruptedEntry()` which writes EMPTY to both copies

---

## 6. State Machine (Unchanged from Rev1)

### States

```
EMPTY              — Slot is free (recordState=EMPTY, valid CRC)
PENDING           — Intent stored, execute NOT yet called
EXECUTING         — Execute called, commit NOT yet done
COMMITTED         — Execute + commit succeeded (terminal durable)
COMMITTED_UNKNOWN — Reconciled: cannot determine (terminal durable)
UNKNOWN           — Cannot determine (clearable, allows retry with caution)
FAILED            — Proven not executed (clearable, allows retry)
CORRUPTED         — Both copies invalid (terminal safety — operator recovery)
EXECUTION_FAILED_OUTPUT_MISMATCH — Execute ran, wrong output (terminal durable)
```

### Invariant Table (recordState is sole authority)

| Record State | CRC Valid? | Action |
|--------------|------------|--------|
| EMPTY | ✅ | Slot is free |
| PENDING | ✅ | Accept as PENDING |
| EXECUTING | ✅ | Accept as EXECUTING |
| COMMITTED | ✅ | Accept as COMMITTED |
| COMMITTED_UNKNOWN | ✅ | Accept as COMMITTED_UNKNOWN |
| UNKNOWN | ✅ | Accept as UNKNOWN |
| FAILED | ✅ | Accept as FAILED |
| CORRUPTED | ✅ | Accept as CORRUPTED (both copies were corrupt) |
| EXECUTION_FAILED_OUTPUT_MISMATCH | ✅ | Accept as EXECUTION_FAILED_OUTPUT_MISMATCH |
| Any value | ❌ | This copy is INVALID — try other copy, or CORRUPTED if both fail |

**No separate commit flag. No separate state byte. recordState in the record is the ONLY authority.**

### State Transitions (monotonic — unchanged from Rev1)

```
(none/EMPTY) → PENDING                    (storeIntent)
PENDING → EXECUTING                       (markExecuting)
EXECUTING → COMMITTED                     (commitTransaction)
EXECUTING → EXECUTION_FAILED_OUTPUT_MISMATCH  (commitTransactionFailed)
PENDING → UNKNOWN                         (reconciliation)
PENDING → FAILED                          (reconciliation — proven not executed)
EXECUTING → UNKNOWN                       (reconciliation)
Any non-terminal → CORRUPTED              (both copies invalid)
Any non-terminal → EMPTY                   (clearEntry / recoverCorruptedEntry)
```

### Retry Policy (C8C-011/C8CR1 contract — unchanged)

```
UNKNOWN → NEVER auto-retry by journal
          PWA receives "AMBIGUOUS" ACK
          Retry policy determined by command transaction policy

FAILED → Auto-retry allowed (proven not executed)

CORRUPTED → NEVER auto-retry
            Operator must use recoverCorruptedEntry()

EXECUTION_FAILED_OUTPUT_MISMATCH → NEVER auto-retry
                                   Hardware problem — operator investigation
```

---

## 7. Crash Matrix (Dual-Copy Scenarios)

### Notation

- **A** = copy A (tj_ra_N)
- **B** = copy B (tj_rb_N)
- **genA** = generation of copy A
- **genB** = generation of copy B
- **VALID** = CRC passes
- **INVALID** = CRC fails or blob missing

### storeIntent() — write PENDING to inactive copy

Pre-state: A=VALID(gen=N, state=EMPTY), B=VALID(gen=N-1, state=EMPTY) or B=INVALID

| Crash Point | A State | B State | Recovery |
|-------------|---------|---------|----------|
| Before write to B | VALID(gen=N, EMPTY) | VALID(gen=N-1, EMPTY) or INVALID | Pick A (gen=N, EMPTY) → slot free |
| During write to B (torn) | VALID(gen=N, EMPTY) | INVALID | Pick A (gen=N, EMPTY) → slot free |
| After write to B, before verify | VALID(gen=N, EMPTY) | VALID(gen=N+1, PENDING) | Pick B (gen=N+1, PENDING) → PENDING |
| After verify | VALID(gen=N, EMPTY) | VALID(gen=N+1, PENDING) | Pick B (gen=N+1, PENDING) → PENDING |

**Safe**: Torn write to B → A still valid (EMPTY). No data loss.

### markExecuting() — update to EXECUTING (copy-on-write)

Pre-state: A=VALID(gen=N+1, PENDING), B=INVALID (or older)

| Crash Point | A State | B State | Recovery |
|-------------|---------|---------|----------|
| Before write to A | VALID(gen=N+1, PENDING) | INVALID | Pick A → PENDING → reconcile |
| During write to A (torn) | INVALID | INVALID (old) | Both INVALID → CORRUPTED |
| After write to A | VALID(gen=N+2, EXECUTING) | INVALID | Pick A → EXECUTING → reconcile |

**Risk**: If B was already INVALID (torn from previous operation), and A tears during markExecuting → both INVALID → CORRUPTED. This is acceptable (better than wrong state). Probability is low (requires two consecutive torn writes to same slot).

**Mitigation**: Before writing to A, verify B is valid. If B is invalid, write to B first (recover B), then proceed with COW to A.

### commitTransaction() — update to COMMITTED (copy-on-write)

Pre-state: A=VALID(gen=N+2, EXECUTING), B=INVALID

| Crash Point | A State | B State | Recovery |
|-------------|---------|---------|----------|
| Before write to B | VALID(gen=N+2, EXECUTING) | INVALID | Pick A → EXECUTING → reconcile UNKNOWN |
| During write to B (torn) | VALID(gen=N+2, EXECUTING) | INVALID | Pick A → EXECUTING → reconcile UNKNOWN |
| After write to B | VALID(gen=N+2, EXECUTING) | VALID(gen=N+3, COMMITTED) | Pick B → COMMITTED → replay ACK |

**Safe**: Torn write to B → A still valid (EXECUTING). Reconciliation produces UNKNOWN (correct — cannot determine if execute ran). No false COMMITTED.

### clearEntry() — write EMPTY (copy-on-write)

Pre-state: A=VALID(gen=N+3, FAILED), B=INVALID

| Crash Point | A State | B State | Recovery |
|-------------|---------|---------|----------|
| Before write to B | VALID(gen=N+3, FAILED) | INVALID | Pick A → FAILED → clearEntry retry |
| During write to B (torn) | VALID(gen=N+3, FAILED) | INVALID | Pick A → FAILED → clearEntry retry |
| After write to B | VALID(gen=N+3, FAILED) | VALID(gen=N+4, EMPTY) | Pick B → EMPTY → slot free |

**Safe**: Torn write → old state (FAILED) preserved. clearEntry can retry. No resurrection.

### recoverCorruptedEntry() — write EMPTY to BOTH copies

Pre-state: A=INVALID, B=INVALID (CORRUPTED)

| Crash Point | A State | B State | Recovery |
|-------------|---------|---------|----------|
| Before write to A | INVALID | INVALID | Both INVALID → CORRUPTED (retry) |
| During write to A (torn) | INVALID | INVALID | Both INVALID → CORRUPTED (retry) |
| After write to A, before B | VALID(gen=N+1, EMPTY) | INVALID | Pick A → EMPTY → slot free |
| During write to B (torn) | VALID(gen=N+1, EMPTY) | INVALID | Pick A → EMPTY → slot free |
| After write to B | VALID(gen=N+1, EMPTY) | VALID(gen=N+2, EMPTY) | Pick B → EMPTY → slot free |

**Safe**: Even if B write fails, A has EMPTY → slot is free. No resurrection possible (EMPTY has higher generation than corrupt data).

### Both Copies Corrupt (Worst Case)

| Scenario | A State | B State | Recovery |
|----------|---------|---------|----------|
| Both torn simultaneously | INVALID | INVALID | CORRUPTED — quarantine, operator recovery |
| Flash page failure | INVALID | INVALID | CORRUPTED — quarantine, operator recovery |

**This is the fundamental limit**: dual-copy reduces probability of total loss but cannot eliminate it. If both copies are on the same flash chip and the chip fails, both are lost. Hardware redundancy (separate flash) would be needed for true elimination — out of scope.

---

## 8. Eviction Contract (Fixes C8CR1-005)

### Exactly-Once Guarantee

```
Exactly-once guarantee:
    Applies ONLY within durable requestId retention window.

Retention window:
    Defined by journal capacity (64 slots) and eviction policy.

After eviction:
    requestId is NO LONGER tracked by journal.
    Command becomes UNKNOWN from journal perspective.
    PWA retry after eviction may result in re-execution.

Non-idempotent commands:
    MUST NOT rely on journal retention alone.
    MUST use application-level idempotency (e.g., version numbers, state checks).
```

### Eviction Policy

```
When journal is full and storeIntent() is called:
1. Find oldest entry (lowest generation) with state=COMMITTED or COMMITTED_UNKNOWN
2. If found: evict (write EMPTY to both copies, generation++)
3. If NOT found (all entries are PENDING/EXECUTING/etc.):
   - REJECT storeIntent with "JOURNAL_FULL"
   - PWA must wait for in-flight transactions to complete
```

**Why only COMMITTED entries can be evicted:**
- COMMITTED entries have already been ACKed (or ACK is queued)
- PWA has received (or will receive) the ACK
- Evicting a COMMITTED entry only affects future retries with same requestId
- Evicting PENDING/EXECUTING would lose in-flight transaction state → unsafe

### Eviction Safety Contract

```
Eviction is safe IF AND ONLY IF:
1. Entry state is COMMITTED or COMMITTED_UNKNOWN
2. ACK has been delivered to PWA (or is in ACK queue)
3. requestId is no longer needed for deduplication

Eviction is UNSAFE for:
- PENDING (execute may not have run)
- EXECUTING (execute may have run, commit not done)
- UNKNOWN (cannot determine)
- FAILED (should be cleared, not evicted)
- CORRUPTED (should be recovered, not evicted)
- EXECUTION_FAILED_OUTPUT_MISMATCH (operator must investigate)
```

---

## 9. Generation Semantics (Fixes C8CR1-006)

### What Generation Does

- **Versioning**: Each write increments generation. Higher generation = newer write.
- **Recovery selector**: On boot, pick highest-generation VALID copy.
- **Ordering**: Determines which copy is "active" when both are valid.

### What Generation Does NOT Do

- **Does NOT detect torn writes** (CRC does that)
- **Does NOT provide atomicity** (dual-copy COW does that)
- **Does NOT prevent resurrection** (EMPTY state + generation ordering does that)

### Generation Wrap

- uint32, wraps at 4,294,967,295
- On wrap: generation goes from 0xFFFFFFFF to 0x00000000
- Recovery selector: if genA=0 and genB=0xFFFFFFFF, pick B (higher)
- After wrap, genA=1 would be picked over genB=0 (but genB was 0xFFFFFFFF → wrapped to 0 → genA=1 > 0)
- **Edge case**: if generation wraps AND both copies have same generation after wrap, pick either (they should be identical)
- Acceptable: wrap requires 4 billion writes to same slot — at 100 writes/day, that's 117,000 years

---

## 10. NVS Partition Size Constraint

### Current NVS Partition

ESP32 default partition table (`default.csv`) allocates ~16KB for NVS.

### Dual-Copy Storage Requirements

```
64 slots × 2 copies × 1.2KB = 153.6KB
```

This EXCEEDS the default 16KB NVS partition.

### Options

| Option | Pros | Cons |
|--------|------|------|
| Reduce journal to 8 slots | Fits in 16KB NVS (8×2×1.2KB=19.2KB — still tight) | Shorter retention window |
| Reduce journal to 4 slots | Fits easily (4×2×1.2KB=9.6KB) | Very short retention |
| Increase NVS partition to 256KB | Full 64 slots | Requires custom partition table |
| Use LittleFS for journal | Separate from NVS, more space | LittleFS has different wear-leveling semantics |
| Reduce record size (smaller ackJson) | More slots in same space | Limited ACK data |

### Recommendation

Increase NVS partition to 256KB via custom partition table (`partitions.csv`).
This allows 64 dual-copy slots with room for other NVS data (WiFi creds, config, etc.).

**This is a build-time configuration change, not a code change.** Documented for implementation phase.

---

## 11. NVS Key Naming

NVS key max length = 15 chars.

```
tj_ra_63   = 8 chars ✅ (record A, slot 63)
tj_rb_63   = 8 chars ✅ (record B, slot 63)
tj_meta    = 7 chars ✅ (journal metadata: writeIdx, journalSize)
```

No tombstone keys needed (tombstone eliminated).

---

## 12. API Summary (For Implementation Phase — NOT YET IMPLEMENTED)

### Public API

```cpp
// Boot
void begin();
void setBootPhase(BootPhase phase);
void captureOutputSnapshot();
uint8_t reconcilePendingEntries();

// Transaction lifecycle
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

// Recovery
TransactionState reconcileEntry(requestId);
bool clearEntry(requestId);
bool recoverCorruptedEntry(requestId);

// ACK queue
void queueAck(requestId, ackJson);
uint8_t processPendingAcks();
void dequeueAck(requestId);
```

### Internal (Private) API

```cpp
// Dual-copy operations
bool _writeRecord(slotIdx, record, copySelector);  // COW write to inactive copy
bool _readRecord(slotIdx, record, copySelector);   // read specific copy
bool _verifyRecord(slotIdx, copySelector);          // re-read + CRC check
uint8_t _selectActiveCopy(slotIdx);                 // returns 0=A, 1=B, based on gen+CRC
bool _eraseBothCopies(slotIdx);                     // write EMPTY to both

// Serialization
size_t _serializeRecord(record, buffer, bufSize);
bool _deserializeRecord(buffer, len, record);
uint32_t _computeCRC(data, len);
```

### Removed (vs Rev1)

- `storeTransaction()` — legacy API removed (C8C-009 fix)
- `_writeTombstoneNVS()` / `_hasTombstoneNVS()` / `_removeTombstoneNVS()` — tombstone eliminated
- `_tombstoneKey()` — tombstone eliminated

---

## 13. Honest Limitations (Unchanged)

1. **Snapshot reflects safe-OFF, not pre-crash state** — hardware limitation
2. **GPIO output ≠ physical relay contact** — welded/stuck undetectable
3. **Non-relay commands cannot be reconciled via GPIO** — marked UNKNOWN
4. **Hardware power-loss testing NOT RUN** — designed behavior only
5. **Dual-copy does NOT eliminate total flash failure** — if flash chip dies, both copies lost
6. **NVS partition size must be increased** — current 16KB insufficient for 64 dual-copy slots

---

## 14. What This Design Does NOT Solve

- Pre-crash GPIO state recovery (hardware revision needed)
- Physical relay contact verification (feedback hardware needed)
- Multi-output transaction model (precharge) — separate cycle needed
- 16-relay / I/O expander — separate cycle needed
- F-008 PWA credential architecture — separate cycle needed
- Total flash chip failure (hardware redundancy needed)

---

## 15. Auditor Approval Gate

**This design is NOT approved for implementation.**

Auditor must review:
1. Dual-copy COW architecture (§2) — is the write protocol correct?
2. Power-loss model (§3) — is it honest and complete?
3. Record layout (§4) — is requestId INSIDE CRC (contradiction fixed)?
4. Tombstone removal (§5) — does recordState=EMPTY + generation prevent resurrection?
5. Crash matrix (§7) — are all dual-copy scenarios covered?
6. Eviction contract (§8) — is exactly-once window defined?
7. Generation semantics (§9) — is it recovery selector only (not torn-write detector)?
8. NVS size constraint (§10) — is the recommendation acceptable?

**After auditor approval, implementation Phase 1-6 may begin. Until then: NO CODE.**

Precharge: BLOCKED
I/O expander: BLOCKED
16-relay: BLOCKED
