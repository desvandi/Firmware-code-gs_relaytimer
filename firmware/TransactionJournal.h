// =============================================================================
// Services/TransactionJournal.h — Durable transaction journal for MQTT commands
// =============================================================================
// CYCLE-8A (AUDIT-7-001): Transaction Recovery State Machine
//
// PROBLEM (auditor AUDIT-7-001):
//   Cycle 7's intent-first journaling closed the execute→store gap BUT did not
//   achieve true atomicity between NVS commit and physical GPIO state.
//
//   Three crash scenarios that Cycle 7 did NOT handle:
//     A) PENDING stored, relay NEVER executed (crash between storeIntent and execute)
//        → After boot: PENDING, GPIO = OFF (unchanged)
//        → PWA retries → isProcessed()=true → DUPLICATE rejected → COMMAND LOST
//     B) PENDING stored, relay EXECUTED, crash before commit
//        → After boot: PENDING, GPIO = ON (changed)
//        → No durable proof physical action happened
//     C) relay executed, commitTransaction FAILED
//        → DURABILITY_FAILURE reported to PWA
//        → PWA retries → PENDING blocks as DUPLICATE
//        → State ambiguity: client thinks "failed", hardware is "succeeded"
//
// CYCLE-8A SOLUTION: Recoverable state machine + boot reconciliation
//
//   Transaction states:
//     NEW          — command received, not yet persisted (transient, not in journal)
//     PENDING      — intent stored to NVS, execute NOT yet called
//     EXECUTING    — execute called, commit NOT yet done (GPIO may or may not have changed)
//     COMMITTED    — execute + commit succeeded (durable success)
//     COMMITTED_UNKNOWN — reconcile found GPIO matches desired (likely succeeded, unprovable)
//     FAILED       — reconcile found GPIO doesn't match desired (execute didn't happen or failed)
//
//   Boot reconciliation (new):
//     On boot, scan all PENDING and EXECUTING entries.
//     For each relay command (channelId > 0):
//       - Read actual GPIO output state via RelayDriver::readLogicalState()
//       - If GPIO == desiredState → mark COMMITTED_UNKNOWN (likely succeeded)
//       - If GPIO != desiredState → mark FAILED (execute didn't happen)
//     For non-relay commands (channelId == 0): leave as-is, log warning.
//
//   On retry (during normal operation):
//     - COMMITTED or COMMITTED_UNKNOWN → replay stored ACK (true duplicate)
//     - FAILED → clear entry, allow retry with same requestId
//     - PENDING or EXECUTING → reconcile first, then decide
//
//   ACK messages (honest):
//     - "Command executed" (COMMITTED — execute + commit both succeeded)
//     - "Command may have executed (reconciled from PENDING, GPIO matches)" (COMMITTED_UNKNOWN)
//     - "Command did not execute (reconciled from PENDING, GPIO doesn't match)" (FAILED)
//     - "Durability failure — retry recommended" (commitTransaction returned false)
//
//   PHYSICAL STATE UNCERTAINTY (documented limitation, addressed in Cycle 8B):
//     - GPIO output state ≠ physical relay contact state.
//     - A welded relay could have GPIO=ON but contact=OFF (or vice versa).
//     - Without contact feedback hardware, we CANNOT verify physical state.
//     - readLogicalState() tells us what we COMMANDED, not what the relay DID.
//     - For 220V safety: ACK messages include "physicalState: unknown" disclaimer.
//
// STORAGE LAYOUT (version 2 — incompatible with version 1, cleared on upgrade):
//   tj_entry_N: blob [magic(2) + ver(1) + reserved(1) + CRC32(4) + payload]
//   tj_commit_N: single byte (0=uncommitted, 1=committed)
//   tj_state_N: single byte (transactionState enum) — CYCLE-8A NEW
//   tj_count:   journal size (metadata only)
//   tj_widx:    next write slot index
//
//   Blob payload (version 2):
//     [requestId: 1+64]
//     [commandHash: 1+64]
//     [channelId: 1]              ← CYCLE-8A NEW (0 = N/A for non-relay commands)
//     [desiredState: 1]           ← CYCLE-8A NEW (0=OFF, 1=ON, 255=N/A)
//     [previousKnownState: 1]     ← CYCLE-8A NEW (what we thought relay was before)
//     [attempt: 1]                 ← CYCLE-8A NEW (retry counter, 0-indexed)
//     [timestamp: 4]                ← CYCLE-8A NEW (unix seconds, uint32)
//     [ackJson: 2+1024]
// =============================================================================
#pragma once
#ifndef TIMER12_SERVICES_TRANSACTION_JOURNAL_H
#define TIMER12_SERVICES_TRANSACTION_JOURNAL_H

