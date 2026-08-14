// =============================================================================
// Services/TransactionJournal.h — Durable transaction journal for MQTT commands
// =============================================================================
// CYCLE-8B (C8A-001 P0): Deterministic Boot Recovery
//
// PROBLEM (auditor C8A-001):
//   Cycle 8A's reconciliation ran AFTER RelayEngine.forceRefresh() had already
//   changed GPIO outputs based on scheduler/PIR/manual logic. This contaminated
//   the evidence — GPIO no longer reflected "state at crash" but "state after
//   boot initialization". This produced false FAILED and false COMMITTED_UNKNOWN.
//
// CYCLE-8B SOLUTION: BootRecoveryPhase — capture raw output state BEFORE any
//   application logic runs, then reconcile using that snapshot.
//
//   New boot sequence:
//     1. Minimal safe GPIO init (OUTPUT, all OFF — known safe state)
//        NOTE: this DOES change GPIO, but to a known-safe value (OFF).
//        We capture the snapshot AFTER this, so the snapshot reflects the
//        safe state, not the pre-crash state. This is a CONSCIOUS TRADE-OFF:
//          - Pre-crash state is unrecoverable without battery-backed GPIO register
//          - Safe-state init prevents relay glitches during boot
//          - Reconciliation now knows the "starting point" deterministically
//     2. Load NVS: config, journal, RTC
//     3. captureOutputSnapshot() — read all GPIO pins, store in RAM
//        (This snapshot is the BASELINE for reconciliation)
//     4. reconcileWithSnapshot() — compare journal desiredState to snapshot
//        - For PENDING entries: execute DEFINITELY didn't run (journal says so)
//          → mark FAILED (allow retry) — CORRECT
//        - For EXECUTING entries: execute MAY have run, but GPIO is now OFF
//          (safe init). Cannot determine → mark COMMITTED_UNKNOWN (conservative)
//     5. Restore application state (scheduler, PIR, manual overrides from NVS)
//     6. Start RelayEngine (NOW it can apply logic, after recovery is done)
//     7. Switch to RUNNING mode — accept commands, PIR triggers, scheduler ticks
//
//   HONEST LIMITATION:
//     The snapshot is taken AFTER safe-state init, so it does NOT reflect
//     pre-crash GPIO state. This means:
//       - We CANNOT determine if execute ran before crash (only journal state tells us)
//       - PENDING entries: execute DEFINITELY didn't run (journal says so)
//         → mark FAILED (allow retry) — this is CORRECT
//       - EXECUTING entries: execute MAY have run, but GPIO is now OFF (safe init)
//         → mark COMMITTED_UNKNOWN (conservative) — PWA gets disclaimer
//     This is the best we can do without battery-backed GPIO register.
//     For true pre-crash GPIO recovery, hardware revision with GPIO state
//     preservation (or latching relays) is needed.
//
// CYCLE-8A (unchanged):
//   Transaction states: PENDING, EXECUTING, COMMITTED, COMMITTED_UNKNOWN, FAILED
//   storeIntent(), markExecuting(), commitTransaction(), clearEntry()
// =============================================================================
#pragma once
#ifndef TIMER12_SERVICES_TRANSACTION_JOURNAL_H
#define TIMER12_SERVICES_TRANSACTION_JOURNAL_H

#include <Arduino.h>
#include <functional>

namespace Services {

// CYCLE-8A: Transaction state machine.
enum class TransactionState : uint8_t {
  PENDING            = 0,
  EXECUTING          = 1,
  COMMITTED          = 2,
  COMMITTED_UNKNOWN  = 3,
  FAILED             = 4,
};

// CYCLE-8B: System boot phase — controls what can modify relay state.
enum class BootPhase : uint8_t {
  PRE_INIT       = 0,  // Before any initialization (relays in unknown state)
  SAFE_INIT      = 1,  // GPIO set to OUTPUT, all OFF (known safe state)
  LOADING_NVS    = 2,  // Loading config, journal, RTC from NVS
  SNAPSHOT       = 3,  // Capturing raw GPIO output state (read-only)
  RECONCILING    = 4,  // Reconciling incomplete transactions
  RESTORING      = 5,  // Restoring application state (scheduler, PIR config)
  RUNNING        = 6,  // Normal operation — RelayEngine active, commands accepted
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
  // CYCLE-8B: Raw output snapshot — capture BEFORE RelayEngine runs
  // =====================================================================

  // Capture current GPIO output state for ALL relay channels into RAM.
  // Must be called AFTER RelayDriver.begin() (which sets safe OFF state)
  // but BEFORE RelayEngine.forceRefresh() (which applies scheduler/PIR logic).
  void captureOutputSnapshot();

  // Returns the captured snapshot state for a channel (false if not captured).
  bool getSnapshotState(uint8_t channelIdx) const;

  // =====================================================================
  // CYCLE-8A: Intent-first API
  // =====================================================================
  bool storeIntent(const String& requestId, const String& commandHash,
                   uint8_t channelId = 0, bool desiredState = false,
                   bool previousKnownState = false);

  bool markExecuting(const String& requestId);

  bool commitTransaction(const String& requestId, const String& ackJson);

  // =====================================================================
  // Lookup helpers
  // =====================================================================

  // CYCLE-8B: isProcessed EXCLUDES FAILED entries (allow retry).
  // PENDING/EXECUTING are "in-flight" — caller should reconcile first.
  bool isProcessed(const String& requestId);

  bool isCommitted(const String& requestId);

  TransactionState getTransactionState(const String& requestId);

  String getCommandHash(const String& requestId);

  String getAckJson(const String& requestId);

  uint8_t getChannelId(const String& requestId);

  bool getDesiredState(const String& requestId);

  // =====================================================================
  // CYCLE-8B: Reconciliation — uses SNAPSHOT (not live GPIO)
  // =====================================================================

  // Reconcile ALL PENDING and EXECUTING entries using the captured snapshot.
  // Must be called AFTER captureOutputSnapshot() and BEFORE RelayEngine starts.
  uint8_t reconcilePendingEntries();

  // Reconcile a SINGLE entry (used on retry during normal operation).
  // Uses LIVE GPIO read (after RUNNING phase, snapshot is stale).
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

  // CYCLE-8B: clearEntry now allows clearing PENDING, EXECUTING, and FAILED.
  // NOT allowed: COMMITTED, COMMITTED_UNKNOWN (these are durable).
  // Fixes C8A-005: invalid commands that leave PENDING entries can now be cleared.
  void clearEntry(const String& requestId);

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

  // CYCLE-8B: boot phase tracking
  BootPhase _bootPhase = BootPhase::PRE_INIT;

  // CYCLE-8B: raw output snapshot (captured before RelayEngine runs)
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

  bool _saveEntryToNVSAtomic(uint8_t idx, bool commitToCommitted);

  bool _commitSlotNVS(uint8_t idx, const String& ackJson);

  bool _setTransactionStateNVS(uint8_t idx, TransactionState state);

  void _clearSlotNVS(uint8_t idx);

  bool _deserializeEntry(const uint8_t* blob, size_t len, uint8_t idx);

  uint32_t _computeCRC(const uint8_t* data, size_t len);

  int _findInJournal(const String& requestId);

  static const char* _stateToString(TransactionState s);
  static const char* _phaseToString(BootPhase p);
};

extern TransactionJournal journal;
void setPublishCallback(std::function<bool(const char* topic, const uint8_t* payload, size_t len)> cb);

} // namespace Services

#endif
