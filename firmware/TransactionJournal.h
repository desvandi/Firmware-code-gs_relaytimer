// =============================================================================
// Services/TransactionJournal.h — Transaction Journal v4 (Rev26 normative)
// =============================================================================
// P2-1 implementation per docs/CYCLE-8C-REV26-FINAL-PREDICATE.md and
// docs/PHASE-2-SCOPE.md §P2-1.
//
// STORAGE MODEL (Rev26 — replaces pre-Rev26 two-phase commit):
//
//   Each slot N (0..63) stores TWO NVS keys:
//     tj_slot_<N>_a   — copy A, BLOB_SIZE=1200 bytes (JournalRecord blob)
//     tj_slot_<N>_b   — copy B, BLOB_SIZE=1200 bytes (JournalRecord blob)
//
//   No separate commit flag (pre-Rev26 tj_commit_<N> is REMOVED). Commit is
//   encoded in recordState byte of canonical payload. Durability comes from
//   dual-copy canonical equivalence + CRC verification.
//
//   ACK queue: tj_ackq — single 2056-byte blob containing:
//     [count:1] [reserved:3] [AckRecord × 8 = 2048] [queueCRC:4]
//
// INVARIANTS (Rev26 normative):
//
//   I0  — Journal API only from executor task (TaskHandle check, runtime panic)
//   I0a — Observation and mutation are mutually exclusive (RAII guard + panic)
//   I1  — Canonical equivalence + 9-row recovery decision table
//   I2  — Eviction safety (I2a-I2e + auth gate for NON_IDEMPOTENT)
//   I3  — ACK lifecycle independent of transaction lifecycle
//
// RECOVERY DECISION TABLE (Rev14 §I1, Rev26 confirmed):
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
//   | 9 | VALID    | VALID    | GEN_INVALID (distance > 1)  | CORRUPTED   |
//
// EVICTION (Rev26 §4 — current implementation):
//
//   IDEMPOTENT + (PUBLISH_ACCEPTED+durable_queue OR FAILED_EXHAUSTED) → YES
//   NON_IDEMPOTENT → NEVER (AUTH_EVIDENCE_AUTHENTICATED is UNACHIEVABLE)
//   UNKNOWN → NEVER (default RETAIN per I2e)
//
// =============================================================================
#pragma once
#ifndef TIMER12_SERVICES_TRANSACTION_JOURNAL_H
#define TIMER12_SERVICES_TRANSACTION_JOURNAL_H

#include <Arduino.h>
#include <functional>
#include "JournalRecord.h"  // Phase 1 primitive (Rev26 foundation)

