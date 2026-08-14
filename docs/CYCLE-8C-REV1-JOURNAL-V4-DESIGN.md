# CYCLE-8C-Rev1: Transaction Journal v4 — Design Specification

**Status**: DESIGN ONLY — NO CODE IMPLEMENTATION
**Purpose**: Menunggu audit auditor sebelum implementasi
**Auditor instruction**: "Saya ingin engineer terlebih dahulu membuat: 1) Formal invariant table, 2) Crash matrix T0-Tn, 3) Redesign single-source-of-truth journal record"

---

## 1. Root Cause Analysis

Setelah 5 cycle (7, 8A, 8B, 8B-Rev1, 8C), pattern bug berulang muncul:

| Cycle | Bug | Root Cause |
|-------|-----|------------|
| 7 | intent/execute ambiguity | Tidak ada durable intent |
| 8A | boot contamination | Reconciliation setelah RelayEngine |
| 8B | state reset saat commit | Fungsi commit reused untuk create |
| 8B-Rev1 | corruption → free slot | Blob corrupt dianggap slot bebas |
| 8C | commit=0 + COMMITTED valid | Invariant terlalu longgar |

**Akar masalah**: Metadata transaction tersebar di beberapa NVS keys yang harus konsisten secara manual:
- `tj_entry_N` (blob)
- `tj_commit_N` (commit flag)
- `tj_state_N` (state byte)
- `tj_tomb_<hash>` (tombstone)

Setiap key adalah sumber kebenaran yang berbeda. Crash di antara write membuat kombinasi inconsistent. Validator mencoba merekonsiliasi, tapi invariant table tidak cukup ketat.

**Solusi**: Single self-contained record dengan generation number + CRC. Satu NVS key per slot. Tidak ada separate commit flag atau state byte.

---

## 2. Journal Record v4 — Single Source of Truth

### Record Layout (single NVS key: `tj_rec_N`)

```
Offset  Field              Size  Description
------  ----------------   ----  ------------------------------------------
0       magic              2     0x54, 0x4A ("TJ")
2       schemaVersion      1     4 (journal v4)
3       recordState        1     enum (see §3)
4       generation         4     uint32 LE — incremented on every write to this slot
8       recordCRC          4     CRC32 over bytes 12..end (everything after CRC)
12      requestIdLen       1     1..64
13      requestId          var   requestIdLen bytes
..      commandHashLen    1     1..64
..      commandHash       var   commandHashLen bytes
..      channelId          1     0=N/A, 1..NUM_CHANNELS
..      desiredState       1     0=OFF, 1=ON, 0xFF=N/A
..      previousKnownState 1     0=OFF, 1=ON
..      attempt            1     retry counter
..      timestamp          4     uint32 LE, unix seconds
..      ackLen              2     uint16 LE, 0..1024
..      ackJson             var   ackLen bytes
..      (padding to fixed size)
```

**Key design decisions:**

1. **No separate commit flag** — `recordState` field is authoritative.
   - `COMMITTED` state IS the commit point.
   - No separate byte to flip.
   - Eliminates commit=0 + COMMITTED inconsistency (C8C-001).

2. **Generation number** — incremented on every write.
   - Detects torn writes: if generation is N but CRC fails, a write was interrupted.
   - On recovery: if CRC fails, we know which generation attempted (but not completed).
   - Generation wraps at uint32 max (acceptable for journal lifetime).

3. **CRC over entire record** (after CRC field).
   - Single integrity check — no separate blob/state/commit to reconcile.
   - If CRC fails: record is CORRUPTED (entire record, not just one field).

4. **Fixed-size slot** — no variable-length allocation issues.
   - BLOB_SIZE = 1200 bytes (same as v3).
   - Padding zeros after ackJson.

5. **recordState is the ONLY state source** — no separate state byte.
   - Eliminates state byte vs blob inconsistency.

### NVS Key Layout (v4)