#include <Arduino.h>
#include <functional>

namespace Services {

// CYCLE-8A: Transaction state machine.
//   Stored in NVS key tj_state_N (1 byte per slot).
enum class TransactionState : uint8_t {
  PENDING            = 0,  // Intent stored, execute NOT yet called
  EXECUTING          = 1,  // Execute called, commit NOT yet done
  COMMITTED          = 2,  // Execute + commit succeeded
  COMMITTED_UNKNOWN  = 3,  // Reconciled: GPIO matches desired (likely succeeded)
  FAILED             = 4,  // Reconciled: GPIO doesn't match (execute didn't happen or failed)
};

class TransactionJournal {
public:
  void begin();

  // =====================================================================
  // CYCLE-8A: Intent-first API with expanded metadata
  // =====================================================================

  // Store durable INTENT record BEFORE execute. Entry is PENDING (commit=0, state=PENDING).
  // channelId: 1-12 for relay commands, 0 for non-relay commands (schedule/config/etc.)
  // desiredState: true=ON, false=OFF, only meaningful for relay commands (channelId > 0)
  // previousKnownState: what we believe the relay state was BEFORE this command
  // Returns true if intent successfully persisted to NVS.
  bool storeIntent(const String& requestId, const String& commandHash,
                   uint8_t channelId = 0, bool desiredState = false,
                   bool previousKnownState = false);

  // Mark a PENDING entry as EXECUTING (called right before relay.execute()).
  // This is a separate NVS write to narrow the crash window — if crash happens
  // between storeIntent and markExecuting, we know execute was NEVER called.
  // Returns true if state transition succeeded.
  bool markExecuting(const String& requestId);

  // Commit a transaction after execute + ACK JSON ready.
  // Flips commit=0 → commit=1, sets state=COMMITTED, stores ackJson.
  // Returns true if commit succeeded.
  bool commitTransaction(const String& requestId, const String& ackJson);

  // =====================================================================
  // Lookup helpers
  // =====================================================================

  // Returns true if requestId is in journal (any state except FAILED).
  // FAILED entries are NOT considered "processed" — they allow retry.
  bool isProcessed(const String& requestId);

  // Returns true if requestId is COMMITTED or COMMITTED_UNKNOWN.
  bool isCommitted(const String& requestId);

  // Returns the transaction state for a requestId.
  // Returns PENDING if not found (conservative default).
  TransactionState getTransactionState(const String& requestId);

  // Returns commandHash for requestId.
  String getCommandHash(const String& requestId);

  // Returns ackJson for requestId (only if COMMITTED or COMMITTED_UNKNOWN).
  String getAckJson(const String& requestId);

  // Returns channelId for requestId (0 if not a relay command or not found).
  uint8_t getChannelId(const String& requestId);

  // Returns desiredState for requestId (only meaningful for relay commands).
  bool getDesiredState(const String& requestId);

  // =====================================================================
  // CYCLE-8A: Reconciliation — called on boot and on retry of PENDING/EXECUTING
  // =====================================================================

  // Reconcile ALL PENDING and EXECUTING entries with current GPIO state.
  // Called once on boot after journal.loadFromNVS().
  // For each relay command:
  //   - Read GPIO output state via RelayDriver::readLogicalState()
  //   - If GPIO == desiredState → mark COMMITTED_UNKNOWN
  //   - If GPIO != desiredState → mark FAILED
  // For non-relay commands (channelId == 0): leave as-is, log warning.
  // Returns number of entries reconciled.
  uint8_t reconcilePendingEntries();