namespace Services {

// -----------------------------------------------------------------------------
// Runtime transaction state machine (Rev26 — includes CORRUPTED as derived).
// On-disk record uses RecordState (from JournalRecord.h); CORRUPTED is
// runtime-only and never serialized (Rev14 §4, Rev9 §2).
//
// NOTE: enum values are PRESERVED from pre-Rev26 for backward compatibility
// with MqttClient.cpp / RelayEngine.cpp callers. Conversion to RecordState
// (Phase 1 on-disk enum) happens internally via _toRecordState().
// -----------------------------------------------------------------------------
enum class TransactionState : uint8_t {
  PENDING                            = 0,
  EXECUTING                          = 1,
  COMMITTED                          = 2,
  COMMITTED_UNKNOWN                  = 3,
  UNKNOWN                            = 4,
  FAILED                             = 5,
  CORRUPTED                          = 6,  // Runtime-derived (not serialized)
  EXECUTION_FAILED_OUTPUT_MISMATCH   = 7,  // Durable terminal
};

inline bool isValidTransactionState(uint8_t raw) {
  return raw <= (uint8_t)TransactionState::EXECUTION_FAILED_OUTPUT_MISMATCH;
}

// -----------------------------------------------------------------------------
// Boot phase management (preserved from Cycle-8B — unchanged by Rev26)
// -----------------------------------------------------------------------------
enum class BootPhase : uint8_t {
  PRE_INIT       = 0,
  SAFE_INIT      = 1,
  LOADING_NVS    = 2,
  SNAPSHOT       = 3,
  RECONCILING    = 4,
  RESTORING      = 5,
  RUNNING        = 6,
};

// -----------------------------------------------------------------------------
// ACK delivery states (Rev26 I3 — durable in tj_ackq blob)
// -----------------------------------------------------------------------------
enum class AckDeliveryState : uint8_t {
  ACK_NOT_SENT         = 0,
  ACK_PUBLISH_ACCEPTED = 1,  // MQTT client publish() returned success
  ACK_BROKER_CONFIRMED = 2,  // Broker PUBACK received (QoS 1)
  ACK_PWA_RECEIVED     = 3,  // PWA sent ack_confirm (currently UNACHIEVABLE)
  ACK_FAILED_EXHAUSTED = 4,  // Retry count exceeded
};

// -----------------------------------------------------------------------------
// Command classification (Rev26 I2b)
// -----------------------------------------------------------------------------
enum class CommandClass : uint8_t {
  UNKNOWN        = 0,  // Default — treat as NON_IDEMPOTENT for safety
  IDEMPOTENT     = 1,
  NON_IDEMPOTENT = 2,
};

// -----------------------------------------------------------------------------
// Auth evidence (Rev26 §2 — currently UNACHIEVABLE)
//
//   AUTH_EVIDENCE_AUTHENTICATED (value 2) is FORBIDDEN in current implementation.
//   No code path may produce it. Only a future audited sender-auth mechanism
//   may set this value. Until then: NON_IDEMPOTENT eviction = NEVER.
// -----------------------------------------------------------------------------
enum class AuthEvidence : uint8_t {
  EVIDENCE_UNAVAILABLE = 0,  // No auth evidence (default, current state)
  EVIDENCE_ACKDIGEST   = 1,  // ackDigest verified (content binding, NOT auth)
  // value 2 = AUTH_EVIDENCE_AUTHENTICATED — FORBIDDEN (Rev26 §2)
};

// -----------------------------------------------------------------------------
// Slot durability state (runtime, derived from copy A/B validity)
// -----------------------------------------------------------------------------
enum class SlotDurability : uint8_t {
  SLOT_EMPTY      = 0,  // Both copies EMPTY (gen=0)
  SLOT_VALID      = 1,  // Both copies valid, canonical-equivalent, gen known
  SLOT_QUARANTINED = 2, // Both copies INVALID or gen mismatch → CORRUPTED
};

// -----------------------------------------------------------------------------
// ObservationGuard (Rev26 I0a — RAII, panic on nested observation)
//
//   Constructor: panics if _observing == true (nested observation forbidden).
//   Destructor: resets _observing to false.
//
//   Used by every observation API. Mutations during observation are
//   forbidden (panic via _assertMutationAllowed()).
// -----------------------------------------------------------------------------
class ObservationGuard {
public:
  explicit ObservationGuard(bool& flag);
  ~ObservationGuard();

  // Non-copyable, non-movable (RAII over fixed flag reference)
  ObservationGuard(const ObservationGuard&) = delete;
  ObservationGuard& operator=(const ObservationGuard&) = delete;
  ObservationGuard(ObservationGuard&&) = delete;
  ObservationGuard& operator=(ObservationGuard&&) = delete;

private:
  bool& _flag;
};

// -----------------------------------------------------------------------------
// AckRecord (Rev26 I3 — durable in tj_ackq blob, 256 bytes per record)
//
//   Layout (256 bytes total):
//     [ackMagic:2] [ackVersion:1] [deliveryState:1]
//     [requestIdLen:1] [requestId:var (max 64)]
//     [commandHashLen:1] [commandHash:var (max 64)]
//     [retryCount:1] [lastAttemptTs:4]
//     [ackLen:2] [ackJson:var (max 1024)]
//     [padding to 256 bytes]
// -----------------------------------------------------------------------------
static const uint16_t ACK_MAGIC1 = 0x41;  // 'A'
static const uint16_t ACK_MAGIC2 = 0x51;  // 'Q'
static const uint8_t  ACK_VERSION = 1;
static const uint16_t ACK_RECORD_SIZE = 256;
static const uint8_t  ACK_QUEUE_CAPACITY = 8;
static const uint16_t ACK_QUEUE_BLOB_SIZE = 4 + (ACK_RECORD_SIZE * ACK_QUEUE_CAPACITY) + 4;  // 2056 bytes

struct AckRecord {
  AckDeliveryState deliveryState;
  String requestId;
  String commandHash;
  uint8_t  retryCount;
  uint32_t lastAttemptTs;
  String ackJson;

  AckRecord()
    : deliveryState(AckDeliveryState::ACK_NOT_SENT)
    , retryCount(0)
    , lastAttemptTs(0)
  {}

  bool isEmpty() const { return requestId.length() == 0; }
};

// =============================================================================
// TransactionJournal — Rev26 dual-copy implementation
// =============================================================================
class TransactionJournal {
public:
  TransactionJournal();
  ~TransactionJournal();

  // ===========================================================================
  // Initialization
  // ===========================================================================
  void begin();

  // ===========================================================================
  // Boot phase management (Cycle-8B — preserved, unchanged by Rev26)
  // ===========================================================================
  void setBootPhase(BootPhase phase);
  BootPhase getBootPhase() const { return _bootPhase; }
  bool isRunning() const { return _bootPhase == BootPhase::RUNNING; }

