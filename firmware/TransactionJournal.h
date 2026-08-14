// =============================================================================
// Services/TransactionJournal.h — Durable transaction journal for MQTT commands
// =============================================================================
// R10G/R10H (audit rounds 10G/10H): NVS-persisted transaction journal.
//
// R10H-1 FIX: NVS atomicity. Previous version did 5 separate NVS writes
// (3 putString + 2 putUChar). Power loss between writes → partial state.
// FIX: Serialize entire entry to single blob → putBytes() atomic write.
//
// R10H-2 FIX: LRU eviction. Previous 16-entry buffer evicted oldest after
// 16 commands, breaking "permanent dedup" claim. Increased to 64 entries.
// Documented honestly: beyond 64 commands, oldest entries CAN be re-executed.
//
// R10H-3: Commit-flag pattern. Each entry has a "valid" flag. On boot,
// entries with flag=false are treated as corrupted (power loss during write)
// and are NOT loaded. This prevents partial entries from causing false
// "already processed" results.
//
// DURABILITY BOUNDARY (honest documentation):
//   The sequence is: execute command → store to NVS → publish ACK.
//   If ESP32 crashes AFTER execute but BEFORE store:
//     - Physical state has changed (relay moved, schedule saved to LittleFS)
//     - Journal does NOT have the entry
//     - PWA retry → journal miss → RE-EXECUTE
//   For SET_STATE (relay ON/OFF): idempotent, re-execute produces same result.
//   For schedule upsert: may create duplicate (but capped at 4 per channel).
//   For config/time: idempotent (overwrite).
//   For OTA: not stored in journal (separate flow with Update library).
//
//   This is a FUNDAMENTAL limitation of software-only transaction durability
//   on ESP32 without hardware transaction support. Accepted as documented risk.
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
  bool isProcessed(const String& requestId);
  String getCommandHash(const String& requestId);
  String getAckJson(const String& requestId);
  bool storeTransaction(const String& requestId, const String& commandHash,
                        const String& ackJson);
  void queueAck(const String& requestId, const String& ackJson);
  uint8_t processPendingAcks();
  uint8_t getPendingAckCount() const { return _pendingAckCount; }
  uint8_t getJournalSize() const { return _journalSize; }

private:
  // R10H-2: Increased from 16 to 64 entries.
  // At 100 commands/day, oldest entry is ~15 hours old when evicted.
  // This gives PWA ample retry window (PWA timeout is 5s, retries for ~2min).
  // Beyond 64 commands, oldest entries CAN be re-executed — documented limitation.
  static const uint8_t JOURNAL_SIZE = 64;
  static const uint8_t MAX_PENDING_ACKS = 8;
  static const uint8_t MAX_ACK_RETRIES = 10;
  static const unsigned long ACK_RETRY_INTERVAL_MS = 2000;

  // R10H-1: Max blob size for NVS write (must fit requestId + hash + ackJson)
  // requestId: max 64 chars + null = 65
  // commandHash: 64 hex chars + null = 65
  // ackJson: max ~1024 chars (relay/schedule/pir/channel ACK all < 500 bytes)
  // valid flag: 1 byte
  // Total: ~1156 bytes, round up to 1200
  static const uint16_t BLOB_SIZE = 1200;

  // RAM cache of NVS journal
  String _journalIds[JOURNAL_SIZE];
  String _journalHashes[JOURNAL_SIZE];
  String _journalAcks[JOURNAL_SIZE];
  bool _journalValid[JOURNAL_SIZE];  // R10H-3: commit flag
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
  // R10H-1: Atomic blob write — serializes entry to single buffer, writes once.
  void _saveEntryToNVSAtomic(uint8_t idx);
  // R10H-1: Deserialize blob back to fields
  bool _deserializeEntry(const uint8_t* blob, size_t len, uint8_t idx);
  int _findInJournal(const String& requestId);
};

extern TransactionJournal journal;
void setPublishCallback(std::function<bool(const char* topic, const uint8_t* payload, size_t len)> cb);

} // namespace Services

#endif
