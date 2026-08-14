// =============================================================================
// Services/TransactionJournal.h — Durable transaction journal v3
// =============================================================================
// CYCLE-8C (C8BR1-001, C8BR1-002 P0): Corruption-Safe State Machine
//
// PROBLEM (auditor C8BR1-001):
//   Cycle 8B-Rev1 freed slots when blob CRC failed. This DESTROYED execution
//   evidence for transactions that had reached EXECUTING (physical side effect
//   may have occurred). After corruption, requestId was lost, allowing
//   blind re-execution. This violated the core invariant:
//   "If physical side effect may have happened, firmware MUST NOT lose
//    durable evidence that transaction reached EXECUTING."
//
// PROBLEM (auditor C8BR1-002):
//   Cycle 8B-Rev1 clearEntry() only updated commit flag + state byte + RAM.
//   The blob (tj_entry_N) was NOT erased from NVS. After reboot, _loadFromNVS()
//   would deserialize the old blob and RESURRECT the cleared transaction.
//   This is "journal resurrection" — cleared entries come back to life.
//
// CYCLE-8C SOLUTION: Corruption-safe state machine with durable tombstones.
//
// NEW STATES:
//   CORRUPTED                   — Blob/state/commit invariant violated (terminal safety)
//   EXECUTION_FAILED_OUTPUT_MISMATCH — Execute ran but GPIO readback != desired (terminal durable)
//
// CORRUPTED state semantics:
//   - Set when: blob CRC fails, state byte invalid, commit/state invariant violated
//   - Entry is NOT freed, NOT cleared, NOT reused
//   - Marked CORRUPTED in NVS state byte (durable)
//   - On retry: PWA receives "JOURNAL_CORRUPTED — recovery required" ACK
//   - No automatic retry, no automatic clear
//   - Operator must explicitly resolve (recovery tool or factory reset)
//   - Preserves requestId so duplicate detection still works (no blind re-execute)
//
// DURABLE TOMBSTONE for clearEntry():
//   - Tombstone is a separate NVS key: tj_tomb_<hash_of_requestId>
//   - Written BEFORE blob is erased (so crash during clear is safe)
//   - Tombstone contains: requestId, timestamp, "cleared" marker
//   - On load: if tombstone exists, slot is NOT resurrected (even if blob is valid)
//   - Tombstone has TTL (24 hours) to prevent unbounded growth
//   - After tombstone confirmed + blob erased, slot is freed
//
// BLOB/STATE/COMMIT INVARIANT VALIDATION on load:
//   Valid combinations (commit flag, state byte):
//     (0, PENDING)              → PENDING (uncommitted)
//     (0, EXECUTING)            → EXECUTING (uncommitted)
//     (0, COMMITTED)            → transitional → treat as EXECUTING → reconcile UNKNOWN
//     (0, UNKNOWN)             → UNKNOWN (reconciled, uncommitted, clearable)
//     (0, FAILED)              → FAILED (reconciled, uncommitted, clearable)
//     (0, CORRUPTED)           → CORRUPTED (do not touch)
//     (0, EXECUTION_FAILED_OUTPUT_MISMATCH) → terminal durable, clearable by operator
//     (1, COMMITTED)           → COMMITTED (durable)
//     (1, anything else)       → INVALID → mark CORRUPTED
//   Invalid combinations → mark CORRUPTED (NOT free)
//
// STATE ENUM VALIDATION:
//   isValidTransactionState(raw) checks raw is in valid enum range
//   If invalid → mark CORRUPTED
//
// OUTPUT_MISMATCH as durable terminal:
//   - When GPIO readback != desired after execute
//   - Transaction committed to journal with state=EXECUTION_FAILED_OUTPUT_MISMATCH
//   - ackJson stored with failure ACK
//   - commit flag flipped to 1 (durable)
//   - PWA receives failure ACK + state in response
//   - No auto-retry (operator must investigate physical relay)
//
// CYCLE-8B-Rev1 (unchanged, still valid):
//   - Separated _createPendingEntryNVS() and _commitExecutingEntryNVS()
//   - Monotonicity validator _isTransitionAllowed()
//   - UNKNOWN state for "cannot determine"
//   - markExecuting() persists attempt in blob
//
// CYCLE-8B (unchanged, still valid):
//   - BootRecoveryPhase: PRE_INIT → SAFE_INIT → SNAPSHOT → RECONCILING → RESTORING → RUNNING
//   - captureOutputSnapshot() before RelayEngine
//   - RelayEngine guard during boot phases
// =============================================================================
#pragma once
#ifndef TIMER12_SERVICES_TRANSACTION_JOURNAL_H
#define TIMER12_SERVICES_TRANSACTION_JOURNAL_H