```
tj_rec_0 .. tj_rec_63     — Journal records (64 slots)
tj_meta                   — Journal metadata (writeIdx, journalSize, generation counter)
tj_tomb_0 .. tj_tomb_63   — Tombstones (indexed by slot, NOT by requestId hash)
```

**Tombstone change**: Tombstone is now per-SLOT, not per-requestId hash.
- Eliminates FNV-1a 32-bit collision risk (C8C-005).
- Tombstone key: `tj_tomb_<slot_idx>` — contains requestId + timestamp + CRC.
- On load: if tombstone exists for slot N, record N is NOT resurrected.

---

## 3. Transaction State Machine (v4)

### States

```
EMPTY              — Slot is free (no record, or recordState=EMPTY)
PENDING           — Intent stored, execute NOT yet called
EXECUTING         — Execute called, commit NOT yet done
COMMITTED         — Execute + commit succeeded (terminal durable)
COMMITTED_UNKNOWN — Reconciled: cannot determine (terminal durable)
UNKNOWN           — Cannot determine (clearable, allows retry with caution)
FAILED            — Proven not executed (clearable, allows retry)
CORRUPTED         — Record integrity violated (terminal safety — operator recovery)
EXECUTION_FAILED_OUTPUT_MISMATCH — Execute ran, wrong output (terminal durable)
```

### Invariant Table (C8C-001 fix — STRICT)

| Record State | Valid? | Action on Load |
|--------------|--------|----------------|
| EMPTY | ✅ | Slot is free |
| PENDING | ✅ | Accept as PENDING |
| EXECUTING | ✅ | Accept as EXECUTING |
| COMMITTED | ✅ | Accept as COMMITTED (durable) |
| COMMITTED_UNKNOWN | ✅ | Accept as COMMITTED_UNKNOWN (durable) |
| UNKNOWN | ✅ | Accept as UNKNOWN |
| FAILED | ✅ | Accept as FAILED |
| CORRUPTED | ✅ | Accept as CORRUPTED (terminal safety) |
| EXECUTION_FAILED_OUTPUT_MISMATCH | ✅ | Accept as EXECUTION_FAILED_OUTPUT_MISMATCH (durable) |
| Any other value | ❌ | Mark CORRUPTED (invalid enum) |

**Critical difference from v3**: Tidak ada separate commit flag. `recordState` adalah satu-satunya sumber kebenaran. Tidak ada kombinasi "commit=0 + state=COMMITTED" yang mungkin — karena tidak ada commit flag.

### State Transitions (monotonic)

```
(none/EMPTY) → PENDING                    (storeIntent)
PENDING → EXECUTING                       (markExecuting)
EXECUTING → COMMITTED                     (commitTransaction)
EXECUTING → EXECUTION_FAILED_OUTPUT_MISMATCH  (commitTransactionFailed)
PENDING → UNKNOWN                         (reconciliation — cannot determine)
PENDING → FAILED                          (reconciliation — proven not executed)
EXECUTING → UNKNOWN                       (reconciliation — cannot determine)
Any non-terminal → CORRUPTED              (integrity violation detected)

CLEARABLE (via clearEntry):
  PENDING, EXECUTING, UNKNOWN, FAILED

NOT CLEARABLE (terminal):
  COMMITTED, COMMITTED_UNKNOWN, EXECUTION_FAILED_OUTPUT_MISMATCH, CORRUPTED

CORRUPTED → (cleared)                     (recoverCorruptedEntry — operator only)
```

### Forbidden Transitions (enforced by validator)

```
EXECUTING → PENDING                        (was C8B-001 bug)
COMMITTED → anything                       (terminal)
COMMITTED_UNKNOWN → anything               (terminal)
EXECUTION_FAILED_OUTPUT_MISMATCH → anything (terminal)
CORRUPTED → anything except (cleared)      (terminal safety)
UNKNOWN → PENDING/EXECUTING/COMMITTED      (semi-terminal)
FAILED → PENDING/EXECUTING/COMMITTED      (semi-terminal)
```

---

