// =============================================================================
// Services/TransactionJournal.h — Durable transaction journal for MQTT commands
// =============================================================================
// CYCLE-8B-Rev1 (C8B-001 P0): Monotonic state machine fix
//
// PROBLEM (auditor C8B-001):
//   Cycle 8B used _saveEntryToNVSAtomic() for TWO different operations:
//     1. Creating new PENDING intent (storeIntent)
//     2. Committing EXECUTING entry (commitTransaction)
//   The function's Phase 0 always cleared commit flag + set state=PENDING.
//   For the commit path, this RESET EXECUTING → PENDING before writing new blob.
//   Crash window between Phase 0 and Phase 2 (commit flip) left entry as PENDING.
//   After reboot, reconciliation saw PENDING + desired=ON → marked FAILED.
//   But FAILED no longer meant "execute never ran" — it could mean "execute ran
//   but commit procedure reset state". This is a NON-MONOTONIC state machine.
//
// CYCLE-8B-Rev1 SOLUTION: Separate operations with distinct NVS write sequences.
//
//   NEW state machine (monotonic — forward transitions only):
//
//     (none) → PENDING          via storeIntent() / createPendingEntry()
//     PENDING → EXECUTING       via markExecuting()
//     EXECUTING → COMMITTED     via commitTransaction() / commitExecutingEntry()
//     PENDING → UNKNOWN         via reconciliation (cannot determine)
//     EXECUTING → UNKNOWN       via reconciliation (cannot determine)
//     PENDING → FAILED          via reconciliation (proven not executed)
//     * → (cleared)             via clearEntry() (for PENDING/EXECUTING/FAILED/UNKNOWN)
//
//   FORBIDDEN transitions (would violate monotonicity):
//     EXECUTING → PENDING       (was happening in Cycle 8B due to C8B-001)
//     COMMITTED → PENDING
//     COMMITTED → EXECUTING
//     COMMITTED_UNKNOWN → PENDING
//     COMMITTED_UNKNOWN → EXECUTING
//     COMMITTED_UNKNOWN → FAILED
//     COMMITTED → any state     (COMMITTED is terminal — durable, cannot transition)
//
//   NEW state: UNKNOWN (replaces some FAILED uses per C8B-002)
//     - FAILED: PROVEN not executed (only from PENDING + desired=ON + snapshot=OFF)
//     - UNKNOWN: cannot determine (from EXECUTING after crash, or non-relay commands)
//     - isProcessed() returns true for COMMITTED, COMMITTED_UNKNOWN, UNKNOWN
//     - isProcessed() returns false for PENDING, EXECUTING, FAILED (allow retry)
//
//   Operation separation:
//     createPendingEntry():  NEW slot, write blob with state=PENDING, commit=0
//       - Phase 0: clear commit (only if slot was previously used)
//       - Phase 1: write blob
//       - Phase 1b: persist writeIdx (new slot only)
//       - Phase 2: set state=PENDING (no commit flip)
//     markExecuting():       update state PENDING → EXECUTING (single NVS byte write)
//       - Does NOT rewrite blob
//       - Persists attempt counter atomically with state
//     commitExecutingEntry():  flip commit 0→1, update ackJson, set state=COMMITTED
//       - Does NOT clear commit flag (preserves EXECUTING evidence)
//       - Phase 1: write blob with NEW ackJson + state=COMMITTED (but commit still 0)
//       - Phase 2: flip commit flag 0 → 1 (atomic commit point)
//       - If crash during Phase 1: entry still EXECUTING (blob may be partial,
//         but commit flag still 0, state still EXECUTING — reconciliation will
//         mark UNKNOWN, NOT FAILED)
//
// CYCLE-8B (unchanged, still valid):
//   BootRecoveryPhase: PRE_INIT → SAFE_INIT → LOADING_NVS → SNAPSHOT →
//                       RECONCILING → RESTORING → RUNNING
//   captureOutputSnapshot(): capture GPIO before RelayEngine runs
//   RelayEngine guard: skip during non-RUNNING phases
//   MQTT command guard: reject during boot recovery
//
// HONEST LIMITATIONS (unchanged from Cycle 8B):
//   1. Snapshot reflects safe-OFF, not pre-crash state (hardware limitation)
//   2. GPIO output ≠ physical relay contact (welded/stuck undetectable)
//   3. Non-relay commands cannot be reconciled via GPIO (marked UNKNOWN)
//   4. Hardware power-loss testing NOT RUN
// =============================================================================
#pragma once
#ifndef TIMER12_SERVICES_TRANSACTION_JOURNAL_H
#define TIMER12_SERVICES_TRANSACTION_JOURNAL_H

#include <Arduino.h>
#include <functional>

