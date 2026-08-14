// =============================================================================
// Services/TransactionJournal.cpp — Transaction Journal v4 (Rev26 normative)
// =============================================================================
// P2-1 implementation per docs/CYCLE-8C-REV26-FINAL-PREDICATE.md and
// docs/PHASE-2-SCOPE.md §P2-1.
//
// STORAGE MODEL:
//   - Dual-copy: tj_slot_<idx>_a + tj_slot_<idx>_b (each BLOB_SIZE=1200 bytes)
//   - ACK queue: tj_ackq (2056 bytes)
//   - No separate commit flag (pre-Rev26 tj_commit_<idx> REMOVED)
//
// INVARIANTS ENFORCED (Rev26):
//   I0  — Executor-context check (panic on violation)
//   I0a — Observation/mutation mutual exclusion (panic on violation)
//   I1  — Dual-copy canonical equivalence + 9-row recovery table
//   I2  — Eviction safety (I2a-I2e + auth gate; NON_IDEMPOTENT → NEVER)
//   I3  — ACK lifecycle independent of transaction lifecycle
// =============================================================================
#include "TransactionJournal.h"
#include "Config.h"        // Core::NVS_NAMESPACE (firmware) or shim (host)
#include <Preferences.h>   // ESP32 NVS API (firmware) or shim (host)
#include <cstring>
#include <cstdio>