## 4. Crash Matrix T0-Tn (Formal)

### Write Operations

Journal v4 memiliki operasi write berikut:

1. **storeIntent()** — write new PENDING record
2. **markExecuting()** — update record: PENDING → EXECUTING (with attempt++)
3. **commitTransaction()** — update record: EXECUTING → COMMITTED (with ackJson)
4. **commitTransactionFailed()** — update record: EXECUTING → EXECUTION_FAILED_OUTPUT_MISMATCH
5. **reconcilePendingEntries()** — update record: PENDING/EXECUTING → UNKNOWN/FAILED
6. **clearEntry()** — write tombstone + erase record
7. **recoverCorruptedEntry()** — write tombstone + erase record (for CORRUPTED only)

### Single-Write Atomicity

Setiap operasi adalah **single putBytes()** ke `tj_rec_N`. Tidak ada multi-step commit.

**Record write sequence:**
```
1. Serialize record to buffer (in RAM)
2. Compute CRC over buffer
3. Write CRC + buffer to NVS via putBytes(tj_rec_N, ...)
4. Verify write success (return value check)
```

Jika crash terjadi:
- **Sebelum step 3**: record lama masih utuh (tidak berubah)
- **Saat step 3 (torn write)**: record corrupt → CRC fail on load → CORRUPTED
- **Setelah step 3**: record baru utuh → load normal

### T0-Tn Crash Matrix per Operation

#### storeIntent() — write PENDING record

| Time | Crash Point | NVS State | Load Behavior |
|------|-------------|-----------|---------------|
| T0 | Before write | Old record (or empty) | Old record loaded (or EMPTY) |
| T1 | During putBytes (torn) | Partial new record | CRC fail → CORRUPTED |
| T2 | After putBytes | New PENDING record | PENDING loaded → reconcile → FAILED (if desired=ON) |

**Safe**: T1 produces CORRUPTED (not free slot). T0 preserves old state.

#### markExecuting() — update record PENDING → EXECUTING

| Time | Crash Point | NVS State | Load Behavior |
|------|-------------|-----------|---------------|
| T0 | Before write | PENDING record | PENDING loaded → reconcile → FAILED or UNKNOWN |
| T1 | During putBytes (torn) | Partial record | CRC fail → CORRUPTED |
| T2 | After putBytes | EXECUTING record | EXECUTING loaded → reconcile → UNKNOWN |

**Safe**: T1 produces CORRUPTED. T0/T2 produce correct state.

**Note**: Karena single putBytes(), tidak ada window di mana state=PENDING tapi attempt sudah berubah. Record ditulis atomic — baik semua berubah atau tidak sama sekali.

#### commitTransaction() — update record EXECUTING → COMMITTED

| Time | Crash Point | NVS State | Load Behavior |
|------|-------------|-----------|---------------|
| T0 | Before write | EXECUTING record | EXECUTING loaded → reconcile → UNKNOWN |
| T1 | During putBytes (torn) | Partial record | CRC fail → CORRUPTED |
| T2 | After putBytes | COMMITTED record (with ackJson) | COMMITTED loaded → replay ACK |

**Safe**: T1 produces CORRUPTED. T0/T2 produce correct state.

**Critical difference from v3**: Tidak ada window di mana `state=COMMITTED` tapi `commit=0`. Karena record adalah single write, jika crash terjadi saat menulis COMMITTED record, record lama (EXECUTING) tetap utuh ATAU record baru (COMMITTED) sudah lengkap. Tidak ada kombinasi intermediate.

#### clearEntry() — write tombstone + erase record

| Time | Crash Point | NVS State | Load Behavior |
|------|-------------|-----------|---------------|
| T0 | Before tombstone | Record exists (PENDING/EXECUTING/etc.) | Record loaded normally |
| T1 | After tombstone write, before erase | Tombstone exists + record exists | Tombstone check → record NOT resurrected (erased during load) |
| T2 | After erase, before state clear | Tombstone exists + record erased | Tombstone check → slot EMPTY |
| T3 | After complete | Tombstone exists + record erased + slot EMPTY | Tombstone check → EMPTY |