  // ===========================================================================
  // Raw output snapshot (Cycle-8B — preserved)
  // ===========================================================================
  void captureOutputSnapshot();
  bool getSnapshotState(uint8_t channelIdx) const;

  // ===========================================================================
  // Mutation API (Rev26 — all call _assertExecutorContext + _assertMutationAllowed)
  //
  //   storeIntent(): write PENDING entry to both copies with new generation
  //   markExecuting(): PENDING → EXECUTING, write both copies
  //   commitTransaction(): EXECUTING → COMMITTED, write both copies + queue ACK
  //   commitTransactionFailed(): EXECUTING → FAILED / OUTPUT_MISMATCH
  //   clearEntry(): write EMPTY to both copies (subject to eviction safety I2a-I2e)
  //   recoverCorruptedEntry(): write EMPTY(gen=0) to both copies unconditionally
  // ===========================================================================
  bool storeIntent(const String& requestId, const String& commandHash,
                   uint8_t channelId = 0, bool desiredState = false,
                   bool previousKnownState = false);

  bool markExecuting(const String& requestId);

  bool commitTransaction(const String& requestId, const String& ackJson);

  bool commitTransactionFailed(const String& requestId, const String& ackJson,
                                TransactionState failureState);

  // clearEntry(): clears PENDING/EXECUTING/FAILED entries only.
  // NOT allowed for: COMMITTED, COMMITTED_UNKNOWN, CORRUPTED, OUTPUT_MISMATCH.
  // For COMMITTED entries: subject to eviction safety I2a-I2e.
  bool clearEntry(const String& requestId);

  // recoverCorruptedEntry(): operator-initiated recovery for CORRUPTED slots.
  // Writes EMPTY(gen=0) to both copies unconditionally.
  bool recoverCorruptedEntry(const String& requestId);

  // ===========================================================================
  // Lookup helpers (observation — read-only, no mutation)
  // ===========================================================================
  bool isProcessed(const String& requestId);
  bool isCommitted(const String& requestId);
  TransactionState getTransactionState(const String& requestId);
  String getCommandHash(const String& requestId);
  String getAckJson(const String& requestId);
  uint8_t getChannelId(const String& requestId);
  bool getDesiredState(const String& requestId);
  uint8_t getJournalSize() const { return _journalSize; }

  // ===========================================================================
  // Reconciliation (Rev26 — observation, uses 9-row decision table)
  // ===========================================================================
  uint8_t reconcilePendingEntries();
  TransactionState reconcileEntry(const String& requestId);

  // ===========================================================================
  // ACK delivery queue (Rev26 I3 — durable in tj_ackq blob)
  // ===========================================================================
  void queueAck(const String& requestId, const String& ackJson);
  uint8_t processPendingAcks();
  uint8_t getPendingAckCount() const { return _ackQueueCount; }
  void dequeueAck(const String& requestId);

  // Update ACK delivery state (called by MQTT client on PUBACK / PWA ack_confirm)
  bool updateAckDeliveryState(const String& requestId, AckDeliveryState newState);

  // ===========================================================================
  // Test/introspection helpers (for host test harness — not for production callers)
  // ===========================================================================
  // Host tests can register a fake executor task handle to test executor-context panic
  static void _setExecutorTaskForTest(void* taskHandle);
  // Host tests can inspect internal state
  SlotDurability _getSlotDurability(uint8_t slotIdx) const;
  uint32_t _getSlotGeneration(uint8_t slotIdx) const;
  uint8_t _findSlotByRequestId(const String& requestId) const;
  // Force-load slot from NVS (for testing recovery)
  bool _forceReloadSlot(uint8_t slotIdx);

private:
  // ===========================================================================
  // Constants (Rev26 — match Phase 1 JournalRecord.h)
  // ===========================================================================
  static const uint8_t  JOURNAL_SIZE = 64;
  static const uint8_t  MAX_ACK_RETRIES = 10;
  static const unsigned long ACK_RETRY_INTERVAL_MS = 2000;

  // ===========================================================================
  // In-RAM slot cache (mirrors NVS state for fast lookup)
  // ===========================================================================
  struct SlotInfo {
    JournalRecord record;        // Last-known-good record (after reconciliation)
    SlotDurability durability;  // SLOT_EMPTY / SLOT_VALID / SLOT_QUARANTINED
    bool inUse;                  // Slot has a non-EMPTY record

    SlotInfo()
      : durability(SlotDurability::SLOT_EMPTY)
      , inUse(false)
    {}
  };

  SlotInfo _slots[JOURNAL_SIZE];
  uint8_t  _journalSize = 0;       // Count of non-EMPTY slots
  uint8_t  _journalWriteIdx = 0;  // Next slot to consider for new entry (LRU)