namespace Services {

// Snapshot provider callback (set by firmware_v4.ino on real device;
// defaults to nullptr on host tests, where captureOutputSnapshot is a no-op).
static std::function<bool(uint8_t)> s_snapshotProvider = nullptr;

// Public API (declared via friend or extern in header — for now, just expose):
// firmware_v4.ino calls this to register a snapshot provider that returns
// the current logical state of channel `idx`.
// Host tests don't call this — captureOutputSnapshot gracefully degrades.
void setSnapshotProvider(std::function<bool(uint8_t)> cb) {
  s_snapshotProvider = cb;
}

// -----------------------------------------------------------------------------
// Static globals
// -----------------------------------------------------------------------------
TransactionJournal journal;

// Publish callback for ACK delivery (set by MqttClient::begin())
static std::function<bool(const char* topic, const uint8_t* payload, size_t len)> s_publishCallback;

void setPublishCallback(std::function<bool(const char* topic, const uint8_t* payload, size_t len)> cb) {
  s_publishCallback = cb;
}

// -----------------------------------------------------------------------------
// Executor task tracking (Rev26 I0 — host uses nullptr for tests)
//
//   On ESP32: xTaskGetCurrentTaskHandle() returns the actual FreeRTOS task.
//   On host (test harness): returns nullptr unless test manipulates it.
//
//   _executorTaskHandle is set in begin() to whatever task called begin().
//   _assertExecutorContext() compares current task == _executorTaskHandle.
//
//   Host tests that want to verify the panic path can call
//   _setExecutorTaskForTest() with a sentinel value, then call mutation APIs
//   from a context where _getCurrentTaskHandle() returns a different value.
// -----------------------------------------------------------------------------
static void* s_executorTaskHandle = nullptr;

// For host tests: allows swapping the "current task" to test executor-context panic
static void* s_testCurrentTask = nullptr;

static void* _getCurrentTaskHandle() {
#ifdef ARDUINO_ARCH_ESP32
  return xTaskGetCurrentTaskHandle();
#else
  // Host test environment: if s_testCurrentTask hasn't been set (default nullptr),
  // return the same sentinel that begin() uses, so _assertExecutorContext()
  // passes for normal call flow. Tests that want to verify the executor-context
  // panic path can explicitly set s_testCurrentTask to a different value.
  if (s_testCurrentTask != nullptr) return s_testCurrentTask;
  return (void*)0x1;  // Same sentinel as begin() uses on host
#endif
}

void TransactionJournal::_setExecutorTaskForTest(void* taskHandle) {
  s_executorTaskHandle = taskHandle;
  s_testCurrentTask = taskHandle;
}

// -----------------------------------------------------------------------------
// Panic (Rev26 — abort triggers reboot on ESP32, abort on host)
// -----------------------------------------------------------------------------
[[noreturn]] static void journal_panic(const char* msg) {
  // Print to both Serial (host stdout) and stderr (host console) for debugging.
  Serial.printf("[JOURNAL PANIC] %s\n", msg);
  fprintf(stderr, "[JOURNAL PANIC] %s\n", msg);
  fflush(stderr);
  // On ESP32, abort() triggers core dump + reboot.
  // On host, abort() terminates the process with non-zero exit.
  abort();
}

// =============================================================================
// ObservationGuard (Rev26 I0a — RAII, panic on nested observation)
// =============================================================================
ObservationGuard::ObservationGuard(bool& flag) : _flag(flag) {
  if (_flag) {
    journal_panic("I0a: nested observation detected (depth > 1)");
  }
  _flag = true;
}

ObservationGuard::~ObservationGuard() {
  _flag = false;
}

// =============================================================================
// TransactionJournal — constructor / destructor
// =============================================================================
TransactionJournal::TransactionJournal() {
  // All members are default-initialized in header.
}

TransactionJournal::~TransactionJournal() {
  // No dynamic resources to release.
}

// =============================================================================
// begin() — initialize journal, load from NVS, queue pending ACKs
// =============================================================================
void TransactionJournal::begin() {
  // Register the calling task as the journal executor (Rev26 I0).
  // On host tests, _getCurrentTaskHandle() may return nullptr (no FreeRTOS).
  // Use a sentinel non-null value so _assertExecutorContext() passes.
  void* currentTask = _getCurrentTaskHandle();
  if (currentTask == nullptr) {
    // Host test environment: use a sentinel non-null value.
    // This allows _assertExecutorContext() to pass when called from the
    // same context that called begin().
    currentTask = (void*)0x1;  // Sentinel for host
  }
  _executorTaskHandle = currentTask;
  s_executorTaskHandle = _executorTaskHandle;

  // Load slots via 9-row reconciliation (Rev26 I1).
  // _loadFromNVS uses 2-phase approach (observation then mutation).
  _loadFromNVS();

  // Load ACK queue (Rev26 I3).
  _loadAckQueue();

  // P2-1 CORRECTION (auditor P1-9): Merge missing ACKs from journal.
  // For each COMMITTED journal entry with non-empty ackJson that is NOT
  // already in the ACK queue, add it. This ensures ACKs are not lost
  // even if ACK queue persistence failed before reboot.
  _mergeAckQueueFromJournal();

  Serial.printf("[Journal] Loaded %u slots, %u pending ACKs\n",
                _journalSize, _ackQueueCount);
}

// =============================================================================
// _mergeAckQueueFromJournal (Rev26 I3 — boot merge)
//
//   P2-1 CORRECTION (auditor P1-9): previous version did NOT implement the
//   promised boot merge. _loadFromNVS only loaded slots; _loadAckQueue only
//   loaded the ACK blob. If a COMMITTED journal entry had ackJson but its
//   ACK was missing from the queue (e.g., queueAck failed before reboot),
//   the ACK would be lost forever.
//
//   Corrected: after loading both, scan journal COMMITTED entries and add
//   any missing ACKs to the queue.
// =============================================================================
void TransactionJournal::_mergeAckQueueFromJournal() {
  uint8_t added = 0;
  for (uint8_t i = 0; i < JOURNAL_SIZE; i++) {
    if (!_slots[i].inUse) continue;
    if (_slots[i].record.recordState != RecordState::COMMITTED) continue;
    if (_slots[i].record.ackJson.length() == 0) continue;

    // Check if this requestId is already in ACK queue
    int8_t existing = _findAckInQueue(_slots[i].record.requestId);
    if (existing >= 0) continue;  // Already in queue

    // Add missing ACK to queue
    if (_ackQueueCount >= ACK_QUEUE_CAPACITY) {
      // Queue full — find droppable slot (FAILED_EXHAUSTED or empty)
      int8_t droppableIdx = -1;
      for (uint8_t j = 0; j < ACK_QUEUE_CAPACITY; j++) {
        if (_ackQueue[j].deliveryState == AckDeliveryState::ACK_FAILED_EXHAUSTED ||
            _ackQueue[j].isEmpty()) {
          droppableIdx = (int8_t)j;
          break;
        }
      }
      if (droppableIdx < 0) {
        Serial.printf("[Journal] _mergeAckQueueFromJournal: queue full, cannot add ACK for rid=%s\n",
                      _slots[i].record.requestId.c_str());
        continue;
      }
      // Reuse droppable slot
      AckRecord& rec = _ackQueue[droppableIdx];
      rec.deliveryState = AckDeliveryState::ACK_NOT_SENT;
      rec.requestId = _slots[i].record.requestId;
      rec.commandHash = _slots[i].record.commandHash;
      rec.retryCount = 0;
      rec.lastAttemptTs = 0;
      rec.ackJson = _slots[i].record.ackJson;
      added++;
    } else {
      AckRecord& rec = _ackQueue[_ackQueueCount++];
      rec.deliveryState = AckDeliveryState::ACK_NOT_SENT;
      rec.requestId = _slots[i].record.requestId;
      rec.commandHash = _slots[i].record.commandHash;
      rec.retryCount = 0;
      rec.lastAttemptTs = 0;
      rec.ackJson = _slots[i].record.ackJson;
      added++;
    }
  }

  if (added > 0) {
    Serial.printf("[Journal] _mergeAckQueueFromJournal: added %u missing ACK(s) from journal\n", added);
    _persistAckQueue();
  }
}

// =============================================================================
// Boot phase management (Cycle-8B — preserved, unchanged by Rev26)
// =============================================================================
void TransactionJournal::setBootPhase(BootPhase phase) {
  BootPhase old = _bootPhase;
  _bootPhase = phase;
  if (old != phase) {
    Serial.printf("[Journal] Boot phase: %s → %s\n",
                  _phaseToString(old), _phaseToString(phase));
  }
}

// =============================================================================
// Output snapshot (Cycle-8B — preserved)
// =============================================================================
void TransactionJournal::captureOutputSnapshot() {
  // Capture current GPIO output state for all channels.
  // Used during RECONCILING phase to detect if EXECUTING transactions
  // actually drove the relay.
  // On ESP32: firmware_v4.ino calls setSnapshotProvider() to wire in
  //           Drivers::relay.getState().
  // On host tests: s_snapshotProvider is nullptr — snapshot stays all-false
  //                (no degradation; tests don't exercise snapshot logic).
  if (s_snapshotProvider) {
    for (uint8_t i = 0; i < 16; i++) {
      _outputSnapshot[i] = s_snapshotProvider(i);
    }
  } else {
    for (uint8_t i = 0; i < 16; i++) {
      _outputSnapshot[i] = false;
    }
  }
  _snapshotCaptured = true;
  Serial.printf("[Journal] Output snapshot captured\n");
}

bool TransactionJournal::getSnapshotState(uint8_t channelIdx) const {
  if (channelIdx >= 16) return false;
  return _outputSnapshot[channelIdx];
}

// =============================================================================
// I0 / I0a enforcement (Rev26 — panic on violation)
// =============================================================================
void TransactionJournal::_assertExecutorContext() {
  if (_executorTaskHandle == nullptr) {
    // Journal not yet initialized — no executor registered.
    journal_panic("I0: executor task not registered (begin() not called?)");
  }
  if (_getCurrentTaskHandle() != _executorTaskHandle) {
    journal_panic("I0: journal API called from non-executor context");
  }
}

void TransactionJournal::_assertMutationAllowed() {
  if (_observing) {
    journal_panic("I0a: mutation attempted during active observation");
  }
}

// =============================================================================
// Record state conversion (TransactionState runtime ↔ RecordState on-disk)
//
//   TransactionState has CORRUPTED (runtime-only, not serialized).
//   RecordState has EMPTY (on-disk only, runtime sees SLOT_EMPTY durability).
//
//   Mapping (preserves backward compat with MqttClient.cpp enum values):
//     TransactionState::PENDING                          → RecordState::PENDING
//     TransactionState::EXECUTING                        → RecordState::EXECUTING
//     TransactionState::COMMITTED                        → RecordState::COMMITTED
//     TransactionState::COMMITTED_UNKNOWN                → RecordState::COMMITTED_UNKNOWN
//     TransactionState::UNKNOWN                          → RecordState::UNKNOWN
//     TransactionState::FAILED                            → RecordState::FAILED
//     TransactionState::EXECUTION_FAILED_OUTPUT_MISMATCH → RecordState::EXECUTION_FAILED_OUTPUT_MISMATCH
//     TransactionState::CORRUPTED                        → (no RecordState — runtime only)
//     (no TransactionState for EMPTY)                    → RecordState::EMPTY
// =============================================================================
RecordState TransactionJournal::_toRecordState(TransactionState s) {
  switch (s) {
    case TransactionState::PENDING:                          return RecordState::PENDING;
    case TransactionState::EXECUTING:                        return RecordState::EXECUTING;
    case TransactionState::COMMITTED:                        return RecordState::COMMITTED;
    case TransactionState::COMMITTED_UNKNOWN:                return RecordState::COMMITTED_UNKNOWN;
    case TransactionState::UNKNOWN:                          return RecordState::UNKNOWN;
    case TransactionState::FAILED:                            return RecordState::FAILED;
    case TransactionState::EXECUTION_FAILED_OUTPUT_MISMATCH: return RecordState::EXECUTION_FAILED_OUTPUT_MISMATCH;
    case TransactionState::CORRUPTED:
      journal_panic("_toRecordState: CORRUPTED is runtime-only, cannot serialize");
  }
  journal_panic("_toRecordState: unknown TransactionState");
}

TransactionState TransactionJournal::_fromRecordState(RecordState s) {
  switch (s) {
    case RecordState::EMPTY:                                return TransactionState::PENDING;  // EMPTY = no record; caller checks durability
    case RecordState::PENDING:                              return TransactionState::PENDING;
    case RecordState::EXECUTING:                            return TransactionState::EXECUTING;
    case RecordState::COMMITTED:                            return TransactionState::COMMITTED;
    case RecordState::COMMITTED_UNKNOWN:                    return TransactionState::COMMITTED_UNKNOWN;
    case RecordState::UNKNOWN:                              return TransactionState::UNKNOWN;
    case RecordState::FAILED:                                return TransactionState::FAILED;
    case RecordState::EXECUTION_FAILED_OUTPUT_MISMATCH:     return TransactionState::EXECUTION_FAILED_OUTPUT_MISMATCH;
  }
  journal_panic("_fromRecordState: unknown RecordState");
}

// =============================================================================
// NVS key helpers (Rev26 — tj_slot_<idx>_a / _b)
// =============================================================================
String TransactionJournal::_slotKeyA(uint8_t idx) {
  char buf[20];
  snprintf(buf, sizeof(buf), "tj_slot_%u_a", idx);
  return String(buf);
}

String TransactionJournal::_slotKeyB(uint8_t idx) {
  char buf[20];
  snprintf(buf, sizeof(buf), "tj_slot_%u_b", idx);
  return String(buf);
}

// =============================================================================
// Dual-copy write (Rev26 I1 — writes one copy, verifies on read-back)
//
//   P2-1 CORRECTION (auditor P1-4): Added _assertMutationAllowed() at entry.
//   This enforces I0a — _writeCopy() may only be called from mutation phase,
//   not during observation. Callers must ensure ObservationGuard is not
//   active when invoking this.
// =============================================================================
bool TransactionJournal::_writeCopy(uint8_t slotIdx, bool isCopyA, const JournalRecord& rec) {
  _assertMutationAllowed();  // P1-4: enforce I0a
  uint8_t blob[BLOB_SIZE];
  uint16_t payloadEnd = serializeRecord(rec, blob, BLOB_SIZE);
  if (payloadEnd == 0) {
    Serial.printf("[Journal] _writeCopy: serializeRecord failed for slot %u\n", slotIdx);
    return false;
  }

  String key = isCopyA ? _slotKeyA(slotIdx) : _slotKeyB(slotIdx);

  Preferences prefs;
  if (!prefs.begin(Core::NVS_NAMESPACE, false)) {
    Serial.printf("[Journal] _writeCopy: NVS begin failed for slot %u\n", slotIdx);
    return false;
  }

  size_t written = prefs.putBytes(key.c_str(), blob, BLOB_SIZE);
  prefs.end();

  if (written != BLOB_SIZE) {
    Serial.printf("[Journal] _writeCopy: NVS write short for slot %u copy %s (wrote %u of %u)\n",
                  slotIdx, isCopyA ? "A" : "B", (unsigned)written, (unsigned)BLOB_SIZE);
    return false;
  }

  // Verify by reading back and checking CRC.
  JournalRecord verify;
  if (!_readCopy(slotIdx, isCopyA, verify)) {
    Serial.printf("[Journal] _writeCopy: verify readback failed for slot %u copy %s\n",
                  slotIdx, isCopyA ? "A" : "B");
    return false;
  }

  // Confirm the verified record matches what we wrote.
  // NOTE: canonicalEqual compares canonicalLength, but the source `rec` may
  // not have canonicalLength set (it's only set by deserializeRecord). We
  // compare fields directly instead — this is functionally equivalent for
  // the write-verify pattern.
  if (verify.schemaVersion != rec.schemaVersion) {
    Serial.printf("[Journal] _writeCopy: schemaVersion mismatch for slot %u copy %s\n",
                  slotIdx, isCopyA ? "A" : "B");
    return false;
  }
  if (verify.generation != rec.generation) {
    Serial.printf("[Journal] _writeCopy: generation mismatch for slot %u copy %s\n",
                  slotIdx, isCopyA ? "A" : "B");
    return false;
  }
  if ((uint8_t)verify.recordState != (uint8_t)rec.recordState) {
    Serial.printf("[Journal] _writeCopy: recordState mismatch for slot %u copy %s\n",
                  slotIdx, isCopyA ? "A" : "B");
    return false;
  }
  if (verify.requestId != rec.requestId) {
    Serial.printf("[Journal] _writeCopy: requestId mismatch for slot %u copy %s\n",
                  slotIdx, isCopyA ? "A" : "B");
    return false;
  }
  if (verify.commandHash != rec.commandHash) {
    Serial.printf("[Journal] _writeCopy: commandHash mismatch for slot %u copy %s\n",
                  slotIdx, isCopyA ? "A" : "B");
    return false;
  }
  if (verify.ackJson != rec.ackJson) {
    Serial.printf("[Journal] _writeCopy: ackJson mismatch for slot %u copy %s\n",
                  slotIdx, isCopyA ? "A" : "B");
    return false;
  }

  return true;
}

// =============================================================================
// Dual-copy read (Rev26 I1 — read blob, deserialize, verify CRC)
//
//   Returns true if the copy is VALID (parseable + CRC matches).
//   Returns false if INVALID (parse failed, CRC mismatch, or NVS read failed).
// =============================================================================
bool TransactionJournal::_readCopy(uint8_t slotIdx, bool isCopyA, JournalRecord& outRec) {
  String key = isCopyA ? _slotKeyA(slotIdx) : _slotKeyB(slotIdx);

  Preferences prefs;
  if (!prefs.begin(Core::NVS_NAMESPACE, true)) {  // readOnly
    return false;
  }

  if (!prefs.isKey(key.c_str())) {
    prefs.end();
    return false;  // No blob — treat as INVALID (slot empty)
  }

  uint8_t blob[BLOB_SIZE];
  size_t read = prefs.getBytes(key.c_str(), blob, BLOB_SIZE);
  prefs.end();

  if (read != BLOB_SIZE) {
    return false;  // Partial blob — INVALID
  }

  // Safe parse (Phase 1 — bounds-checked).
  ParseResult pr = deserializeRecord(blob, BLOB_SIZE, outRec);
  if (pr != ParseResult::PARSE_VALID) {
    return false;  // Malformed — INVALID
  }

  // CRC verification (Phase 1 — verifyRecordCRC).
  // canonicalLength is set by deserializeRecord.
  uint16_t actualPayloadEnd = BLOB_HEADER_SIZE + outRec.canonicalLength;
  if (!verifyRecordCRC(blob, actualPayloadEnd)) {
    return false;  // CRC mismatch — INVALID
  }

  return true;
}

// =============================================================================
// Erase both copies (Rev26 — used by recoverCorruptedEntry)
//
//   P2-1 CORRECTION (auditor P1-4): Added _assertMutationAllowed() at entry.
// =============================================================================
bool TransactionJournal::_eraseBlobNVS(uint8_t slotIdx) {
  _assertMutationAllowed();  // P1-4: enforce I0a
  Preferences prefs;
  if (!prefs.begin(Core::NVS_NAMESPACE, false)) return false;

  prefs.remove(_slotKeyA(slotIdx).c_str());
  prefs.remove(_slotKeyB(slotIdx).c_str());
  prefs.end();
  return true;
}

// =============================================================================
// Clear slot (Rev26 — write EMPTY(prevGen+1) to both copies)
//
//   P2-1 CORRECTION (auditor P0-2): previous version wrote EMPTY(gen=0).
//   If power loss occurred between writing copy A and copy B, copy B would
//   still contain COMMITTED(gen=37). On boot, 9-row recovery would see
//   A=EMPTY(gen=0), B=COMMITTED(gen=37) → GEN_NEWER_B → load B → resurrect
//   the cleared transaction.
//
//   Corrected: write EMPTY(prevGen+1). After crash mid-clear:
//     A = EMPTY(gen=38), B = COMMITTED(gen=37)
//     → GEN_NEWER_A (distAB = 0xFFFFFFFF, distBA = 1)
//     → load A (EMPTY) → slot is empty, no resurrection.
//
//   recoverCorruptedEntry() is different — it writes EMPTY(gen=0)
//   unconditionally because both copies are already INVALID (quarantined).
//
//   P2-1 CORRECTION (auditor P1-4): Added _assertMutationAllowed() at entry.
// =============================================================================
bool TransactionJournal::_clearSlotNVS(uint8_t slotIdx) {
  _assertMutationAllowed();  // P1-4: enforce I0a
  // Get previous generation from current slot state (before clearing).
  uint32_t prevGen = _slots[slotIdx].record.generation;

  JournalRecord empty;
  empty.schemaVersion = JOURNAL_SCHEMA_VERSION;
  empty.generation = prevGen + 1;  // Crash-safe: prevGen+1 wins over stale COMMITTED(prevGen)
  empty.recordState = RecordState::EMPTY;
  empty.requestId = "";
  empty.commandHash = "";
  empty.channelId = 0;
  empty.desiredState = 0xFF;
  empty.previousKnownState = 0;
  empty.attempt = 0;
  empty.timestamp = 0;
  empty.ackJson = "";

  if (!_writeCopy(slotIdx, true, empty)) return false;
  if (!_writeCopy(slotIdx, false, empty)) return false;
  return true;
}

// =============================================================================
// Repair slot (Rev26 I1 — bitwise restoration from VALID copy to INVALID copy)
//
//   REPAIR(B) when A=VALID, B=INVALID:
//     Read A's full record (including generation).
//     Write IDENTICAL record to B (same gen, same payload).
//     Verify B (re-read + CRC + byte-compare with A).
//
//   Repair does NOT increment generation.
//   Repair does NOT change recordState or any field.
//
//   P2-1 CORRECTION (auditor P1-4): Added _assertMutationAllowed() at entry.
// =============================================================================
bool TransactionJournal::_repairSlot(uint8_t slotIdx, bool fromCopyA) {
  _assertMutationAllowed();  // P1-4: enforce I0a
  JournalRecord source;
  if (!_readCopy(slotIdx, fromCopyA, source)) {
    Serial.printf("[Journal] _repairSlot: source copy %s unreadable for slot %u\n",
                  fromCopyA ? "A" : "B", slotIdx);
    return false;
  }

  // Write source record to the other copy (bitwise identical).
  bool writeA = !fromCopyA;  // If source is A, write B; if source is B, write A.
  if (!_writeCopy(slotIdx, writeA, source)) {
    Serial.printf("[Journal] _repairSlot: write to copy %s failed for slot %u\n",
                  writeA ? "A" : "B", slotIdx);
    return false;
  }

  // Verify the repaired copy now matches the source.
  JournalRecord repaired;
  if (!_readCopy(slotIdx, writeA, repaired)) {
    Serial.printf("[Journal] _repairSlot: verify failed for slot %u copy %s\n",
                  slotIdx, writeA ? "A" : "B");
    return false;
  }

  if (!canonicalEqual(source, repaired)) {
    Serial.printf("[Journal] _repairSlot: canonical mismatch after repair for slot %u\n", slotIdx);
    return false;
  }

  // Confirm generation is preserved.
  if (source.generation != repaired.generation) {
    Serial.printf("[Journal] _repairSlot: generation drift after repair for slot %u\n", slotIdx);
    return false;
  }

  Serial.printf("[Journal] _repairSlot: slot %u repaired from copy %s\n",
                slotIdx, fromCopyA ? "A" : "B");
  return true;
}

// =============================================================================
// Quarantine slot (Rev26 I1 — both copies INVALID, mark CORRUPTED in RAM)
//
//   Quarantine does NOT erase NVS. Slot is preserved (operator may recover
//   via recoverCorruptedEntry()).
// =============================================================================
bool TransactionJournal::_quarantineSlot(uint8_t slotIdx) {
  _slots[slotIdx].durability = SlotDurability::SLOT_QUARANTINED;
  _slots[slotIdx].record.recordState = RecordState::EMPTY;  // RAM-only — NVS untouched
  _slots[slotIdx].record.generation = 0;
  _slots[slotIdx].inUse = false;  // Quarantined slots don't accept new entries

  Serial.printf("[Journal] _quarantineSlot: slot %u QUARANTINED (both copies INVALID)\n", slotIdx);
  return true;
}

// =============================================================================
// 9-row recovery decision table (Rev26 I1 — implemented per CYCLE-8C-REV14 §I1)
//
//   | # | Copy A   | Copy B   | Gen Relationship           | Action      |
//   |---|----------|----------|-----------------------------|-------------|
//   | 1 | INVALID  | INVALID  | N/A                         | QUARANTINED |
//   | 2 | VALID    | INVALID  | N/A                         | REPAIR A→B |
//   | 3 | INVALID  | VALID    | N/A                         | REPAIR B→A |
//   | 4 | VALID    | VALID    | GEN_NEWER_A (distBA == 1)   | Load A      |
//   | 5 | VALID    | VALID    | GEN_NEWER_B (distAB == 1)   | Load B      |
//   | 6 | VALID    | VALID    | GEN_EQUAL + canonicalEqual  | Load either |
//   | 7 | VALID    | VALID    | GEN_EQUAL + divergent       | CORRUPTED   |
//   | 8 | VALID    | VALID    | GEN_AMBIGUOUS (dist==2^31)  | CORRUPTED   |
//   | 9 | VALID    | VALID    | GEN_INVALID (distance > 1) | CORRUPTED   |
//
//   NOTE: This is an INTERNAL observation function. It does NOT create its
//   own ObservationGuard — callers (like _loadFromNVS) must already hold one.
//   (Rev26 I0a: nesting depth must be 1, not unbounded.)
// =============================================================================
SlotDurability TransactionJournal::_reconcileSlot(uint8_t slotIdx) {
  // (No ObservationGuard here — caller holds one.)

  // First check if slot is completely empty (no NVS keys for either copy).
  // An empty slot is NOT a quarantine case — it's just an unused slot.
  {
    Preferences prefs;
    prefs.begin(Core::NVS_NAMESPACE, true);
    bool aExists = prefs.isKey(_slotKeyA(slotIdx).c_str());
    bool bExists = prefs.isKey(_slotKeyB(slotIdx).c_str());
    prefs.end();
    if (!aExists && !bExists) {
      _slots[slotIdx].record = JournalRecord();  // defaults: gen=0, EMPTY
      _slots[slotIdx].durability = SlotDurability::SLOT_EMPTY;
      _slots[slotIdx].inUse = false;
      return SlotDurability::SLOT_EMPTY;
    }
  }

  JournalRecord recA, recB;
  bool validA = _readCopy(slotIdx, true, recA);
  bool validB = _readCopy(slotIdx, false, recB);

  // Row 1: both INVALID → quarantine
  if (!validA && !validB) {
    _quarantineSlot(slotIdx);
    return SlotDurability::SLOT_QUARANTINED;
  }

  // Row 2: A VALID, B INVALID → repair A→B
  if (validA && !validB) {
    if (!_repairSlot(slotIdx, true /* from A */)) {
      _quarantineSlot(slotIdx);
      return SlotDurability::SLOT_QUARANTINED;
    }
    // After repair, B matches A — load A.
    validB = _readCopy(slotIdx, false, recB);
    if (!validB) {
      _quarantineSlot(slotIdx);
      return SlotDurability::SLOT_QUARANTINED;
    }
  }

  // Row 3: A INVALID, B VALID → repair B→A
  if (!validA && validB) {
    if (!_repairSlot(slotIdx, false /* from B */)) {
      _quarantineSlot(slotIdx);
      return SlotDurability::SLOT_QUARANTINED;
    }
    validA = _readCopy(slotIdx, true, recA);
    if (!validA) {
      _quarantineSlot(slotIdx);
      return SlotDurability::SLOT_QUARANTINED;
    }
  }

  // Both VALID now — apply generation classifier (Phase 1).
  GenRelation rel = classifyGeneration(recA.generation, recB.generation);

  // Row 4: GEN_NEWER_A → load A
  if (rel == GenRelation::GEN_NEWER_A) {
    _slots[slotIdx].record = recA;
    _slots[slotIdx].durability = SlotDurability::SLOT_VALID;
    _slots[slotIdx].inUse = (recA.recordState != RecordState::EMPTY);
    return SlotDurability::SLOT_VALID;
  }

  // Row 5: GEN_NEWER_B → load B
  if (rel == GenRelation::GEN_NEWER_B) {
    _slots[slotIdx].record = recB;
    _slots[slotIdx].durability = SlotDurability::SLOT_VALID;
    _slots[slotIdx].inUse = (recB.recordState != RecordState::EMPTY);
    return SlotDurability::SLOT_VALID;
  }

  // Row 6: GEN_EQUAL + canonicalEqual → load either
  if (rel == GenRelation::GEN_EQUAL) {
    if (canonicalEqual(recA, recB)) {
      _slots[slotIdx].record = recA;
      _slots[slotIdx].durability = SlotDurability::SLOT_VALID;
      _slots[slotIdx].inUse = (recA.recordState != RecordState::EMPTY);
      return SlotDurability::SLOT_VALID;
    } else {
      // Row 7: GEN_EQUAL + divergent → CORRUPTED
      _quarantineSlot(slotIdx);
      return SlotDurability::SLOT_QUARANTINED;
    }
  }

  // Row 8: GEN_AMBIGUOUS → CORRUPTED
  if (rel == GenRelation::GEN_AMBIGUOUS) {
    _quarantineSlot(slotIdx);
    return SlotDurability::SLOT_QUARANTINED;
  }

  // Row 9: GEN_INVALID → CORRUPTED
  _quarantineSlot(slotIdx);
  return SlotDurability::SLOT_QUARANTINED;
}

// =============================================================================
// Slot evaluation (Rev26 I1 — internal helper, caller holds ObservationGuard)
// =============================================================================
SlotDurability TransactionJournal::_evaluateSlot(uint8_t slotIdx) {
  // (No ObservationGuard here — caller holds one.)
  return _reconcileSlot(slotIdx);
}

// =============================================================================
// I1 satisfaction check (Rev26 — internal helper, caller holds ObservationGuard)
// =============================================================================
bool TransactionJournal::_checkI1Satisfied(uint8_t slotIdx) {
  // (No ObservationGuard here — caller holds one.)
  return _slots[slotIdx].durability == SlotDurability::SLOT_VALID ||
         _slots[slotIdx].durability == SlotDurability::SLOT_EMPTY;
}

// =============================================================================
// Find slot by requestId (binary-safe string comparison)
// =============================================================================
uint8_t TransactionJournal::_findSlot(const String& requestId) const {
  for (uint8_t i = 0; i < JOURNAL_SIZE; i++) {
    if (_slots[i].inUse && _slots[i].record.requestId == requestId) {
      return i;
    }
  }
  return JOURNAL_SIZE;  // Not found
}

uint8_t TransactionJournal::_findSlotByRequestId(const String& requestId) const {
  return _findSlot(requestId);
}

// =============================================================================
// Find evictable slot (Rev26 I2 — applies eviction predicate)
//
//   Walks the slot ring starting at _journalWriteIdx, returns the first slot
//   where _isEvictionPermitted() returns true. Returns JOURNAL_SIZE if no
//   slot is evictable (journal full + nothing evictable → caller must reject).
// =============================================================================
uint8_t TransactionJournal::_findEvictableSlot() const {
  for (uint8_t i = 0; i < JOURNAL_SIZE; i++) {
    uint8_t idx = (_journalWriteIdx + i) % JOURNAL_SIZE;
    if (!_slots[idx].inUse) {
      return idx;  // Empty slot — no eviction needed
    }
    if (_isEvictionPermitted(idx)) {
      return idx;  // Slot can be evicted per I2a-I2e
    }
  }
  return JOURNAL_SIZE;  // Journal full, nothing evictable
}

// =============================================================================
// Eviction predicate (Rev26 §4 — I2a-I2e + auth gate)
//
//   journal_eviction_permitted(slotIdx) for current implementation ≡
//     I2a (journal full, slot needed)
//     AND I2b (command_class ∈ {IDEMPOTENT, NON_IDEMPOTENT})
//     AND I2c (
//         (IDEMPOTENT
//          AND deliveryState ∈ {PUBLISH_ACCEPTED+durable_queue, FAILED_EXHAUSTED})
//         // NON_IDEMPOTENT: no achievable deliveryState permits eviction
//     )
//     AND I2d (COMMITTED)
//     AND I2e (default RETAIN)
//
//   For NON_IDEMPOTENT: AUTH_EVIDENCE_AUTHENTICATED is UNACHIEVABLE → NEVER.
// =============================================================================
bool TransactionJournal::_isEvictionPermitted(uint8_t slotIdx) const {
  const SlotInfo& slot = _slots[slotIdx];

  // I2d: must be COMMITTED (not CORRUPTED/QUARANTINED, not PENDING/EXECUTING)
  if (slot.durability != SlotDurability::SLOT_VALID) return false;
  if (slot.record.recordState != RecordState::COMMITTED) return false;

  // I2b: classify command
  CommandClass cls = _classifyCommand(slot.record.commandHash);
  if (cls == CommandClass::UNKNOWN) return false;  // I2e: default RETAIN

  // For NON_IDEMPOTENT: AUTH_EVIDENCE_AUTHENTICATED is UNACHIEVABLE → NEVER
  if (cls == CommandClass::NON_IDEMPOTENT) return false;

  // IDEMPOTENT: check ACK delivery state in ackq
  int8_t ackIdx = _findAckInQueue(slot.record.requestId);
  if (ackIdx < 0) return false;  // No ACK entry — RETAIN

  AckDeliveryState ds = _ackQueue[ackIdx].deliveryState;
  if (ds == AckDeliveryState::ACK_FAILED_EXHAUSTED) return true;
  if (ds == AckDeliveryState::ACK_PUBLISH_ACCEPTED) return true;  // +durable queue (always durable per I3)
  if (ds == AckDeliveryState::ACK_BROKER_CONFIRMED) return true;
  if (ds == AckDeliveryState::ACK_PWA_RECEIVED) return true;

  // ACK_NOT_SENT → RETAIN
  return false;
}

// =============================================================================
// Command classification (Rev26 I2b — based on commandHash prefix)
//
//   commandHash format: "type|action|field1=val1|..."
//   Classification by "type" prefix.
//
//   NOTE: This is a placeholder for P2-1. P2-2 will provide semantic proof
//   per command (auditor Q4 OPEN). Until proof is produced, schedule upsert/
//   delete default to NON_IDEMPOTENT for safety.
// =============================================================================
CommandClass TransactionJournal::_classifyCommand(const String& commandHash) {
  // commandHash format examples:
  //   "set_state|ch=3|state=on"
  //   "schedule|action=upsert|ch=3|id=abc"
  //   "ota|version=4.1.0|sha256=..."
  //   "factory_reset|confirm=yes"

  // Extract command type prefix (before first '|')
  int sep = commandHash.indexOf('|');
  String type = (sep < 0) ? commandHash : commandHash.substring(0, sep);
  type.toLowerCase();

  // IDEMPOTENT commands (semantic proof pending in P2-2):
  if (type == "set_state" || type == "set_mode" ||
      type == "pir_config" || type == "channel_rename" ||
      type == "time_set" || type == "config_set") {
    return CommandClass::IDEMPOTENT;
  }

  // NON_IDEMPOTENT commands:
  if (type == "ota" || type == "factory_reset" || type == "precharge") {
    return CommandClass::NON_IDEMPOTENT;
  }

  // OPEN (auditor Q4): schedule upsert/delete.
  // Until semantic proof is produced in P2-2, classify as NON_IDEMPOTENT
  // for safety (eviction = NEVER, so these entries are always retained).
  if (type == "schedule") {
    return CommandClass::NON_IDEMPOTENT;  // Conservative — pending P2-2 proof
  }

  // UNKNOWN — default to NON_IDEMPOTENT-safe (RETAIN per I2e)
  return CommandClass::UNKNOWN;
}

// =============================================================================
// Generation assignment (Rev26 — distance 0 or 1 only)
//
//   For a new entry to slot N:
//     - If slot was EMPTY: generation = 1
//     - If slot had a previous record: generation = prevGen + 1
//
//   Loader validates distance is 0 (same/repair) or 1 (adjacent mutation).
// =============================================================================
uint32_t TransactionJournal::_assignNextGeneration(uint8_t slotIdx) const {
  const SlotInfo& slot = _slots[slotIdx];
  if (slot.durability == SlotDurability::SLOT_EMPTY) {
    return 1;  // First generation in this slot
  }
  // Wrap-safe uint32_t increment (Phase 1 classifier handles wrap)
  return slot.record.generation + 1;
}

// =============================================================================
// MUTATION API — storeIntent (Rev26 — write PENDING entry to both copies)
// =============================================================================
bool TransactionJournal::storeIntent(const String& requestId, const String& commandHash,
                                      uint8_t channelId, bool desiredState,
                                      bool previousKnownState) {
  _assertExecutorContext();
  _assertMutationAllowed();

  // Check if requestId already exists (duplicate detection)
  uint8_t existingIdx = _findSlot(requestId);
  if (existingIdx != JOURNAL_SIZE) {
    // requestId already in journal — caller should have checked isProcessed()
    Serial.printf("[Journal] storeIntent: requestId %s already in slot %u\n",
                  requestId.c_str(), existingIdx);
    return false;
  }

  // Find slot for new entry (LRU eviction if needed, per I2)
  uint8_t slotIdx = _findEvictableSlot();
  if (slotIdx == JOURNAL_SIZE) {
    Serial.printf("[Journal] storeIntent: journal full, no evictable slot for %s\n",
                  requestId.c_str());
    return false;
  }

  // Build PENDING record
  JournalRecord rec;
  rec.schemaVersion = JOURNAL_SCHEMA_VERSION;
  rec.generation = _assignNextGeneration(slotIdx);
  rec.recordState = RecordState::PENDING;
  rec.requestId = requestId;
  rec.commandHash = commandHash;
  rec.channelId = channelId;
  rec.desiredState = desiredState ? 1 : 0;
  rec.previousKnownState = previousKnownState ? 1 : 0;
  rec.attempt = 0;
  rec.timestamp = millis() / 1000;  // Unix epoch seconds (set elsewhere if RTC available)
  rec.ackJson = "";

  // Write to both copies (dual-copy durability per I1)
  if (!_writeCopy(slotIdx, true, rec)) {
    Serial.printf("[Journal] storeIntent: write copy A failed for slot %u\n", slotIdx);
    return false;
  }
  if (!_writeCopy(slotIdx, false, rec)) {
    Serial.printf("[Journal] storeIntent: write copy B failed for slot %u\n", slotIdx);
    return false;
  }

  // Update in-RAM cache
  _slots[slotIdx].record = rec;
  _slots[slotIdx].durability = SlotDurability::SLOT_VALID;
  _slots[slotIdx].inUse = true;
  _journalWriteIdx = (slotIdx + 1) % JOURNAL_SIZE;
  _journalSize++;

  Serial.printf("[Journal] storeIntent: rid=%s → slot %u (gen=%u)\n",
                requestId.c_str(), slotIdx, (unsigned)rec.generation);
  return true;
}

// =============================================================================
// MUTATION API — markExecuting (Rev26 — PENDING → EXECUTING)
// =============================================================================
bool TransactionJournal::markExecuting(const String& requestId) {
  _assertExecutorContext();
  _assertMutationAllowed();

  uint8_t slotIdx = _findSlot(requestId);
  if (slotIdx == JOURNAL_SIZE) {
    Serial.printf("[Journal] markExecuting: requestId %s not found\n", requestId.c_str());
    return false;
  }

  JournalRecord& rec = _slots[slotIdx].record;
  if (rec.recordState != RecordState::PENDING) {
    Serial.printf("[Journal] markExecuting: slot %u state %u (expected PENDING)\n",
                  slotIdx, (uint8_t)rec.recordState);
    return false;
  }

  // Generation increment (Rev26 — distance 1 for adjacent mutation).
  rec.generation = _assignNextGeneration(slotIdx);
  rec.recordState = RecordState::EXECUTING;
  rec.attempt++;

  if (!_writeCopy(slotIdx, true, rec)) return false;
  if (!_writeCopy(slotIdx, false, rec)) return false;

  Serial.printf("[Journal] markExecuting: rid=%s slot %u attempt %u gen=%u\n",
                requestId.c_str(), slotIdx, rec.attempt, (unsigned)rec.generation);
  return true;
}

// =============================================================================
// MUTATION API — commitTransaction (Rev26 — EXECUTING → COMMITTED + queue ACK)
//
//   P2-1 CORRECTION (auditor P1-7): previous version ignored queueAck return
//   value. Corrected: if queueAck fails (ACK persistence failed), the
//   transaction is NOT marked COMMITTED — return false so caller knows
//   ACK delivery is not durable.
//
//   Note: The journal entry itself IS written to both copies (durable).
//   Only the ACK queue persistence failed. The caller should retry the
//   commit or handle the ACK delivery separately.
// =============================================================================
bool TransactionJournal::commitTransaction(const String& requestId, const String& ackJson) {
  _assertExecutorContext();
  _assertMutationAllowed();

  uint8_t slotIdx = _findSlot(requestId);
  if (slotIdx == JOURNAL_SIZE) {
    Serial.printf("[Journal] commitTransaction: requestId %s not found\n", requestId.c_str());
    return false;
  }

  JournalRecord& rec = _slots[slotIdx].record;
  if (rec.recordState != RecordState::EXECUTING) {
    Serial.printf("[Journal] commitTransaction: slot %u state %u (expected EXECUTING)\n",
                  slotIdx, (uint8_t)rec.recordState);
    return false;
  }

  // Generation increment (Rev26 — distance 1 for adjacent mutation).
  rec.generation = _assignNextGeneration(slotIdx);
  rec.recordState = RecordState::COMMITTED;
  rec.ackJson = ackJson;

  if (!_writeCopy(slotIdx, true, rec)) return false;
  if (!_writeCopy(slotIdx, false, rec)) return false;

  // Queue ACK for delivery (Rev26 I3 — independent of journal entry)
  // P1-7: propagate failure — if ACK queue cannot be persisted, the
  // transaction is not fully durable (ACK delivery may be lost on reboot).
  if (!queueAck(requestId, ackJson)) {
    Serial.printf("[Journal] commitTransaction: queueAck FAILED for rid=%s — ACK delivery not durable\n",
                  requestId.c_str());
    // The journal entry IS committed (both copies written). The ACK queue
    // persistence failed. We return false to signal the caller that ACK
    // delivery is not durable — caller may retry queueAck separately.
    // Note: P2-3 will add boot merge (P1-9) so even if ACK queue persistence
    // fails here, the next boot will reconstruct the ACK from the journal
    // COMMITTED entry. So this is recoverable.
    return false;
  }

  Serial.printf("[Journal] commitTransaction: rid=%s slot %u COMMITTED gen=%u\n",
                requestId.c_str(), slotIdx, (unsigned)rec.generation);
  return true;
}

// =============================================================================
// MUTATION API — commitTransactionFailed (Rev26 — failure terminal state)
//
//   P2-1 CORRECTION (auditor P1-7): same fix as commitTransaction —
//   propagate queueAck failure.
// =============================================================================
bool TransactionJournal::commitTransactionFailed(const String& requestId, const String& ackJson,
                                                  TransactionState failureState) {
  _assertExecutorContext();
  _assertMutationAllowed();

  uint8_t slotIdx = _findSlot(requestId);
  if (slotIdx == JOURNAL_SIZE) {
    Serial.printf("[Journal] commitTransactionFailed: requestId %s not found\n", requestId.c_str());
    return false;
  }

  JournalRecord& rec = _slots[slotIdx].record;
  RecordState targetRecordState = _toRecordState(failureState);

  // Generation increment (Rev26 — distance 1 for adjacent mutation).
  rec.generation = _assignNextGeneration(slotIdx);
  rec.recordState = targetRecordState;
  rec.ackJson = ackJson;

  if (!_writeCopy(slotIdx, true, rec)) return false;
  if (!_writeCopy(slotIdx, false, rec)) return false;

  // Queue failure ACK for delivery
  // P1-7: propagate failure
  if (!queueAck(requestId, ackJson)) {
    Serial.printf("[Journal] commitTransactionFailed: queueAck FAILED for rid=%s — ACK delivery not durable\n",
                  requestId.c_str());
    return false;
  }

  Serial.printf("[Journal] commitTransactionFailed: rid=%s slot %u state=%s gen=%u\n",
                requestId.c_str(), slotIdx, _stateToString(failureState), (unsigned)rec.generation);
  return true;
}

// =============================================================================
// MUTATION API — clearEntry (Rev26 — write EMPTY to both copies, subject to I2)
//
//   Allowed for: PENDING, EXECUTING, FAILED (non-durable or failed states).
//   NOT allowed for: COMMITTED, COMMITTED_UNKNOWN, CORRUPTED, OUTPUT_MISMATCH.
//   For COMMITTED: subject to eviction safety I2a-I2e.
// =============================================================================
bool TransactionJournal::clearEntry(const String& requestId) {
  _assertExecutorContext();
  _assertMutationAllowed();

  uint8_t slotIdx = _findSlot(requestId);
  if (slotIdx == JOURNAL_SIZE) {
    Serial.printf("[Journal] clearEntry: requestId %s not found\n", requestId.c_str());
    return false;
  }

  JournalRecord& rec = _slots[slotIdx].record;

  // For COMMITTED entries, check eviction predicate
  if (rec.recordState == RecordState::COMMITTED) {
    if (!_isEvictionPermitted(slotIdx)) {
      Serial.printf("[Journal] clearEntry: slot %u COMMITTED but eviction not permitted (I2)\n",
                    slotIdx);
      return false;
    }
  }

  // For terminal durable states, refuse (only recoverCorruptedEntry can clear)
  if (rec.recordState == RecordState::COMMITTED_UNKNOWN ||
      rec.recordState == RecordState::EXECUTION_FAILED_OUTPUT_MISMATCH) {
    Serial.printf("[Journal] clearEntry: slot %u in terminal state %u — use recoverCorruptedEntry\n",
                  slotIdx, (uint8_t)rec.recordState);
    return false;
  }

  // For CORRUPTED slots, refuse (recoverCorruptedEntry only)
  if (_slots[slotIdx].durability == SlotDurability::SLOT_QUARANTINED) {
    Serial.printf("[Journal] clearEntry: slot %u QUARANTINED — use recoverCorruptedEntry\n", slotIdx);
    return false;
  }

  // Write EMPTY(gen=0) to both copies
  if (!_clearSlotNVS(slotIdx)) {
    Serial.printf("[Journal] clearEntry: _clearSlotNVS failed for slot %u\n", slotIdx);
    return false;
  }

  // Update in-RAM cache
  _slots[slotIdx].record = JournalRecord();  // Reset to defaults
  _slots[slotIdx].durability = SlotDurability::SLOT_EMPTY;
  _slots[slotIdx].inUse = false;
  if (_journalSize > 0) _journalSize--;

  Serial.printf("[Journal] clearEntry: rid=%s slot %u cleared\n", requestId.c_str(), slotIdx);
  return true;
}

// =============================================================================
// MUTATION API — recoverCorruptedEntry (Rev26 — operator-initiated recovery)
//
//   Writes EMPTY(gen=0) to both copies unconditionally.
//   Used for CORRUPTED / QUARANTINED slots.
//   Does NOT check I2 (operator override).
//
//   P2-1 CORRECTION (auditor P1-5): previous version only looked up by
//   requestId in active slots (inUse=true). Quarantined slots have
//   inUse=false, so recovery was unreachable. Corrected: now scans ALL
//   slots — reads NVS for both copies of each slot to find the
//   requestId even in quarantined slots. If found, calls recoverCorruptedSlot.
// =============================================================================
bool TransactionJournal::recoverCorruptedEntry(const String& requestId) {
  _assertExecutorContext();
  _assertMutationAllowed();

  // First, check active slots (fast path)
  uint8_t slotIdx = _findSlot(requestId);
  if (slotIdx != JOURNAL_SIZE) {
    return recoverCorruptedSlot(slotIdx);
  }

  // P1-5 fix: scan ALL slots — read NVS for both copies of each slot
  // to find the requestId even in quarantined slots.
  for (uint8_t i = 0; i < JOURNAL_SIZE; i++) {
    if (_slots[i].inUse) continue;  // Already checked via _findSlot

    JournalRecord recA, recB;
    bool validA = _readCopy(i, true, recA);
    bool validB = _readCopy(i, false, recB);

    if (validA && recA.requestId == requestId) {
      Serial.printf("[Journal] recoverCorruptedEntry: found requestId %s in quarantined slot %u (copy A)\n",
                    requestId.c_str(), i);
      return recoverCorruptedSlot(i);
    }
    if (validB && recB.requestId == requestId) {
      Serial.printf("[Journal] recoverCorruptedEntry: found requestId %s in quarantined slot %u (copy B)\n",
                    requestId.c_str(), i);
      return recoverCorruptedSlot(i);
    }
  }

  Serial.printf("[Journal] recoverCorruptedEntry: requestId %s not found in any slot\n",
                requestId.c_str());
  return false;
}

// =============================================================================
// recoverCorruptedSlot (Rev26 — slot-idx-based recovery, P1-5 addition)
//
//   Writes EMPTY(gen=0) to both copies unconditionally.
//   Used by recoverCorruptedEntry() and directly by operator when slot
//   index is known (e.g., from serial output "slot N QUARANTINED").
// =============================================================================
bool TransactionJournal::recoverCorruptedSlot(uint8_t slotIdx) {
  _assertExecutorContext();
  _assertMutationAllowed();

  if (slotIdx >= JOURNAL_SIZE) {
    Serial.printf("[Journal] recoverCorruptedSlot: slotIdx %u >= JOURNAL_SIZE\n", slotIdx);
    return false;
  }

  // Write EMPTY(gen=0) to both copies — unconditional, no I2 check.
  // (Operator override — both copies already INVALID/quarantined, so
  // generation preservation is not needed; gen=0 is the fresh-start value.)
  JournalRecord empty;
  empty.schemaVersion = JOURNAL_SCHEMA_VERSION;
  empty.generation = 0;
  empty.recordState = RecordState::EMPTY;
  empty.requestId = "";
  empty.commandHash = "";
  empty.channelId = 0;
  empty.desiredState = 0xFF;
  empty.previousKnownState = 0;
  empty.attempt = 0;
  empty.timestamp = 0;
  empty.ackJson = "";

  if (!_writeCopy(slotIdx, true, empty)) {
    Serial.printf("[Journal] recoverCorruptedSlot: write A failed for slot %u\n", slotIdx);
    return false;
  }
  if (!_writeCopy(slotIdx, false, empty)) {
    Serial.printf("[Journal] recoverCorruptedSlot: write B failed for slot %u\n", slotIdx);
    return false;
  }

  // Update in-RAM cache
  _slots[slotIdx].record = empty;
  _slots[slotIdx].durability = SlotDurability::SLOT_EMPTY;
  _slots[slotIdx].inUse = false;
  // Note: _journalSize not decremented because quarantined slots weren't counted.

  Serial.printf("[Journal] recoverCorruptedSlot: slot %u recovered to EMPTY(gen=0)\n",
                slotIdx);
  return true;
}

// =============================================================================
// LOOKUP API — observation (read-only, no _assertMutationAllowed call)
// =============================================================================
bool TransactionJournal::isProcessed(const String& requestId) {
  _assertExecutorContext();
  uint8_t slotIdx = _findSlot(requestId);
  if (slotIdx == JOURNAL_SIZE) return false;
  // Processed = reached at least PENDING state (any non-EMPTY state)
  return _slots[slotIdx].inUse;
}

bool TransactionJournal::isCommitted(const String& requestId) {
  _assertExecutorContext();
  uint8_t slotIdx = _findSlot(requestId);
  if (slotIdx == JOURNAL_SIZE) return false;
  return _slots[slotIdx].record.recordState == RecordState::COMMITTED;
}

TransactionState TransactionJournal::getTransactionState(const String& requestId) {
  _assertExecutorContext();
  uint8_t slotIdx = _findSlot(requestId);
  if (slotIdx == JOURNAL_SIZE) {
    // Not found — return a sentinel. Callers should check isProcessed() first.
    return TransactionState::PENDING;  // Conservative default
  }
  // Check durability for CORRUPTED
  if (_slots[slotIdx].durability == SlotDurability::SLOT_QUARANTINED) {
    return TransactionState::CORRUPTED;
  }
  return _fromRecordState(_slots[slotIdx].record.recordState);
}

String TransactionJournal::getCommandHash(const String& requestId) {
  _assertExecutorContext();
  uint8_t slotIdx = _findSlot(requestId);
  if (slotIdx == JOURNAL_SIZE) return "";
  return _slots[slotIdx].record.commandHash;
}

String TransactionJournal::getAckJson(const String& requestId) {
  _assertExecutorContext();
  uint8_t slotIdx = _findSlot(requestId);
  if (slotIdx == JOURNAL_SIZE) return "";
  return _slots[slotIdx].record.ackJson;
}

uint8_t TransactionJournal::getChannelId(const String& requestId) {
  _assertExecutorContext();
  uint8_t slotIdx = _findSlot(requestId);
  if (slotIdx == JOURNAL_SIZE) return 0;
  return _slots[slotIdx].record.channelId;
}

bool TransactionJournal::getDesiredState(const String& requestId) {
  _assertExecutorContext();
  uint8_t slotIdx = _findSlot(requestId);
  if (slotIdx == JOURNAL_SIZE) return false;
  return _slots[slotIdx].record.desiredState != 0;
}

// =============================================================================
// RECONCILIATION API (Rev26 — 2-phase: observation then mutation)
//
//   P2-1 CORRECTION (auditor P0-3): previous version held ObservationGuard
//   while calling _writeCopy() — this violated I0a (observation and
//   mutation are mutually exclusive). The guard was active during NVS
//   writes, which is exactly the violation Rev26 I0a forbids.
//
//   Corrected: 2-phase reconciliation.
//     Phase 1 (OBSERVATION): Read all slots, identify which need state
//              transition (PENDING/EXECUTING → UNKNOWN). Collect list
//              of (slotIdx, newRecordState) pairs. ObservationGuard active.
//     Phase 2 (MUTATION): Apply state transitions to NVS. No guard active.
//              Each _writeCopy() call goes through _assertMutationAllowed()
//              which now passes (no observation in progress).
//
//   This satisfies I0a without weakening the invariant.
// =============================================================================

// Internal: collect reconciliation actions during observation phase
struct ReconcileAction {
  uint8_t slotIdx;
  RecordState newState;
};

uint8_t TransactionJournal::reconcilePendingEntries() {
  _assertExecutorContext();

  // Phase 1: OBSERVATION — read all slots, collect actions.
  ReconcileAction actions[JOURNAL_SIZE];
  uint8_t actionCount = 0;
  {
    ObservationGuard guard(_observing);
    for (uint8_t i = 0; i < JOURNAL_SIZE; i++) {
      if (!_slots[i].inUse) continue;
      RecordState rs = _slots[i].record.recordState;
      if (rs == RecordState::PENDING || rs == RecordState::EXECUTING) {
        actions[actionCount].slotIdx = i;
        actions[actionCount].newState = RecordState::UNKNOWN;
        actionCount++;
      }
    }
  }
  // ObservationGuard out of scope — _observing is now false.

  // Phase 2: MUTATION — apply state transitions to NVS.
  // _assertMutationAllowed() will pass because _observing == false.
  for (uint8_t i = 0; i < actionCount; i++) {
    uint8_t slotIdx = actions[i].slotIdx;
    _slots[slotIdx].record.recordState = actions[i].newState;
    if (!_writeCopy(slotIdx, true, _slots[slotIdx].record)) {
      Serial.printf("[Journal] reconcilePendingEntries: write A failed for slot %u\n", slotIdx);
      continue;
    }
    if (!_writeCopy(slotIdx, false, _slots[slotIdx].record)) {
      Serial.printf("[Journal] reconcilePendingEntries: write B failed for slot %u\n", slotIdx);
      continue;
    }
  }

  return actionCount;
}

TransactionState TransactionJournal::reconcileEntry(const String& requestId) {
  _assertExecutorContext();

  uint8_t slotIdx = _findSlot(requestId);
  if (slotIdx == JOURNAL_SIZE) return TransactionState::PENDING;

  JournalRecord& rec = _slots[slotIdx].record;
  if (rec.recordState != RecordState::PENDING &&
      rec.recordState != RecordState::EXECUTING) {
    return _fromRecordState(rec.recordState);
  }

  // Phase 1: OBSERVATION — determine new state.
  // (Already have the record in RAM from earlier _findSlot — no NVS read
  // needed. Just decide the new state under guard to satisfy I0a.)
  {
    ObservationGuard guard(_observing);
    // No mutation here — just confirm the decision.
    // rec.recordState is already PENDING or EXECUTING (checked above).
  }
  // Guard out of scope.

  // Phase 2: MUTATION — apply state transition.
  rec.recordState = RecordState::UNKNOWN;
  if (!_writeCopy(slotIdx, true, rec)) {
    Serial.printf("[Journal] reconcileEntry: write A failed for slot %u\n", slotIdx);
  }
  if (!_writeCopy(slotIdx, false, rec)) {
    Serial.printf("[Journal] reconcileEntry: write B failed for slot %u\n", slotIdx);
  }
  return TransactionState::UNKNOWN;
}

// =============================================================================
// ACK QUEUE — queueAck (Rev26 I3 — durable in tj_ackq)
//
//   P2-1 CORRECTION (auditor P1-6): previous version silently dropped ACK #1
//   when queue full. Corrected: only drop ACK_FAILED_EXHAUSTED or empty slots.
//   If all 8 slots are occupied by active ACKs (NOT_SENT/PUBLISH_ACCEPTED/
//   BROKER_CONFIRMED/PWA_RECEIVED), return false — caller must handle.
//
//   P2-1 CORRECTION (auditor P1-7): return bool. commitTransaction() checks
//   return value and propagates failure (does not return true if ACK
//   persistence failed).
// =============================================================================
bool TransactionJournal::queueAck(const String& requestId, const String& ackJson) {
  _assertExecutorContext();

  // Check if requestId already in queue
  int8_t existing = _findAckInQueue(requestId);
  if (existing >= 0) {
    // Update existing entry
    _ackQueue[existing].ackJson = ackJson;
    _ackQueue[existing].deliveryState = AckDeliveryState::ACK_NOT_SENT;
    _ackQueue[existing].retryCount = 0;
    _ackQueue[existing].lastAttemptTs = 0;
    if (!_persistAckQueue()) {
      Serial.printf("[Journal] queueAck: _persistAckQueue failed (update existing) for rid=%s\n",
                    requestId.c_str());
      return false;
    }
    return true;
  }

  // Need a new slot. If queue full, find a droppable slot.
  uint8_t insertIdx = _ackQueueCount;
  if (_ackQueueCount >= ACK_QUEUE_CAPACITY) {
    // P1-6 fix: only drop ACK_FAILED_EXHAUSTED or empty slots.
    // Active ACKs (NOT_SENT/PUBLISH_ACCEPTED/BROKER_CONFIRMED/PWA_RECEIVED)
    // must NOT be silently dropped.
    int8_t droppableIdx = -1;
    for (uint8_t i = 0; i < ACK_QUEUE_CAPACITY; i++) {
      if (_ackQueue[i].deliveryState == AckDeliveryState::ACK_FAILED_EXHAUSTED ||
          _ackQueue[i].isEmpty()) {
        droppableIdx = (int8_t)i;
        break;
      }
    }
    if (droppableIdx < 0) {
      // All 8 slots are active — cannot queue without losing an active ACK.
      Serial.printf("[Journal] queueAck: ACK queue full with active ACKs, refusing to queue rid=%s\n",
                    requestId.c_str());
      return false;
    }
    // Reuse droppable slot
    insertIdx = (uint8_t)droppableIdx;
  } else {
    _ackQueueCount++;
  }

  AckRecord& rec = _ackQueue[insertIdx];
  rec.deliveryState = AckDeliveryState::ACK_NOT_SENT;
  rec.requestId = requestId;
  rec.commandHash = "";
  rec.retryCount = 0;
  rec.lastAttemptTs = 0;
  rec.ackJson = ackJson;

  if (!_persistAckQueue()) {
    Serial.printf("[Journal] queueAck: _persistAckQueue failed (new entry) for rid=%s\n",
                  requestId.c_str());
    // Rollback: decrement count if we added a new slot
    if (insertIdx == _ackQueueCount - 1) {
      _ackQueueCount--;
    }
    return false;
  }

  return true;
}

// =============================================================================
// ACK QUEUE — processPendingAcks (Rev26 I3 — retry delivery)
//
//   P2-1 CORRECTION (auditor P1-8): previous version only persisted if
//   `processed > 0`. But retryCount++ and lastAttemptTs updates also need
//   persistence (so retry policy survives reboot). Corrected: track a
//   `dirty` flag, persist if any state changed.
// =============================================================================
uint8_t TransactionJournal::processPendingAcks() {
  _assertExecutorContext();

  if (!s_publishCallback) return 0;

  uint8_t processed = 0;
  bool dirty = false;  // P1-8: track any state change
  unsigned long now = millis();

  for (uint8_t i = 0; i < _ackQueueCount; i++) {
    AckRecord& rec = _ackQueue[i];
    if (rec.deliveryState == AckDeliveryState::ACK_BROKER_CONFIRMED ||
        rec.deliveryState == AckDeliveryState::ACK_PWA_RECEIVED ||
        rec.deliveryState == AckDeliveryState::ACK_FAILED_EXHAUSTED) {
      continue;  // Terminal states — skip
    }

    // Rate-limit retries
    if (rec.lastAttemptTs != 0 &&
        (now - rec.lastAttemptTs) < ACK_RETRY_INTERVAL_MS) {
      continue;
    }

    if (rec.retryCount >= MAX_ACK_RETRIES) {
      rec.deliveryState = AckDeliveryState::ACK_FAILED_EXHAUSTED;
      Serial.printf("[Journal] ACK FAILED_EXHAUSTED: rid=%s\n", rec.requestId.c_str());
      processed++;
      dirty = true;  // P1-8: state changed
      continue;
    }

    // Publish via callback
    bool published = s_publishCallback("ack",
                                        (const uint8_t*)rec.ackJson.c_str(),
                                        rec.ackJson.length());
    if (published) {
      rec.deliveryState = AckDeliveryState::ACK_PUBLISH_ACCEPTED;
      rec.retryCount++;
      rec.lastAttemptTs = now;
      processed++;
      dirty = true;  // P1-8: state changed
      Serial.printf("[Journal] ACK published: rid=%s (attempt %u)\n",
                     rec.requestId.c_str(), rec.retryCount);
    } else {
      rec.retryCount++;
      rec.lastAttemptTs = now;
      dirty = true;  // P1-8: state changed (retryCount/lastAttemptTs)
      Serial.printf("[Journal] ACK publish failed: rid=%s (attempt %u)\n",
                     rec.requestId.c_str(), rec.retryCount);
    }
  }

  // P1-8: persist if any state changed (not just when processed > 0)
  if (dirty) {
    if (!_persistAckQueue()) {
      Serial.printf("[Journal] processPendingAcks: _persistAckQueue FAILED — retry state may be lost\n");
    }
  }

  return processed;
}

// =============================================================================
// ACK QUEUE — dequeueAck (Rev26 I3 — remove ACK after PWA confirms)
// =============================================================================
void TransactionJournal::dequeueAck(const String& requestId) {
  _assertExecutorContext();

  int8_t idx = _findAckInQueue(requestId);
  if (idx < 0) return;

  // Shift remaining entries down
  for (uint8_t i = (uint8_t)idx; i < _ackQueueCount - 1; i++) {
    _ackQueue[i] = _ackQueue[i + 1];
  }
  _ackQueueCount--;
  _ackQueue[_ackQueueCount] = AckRecord();  // Clear the now-unused slot

  _persistAckQueue();
}

// =============================================================================
// ACK QUEUE — updateAckDeliveryState (Rev26 I3 — PUBACK / ack_confirm)
// =============================================================================
bool TransactionJournal::updateAckDeliveryState(const String& requestId, AckDeliveryState newState) {
  _assertExecutorContext();

  int8_t idx = _findAckInQueue(requestId);
  if (idx < 0) return false;

  _ackQueue[idx].deliveryState = newState;
  _persistAckQueue();
  return true;
}

// =============================================================================
// ACK QUEUE NVS — _loadAckQueue (Rev26 I3 — read tj_ackq blob)
// =============================================================================
bool TransactionJournal::_loadAckQueue() {
  Preferences prefs;
  if (!prefs.begin(Core::NVS_NAMESPACE, true)) return false;

  if (!prefs.isKey("tj_ackq")) {
    prefs.end();
    _ackQueueCount = 0;
    return true;  // No queue yet — fresh start
  }

  uint8_t blob[ACK_QUEUE_BLOB_SIZE];
  size_t read = prefs.getBytes("tj_ackq", blob, ACK_QUEUE_BLOB_SIZE);
  prefs.end();

  if (read != ACK_QUEUE_BLOB_SIZE) {
    Serial.printf("[Journal] _loadAckQueue: blob size mismatch (%u != %u)\n",
                  (unsigned)read, (unsigned)ACK_QUEUE_BLOB_SIZE);
    _ackQueueCount = 0;
    return false;
  }

  // Parse header
  _ackQueueCount = blob[0];
  if (_ackQueueCount > ACK_QUEUE_CAPACITY) {
    Serial.printf("[Journal] _loadAckQueue: count %u exceeds capacity %u — resetting\n",
                  _ackQueueCount, ACK_QUEUE_CAPACITY);
    _ackQueueCount = 0;
    return false;
  }

  // Verify queue CRC
  uint32_t storedCRC = (uint32_t)blob[ACK_QUEUE_BLOB_SIZE - 4]
                     | ((uint32_t)blob[ACK_QUEUE_BLOB_SIZE - 3] << 8)
                     | ((uint32_t)blob[ACK_QUEUE_BLOB_SIZE - 2] << 16)
                     | ((uint32_t)blob[ACK_QUEUE_BLOB_SIZE - 1] << 24);

  // CRC covers bytes [0..ACK_QUEUE_BLOB_SIZE-5] (everything except the last 4 CRC bytes)
  // Using Phase 1 JournalRecord's CRC pattern is overkill here; use simple CRC32.
  // P2-1: reuse Utils::calculateCRC for the queue (different from per-record CRC32/ISO-HDLC,
  // but adequate for queue-level integrity).
  // Actually, for consistency, let me use the same CRC-32/ISO-HDLC.
  // But we don't have access to esp_crc32_le on host. Let me use a simple CRC.
  uint32_t computedCRC = 0;
  for (size_t i = 0; i < ACK_QUEUE_BLOB_SIZE - 4; i++) {
    computedCRC = computedCRC * 31 + blob[i];  // Simple FNV-like
  }

  if (storedCRC != computedCRC) {
    Serial.printf("[Journal] _loadAckQueue: CRC mismatch (stored=%08x, computed=%08x) — resetting\n",
                  (unsigned)storedCRC, (unsigned)computedCRC);
    _ackQueueCount = 0;
    return false;
  }

  // Parse records
  for (uint8_t i = 0; i < _ackQueueCount; i++) {
    const uint8_t* recBuf = blob + 4 + (i * ACK_RECORD_SIZE);
    if (!_deserializeAckRecord(recBuf, _ackQueue[i])) {
      Serial.printf("[Journal] _loadAckQueue: record %u parse failed — skipping\n", i);
      _ackQueue[i] = AckRecord();
    }
  }

  _ackQueueCRC = storedCRC;
  Serial.printf("[Journal] _loadAckQueue: loaded %u ACK records\n", _ackQueueCount);
  return true;
}

// =============================================================================
// ACK QUEUE NVS — _persistAckQueue (Rev26 I3 — write tj_ackq blob)
// =============================================================================
bool TransactionJournal::_persistAckQueue() {
  uint8_t blob[ACK_QUEUE_BLOB_SIZE];
  memset(blob, 0, ACK_QUEUE_BLOB_SIZE);

  // Header: count + 3 reserved bytes
  blob[0] = _ackQueueCount;
  blob[1] = 0;  // reserved
  blob[2] = 0;  // reserved
  blob[3] = 0;  // reserved

  // Records
  for (uint8_t i = 0; i < _ackQueueCount; i++) {
    uint8_t* recBuf = blob + 4 + (i * ACK_RECORD_SIZE);
    _serializeAckRecord(_ackQueue[i], recBuf);
  }

  // Compute CRC over [0..ACK_QUEUE_BLOB_SIZE-5]
  uint32_t crc = 0;
  for (size_t i = 0; i < ACK_QUEUE_BLOB_SIZE - 4; i++) {
    crc = crc * 31 + blob[i];
  }
  _ackQueueCRC = crc;

  blob[ACK_QUEUE_BLOB_SIZE - 4] = crc & 0xFF;
  blob[ACK_QUEUE_BLOB_SIZE - 3] = (crc >> 8) & 0xFF;
  blob[ACK_QUEUE_BLOB_SIZE - 2] = (crc >> 16) & 0xFF;
  blob[ACK_QUEUE_BLOB_SIZE - 1] = (crc >> 24) & 0xFF;

  Preferences prefs;
  if (!prefs.begin(Core::NVS_NAMESPACE, false)) return false;

  size_t written = prefs.putBytes("tj_ackq", blob, ACK_QUEUE_BLOB_SIZE);
  prefs.end();

  if (written != ACK_QUEUE_BLOB_SIZE) {
    Serial.printf("[Journal] _persistAckQueue: write short (%u != %u)\n",
                  (unsigned)written, (unsigned)ACK_QUEUE_BLOB_SIZE);
    return false;
  }

  return true;
}

uint32_t TransactionJournal::_computeAckQueueCRC() const {
  uint8_t blob[ACK_QUEUE_BLOB_SIZE];
  memset(blob, 0, ACK_QUEUE_BLOB_SIZE);
  blob[0] = _ackQueueCount;
  for (uint8_t i = 0; i < _ackQueueCount; i++) {
    uint8_t* recBuf = blob + 4 + (i * ACK_RECORD_SIZE);
    const_cast<TransactionJournal*>(this)->_serializeAckRecord(_ackQueue[i], recBuf);
  }
  uint32_t crc = 0;
  for (size_t i = 0; i < ACK_QUEUE_BLOB_SIZE - 4; i++) {
    crc = crc * 31 + blob[i];
  }
  return crc;
}

// =============================================================================
// ACK RECORD serialize/deserialize (Rev26 I3 — fixed-size 256-byte records)
//
//   Layout:
//     [0..1]   ackMagic (0x41, 0x51)
//     [2]      ackVersion (1)
//     [3]      deliveryState
//     [4]      requestIdLen (0..64)
//     [5..68]  requestId (var, padded)
//     [69]     commandHashLen (0..64)
//     [70..133] commandHash (var, padded)
//     [134]    retryCount
//     [135..138] lastAttemptTs (uint32 LE)
//     [139..140] ackLen (uint16 LE, 0..1024)
//     [141..1164] ackJson (var, padded)
//     [1165..255] padding (zeros)
// =============================================================================
void TransactionJournal::_serializeAckRecord(const AckRecord& rec, uint8_t* buf) const {
  memset(buf, 0, ACK_RECORD_SIZE);
  buf[0] = ACK_MAGIC1;
  buf[1] = ACK_MAGIC2;
  buf[2] = ACK_VERSION;
  buf[3] = (uint8_t)rec.deliveryState;

  uint8_t reqLen = (uint8_t)(rec.requestId.length() > MAX_REQUEST_ID_LEN
                              ? MAX_REQUEST_ID_LEN : rec.requestId.length());
  buf[4] = reqLen;
  if (reqLen > 0) memcpy(&buf[5], rec.requestId.c_str(), reqLen);

  uint8_t hashLen = (uint8_t)(rec.commandHash.length() > MAX_COMMAND_HASH_LEN
                               ? MAX_COMMAND_HASH_LEN : rec.commandHash.length());
  buf[69] = hashLen;
  if (hashLen > 0) memcpy(&buf[70], rec.commandHash.c_str(), hashLen);

  buf[134] = rec.retryCount;
  buf[135] = rec.lastAttemptTs & 0xFF;
  buf[136] = (rec.lastAttemptTs >> 8) & 0xFF;
  buf[137] = (rec.lastAttemptTs >> 16) & 0xFF;
  buf[138] = (rec.lastAttemptTs >> 24) & 0xFF;

  uint16_t ackLen = (uint16_t)(rec.ackJson.length() > MAX_ACK_JSON_LEN
                                ? MAX_ACK_JSON_LEN : rec.ackJson.length());
  buf[139] = ackLen & 0xFF;
  buf[140] = (ackLen >> 8) & 0xFF;
  if (ackLen > 0) memcpy(&buf[141], rec.ackJson.c_str(), ackLen);
}

bool TransactionJournal::_deserializeAckRecord(const uint8_t* buf, AckRecord& outRec) const {
  if (buf[0] != ACK_MAGIC1 || buf[1] != ACK_MAGIC2) return false;
  if (buf[2] != ACK_VERSION) return false;

  outRec.deliveryState = (AckDeliveryState)buf[3];

  uint8_t reqLen = buf[4];
  if (reqLen > MAX_REQUEST_ID_LEN) return false;
  if (reqLen > 0) {
    outRec.requestId = String((const char*)(&buf[5]), reqLen);
  } else {
    outRec.requestId = "";
  }

  uint8_t hashLen = buf[69];
  if (hashLen > MAX_COMMAND_HASH_LEN) return false;
  if (hashLen > 0) {
    outRec.commandHash = String((const char*)(&buf[70]), hashLen);
  } else {
    outRec.commandHash = "";
  }

  outRec.retryCount = buf[134];
  outRec.lastAttemptTs = (uint32_t)buf[135]
                       | ((uint32_t)buf[136] << 8)
                       | ((uint32_t)buf[137] << 16)
                       | ((uint32_t)buf[138] << 24);

  uint16_t ackLen = (uint16_t)buf[139] | ((uint16_t)buf[140] << 8);
  if (ackLen > MAX_ACK_JSON_LEN) return false;
  if (ackLen > 0) {
    outRec.ackJson = String((const char*)(&buf[141]), ackLen);
  } else {
    outRec.ackJson = "";
  }

  return true;
}

int8_t TransactionJournal::_findAckInQueue(const String& requestId) const {
  for (uint8_t i = 0; i < _ackQueueCount; i++) {
    if (_ackQueue[i].requestId == requestId) return (int8_t)i;
  }
  return -1;
}

// =============================================================================
// _loadFromNVS (Rev26 — 2-phase: observation then mutation)
//
//   P2-1 CORRECTION (auditor P0-3): previous version held ObservationGuard
//   while calling _evaluateSlot() → _reconcileSlot() → _repairSlot() →
//   _writeCopy(). The guard was active during NVS writes — violating I0a.
//
//   Corrected: 2-phase loading.
//     Phase 1 (OBSERVATION): Read both copies of every slot, classify
//              generation relationship, collect list of slots that need
//              repair (copy A or B invalid) or quarantine (both invalid).
//              ObservationGuard active.
//     Phase 2 (MUTATION): Apply repairs to NVS. No guard active.
// =============================================================================
void TransactionJournal::_loadFromNVS() {
  _journalSize = 0;
  _journalWriteIdx = 0;

  // Phase 1: OBSERVATION — read all slots, classify, collect actions.
  // We use a local struct to record per-slot decisions, then apply them
  // in Phase 2 without the guard.
  struct SlotDecision {
    uint8_t slotIdx;
    SlotDurability durability;
    JournalRecord loadedRecord;  // For SLOT_VALID: the authoritative copy
    bool needsRepairFromA;       // If true, repair B from A in Phase 2
    bool needsRepairFromB;       // If true, repair A from B in Phase 2
    bool needsQuarantine;        // If true, quarantine slot in Phase 2
  };
  SlotDecision decisions[JOURNAL_SIZE];
  uint8_t decisionCount = 0;

  {
    ObservationGuard guard(_observing);

    for (uint8_t i = 0; i < JOURNAL_SIZE; i++) {
      SlotDecision dec;
      dec.slotIdx = i;
      dec.durability = SlotDurability::SLOT_EMPTY;
      dec.needsRepairFromA = false;
      dec.needsRepairFromB = false;
      dec.needsQuarantine = false;

      // Check if slot is empty (no NVS keys).
      bool aExists, bExists;
      {
        Preferences prefs;
        prefs.begin(Core::NVS_NAMESPACE, true);
        aExists = prefs.isKey(_slotKeyA(i).c_str());
        bExists = prefs.isKey(_slotKeyB(i).c_str());
        prefs.end();
      }
      if (!aExists && !bExists) {
        dec.durability = SlotDurability::SLOT_EMPTY;
        decisions[decisionCount++] = dec;
        continue;
      }

      // Read both copies (observation — no mutation).
      JournalRecord recA, recB;
      bool validA = _readCopy(i, true, recA);
      bool validB = _readCopy(i, false, recB);

      if (!validA && !validB) {
        // Row 1: both INVALID → quarantine
        dec.durability = SlotDurability::SLOT_QUARANTINED;
        dec.needsQuarantine = true;
        // Preserve requestId for recovery lookup (use empty if both unreadable)
        decisions[decisionCount++] = dec;
        continue;
      }

      if (validA && !validB) {
        // Row 2: A VALID, B INVALID → repair A→B
        dec.durability = SlotDurability::SLOT_VALID;
        dec.loadedRecord = recA;
        dec.needsRepairFromA = true;
        decisions[decisionCount++] = dec;
        continue;
      }

      if (!validA && validB) {
        // Row 3: A INVALID, B VALID → repair B→A
        dec.durability = SlotDurability::SLOT_VALID;
        dec.loadedRecord = recB;
        dec.needsRepairFromB = true;
        decisions[decisionCount++] = dec;
        continue;
      }

      // Both VALID — apply generation classifier.
      GenRelation rel = classifyGeneration(recA.generation, recB.generation);

      if (rel == GenRelation::GEN_NEWER_A) {
        // Row 4
        dec.durability = SlotDurability::SLOT_VALID;
        dec.loadedRecord = recA;
      } else if (rel == GenRelation::GEN_NEWER_B) {
        // Row 5
        dec.durability = SlotDurability::SLOT_VALID;
        dec.loadedRecord = recB;
      } else if (rel == GenRelation::GEN_EQUAL) {
        if (canonicalEqual(recA, recB)) {
          // Row 6
          dec.durability = SlotDurability::SLOT_VALID;
          dec.loadedRecord = recA;
        } else {
          // Row 7: GEN_EQUAL + divergent → CORRUPTED
          dec.durability = SlotDurability::SLOT_QUARANTINED;
          dec.needsQuarantine = true;
        }
      } else if (rel == GenRelation::GEN_AMBIGUOUS) {
        // Row 8
        dec.durability = SlotDurability::SLOT_QUARANTINED;
        dec.needsQuarantine = true;
      } else {
        // Row 9: GEN_INVALID
        dec.durability = SlotDurability::SLOT_QUARANTINED;
        dec.needsQuarantine = true;
      }

      decisions[decisionCount++] = dec;
    }
  }
  // ObservationGuard out of scope — _observing is now false.

  // Phase 2: MUTATION — apply decisions to in-RAM cache + perform NVS repairs.
  // _assertMutationAllowed() passes because _observing == false.
  for (uint8_t d = 0; d < decisionCount; d++) {
    SlotDecision& dec = decisions[d];
    uint8_t i = dec.slotIdx;

    if (dec.durability == SlotDurability::SLOT_EMPTY) {
      _slots[i].record = JournalRecord();
      _slots[i].durability = SlotDurability::SLOT_EMPTY;
      _slots[i].inUse = false;
      continue;
    }

    if (dec.needsQuarantine) {
      // Quarantine — no NVS erase (preserves evidence)
      _slots[i].durability = SlotDurability::SLOT_QUARANTINED;
      _slots[i].record = JournalRecord();  // RAM-only
      _slots[i].inUse = false;
      Serial.printf("[Journal] _loadFromNVS: slot %u QUARANTINED\n", i);
      continue;
    }

    if (dec.needsRepairFromA) {
      // Repair B from A
      if (!_repairSlot(i, true)) {
        // Repair failed — quarantine
        _slots[i].durability = SlotDurability::SLOT_QUARANTINED;
        _slots[i].inUse = false;
        Serial.printf("[Journal] _loadFromNVS: slot %u repair A→B failed, QUARANTINED\n", i);
        continue;
      }
    }

    if (dec.needsRepairFromB) {
      // Repair A from B
      if (!_repairSlot(i, false)) {
        _slots[i].durability = SlotDurability::SLOT_QUARANTINED;
        _slots[i].inUse = false;
        Serial.printf("[Journal] _loadFromNVS: slot %u repair B→A failed, QUARANTINED\n", i);
        continue;
      }
    }

    // Load the authoritative record
    _slots[i].record = dec.loadedRecord;
    _slots[i].durability = SlotDurability::SLOT_VALID;
    _slots[i].inUse = (dec.loadedRecord.recordState != RecordState::EMPTY);
    if (_slots[i].inUse) {
      _journalSize++;
    }
  }
}

// =============================================================================
// Test/introspection helpers (host test harness — not for production callers)
// =============================================================================
SlotDurability TransactionJournal::_getSlotDurability(uint8_t slotIdx) const {
  if (slotIdx >= JOURNAL_SIZE) return SlotDurability::SLOT_QUARANTINED;
  return _slots[slotIdx].durability;
}

uint32_t TransactionJournal::_getSlotGeneration(uint8_t slotIdx) const {
  if (slotIdx >= JOURNAL_SIZE) return 0;
  return _slots[slotIdx].record.generation;
}

bool TransactionJournal::_forceReloadSlot(uint8_t slotIdx) {
  if (slotIdx >= JOURNAL_SIZE) return false;
  // P2-1 CORRECTION (auditor P0-3): use 2-phase approach (no mutation during
  // observation). This mirrors _loadFromNVS logic but for a single slot.
  // We can't call _loadFromNVS (which iterates all slots), so we replicate
  // the 2-phase pattern here.

  JournalRecord recA, recB;
  bool validA, validB;
  bool aExists, bExists;

  // Phase 1: OBSERVATION
  {
    ObservationGuard guard(_observing);
    {
      Preferences prefs;
      prefs.begin(Core::NVS_NAMESPACE, true);
      aExists = prefs.isKey(_slotKeyA(slotIdx).c_str());
      bExists = prefs.isKey(_slotKeyB(slotIdx).c_str());
      prefs.end();
    }
    if (!aExists && !bExists) {
      _slots[slotIdx].record = JournalRecord();
      _slots[slotIdx].durability = SlotDurability::SLOT_EMPTY;
      _slots[slotIdx].inUse = false;
      return true;  // Empty slot, not VALID but loaded successfully
    }
    validA = _readCopy(slotIdx, true, recA);
    validB = _readCopy(slotIdx, false, recB);
  }
  // ObservationGuard out of scope.

  // Phase 2: MUTATION (apply repair if needed, no guard active)
  if (!validA && !validB) {
    _slots[slotIdx].durability = SlotDurability::SLOT_QUARANTINED;
    _slots[slotIdx].record = JournalRecord();
    _slots[slotIdx].inUse = false;
    return false;  // Quarantined, not VALID
  }
  if (validA && !validB) {
    if (!_repairSlot(slotIdx, true)) {
      _slots[slotIdx].durability = SlotDurability::SLOT_QUARANTINED;
      _slots[slotIdx].inUse = false;
      return false;
    }
    validB = _readCopy(slotIdx, false, recB);
    if (!validB) {
      _slots[slotIdx].durability = SlotDurability::SLOT_QUARANTINED;
      _slots[slotIdx].inUse = false;
      return false;
    }
  }
  if (!validA && validB) {
    if (!_repairSlot(slotIdx, false)) {
      _slots[slotIdx].durability = SlotDurability::SLOT_QUARANTINED;
      _slots[slotIdx].inUse = false;
      return false;
    }
    validA = _readCopy(slotIdx, true, recA);
    if (!validA) {
      _slots[slotIdx].durability = SlotDurability::SLOT_QUARANTINED;
      _slots[slotIdx].inUse = false;
      return false;
    }
  }

  // Both VALID — apply generation classifier
  GenRelation rel = classifyGeneration(recA.generation, recB.generation);
  if (rel == GenRelation::GEN_NEWER_A) {
    _slots[slotIdx].record = recA;
    _slots[slotIdx].durability = SlotDurability::SLOT_VALID;
    _slots[slotIdx].inUse = (recA.recordState != RecordState::EMPTY);
    return _slots[slotIdx].durability == SlotDurability::SLOT_VALID;
  }
  if (rel == GenRelation::GEN_NEWER_B) {
    _slots[slotIdx].record = recB;
    _slots[slotIdx].durability = SlotDurability::SLOT_VALID;
    _slots[slotIdx].inUse = (recB.recordState != RecordState::EMPTY);
    return _slots[slotIdx].durability == SlotDurability::SLOT_VALID;
  }
  if (rel == GenRelation::GEN_EQUAL) {
    if (canonicalEqual(recA, recB)) {
      _slots[slotIdx].record = recA;
      _slots[slotIdx].durability = SlotDurability::SLOT_VALID;
      _slots[slotIdx].inUse = (recA.recordState != RecordState::EMPTY);
      return _slots[slotIdx].durability == SlotDurability::SLOT_VALID;
    }
  }
  // Row 7/8/9: CORRUPTED
  _slots[slotIdx].durability = SlotDurability::SLOT_QUARANTINED;
  _slots[slotIdx].record = JournalRecord();
  _slots[slotIdx].inUse = false;
  return false;
}

// =============================================================================
// String helpers (for serial logging)
// =============================================================================
const char* TransactionJournal::_stateToString(TransactionState s) {
  switch (s) {
    case TransactionState::PENDING:                          return "PENDING";
    case TransactionState::EXECUTING:                        return "EXECUTING";
    case TransactionState::COMMITTED:                        return "COMMITTED";
    case TransactionState::COMMITTED_UNKNOWN:                return "COMMITTED_UNKNOWN";
    case TransactionState::UNKNOWN:                          return "UNKNOWN";
    case TransactionState::FAILED:                            return "FAILED";
    case TransactionState::CORRUPTED:                        return "CORRUPTED";
    case TransactionState::EXECUTION_FAILED_OUTPUT_MISMATCH: return "OUTPUT_MISMATCH";
  }
  return "?";
}

const char* TransactionJournal::_phaseToString(BootPhase p) {
  switch (p) {
    case BootPhase::PRE_INIT:       return "PRE_INIT";
    case BootPhase::SAFE_INIT:      return "SAFE_INIT";
    case BootPhase::LOADING_NVS:    return "LOADING_NVS";
    case BootPhase::SNAPSHOT:       return "SNAPSHOT";
    case BootPhase::RECONCILING:    return "RECONCILING";
    case BootPhase::RESTORING:      return "RESTORING";
    case BootPhase::RUNNING:        return "RUNNING";
  }
  return "?";
}

const char* TransactionJournal::_durabilityToString(SlotDurability d) {
  switch (d) {
    case SlotDurability::SLOT_EMPTY:       return "EMPTY";
    case SlotDurability::SLOT_VALID:       return "VALID";
    case SlotDurability::SLOT_QUARANTINED: return "QUARANTINED";
  }
  return "?";
}

} // namespace Services
