// =============================================================================
// Services/TransactionJournal.h — Durable transaction journal for MQTT commands
// =============================================================================
// R10G-1/R10G-2 (audit round 10G): NVS-persisted transaction journal.
//
// PROBLEM (engineer audit R10F):
//   - RAM-only dedup buffer lost on reboot → same requestId re-executed
//   - publish()==true doesn't guarantee PWA received ACK (QoS 0)
//   - 15-min TTL allowed re-execution after expiry (dangerous for non-idempotent)
//   - No ACK retry → PWA timeout → retry → re-execute
//
// SOLUTION:
//   1. Transaction Journal (NVS-persisted):
//      - Stores {requestId, commandHash, ackJson} for successful commands
//      - Survives reboot (NVS flash)
//      - same requestId → NEVER execute again → always replay result
//      - No TTL expiry (requestId is permanent once stored)
//      - Buffer size: 16 entries (LRU eviction — oldest overwritten when full)
//
//   2. ACK Retry Queue (RAM, rebuilt from NVS on boot):
//      - After execute, ACK is queued for delivery
//      - Loop() retries pending ACKs every 2s
//      - Max 10 retries per ACK, then give up (PWA will timeout + retry,
//        which hits journal → replay, no re-execution)
//      - Bounded: max 8 pending ACKs in queue
//
// TRADE-OFF:
//   - NVS write per command: ~100k writes per flash sector, 16 sectors
//   - At 100 commands/day: ~3.6 years flash lifetime (acceptable)
//   - For high-frequency commands: consider wear-leveling or FRAM
//
// CRITICAL CONTRACT:
//   If requestId is in journal → command was already executed successfully.
//   Firmware MUST NOT re-execute. Always replay stored ackJson.
//   This holds across reboots, TTL expiry, and publish failures.
// =============================================================================
#pragma once
#ifndef TIMER12_SERVICES_TRANSACTION_JOURNAL_H
#define TIMER12_SERVICES_TRANSACTION_JOURNAL_H

#include <Arduino.h>
#include <functional>

namespace Services {

class TransactionJournal {
public:
  // Initialize — loads journal from NVS into RAM cache.
  // Called once in setup() BEFORE MQTT client begins.
  void begin();

  // Check if requestId exists in journal (command already executed).
  // Returns true if found (regardless of age — NO TTL expiry).
  bool isProcessed(const String& requestId);

  // Get the commandHash stored for a requestId.
  // Returns empty string if not found.
  String getCommandHash(const String& requestId);

  // Get the ACK JSON stored for a requestId (for replay).
  // Returns empty string if not found.
  String getAckJson(const String& requestId);

  // Store a successful transaction.
  // Called AFTER command executed + ACK JSON constructed.
  // The ACK will be queued for delivery via queueAck().
  // Returns true if stored successfully.
  bool storeTransaction(const String& requestId, const String& commandHash,
                        const String& ackJson);

  // Queue an ACK for delivery (or re-delivery).
  // Called by storeTransaction (first delivery) and by loop() (retries).
  void queueAck(const String& requestId, const String& ackJson);

  // Process pending ACK queue — called from loop().
  // Retries publishing ACKs that haven't been confirmed delivered.
  // Returns number of ACKs successfully published this tick.
  uint8_t processPendingAcks();

  // Get count of pending ACKs (for diagnostics).
  uint8_t getPendingAckCount() const { return _pendingAckCount; }

  // Get count of transactions in journal.
  uint8_t getJournalSize() const { return _journalSize; }

private:
  static const uint8_t JOURNAL_SIZE = 16;  // NVS entries (LRU)
  static const uint8_t MAX_PENDING_ACKS = 8;
  static const uint8_t MAX_ACK_RETRIES = 10;
  static const unsigned long ACK_RETRY_INTERVAL_MS = 2000;  // 2s between retries

  // RAM cache of NVS journal (loaded on boot)
  String _journalIds[JOURNAL_SIZE];
  String _journalHashes[JOURNAL_SIZE];
  String _journalAcks[JOURNAL_SIZE];
  uint8_t _journalSize = 0;
  uint8_t _journalWriteIdx = 0;  // LRU write pointer

  // Pending ACK queue (RAM only — rebuilt from journal on boot)
  struct PendingAck {
    String requestId;
    String ackJson;
    uint8_t retryCount;
    unsigned long lastAttemptMs;
  };
  PendingAck _pendingAcks[MAX_PENDING_ACKS];
  uint8_t _pendingAckCount = 0;

  void _loadFromNVS();
  void _saveEntryToNVS(uint8_t idx);
  int _findInJournal(const String& requestId);
};

extern TransactionJournal journal;

// R10G-2: Set publish callback for ACK retry queue.
// Called by MqttClient::begin() to inject the MQTT publish function.
// TransactionJournal uses this to publish ACKs from processPendingAcks().
void setPublishCallback(std::function<bool(const char* topic, const uint8_t* payload, size_t len)> cb);

} // namespace Services

#endif