**Safe**: T1+ protected by tombstone. T0 preserves old state.

**Tombstone lifecycle**:
- Written as `tj_tomb_<slot_idx>` (contains requestId + timestamp + CRC)
- Checked on load before resurrecting any record
- Removed when slot is reused (new storeIntent overwrites tombstone)
- Has max age (24 hours) — old tombstones are garbage-collected on boot

#### recoverCorruptedEntry() — operator recovery for CORRUPTED

| Time | Crash Point | NVS State | Load Behavior |
|------|-------------|-----------|---------------|
| T0 | Before tombstone | CORRUPTED record | CORRUPTED loaded (still blocked) |
| T1 | After tombstone, before erase | Tombstone + CORRUPTED record | Tombstone check → CORRUPTED NOT resurrected (erased) |
| T2 | After erase | Tombstone + empty | EMPTY |
| T3 | After complete | Tombstone + empty | EMPTY |

**Safe**: Recovery is durable. Tombstone protects against CORRUPTED resurrection.

### NVS Write Failure Handling

Jika `putBytes()` return value != expected:
- **Record write (storeIntent/markExecuting/commit)**: return false, RAM NOT updated, entry stays in previous state
- **Tombstone write**: return false, clearEntry/recoverCorruptedEntry returns false, RAM NOT updated
- **Erase**: log warning, but tombstone already protects → continue (return true with warning)

**Critical**: clearEntry() returns false if tombstone write fails (C8C-003 fix).
recoverCorruptedEntry() returns false if tombstone write fails (C8C-004 fix).

---

## 5. Corruption Handling (C8C-007 fix)

### Problem in v3
Ketika blob CRC fail, requestId tidak bisa di-extract (CRC melindungi seluruh payload). v3 menggunakan placeholder `"CORRUPTED_SLOT_N"` yang tidak memblokir requestId asli.

### v4 Solution: Separate requestId integrity

Record v4 memiliki **dua layer integrity**:

1. **requestId is stored OUTSIDE CRC-protected payload** (in fixed-position header)
   ```
   [magic(2)] [schemaVer(1)] [recordState(1)] [generation(4)]
   [requestIdLen(1)] [requestId(64)]  ← NOT covered by CRC
   [CRC(4)]  ← CRC covers everything after this
   [commandHash] [channelId] [desiredState] ... [ackJson]
   ```

2. **If CRC fails**: requestId masih bisa dibaca (tidak terproteksi CRC, tapi posisinya fixed).
   - Kita TIDAK mempercayai requestId secara buta (mungkin corrupt).
   - Tapi kita bisa menggunakannya untuk **warning** dan **best-effort duplicate detection**.
   - Record ditandai CORRUPTED (requestId preserved untuk audit).

3. **If CRC passes**: seluruh record valid, requestId terpercaya.

**Trade-off**: requestId di luar CRC ber bisa corrupt tanpa terdeteksi. Tapi:
- Jika requestId corrupt DAN CRC pass → sangat tidak mungkin (CRC melindungi field lain)
- Jika requestId corrupt DAN CRC fail → CORRUPTED, requestId "mungkin" corrupt (audit log mencatat)
- Jika requestId pass DAN CRC pass → record valid

**Alternative**: Double-store requestId (once outside CRC for recovery, once inside CRC for integrity). Lebih aman tapi lebih kompleks. Deferred to implementation decision.

### CORRUPTED Behavior

- `isProcessed(requestId)` returns true untuk CORRUPTED (requestId blocked)
- Jika requestId tidak bisa dibaca (magic fail), gunakan placeholder `"CORRUPTED_SLOT_N"`
  - Dalam kasus ini, requestId asli tidak diketahui → tidak bisa blokir
  - Log warning: "CORRUPTED entry with unreadable requestId — slot quarantined"
  - Slot tetap occupied (tidak free, tidak reuse)