  // Reconcile a SINGLE entry (used on retry during normal operation).
  // Returns the new state after reconciliation.
  TransactionState reconcileEntry(const String& requestId);

  // =====================================================================
  // Legacy API (kept for backward compat — internally calls commitTransaction)
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

  // Remove a queued ACK (called when immediate publish succeeds).
  void dequeueAck(const String& requestId);

  // Clear a FAILED entry (allows retry with same requestId).
  void clearEntry(const String& requestId);

private:
  static const uint8_t JOURNAL_SIZE = 64;
  static const uint8_t MAX_PENDING_ACKS = 8;
  static const uint8_t MAX_ACK_RETRIES = 10;
  static const unsigned long ACK_RETRY_INTERVAL_MS = 2000;

  // Blob layout (version 2):
  //   [0..1]  magic = 0x54, 0x4A ("TJ")
  //   [2]     version = 2 (CYCLE-8A: was 1)
  //   [3]     reserved (always 0)
  //   [4..7]  CRC32 of payload (bytes 8..end)
  //   [8]     requestId length (max 64)
  //   [9..]   requestId data
  //   [..]    commandHash length (max 64)
  //   [..]    commandHash data
  //   [..]    channelId (1 byte, 0=N/A)
  //   [..]    desiredState (1 byte, 0/1/255)
  //   [..]    previousKnownState (1 byte, 0/1)
  //   [..]    attempt (1 byte)
  //   [..]    timestamp (4 bytes, uint32 LE, unix seconds)
  //   [..]    ackJson length (2 bytes LE, max 1024)
  //   [..]    ackJson data
  static const uint16_t BLOB_MAGIC1 = 0x54;  // 'T'
  static const uint16_t BLOB_MAGIC2 = 0x4A;  // 'J'
  static const uint8_t BLOB_VERSION = 2;       // CYCLE-8A: was 1
  static const uint8_t BLOB_HEADER_SIZE = 8;
  static const uint16_t BLOB_SIZE = 1200;

  String _journalIds[JOURNAL_SIZE];
  String _journalHashes[JOURNAL_SIZE];
  String _journalAcks[JOURNAL_SIZE];
  bool _journalValid[JOURNAL_SIZE];
  bool _journalCommitted[JOURNAL_SIZE];
  // CYCLE-8A: expanded metadata per slot
  TransactionState _journalState[JOURNAL_SIZE];
  uint8_t _journalChannelId[JOURNAL_SIZE];
  bool _journalDesiredState[JOURNAL_SIZE];
  bool _journalPreviousKnownState[JOURNAL_SIZE];
  uint8_t _journalAttempt[JOURNAL_SIZE];
  uint32_t _journalTimestamp[JOURNAL_SIZE];
  uint8_t _journalSize = 0;
  uint8_t _journalWriteIdx = 0;

  struct PendingAck {
    String requestId;
    String ackJson;
    uint8_t retryCount;
    unsigned long lastAttemptMs;
  };
  PendingAck _pendingAcks[MAX_PENDING_ACKS];
  uint8_t _pendingAckCount = 0;

  void _loadFromNVS();

  // Write entry blob + persist writeIdx + optionally commit.
  bool _saveEntryToNVSAtomic(uint8_t idx, bool commitToCommitted);

  // Flip commit flag 0 → 1 + update ackJson for existing entry.
  bool _commitSlotNVS(uint8_t idx, const String& ackJson);

  // CYCLE-8A: Update transaction state for an entry (separate NVS key).
  bool _setTransactionStateNVS(uint8_t idx, TransactionState state);

  void _clearSlotNVS(uint8_t idx);

  bool _deserializeEntry(const uint8_t* blob, size_t len, uint8_t idx);

  uint32_t _computeCRC(const uint8_t* data, size_t len);

  int _findInJournal(const String& requestId);

  // CYCLE-8A: Helper to convert state enum to string (for logging).
  static const char* _stateToString(TransactionState s);
};

extern TransactionJournal journal;
void setPublishCallback(std::function<bool(const char* topic, const uint8_t* payload, size_t len)> cb);

} // namespace Services

#endif
