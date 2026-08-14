// =============================================================================
// Services/TransactionJournal.h — Durable transaction journal for MQTT commands
// =============================================================================
// R10G/R10H/R10I/R10J/R10K (audit rounds 10G-10K): NVS-persisted transaction journal.
//
// CYCLE-7 (auditor #7): INTENT-FIRST JOURNALING (fixes F-001 + F-002 P0).
//
// PROBLEM (auditor finding F-001/F-002):
//   Previous flow: validate → execute → store transaction → publish ACK.
//   Crash window between execute and store → journal miss on retry → re-execute.
//   Also: storeTransaction() return value was ignored by ACK publishers.
//
// NEW ARCHITECTURE (intent-first):
//   validate → storeIntent(requestId, commandHash) → execute → commitTransaction(requestId, ackJson) → publish ACK.
//
//   storeIntent() writes a durable PENDING entry (commit=0) BEFORE execute.
//   isProcessed() returns true for BOTH PENDING and COMMITTED entries.
//   commitTransaction() flips commit=0 → commit=1 (durable ACK record).
//
//   Crash scenarios (all safe for idempotent commands):
//     - Crash before storeIntent: no entry → PWA retry → re-execute (idempotent, safe).
//     - Crash after storeIntent, before execute: PENDING entry → PWA retry →
//         isProcessed()=true, hash matches → return "in-progress" ACK.
//         No re-execution. Entry will be garbage-collected on LRU eviction.
//     - Crash after execute, before commitTransaction: PENDING entry →
//         same as above. Physical state already changed (idempotent). Safe.
//     - Crash after commitTransaction, before publish: COMMITTED entry →
//         PWA retry → isProcessed()=true → replay stored ACK.
//     - Crash after publish: PWA received ACK, journal has COMMITTED entry. Done.
//
//   ACK publishers MUST check commitTransaction() return value. If commit fails:
//   - Do NOT publish success ACK (would falsely claim durable success).
//   - Publish failure ACK with DURABILITY_FAILURE message.
//   - PWA can retry; idempotent commands will re-execute safely.
//
// DURABILITY BOUNDARY (honest documentation, fixes F-004):
//   "exactly-once within retention window" (64 entries, LRU eviction).
//   NOT "permanent, never re-execute". After eviction, requestId can be re-used.
//   For long-term protection: use unique requestId per command (PWA already does).
//
// STORAGE LAYOUT (R10J/R10K, unchanged):
//   tj_entry_N: blob [magic(2) + ver(1) + reserved(1) + CRC32(4) + payload]
//   tj_commit_N: single byte (0=PENDING, 1=COMMITTED)
//   tj_count:   journal size (metadata only)
//   tj_widx:    next write slot index
//
//   Two-phase commit (per slot):
//     Phase 0: clear commit (write 0) — invalidates old entry
//     Phase 1: write blob (commit still 0) — durable data
//     Phase 1b: persist writeIdx — cursor advance
//     Phase 2: set commit=1 — atomic commit point (single byte write)
// =============================================================================
#pragma once
#ifndef TIMER12_SERVICES_TRANSACTION_JOURNAL_H
#define TIMER12_SERVICES_TRANSACTION_JOURNAL_H

#include <Arduino.h>
#include <functional>

namespace Services {

class TransactionJournal {
public:
  void begin();

  // =====================================================================
  // CYCLE-7: Intent-first API (replaces blind storeTransaction for new code)
  // =====================================================================

  // Store durable INTENT record BEFORE execute. Entry is PENDING (commit=0).
  // Returns true if intent successfully persisted to NVS.
  // Caller MUST check return value — if false, do NOT execute.
  // On duplicate requestId (already PENDING or COMMITTED):
  //   - Returns false (caller should treat as duplicate and let _handleCommand
  //     handle via isProcessed() path).
  bool storeIntent(const String& requestId, const String& commandHash);

  // Commit a PENDING transaction after execute + ACK JSON ready.
  // Flips commit=0 → commit=1 and stores ackJson.
  // Returns true if commit succeeded.
  // If commit fails: caller MUST NOT publish success ACK.
  bool commitTransaction(const String& requestId, const String& ackJson);

  // =====================================================================
  // Lookup helpers (check BOTH PENDING and COMMITTED entries)
  // =====================================================================

