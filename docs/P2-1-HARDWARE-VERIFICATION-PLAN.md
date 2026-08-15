# P2-1 Hardware Verification Plan

**Scope:** Power-loss and NVS integrity verification for the TransactionJournal
P2-1 correction package (R4 + R5 combined pass) on real ESP32 hardware.

**Reference documents:**
- `docs/CYCLE-8C-REV26-FINAL-PREDICATE.md` — normative contract
- `docs/P2-1-IMPLEMENTATION-MAPPING.md` — item-to-test mapping
- `firmware/TransactionJournal.cpp` — implementation under test
- `firmware/test/host/TransactionJournalTest.cpp` — host-side proof (130 tests)

**Status:** Draft R5-C7 — for hardware verification engineer.

---

## 1. Objective

Verify on real ESP32 hardware that every mutation entry point in
`TransactionJournal` survives a power-loss at every intra-mutation point, and
that the post-reboot state observed by `begin()` matches the candidate-pattern
guarantees (R4-C1: RAM mirrors durable state, no resurrection; R4-C2: ACK
queue state is consistent with NVS).

The host-side test harness proves the logic, but cannot exercise:

- Real NVS write latency and atomicity (NVS writes can be torn mid-byte).
- Power-loss during flash erase (which can corrupt a flash page).
- Brown-out detector (BOD) reset behavior.
- Watchdog reset during mutation.

These are the hardware-specific failure modes that this plan covers.

---

## 2. Equipment

- ESP32-WROOM-32 dev board (or the production timer12 PCB).
- USB serial cable (for serial monitor + power).
- Bench power supply with fast cut-off (< 1 ms rise/fall) OR a MOSFET
  in series with the 5V line, controlled by a function generator / button.
- Push-button (for manual power-cut tests).
- Host PC with `esptool.py`, `arduino-cli` or `platformio` for flashing.
- `minicom` / `screen` / `arduino-ide serial monitor` for log capture.

---

## 3. Test fixtures

The firmware under test must be instrumented with a serial-test harness that
exposes the following commands over USB serial (115200 baud, 8N1):

| Command | Action |
|---|---|
| `T:SETUP` | Reset journal to fresh state (erase all NVS keys) |
| `T:STORE <rid> <hash>` | `journal.storeIntent(rid, hash, 1, true, false)` |
| `T:EXEC <rid>` | `journal.markExecuting(rid)` |
| `T:COMMIT <rid> <ackJson>` | `journal.commitTransaction(rid, ackJson)` |
| `T:FAIL <rid> <ackJson>` | `journal.commitTransactionFailed(rid, ackJson, FAILED)` |
| `T:STATE <rid>` | `Serial.println(stateToString(journal.getTransactionState(rid)))` |
| `T:ACKQ` | Dump ACK queue state (count + each entry) |
| `T:RELOAD` | `journal.~TransactionJournal(); new (&journal) TransactionJournal(); journal.begin();` |
| `T:STACK` | `Serial.println(uxTaskGetStackHighWaterMark(NULL))` |
| `T:NVS` | Dump every `tj_*` NVS key with size |

If the production firmware does not expose these commands, add a temporary
`#ifdef TIMER12_TEST_HARNESS` block in `HttpServer.cpp` or a new
`TestHandlers.h` that registers them on the serial line. The harness MUST
be compiled out for production builds.

---

## 4. Power-loss test procedure — per mutation point

For each of the 13 mutation entry points listed in the R4-C9 self-audit
matrix, perform the following procedure:

### 4.1 Generic procedure (per mutation point P)

1. Power on, capture serial banner.
2. Send `T:SETUP` — verify "journal cleared" log.
3. Send the setup commands required to reach the pre-state for P (e.g.
   for `markExecuting`, send `T:STORE` first).
4. Send `T:STATE <rid>` — verify pre-state is what P expects.
5. Cut power AT the intra-mutation point of interest (see §4.2 below for
   the specific cut points).
6. Power on, capture serial banner — note the boot merge log.
7. Send `T:STATE <rid>` — capture observed post-state.
8. Send `T:ACKQ` — capture observed ACK queue state.
9. Compare observed post-state with the expected post-state in §4.2.
10. Send `T:NVS` — verify both copies A and B exist and have matching
    generation values.

### 4.2 Specific cut points and expected outcomes

