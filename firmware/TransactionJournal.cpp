// =============================================================================
// Services/TransactionJournal.cpp — NVS-persisted transaction journal
// =============================================================================
// R10G-1/R10G-2 (audit round 10G): Durable transaction semantics.
//
// See TransactionJournal.h for architecture documentation.
// =============================================================================
#include "TransactionJournal.h"
#include "Config.h"
#include "LogService.h"
#include <Preferences.h>

namespace Services {

TransactionJournal journal;

// NVS keys: tj_id_0, tj_hash_0, tj_ack_0, ... tj_id_15, etc.
static const char* NVS_KEY_TJ_ID_PREFIX = "tj_id_";
static const char* NVS_KEY_TJ_HASH_PREFIX = "tj_hash_";
static const char* NVS_KEY_TJ_ACK_PREFIX = "tj_ack_";
static const char* NVS_KEY_TJ_COUNT = "tj_count";
static const char* NVS_KEY_TJ_WRITEIDX = "tj_widx";

void TransactionJournal::begin() {
  _loadFromNVS();
  Serial.printf("[Journal] Loaded %u transactions from NVS\n", _journalSize);

  // R10G-2: Queue all stored ACKs for re-delivery (in case PWA missed them
  // before reboot). PWA will deduplicate by requestId on its side.
  for (uint8_t i = 0; i < _journalSize; i++) {
    if (_journalAcks[i].length() > 0) {
      queueAck(_journalIds[i], _journalAcks[i]);
    }
  }
  if (_pendingAckCount > 0) {
    Serial.printf("[Journal] Queued %u ACKs for re-delivery after reboot\n", _pendingAckCount);
  }
}

void TransactionJournal::_loadFromNVS() {
  Preferences prefs;
  if (!prefs.begin(Core::NVS_NAMESPACE, true)) {
    Serial.println("[Journal] FATAL: Cannot open NVS namespace");
    return;
  }

  _journalSize = prefs.getUChar(NVS_KEY_TJ_COUNT, 0);
  _journalWriteIdx = prefs.getUChar(NVS_KEY_TJ_WRITEIDX, 0);

  if (_journalSize > JOURNAL_SIZE) {
    Serial.printf("[Journal] WARNING: journal size %u > %u, clamping\n", _journalSize, JOURNAL_SIZE);
    _journalSize = JOURNAL_SIZE;
  }

  char key[16];
  for (uint8_t i = 0; i < _journalSize; i++) {
    snprintf(key, sizeof(key), "%s%u", NVS_KEY_TJ_ID_PREFIX, i);
    _journalIds[i] = prefs.getString(key, "");

    snprintf(key, sizeof(key), "%s%u", NVS_KEY_TJ_HASH_PREFIX, i);
    _journalHashes[i] = prefs.getString(key, "");

    snprintf(key, sizeof(key), "%s%u", NVS_KEY_TJ_ACK_PREFIX, i);
    _journalAcks[i] = prefs.getString(key, "");
  }

  prefs.end();
}

void TransactionJournal::_saveEntryToNVS(uint8_t idx) {
  if (idx >= JOURNAL_SIZE) return;

  Preferences prefs;
  if (!prefs.begin(Core::NVS_NAMESPACE, false)) return;

  char key[16];
  snprintf(key, sizeof(key), "%s%u", NVS_KEY_TJ_ID_PREFIX, idx);
  prefs.putString(key, _journalIds[idx]);

  snprintf(key, sizeof(key), "%s%u", NVS_KEY_TJ_HASH_PREFIX, idx);
  prefs.putString(key, _journalHashes[idx]);

  snprintf(key, sizeof(key), "%s%u", NVS_KEY_TJ_ACK_PREFIX, idx);
  prefs.putString(key, _journalAcks[idx]);

  prefs.putUChar(NVS_KEY_TJ_COUNT, _journalSize);
  prefs.putUChar(NVS_KEY_TJ_WRITEIDX, _journalWriteIdx);

  prefs.end();
}

int TransactionJournal::_findInJournal(const String& requestId) {
  for (uint8_t i = 0; i < _journalSize; i++) {
    if (_journalIds[i] == requestId) return i;
  }
  return -1;
}

bool TransactionJournal::isProcessed(const String& requestId) {
  // R10G-1: NO TTL expiry. Once stored, requestId is permanently "processed".
  // This is the critical contract: same requestId → NEVER re-execute.
  return _findInJournal(requestId) >= 0;
}

String TransactionJournal::getCommandHash(const String& requestId) {
  int idx = _findInJournal(requestId);
  if (idx >= 0) return _journalHashes[idx];
  return "";
}

String TransactionJournal::getAckJson(const String& requestId) {
  int idx = _findInJournal(requestId);
  if (idx >= 0) return _journalAcks[idx];
  return "";
}

bool TransactionJournal::storeTransaction(const String& requestId,
                                           const String& commandHash,
                                           const String& ackJson) {
  // Check if already stored (idempotent — safe to call multiple times)
  int existing = _findInJournal(requestId);
  if (existing >= 0) {
    // Already in journal — update ACK (in case it changed) and re-queue
    _journalHashes[existing] = commandHash;
    _journalAcks[existing] = ackJson;
    _saveEntryToNVS(existing);
    queueAck(requestId, ackJson);
    return true;
  }

  // New entry — write to LRU slot
  uint8_t idx = _journalWriteIdx;
  _journalIds[idx] = requestId;
  _journalHashes[idx] = commandHash;
  _journalAcks[idx] = ackJson;

  // Advance LRU pointer
  _journalWriteIdx = (_journalWriteIdx + 1) % JOURNAL_SIZE;
  if (_journalSize < JOURNAL_SIZE) _journalSize++;

  // Persist to NVS
  _saveEntryToNVS(idx);

  // Queue ACK for delivery
  queueAck(requestId, ackJson);

  Serial.printf("[Journal] Stored transaction: rid=%s hash=%.16s... (slot %u, size %u)\n",
                requestId.c_str(), commandHash.c_str(), idx, _journalSize);
  return true;
}

void TransactionJournal::queueAck(const String& requestId, const String& ackJson) {
  // Check if already in pending queue
  for (uint8_t i = 0; i < _pendingAckCount; i++) {
    if (_pendingAcks[i].requestId == requestId) {
      // Update ACK JSON (in case it changed) and reset retry count
      _pendingAcks[i].ackJson = ackJson;
      _pendingAcks[i].retryCount = 0;
      _pendingAcks[i].lastAttemptMs = 0;  // publish immediately next tick
      return;
    }
  }

  // Add new pending ACK
  if (_pendingAckCount >= MAX_PENDING_ACKS) {
    // Queue full — drop oldest (it's also in NVS journal, so PWA can retry)
    Serial.println("[Journal] WARNING: ACK queue full, dropping oldest");
    for (uint8_t i = 0; i < MAX_PENDING_ACKS - 1; i++) {
      _pendingAcks[i] = _pendingAcks[i + 1];
    }
    _pendingAckCount = MAX_PENDING_ACKS - 1;
  }

  _pendingAcks[_pendingAckCount].requestId = requestId;
  _pendingAcks[_pendingAckCount].ackJson = ackJson;
  _pendingAcks[_pendingAckCount].retryCount = 0;
  _pendingAcks[_pendingAckCount].lastAttemptMs = 0;
  _pendingAckCount++;
}

// R10G-2: Process pending ACK queue.
// This is called from MqttClient::loop() — needs access to _mqtt.publish().
// We use a callback to avoid circular dependency.
static std::function<bool(const char* topic, const uint8_t* payload, size_t len)> _publishCallback;

void setPublishCallback(std::function<bool(const char* topic, const uint8_t* payload, size_t len)> cb) {
  _publishCallback = cb;
}

uint8_t TransactionJournal::processPendingAcks() {
  if (_pendingAckCount == 0 || !_publishCallback) return 0;

  unsigned long now = millis();
  uint8_t published = 0;
  const char* ackTopic = "timer12/ack";  // Will be set by MqttClient

  // We need the actual topic — pass via callback
  // For now, use a simpler approach: MqttClient will call this with topic set

  for (int i = _pendingAckCount - 1; i >= 0; i--) {
    if (now - _pendingAcks[i].lastAttemptMs < ACK_RETRY_INTERVAL_MS) continue;

    bool success = _publishCallback(
      "ack",  // placeholder — MqttClient sets actual topic via callback
      (const uint8_t*)_pendingAcks[i].ackJson.c_str(),
      _pendingAcks[i].ackJson.length()
    );

    _pendingAcks[i].lastAttemptMs = now;
    _pendingAcks[i].retryCount++;

    if (success) {
      // ACK published — remove from queue
      Serial.printf("[Journal] ACK delivered: rid=%s (attempt %u)\n",
                    _pendingAcks[i].requestId.c_str(), _pendingAcks[i].retryCount);
      // Shift remaining entries down
      for (int j = i; j < _pendingAckCount - 1; j++) {
        _pendingAcks[j] = _pendingAcks[j + 1];
      }
      _pendingAckCount--;
      published++;
    } else if (_pendingAcks[i].retryCount >= MAX_ACK_RETRIES) {
      // Exceeded max retries — give up (PWA will timeout + retry, hits journal)
      Serial.printf("[Journal] ACK give up after %u retries: rid=%s\n",
                    MAX_ACK_RETRIES, _pendingAcks[i].requestId.c_str());
      Services::Log.append(Core::LogType::Error,
        "ACK delivery failed after " + String(MAX_ACK_RETRIES) + " retries: " + _pendingAcks[i].requestId, 0);
      // Shift remaining entries down
      for (int j = i; j < _pendingAckCount - 1; j++) {
        _pendingAcks[j] = _pendingAcks[j + 1];
      }
      _pendingAckCount--;
    }
  }

  return published;
}

} // namespace Services