namespace Services {

// CYCLE-8B-Rev1: Transaction state machine (monotonic).
//   States are ordered: PENDING < EXECUTING < {COMMITTED, COMMITTED_UNKNOWN, UNKNOWN, FAILED}
//   FAILED and UNKNOWN are terminal-pre-clear (can be cleared to allow retry).
//   COMMITTED and COMMITTED_UNKNOWN are terminal (durable, cannot transition).
enum class TransactionState : uint8_t {
  PENDING            = 0,  // Intent stored, execute NOT yet called
  EXECUTING          = 1,  // Execute called, commit NOT yet done
  COMMITTED          = 2,  // Execute + commit succeeded (terminal, durable)
  COMMITTED_UNKNOWN  = 3,  // Reconciled: cannot determine (terminal, durable)
  UNKNOWN            = 4,  // Cannot determine — may or may not have executed (clearable)
  FAILED             = 5,  // Proven not executed (clearable, allows retry)
};

// CYCLE-8B: System boot phase — controls what can modify relay state.
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
  // CYCLE-8B: Boot recovery phase management
  // =====================================================================
  void setBootPhase(BootPhase phase);
  BootPhase getBootPhase() const { return _bootPhase; }
  bool isRunning() const { return _bootPhase == BootPhase::RUNNING; }

  // =====================================================================
  // CYCLE-8B: Raw output snapshot
  // =====================================================================
  void captureOutputSnapshot();
  bool getSnapshotState(uint8_t channelIdx) const;

  // =====================================================================
  // CYCLE-8B-Rev1: Monotonic state machine API
  // =====================================================================

  // Create a NEW PENDING entry (storeIntent).
  // Fails if requestId already exists in journal.
  // For relay commands, record channelId + desiredState + previousKnownState.
  bool storeIntent(const String& requestId, const String& commandHash,
                   uint8_t channelId = 0, bool desiredState = false,
                   bool previousKnownState = false);

  // Transition PENDING → EXECUTING (markExecuting).
  // Persists attempt counter atomically with state.
  // Fails if entry not in PENDING state (monotonicity check).
  bool markExecuting(const String& requestId);

  // Transition EXECUTING → COMMITTED (commitTransaction).
  // Updates ackJson, flips commit flag 0→1, sets state=COMMITTED.
  // Does NOT reset state to PENDING (fixes C8B-001).
  // Fails if entry not in EXECUTING state (monotonicity check).
  bool commitTransaction(const String& requestId, const String& ackJson);

  // =====================================================================
  // Lookup helpers
  // =====================================================================

  // CYCLE-8B-Rev1: isProcessed() EXCLUDES FAILED, UNKNOWN, PENDING, EXECUTING.
  //   Only COMMITTED and COMMITTED_UNKNOWN are "processed" (replay ACK).
  //   PENDING/EXECUTING are "in-flight" (caller should reconcile or wait).
  //   FAILED is "proven not executed" (allow retry).
  //   UNKNOWN is "cannot determine" (caller decides — default: allow retry with caution).
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

  // Reconcile ALL PENDING and EXECUTING entries using captured snapshot.
  // CYCLE-8B-Rev1: produces FAILED (proven not executed) or UNKNOWN (cannot determine).
  //   - PENDING + desired=ON + snapshot=OFF → FAILED (proven not executed)
  //   - PENDING + desired=OFF → UNKNOWN (idempotent, cannot determine)
  //   - EXECUTING → UNKNOWN (cannot determine — execute may have run)
  //   - Non-relay (channelId=0) → UNKNOWN (cannot verify via GPIO)
  uint8_t reconcilePendingEntries();

  // Reconcile a SINGLE entry (used on retry during RUNNING phase).
  // Uses LIVE GPIO read (after RUNNING, snapshot is stale).
  TransactionState reconcileEntry(const String& requestId);

  // =====================================================================
  // Legacy API
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

  // CYCLE-8B-Rev1: clearEntry allows PENDING, EXECUTING, FAILED, UNKNOWN.
  //   NOT allowed: COMMITTED, COMMITTED_UNKNOWN (terminal, durable).
  //   Returns true if entry was cleared (fixes C8B-007: check NVS write success).
  bool clearEntry(const String& requestId);

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

  // CYCLE-8B-Rev1: SEPARATED operations (fixes C8B-001)
  //   _createPendingEntryNVS: write blob for NEW PENDING entry (state=PENDING, commit=0)
  //   _commitExecutingEntryNVS: write blob with ackJson + flip commit 0→1 + set state=COMMITTED
  //     Does NOT reset state to PENDING (preserves EXECUTING evidence during crash window).
  bool _createPendingEntryNVS(uint8_t idx);
  bool _commitExecutingEntryNVS(uint8_t idx, const String& ackJson);

  bool _setTransactionStateNVS(uint8_t idx, TransactionState state);

  // CYCLE-8B-Rev1: _clearSlotNVS now returns success status (fixes C8B-007)
  bool _clearSlotNVS(uint8_t idx);

  bool _deserializeEntry(const uint8_t* blob, size_t len, uint8_t idx);

  uint32_t _computeCRC(const uint8_t* data, size_t len);

  int _findInJournal(const String& requestId);

  // CYCLE-8B-Rev1: monotonicity validator — checks if transition is allowed.
  bool _isTransitionAllowed(TransactionState from, TransactionState to);

  static const char* _stateToString(TransactionState s);
  static const char* _phaseToString(BootPhase p);
};

extern TransactionJournal journal;
void setPublishCallback(std::function<bool(const char* topic, const uint8_t* payload, size_t len)> cb);

} // namespace Services

#endif