#include <Arduino.h>
#include <functional>

namespace Services {

// CYCLE-8C: Transaction state machine v3 (corruption-safe).
enum class TransactionState : uint8_t {
  PENDING                            = 0,
  EXECUTING                          = 1,
  COMMITTED                          = 2,
  COMMITTED_UNKNOWN                  = 3,
  UNKNOWN                            = 4,
  FAILED                             = 5,
  // CYCLE-8C NEW:
  CORRUPTED                          = 6,  // Blob/state/commit invariant violated (terminal safety)
  EXECUTION_FAILED_OUTPUT_MISMATCH   = 7,  // Execute ran but GPIO readback != desired (terminal durable)
};

// CYCLE-8C: Helper to validate state byte is in valid enum range.
inline bool isValidTransactionState(uint8_t raw) {
  return raw <= (uint8_t)TransactionState::EXECUTION_FAILED_OUTPUT_MISMATCH;
}

// CYCLE-8B: System boot phase
enum class BootPhase : uint8_t {
  PRE_INIT       = 0,
  SAFE_INIT      = 1,
  LOADING_NVS    = 2,
  SNAPSHOT       = 3,
  RECONCILING    = 4,
  RESTORING      = 5,
  RUNNING        = 6,
};

class TransactionJournal {
public:
  void begin();

  // =====================================================================
  // Boot phase management (CYCLE-8B)
  // =====================================================================
  void setBootPhase(BootPhase phase);
  BootPhase getBootPhase() const { return _bootPhase; }
  bool isRunning() const { return _bootPhase == BootPhase::RUNNING; }

  // =====================================================================
  // Raw output snapshot (CYCLE-8B)
  // =====================================================================
  void captureOutputSnapshot();
  bool getSnapshotState(uint8_t channelIdx) const;

  // =====================================================================
  // Monotonic state machine API (CYCLE-8B-Rev1 + CYCLE-8C)
  // =====================================================================
  bool storeIntent(const String& requestId, const String& commandHash,
                   uint8_t channelId = 0, bool desiredState = false,
                   bool previousKnownState = false);

  bool markExecuting(const String& requestId);

  bool commitTransaction(const String& requestId, const String& ackJson);

  // CYCLE-8C: Commit with FAILURE state (for OUTPUT_MISMATCH).
  //   Stores ackJson + state=EXECUTION_FAILED_OUTPUT_MISMATCH + commit=1.
  //   This is a durable terminal state — no auto-retry.
  bool commitTransactionFailed(const String& requestId, const String& ackJson,
                                TransactionState failureState);

  // =====================================================================
  // Lookup helpers
  // =====================================================================
  bool isProcessed(const String& requestId);
  bool isCommitted(const String& requestId);
  TransactionState getTransactionState(const String& requestId);
  String getCommandHash(const String& requestId);
  String getAckJson(const String& requestId);
  uint8_t getChannelId(const String& requestId);
  bool getDesiredState(const String& requestId);

  // =====================================================================
  // Reconciliation
  // =====================================================================
  uint8_t reconcilePendingEntries();
  TransactionState reconcileEntry(const String& requestId);

  // =====================================================================
  // Legacy API (CYCLE-8C: deprecated, will be removed in future cycle)
  // =====================================================================
  bool storeTransaction(const String& requestId, const String& commandHash,
                        const String& ackJson);

  // =====================================================================
  // ACK delivery queue
  // =====================================================================
  void queueAck(const String& requestId, const String& ackJson);
  uint8_t processPendingAcks();
  uint8_t getPendingAckCount() const { return _pendingAckCount; }
  uint8_t getJournalSize() const { return _journalSize; }
  void dequeueAck(const String& requestId);