  // ===========================================================================
  // Boot phase + output snapshot (Cycle-8B — preserved)
  // ===========================================================================
  BootPhase _bootPhase = BootPhase::PRE_INIT;
  bool _outputSnapshot[16] = {false};
  bool _snapshotCaptured = false;

  // ===========================================================================
  // ACK queue (Rev26 I3 — in-RAM mirror of tj_ackq blob)
  // ===========================================================================
  AckRecord _ackQueue[ACK_QUEUE_CAPACITY];
  uint8_t   _ackQueueCount = 0;
  uint32_t  _ackQueueCRC = 0;

  // ===========================================================================
  // Executor + observation state (Rev26 I0 / I0a)
  // ===========================================================================
  void* _executorTaskHandle = nullptr;  // FreeRTOS TaskHandle, set in begin()
  bool  _observing = false;             // RAII-guarded by ObservationGuard

  // Publish callback (for ACK delivery)
  std::function<bool(const char* topic, const uint8_t* payload, size_t len)> _publishCb;

  // ===========================================================================
  // I0 / I0a enforcement (Rev26 — panic on violation)
  // ===========================================================================
  void _assertExecutorContext();
  void _assertMutationAllowed();

  // ===========================================================================
  // Record state conversion (TransactionState runtime ↔ RecordState on-disk)
  // ===========================================================================
  static RecordState _toRecordState(TransactionState s);
  static TransactionState _fromRecordState(RecordState s);

  // ===========================================================================
  // Dual-copy NVS operations (Rev26 I1 — replaces pre-Rev26 two-phase commit)
  // ===========================================================================
  bool _writeCopy(uint8_t slotIdx, bool isCopyA, const JournalRecord& rec);
  bool _readCopy(uint8_t slotIdx, bool isCopyA, JournalRecord& outRec);
  bool _eraseBlobNVS(uint8_t slotIdx);       // Erases both copies
  bool _clearSlotNVS(uint8_t slotIdx);       // Writes EMPTY(gen=0) to both copies
  bool _repairSlot(uint8_t slotIdx, bool fromCopyA);  // Bitwise restore from VALID to INVALID
  bool _quarantineSlot(uint8_t slotIdx);     // Marks slot CORRUPTED (no NVS erase)

  // 9-row recovery decision table (Rev26 I1 — implemented in _reconcileSlot)
  SlotDurability _reconcileSlot(uint8_t slotIdx);

  // Find slot for requestId (returns JOURNAL_SIZE if not found)
  uint8_t _findSlot(const String& requestId) const;

  // Find next available slot for new entry (LRU eviction if needed)
  // Returns JOURNAL_SIZE if journal full and no evictable slot
  uint8_t _findEvictableSlot() const;

  // Eviction predicate (Rev26 §4 — I2a-I2e + auth gate)
  bool _isEvictionPermitted(uint8_t slotIdx) const;

  // Command classification (Rev26 I2b)
  static CommandClass _classifyCommand(const String& commandHash);

  // Generation assignment (Rev26 — distance 0 or 1 only)
  uint32_t _assignNextGeneration(uint8_t slotIdx) const;

  // ===========================================================================
  // ACK queue NVS operations (Rev26 I3)
  // ===========================================================================
  bool _loadAckQueue();
  bool _persistAckQueue();
  uint32_t _computeAckQueueCRC() const;
  void _serializeAckRecord(const AckRecord& rec, uint8_t* buf) const;
  bool _deserializeAckRecord(const uint8_t* buf, AckRecord& outRec) const;
  int8_t _findAckInQueue(const String& requestId) const;

  // ===========================================================================
  // Load from NVS (Rev26 — observation phase, uses ObservationGuard)
  // ===========================================================================
  void _loadFromNVS();

  // ===========================================================================
  // Slot evaluation (Rev26 I1 — observation, uses ObservationGuard)
  // ===========================================================================
  SlotDurability _evaluateSlot(uint8_t slotIdx);
  bool _checkI1Satisfied(uint8_t slotIdx);

  // ===========================================================================
  // Helpers
  // ===========================================================================
  static String _slotKeyA(uint8_t idx);
  static String _slotKeyB(uint8_t idx);
  static const char* _stateToString(TransactionState s);
  static const char* _phaseToString(BootPhase p);
  static const char* _durabilityToString(SlotDurability d);
};

extern TransactionJournal journal;
void setPublishCallback(std::function<bool(const char* topic, const uint8_t* payload, size_t len)> cb);

// P2-1: Snapshot provider callback — firmware_v4.ino registers this so
// TransactionJournal doesn't need to depend on RelayDriver.h directly.
// The callback returns the current logical state of channel `idx`.
void setSnapshotProvider(std::function<bool(uint8_t)> cb);

} // namespace Services

#endif // TIMER12_SERVICES_TRANSACTION_JOURNAL_H