  // Returns true if requestId is in journal (PENDING or COMMITTED).
  bool isProcessed(const String& requestId);

  // Returns true if requestId is COMMITTED (has ackJson ready to replay).
  bool isCommitted(const String& requestId);

  // Returns commandHash for requestId (PENDING or COMMITTED).
  // Empty string if not found.
  String getCommandHash(const String& requestId);

  // Returns ackJson for requestId (only meaningful if COMMITTED).
  // Empty string if not found or still PENDING.
  String getAckJson(const String& requestId);

  // =====================================================================
  // Legacy API (kept for backward compat — internally calls commitTransaction)
  // =====================================================================
  bool storeTransaction(const String& requestId, const String& commandHash,
                        const String& ackJson);

  // =====================================================================
  // ACK delivery queue (unchanged from R10G-2)
  // =====================================================================
  void queueAck(const String& requestId, const String& ackJson);
  uint8_t processPendingAcks();
  uint8_t getPendingAckCount() const { return _pendingAckCount; }
  uint8_t getJournalSize() const { return _journalSize; }

  // Remove a queued ACK (called when immediate publish succeeds — fixes F-006).
  void dequeueAck(const String& requestId);

private:
  static const uint8_t JOURNAL_SIZE = 64;
  static const uint8_t MAX_PENDING_ACKS = 8;
  static const uint8_t MAX_ACK_RETRIES = 10;
  static const unsigned long ACK_RETRY_INTERVAL_MS = 2000;

  // R10I-1: Blob layout (unchanged):
  //   [0..1]  magic = 0x54, 0x4A ("TJ")
  //   [2]     version = 1
  //   [3]     reserved (always 0 — commit flag is in separate NVS key)
  //   [4..7]  CRC32 of payload (bytes 8..end)
  //   [8]     requestId length (max 64)
  //   [9..]   requestId data
  //   [..]    commandHash length (max 64)
  //   [..]    commandHash data
  //   [..]    ackJson length (2 bytes LE, max 1024)
  //   [..]    ackJson data
  static const uint16_t BLOB_MAGIC1 = 0x54;  // 'T'
  static const uint16_t BLOB_MAGIC2 = 0x4A;  // 'J'
  static const uint8_t BLOB_VERSION = 1;
  static const uint8_t BLOB_HEADER_SIZE = 8;  // magic(2) + ver(1) + reserved(1) + CRC(4)
  static const uint16_t BLOB_SIZE = 1200;

  String _journalIds[JOURNAL_SIZE];
  String _journalHashes[JOURNAL_SIZE];
  String _journalAcks[JOURNAL_SIZE];
  bool _journalValid[JOURNAL_SIZE];   // entry exists (PENDING or COMMITTED)
  bool _journalCommitted[JOURNAL_SIZE]; // entry is COMMITTED (commit=1)
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

  // R10J/R10K: Two-phase commit (clear → write blob → writeIdx → commit=1).
  // commitToCommitted=false: write with commit=0 (PENDING intent).
  // commitToCommitted=true:  also set commit=1 (full commit).
  // For new entries: call with commitToCommitted=false for PENDING,
  //   then commitTransaction() flips commit=1 via _commitSlotNVS().
  // For updates to existing entries: caller specifies behavior.
  bool _saveEntryToNVSAtomic(uint8_t idx, bool commitToCommitted);

  // CYCLE-7: Flip commit flag from 0 → 1 for an existing PENDING entry.
  // Does NOT advance writeIdx (entry already exists in journal).
  // Returns true if commit succeeded.
  bool _commitSlotNVS(uint8_t idx, const String& ackJson);

  // R10I-4: Clear slot (write commit=0) before new entry
  void _clearSlotNVS(uint8_t idx);

  // R10I-1: Deserialize + verify magic + version + CRC
  bool _deserializeEntry(const uint8_t* blob, size_t len, uint8_t idx);

  // R10I-1: Compute CRC32 of payload
  uint32_t _computeCRC(const uint8_t* data, size_t len);

  // Find slot index for requestId (PENDING or COMMITTED). -1 if not found.
  int _findInJournal(const String& requestId);
};

extern TransactionJournal journal;
void setPublishCallback(std::function<bool(const char* topic, const uint8_t* payload, size_t len)> cb);

} // namespace Services

#endif