- Operator harus gunakan `recoverCorruptedEntry()` untuk clear

---

## 6. Tombstone Design (C8C-005, C8C-006 fix)

### Problem in v3
- FNV-1a 32-bit hash → collision risk (C8C-005)
- No real TTL/GC (C8C-006)

### v4 Solution: Per-slot tombstone with collision-resistant binding

**Tombstone key**: `tj_tomb_<slot_idx>` (0-63)
- Tidak ada hash — slot index adalah identifier langsung.
- Zero collision risk.

**Tombstone value**:
```
[magic(2)] [version(1)] [reserved(1)]
[requestIdLen(1)] [requestId(64)]
[timestamp(4)]
[crc(4)]
```

**Tombstone check on load**:
1. Untuk setiap slot N (0-63):
2. Baca `tj_tomb_N`
3. Jika tombstone exists:
   - Verify tombstone CRC
   - Baca requestId dari tombstone
   - Bandingkan dengan requestId di record `tj_rec_N`
   - Jika match → record adalah yang di-clear → honor tombstone (erase record, mark slot EMPTY)
   - Jika mismatch → tombstone dari entry lain → ignore tombstone (record valid)
4. Jika tombstone tidak exists → record valid (normal load)

**Tombstone lifecycle (C8C-006 fix)**:
- **Written**: saat clearEntry() atau recoverCorruptedEntry()
- **Checked**: saat _loadFromNVS() untuk setiap slot
- **Removed**: saat slot di-reuse (storeIntent() untuk slot baru)
- **Garbage-collected**: saat boot, jika tombstone age > 24 hours → remove (biarkan slot free)
  - Mencegah unbounded tombstone growth
  - 24 hours cukup untuk menutupi reboot window terpanjang yang reasonable

---

## 7. Retry Policy (C8C-011 fix)

### Problem in v3
Komentar mengizinkan retry otomatis untuk UNKNOWN jika command idempotent. Ini terlalu longgar untuk precharge/multi-output.

### v4 Solution: Explicit retry policy

```
UNKNOWN → NEVER auto-retry by journal
          PWA receives "AMBIGUOUS" ACK
          Retry policy determined by command transaction policy:
            - Relay ON/OFF (single-output, idempotent): PWA may retry
            - Schedule/config: PWA may retry (idempotent)
            - Precharge (multi-output, non-idempotent): PWA must NOT retry
              without operator confirmation

FAILED → Auto-retry allowed (proven not executed)
         Journal clears entry, PWA retries with same requestId

CORRUPTED → NEVER auto-retry
            Operator must use recoverCorruptedEntry()
            Then PWA may retry with same requestId (after operator confirms)

EXECUTION_FAILED_OUTPUT_MISMATCH → NEVER auto-retry
                                   Hardware problem (welded/stuck relay)
                                   Operator must investigate physical relay
```

**Contract**: Journal tidak pernah auto-retry UNKNOWN. Retry decision adalah policy layer, bukan journal layer.

---

## 8. Journal Full / Sequence Wrap

### Journal Full
- 64 slots, LRU eviction
- Jika journal full dan storeIntent() dipanggil:
  - Evict oldest COMMITTED entry (COMMITTED entries are replayable, safe to evict)
  - Jika tidak ada COMMITTED entry → reject storeIntent dengan "JOURNAL_FULL"
  - PWA harus retry setelah beberapa waktu (entries akan di-evict oleh ACK delivery)

### Generation Wrap
- Generation adalah uint32, wraps at 4,294,967,295
- Tidak masalah karena generation hanya untuk torn-write detection, bukan ordering
- Jika generation wraps, tidak ada konflik (CRC melindungi integritas)

---

## 9. ACK Durability (C8C-010 fix)

### Problem in v3
commitTransaction() untuk already-COMMITTED hanya update RAM.

### v4 Solution: Re-write record

