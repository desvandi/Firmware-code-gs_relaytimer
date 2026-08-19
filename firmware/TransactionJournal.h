// =============================================================================
// Services/TransactionJournal.h — Durable transaction journal for MQTT commands
// =============================================================================
// R10G/R10H/R10I (audit rounds 10G/10H/10I): NVS-persisted transaction journal.
//
// R10H-1: Single blob write (putBytes) instead of 5 separate writes.
// R10H-2: Journal size 16 → 64 entries.
// R10H-3: Commit flag (valid byte).
//
// R10I-1 (CRITICAL): Added magic + version + CRC32 to blob format.
//   Engineer audit: "valid=1 saja tidak cukup untuk membuktikan blob tidak
//   korup akibat power-loss."
//   FIX: Blob now has:
//     [magic:2] [version:1] [valid:1] [CRC32:4]
//     [idLen:1] [id:idLen] [hashLen:1] [hash:hashLen]
//     [ackLen:2] [ack:ackLen]
//   CRC32 covers everything AFTER the CRC field (payload).
//   On load: verify magic + version + CRC. If any mismatch → entry is CORRUPT,
//   skipped, and slot is marked free.
//
// R10I-2 (CRITICAL): Two-phase commit via valid byte.
//   Phase 1: Write blob with valid=0 (INCOMPLETE marker).
//   Phase 2: Overwrite ONLY the valid byte to valid=1 (COMMITTED).
//   If power loss during Phase 1: valid=0 → entry rejected on boot.
//   If power loss during Phase 2: valid=0 still → entry rejected on boot.
//   Only if Phase 2 completes: valid=1 + CRC valid → entry accepted.
//
//   NOTE: This is a "best effort" two-phase commit. ESP32 NVS (Preferences)
//   does not provide true atomic blob writes. The valid-byte flip is a single
//   small write that is much less likely to be interrupted than a full blob
//   write. Combined with CRC, this provides strong integrity protection.
//
// R10I-3: Check putBytes() return value. If write fails, mark slot as invalid.
//
// R10I-4: Slot replacement: before writing new entry to LRU slot, clear old
//   entry first (write valid=0). This prevents "double valid" scenario where
//   old + new data both look valid after partial overwrite.
//
// DURABILITY BOUNDARY (honest documentation):
//   Sequence: execute command → store to NVS → publish ACK.
//   If ESP32 crashes AFTER execute but BEFORE store:
//     - Physical state changed (relay moved, schedule saved to LittleFS)
//     - Journal does NOT have the entry
//     - PWA retry → journal miss → RE-EXECUTE
//   For SET_STATE (relay ON/OFF): idempotent, safe.
//   For schedule upsert: may create duplicate (capped at 4 per channel).
//   For config/time: idempotent (overwrite).
//   For OTA: not stored in journal (separate flow).
//   This is FUNDAMENTAL limitation without hardware transaction support.
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
  static const uint8_t JOURNAL_SIZE = 64;
  static const uint8_t MAX_PENDING_ACKS = 8;
  static const uint8_t MAX_ACK_RETRIES = 10;
  static const unsigned long ACK_RETRY_INTERVAL_MS = 2000;

  // R10I-1: Blob layout:
  //   [0..1]  magic = 0x54, 0x4A ("TJ")
  //   [2]     version = 1
  //   [3]     valid flag (0=incomplete, 1=committed)
  //   [4..7]  CRC32 of payload (bytes 8..end)
  //   [8]     requestId length (max 64)
  //   [9..]   requestId data
  //   [..]    commandHash length (max 64)
  //   [..]    commandHash data
  //   [..]    ackJson length (2 bytes LE, max 1024)
  //   [..]    ackJson data
  //
  // Max size: 8 (header) + 1+64 + 1+64 + 2+1024 = 1164 bytes.
  // BLOB_SIZE = 1200 (with margin).
  static const uint16_t BLOB_MAGIC1 = 0x54;  // 'T'
  static const uint16_t BLOB_MAGIC2 = 0x4A;  // 'J'
  static const uint8_t BLOB_VERSION = 1;
  static const uint8_t BLOB_HEADER_SIZE = 8;  // magic(2) + ver(1) + valid(1) + CRC(4)
  static const uint16_t BLOB_SIZE = 1200;

  String _journalIds[JOURNAL_SIZE];
  String _journalHashes[JOURNAL_SIZE];
  String _journalAcks[JOURNAL_SIZE];
  bool _journalValid[JOURNAL_SIZE];
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

  // R10I-2: Two-phase commit
  // Phase 1: serialize blob with valid=0, write to NVS.
  // Phase 2: flip valid byte to 1, rewrite blob.
  // Returns true if both phases succeed.
  bool _saveEntryToNVSAtomic(uint8_t idx);

  // R10I-4: Clear slot (write valid=0) before new entry
  void _clearSlotNVS(uint8_t idx);

  // R10I-1: Deserialize + verify magic + version + CRC
  // Returns true if entry is valid and committed.
  bool _deserializeEntry(const uint8_t* blob, size_t len, uint8_t idx);

  // R10I-1: Compute CRC32 of payload (everything after CRC field)
  uint32_t _computeCRC(const uint8_t* data, size_t len);

  int _findInJournal(const String& requestId);
};

extern TransactionJournal journal;
void setPublishCallback(std::function<bool(const char* topic, const uint8_t* payload, size_t len)> cb);

} // namespace Services

#endif