  // CYCLE-8C: clearEntry now uses durable tombstone (fixes C8BR1-002).
  //   Returns true if entry was cleared (tombstone written + blob erased).
  //   NOT allowed for: COMMITTED, COMMITTED_UNKNOWN, CORRUPTED, EXECUTION_FAILED_OUTPUT_MISMATCH
  //   (these are durable terminal states — operator must explicitly resolve)
  bool clearEntry(const String& requestId);

  // CYCLE-8C: Explicit recovery for CORRUPTED entries (operator-initiated).
  //   Clears CORRUPTED entry + writes tombstone. Used by recovery tooling.
  bool recoverCorruptedEntry(const String& requestId);

private:
  static const uint8_t JOURNAL_SIZE = 64;
  static const uint8_t MAX_PENDING_ACKS = 8;
  static const uint8_t MAX_ACK_RETRIES = 10;
  static const unsigned long ACK_RETRY_INTERVAL_MS = 2000;

  static const uint16_t BLOB_MAGIC1 = 0x54;
  static const uint16_t BLOB_MAGIC2 = 0x4A;
  static const uint8_t BLOB_VERSION = 2;
  static const uint8_t BLOB_HEADER_SIZE = 8;
  static const uint16_t BLOB_SIZE = 1200;

  String _journalIds[JOURNAL_SIZE];
  String _journalHashes[JOURNAL_SIZE];
  String _journalAcks[JOURNAL_SIZE];
  bool _journalValid[JOURNAL_SIZE];
  bool _journalCommitted[JOURNAL_SIZE];
  TransactionState _journalState[JOURNAL_SIZE];
  uint8_t _journalChannelId[JOURNAL_SIZE];
  bool _journalDesiredState[JOURNAL_SIZE];
  bool _journalPreviousKnownState[JOURNAL_SIZE];
  uint8_t _journalAttempt[JOURNAL_SIZE];
  uint32_t _journalTimestamp[JOURNAL_SIZE];
  uint8_t _journalSize = 0;
  uint8_t _journalWriteIdx = 0;

  BootPhase _bootPhase = BootPhase::PRE_INIT;
  bool _outputSnapshot[16] = {false};
  bool _snapshotCaptured = false;

  struct PendingAck {
    String requestId;
    String ackJson;
    uint8_t retryCount;
    unsigned long lastAttemptMs;
  };
  PendingAck _pendingAcks[MAX_PENDING_ACKS];
  uint8_t _pendingAckCount = 0;

  void _loadFromNVS();

  // CYCLE-8B-Rev1: separated operations
  bool _createPendingEntryNVS(uint8_t idx);
  bool _commitExecutingEntryNVS(uint8_t idx, const String& ackJson,
                                  TransactionState targetState = TransactionState::COMMITTED);

  // CYCLE-8C: validate blob/state/commit invariant on load
  bool _validateInvariant(uint8_t idx, bool committed, TransactionState state);
  // CYCLE-8C: mark entry as CORRUPTED (durable, does not free slot)
  bool _markCorruptedNVS(uint8_t idx);

  bool _setTransactionStateNVS(uint8_t idx, TransactionState state);

  // CYCLE-8C: durable tombstone (fixes C8BR1-002)
  bool _writeTombstoneNVS(const String& requestId);
  bool _hasTombstoneNVS(const String& requestId);
  void _removeTombstoneNVS(const String& requestId);
  bool _eraseBlobNVS(uint8_t idx);

  // CYCLE-8B-Rev1: _clearSlotNVS (kept for backward compat, but clearEntry now uses tombstone)
  bool _clearSlotNVS(uint8_t idx);

  bool _deserializeEntry(const uint8_t* blob, size_t len, uint8_t idx);
  uint32_t _computeCRC(const uint8_t* data, size_t len);
  int _findInJournal(const String& requestId);
  bool _isTransitionAllowed(TransactionState from, TransactionState to);

  static const char* _stateToString(TransactionState s);
  static const char* _phaseToString(BootPhase p);
  // CYCLE-8C: tombstone key helper
  static String _tombstoneKey(const String& requestId);
};

extern TransactionJournal journal;
void setPublishCallback(std::function<bool(const char* topic, const uint8_t* payload, size_t len)> cb);

} // namespace Services

#endif