| Mutation point P | Cut point | Expected post-state | Pass criteria |
|---|---|---|---|
| `storeIntent` | During copy A write | Old state (or EMPTY if first write) | `T:STATE` returns `PENDING` or "not found"; both copies consistent |
| `storeIntent` | During copy B write | Old state (or copy A only) | On boot, 9-row recovery: A VALID, B INVALID → repair A→B; `T:STATE` returns `PENDING` |
| `markExecuting` | During copy A write | PENDING (RAM unchanged) | `T:STATE` returns `PENDING`; both copies at gen N (PENDING) |
| `markExecuting` | During copy B write | PENDING (RAM unchanged, A newer on disk) | On boot, 9-row recovery: A newer (EXECUTING) vs B (PENDING) → GEN_NEWER_A → load A; `T:STATE` returns `EXECUTING`. RAM state advanced on boot but that is durable. |
| `commitTransaction` | During copy A write | EXECUTING (RAM unchanged) | `T:STATE` returns `EXECUTING` |
| `commitTransaction` | During copy B write | EXECUTING (RAM unchanged) | On boot: GEN_NEWER_A → load A; `T:STATE` returns `COMMITTED`. Boot merge reconstructs ACK. |
| `commitTransaction` | During `queueAck` | COMMITTED (journal durable, ACK not durable) | `T:STATE` returns `COMMITTED`; `T:ACKQ` shows missing ACK; on next boot, `_mergeAckQueueFromJournal` reconstructs. |
| `commitTransactionFailed` | During copy A or B write | EXECUTING (RAM unchanged) | `T:STATE` returns `EXECUTING`; same recovery as `commitTransaction`. |
| `clearEntry` | During copy A or B write | Old state preserved (PENDING/EXECUTING) | `T:STATE` returns the pre-clear state. |
| `recoverCorruptedSlot` | During copy A or B write | Slot remains QUARANTINED | `T:STATE` returns `CORRUPTED`. |
| `reconcileEntry` | During copy A or B write | PENDING or EXECUTING (RAM unchanged) | `T:STATE` returns the pre-reconcile state. R5-C1 candidate pattern. |
| `reconcilePendingEntries` | During any per-slot write | Best-effort: per-slot failure does not roll back other slots | Each affected slot returns the pre-reconcile state; other slots return UNKNOWN. |
| `queueAck` | During `tj_ackq_hdr` write | Old queue (if existed) or CORRUPTED | `T:ACKQ` shows old count; on next boot, CRC mismatch → CORRUPTED → boot merge. |
| `queueAck` | During `tj_ackq_rec_N` write | CORRUPTED | `T:ACKQ` shows CORRUPTED; on boot, reconstruct. |
| `queueAck` | During `tj_ackq_crc` write | CORRUPTED (CRC stale) | `T:ACKQ` shows CORRUPTED; on boot, reconstruct. |
| `dequeueAck` | During `tj_ackq_hdr` write | Old queue (slot NOT dequeued) | `T:ACKQ` shows the pre-dequeue state. R4-C2 RAM rollback recovers. |
| `updateAckDeliveryState` | During any ACK queue write | Old deliveryState | `T:ACKQ` shows pre-update state. R4-C2 RAM rollback recovers. |
| `processPendingAcks` | During any ACK queue write | Best-effort — no rollback (R4-C2 documented exception) | `T:ACKQ` may show partial state. Re-publish on next boot is acceptable. |

### 4.3 Expected serial output (per test)

Each test must produce serial output similar to:

```
[Journal] Loaded 5 slots, 3 pending ACKs
[Journal] _observeSlot: slot 0 → SLOT_VALID
[Journal] _applySlotDecision: slot 0 loaded (state=PENDING, gen=3)
[Journal] _loadAckQueue: loaded 3 ACK records (CRC32 verified: a1b2c3d4)
[Journal] _mergeAckQueueFromJournal: added 0 missing ACK(s) from journal
```

If any of these lines is missing or shows "CORRUPTED", the test fails.

---

## 5. Pass/fail criteria

A test PASSES if ALL of the following hold:

1. **No panic / crash / abort.** The serial log must not contain
   `[JOURNAL PANIC]`.
2. **Post-state matches §4.2 expected outcome.** `T:STATE` returns the
   documented state.
3. **Both copies consistent after recovery.** `T:NVS` shows both copies
   A and B exist with matching generation values (or both absent for a
   cleared slot).
4. **ACK queue CRC matches (or reconstruction succeeds).** `T:ACKQ`
   shows either `VALID` with the expected count, or `CORRUPTED` with
   successful boot-merge reconstruction.
5. **No duplicate ACKs.** After `_mergeAckQueueFromJournal`, each
   requestId appears at most once in the ACK queue.
6. **No lost COMMITTED entries.** Every journal slot that was COMMITTED
   before the power-cut is still COMMITTED after the boot-merge.

A test FAILS if any of the following occur:

- `[JOURNAL PANIC]` in the serial log.
- `T:STATE` returns a different state than §4.2 expected.
- A `tj_slot_N_a` key exists without a matching `tj_slot_N_b` (or
  vice versa) after `T:RELOAD`.