Jika commitTransaction() dipanggil untuk entry yang sudah COMMITTED:
- Re-write record dengan ackJson baru (single putBytes)
- Generation incremented
- CRC dihitung ulang
- RAM updated setelah NVS write success

Ini memastikan ACK update adalah durable.

---

## 10. Legacy API Removal (C8C-009 fix)

`storeTransaction()` legacy dihapus dari v4.
- Hanya ada satu mutation API: `storeIntent() → markExecuting() → commitTransaction()`
- Atau `storeIntent() → markExecuting() → commitTransactionFailed()`
- Tidak ada shortcut yang bisa bypass invariant.

---

## 11. Validation Before storeIntent (C8BR1-003 fix)

Semua validasi (relay, schedule, PIR, channel, config) dilakukan SEBELUM storeIntent().
- Invalid commands tidak pernah membuat journal entry.
- storeIntent() hanya dipanggil setelah semua validasi pass.

---

## 12. Implementation Plan (SETelah AUDIT APPROVAL)

### Phase 1: Core Data Structure
1. Define `JournalRecord` struct (packed, fixed-size)
2. Implement serialize/deserialize dengan CRC
3. Implement generation counter

### Phase 2: NVS Operations
4. Implement `writeRecord(slot, record)` — single putBytes
5. Implement `readRecord(slot)` — dengan CRC verification
6. Implement tombstone read/write/check/remove

### Phase 3: State Machine
7. Implement `storeIntent()` dengan validation-before
8. Implement `markExecuting()` dengan single-write update
9. Implement `commitTransaction()` dengan single-write commit
10. Implement `commitTransactionFailed()`
11. Implement `reconcilePendingEntries()` (uses snapshot)
12. Implement `reconcileEntry()` (always UNKNOWN)
13. Implement `clearEntry()` dengan tombstone
14. Implement `recoverCorruptedEntry()`

### Phase 4: Boot Sequence
15. Implement `_loadFromNVS()` dengan invariant validation + tombstone check
16. Implement `captureOutputSnapshot()`
17. Update `firmware_v4.ino` boot sequence

### Phase 5: Integration
18. Update `MqttClient.cpp` untuk gunakan new API
19. Update `RelayEngine.cpp` boot phase guard
20. Remove legacy `storeTransaction()`

### Phase 6: Testing
21. Hardware power-loss injection T0-Tn untuk setiap operation
22. Corruption injection (flip bits in NVS)
23. Tombstone lifecycle testing

---

## 13. Honest Limitations (unchanged)

1. **Snapshot reflects safe-OFF, not pre-crash state** — hardware limitation
2. **GPIO output ≠ physical relay contact** — welded/stuck undetectable
3. **Non-relay commands cannot be reconciled via GPIO** — marked UNKNOWN
4. **Hardware power-loss testing NOT RUN** — designed behavior only

---

## 14. What This Design Does NOT Solve

- Pre-crash GPIO state recovery (hardware revision needed)
- Physical relay contact verification (feedback hardware needed)
- Multi-output transaction model (precharge) — separate cycle needed
- 16-relay / I/O expander — separate cycle needed
- F-008 PWA credential architecture — separate cycle needed

---

## 15. Auditor Approval Gate

**This design is NOT approved for implementation.**

Auditor harus review:
1. Invariant table (§3) — apakah sudah ketat?
2. Crash matrix (§4) — apakah ada gap?
3. Single-record design (§2) — apakah ada inconsistency window?
4. Corruption handling (§5) — apakah requestId preservation cukup?
5. Tombstone design (§6) — apakah collision-free?
6. Retry policy (§7) — apakah contract jelas?
7. Journal full/wrap (§8) — apakah safe?
8. ACK durability (§9) — apakah re-write cukup?
9. Legacy removal (§10) — apakah satu API cukup?
10. Validation ordering (§11) — apakah complete?

Setelah auditor approval, baru implementasi Phase 1-6 dimulai.

**Precharge tetap BLOCKED.** I/O expander tetap BLOCKED. 16-relay migration tetap BLOCKED.