- The ACK queue has duplicate requestIds after boot merge.
- A previously-COMMITTED slot returns `PENDING`, `EXECUTING`, `EMPTY`,
  or `CORRUPTED` after the boot merge ( resurrection ).

---

## 6. Stack measurement procedure

The ESP32 `loop()` task stack is 8KB by default. P2-1 mutation paths
must not exceed 4KB of stack (leaving 50% headroom for ISRs and
recursion).

### 6.1 Procedure

1. In `firmware_v4.ino::setup()`, register the journal executor task
   with a known stack size (recommend 8KB):

   ```cpp
   xTaskCreate(journalTask, "journal", 8192, NULL, 5, &journalTaskHandle);
   ```

2. In the journal task, before and after each mutation call, log the
   high-water mark:

   ```cpp
   UBaseType_t before = uxTaskGetStackHighWaterMark(NULL);
   journal.commitTransaction(rid, ackJson);
   UBaseType_t after = uxTaskGetStackHighWaterMark(NULL);
   Serial.printf("[STACK] commit: before=%u after=%u used=%u\n",
                 before, after, before - after);
   ```

3. Capture the serial log during a full boot + 5 store/mark/commit cycles.

### 6.2 Pass criteria

- Peak stack usage during `_loadFromNVS` must be < 4KB (R4-C5 eliminated
  the 90KB `SlotDecision[64]` allocation — verify peak is now bounded
  by a single `SlotDecision` ≈ 1.4KB).
- Peak stack usage during `_persistAckQueue` must be < 3KB (R5-C6 moved
  the 1280-byte `recBuf` to static storage).
- Peak stack usage during `commitTransaction` must be < 3KB.

If any measurement exceeds the budget, file a defect and reduce the
stack allocation (e.g. move another buffer to static storage).

---

## 7. NVS partition verification procedure

The ESP32 NVS partition is 64KB by default. P2-1 uses the following keys:

- `tj_slot_<N>_a` for N in 0..63 — 1200 bytes each, 76.8 KB total
- `tj_slot_<N>_b` for N in 0..63 — 1200 bytes each, 76.8 KB total
- `tj_ackq_hdr` — 4 bytes
- `tj_ackq_rec_<N>` for N in 0..7 — 1280 bytes each, 10.2 KB total
- `tj_ackq_crc` — 4 bytes

Total: ~165 KB. The default 64KB NVS partition is INSUFFICIENT for a
fully-loaded journal. The `platformio.ini` MUST configure a 256KB NVS
partition (or larger) — verify this with `partitions.csv`.

### 7.1 Procedure

1. Flash the firmware.
2. Run `esptool.py --port /dev/ttyUSB0 read_flash <nvs_offset> <nvs_size> nvs.bin`
   (use `gen_esp32part.py` to find the NVS offset/size).
3. Use `nvs_partition_gen.py parse nvs.bin nvs.csv` to dump NVS contents.
4. Verify the expected `tj_*` keys exist with the correct sizes.

### 7.2 Pass criteria

- After `T:SETUP`, the NVS partition contains no `tj_*` keys.
- After `T:STORE` × 64, all 64 slots have both A and B copies (128 keys).
- After 8 successful `T:COMMIT` calls, `tj_ackq_rec_0..7` all exist.
- After `T:RELOAD`, no key sizes have changed (CRC verification proves
  content integrity).

---

## 8. Sign-off checklist

- [ ] §4 — all 13 mutation points × at-least-2 cut points each pass.
- [ ] §5 — no `[JOURNAL PANIC]` in any captured serial log.
- [ ] §6 — peak stack usage under budget for `_loadFromNVS`,
      `_persistAckQueue`, and `commitTransaction`.
- [ ] §7 — NVS partition ≥ 256KB; all expected keys present after a full
      load.
- [ ] Host-side test harness `journal_journal_test_bin` reports 130
      passed, 0 failed (R5-C3/C4 tests included).
- [ ] Host-side `journal_record_test` reports 102 passed, 0 failed.
- [ ] Production firmware build is byte-identical to the firmware
      tested here (use `sha256sum firmware.bin`).

Sign-off:

- Test engineer: ______________________  Date: __________
- Firmware author: ____________________  Date: __________
- QA lead: ___________________________  Date: __________

---

## 9. References

- `docs/CYCLE-8C-REV26-FINAL-PREDICATE.md`
- `docs/CYCLE-8C-REV2-DUAL-COPY-DESIGN.md`
- `docs/CYCLE-8C-REV14-MUTATION-CONSOLIDATION.md`
- `docs/CYCLE-8C-REV15-ACK-TRANSITION.md`
- `firmware/TransactionJournal.cpp` — R4-C9 self-audit matrix at EOF
- `firmware/test/host/TransactionJournalTest.cpp` — TEST 45-48 (R5-C3/C4)

---

End of document.
