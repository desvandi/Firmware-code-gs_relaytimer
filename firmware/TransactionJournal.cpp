// =============================================================================
// Services/TransactionJournal.cpp — NVS-persisted transaction journal
// =============================================================================
// R10H-1: Atomic NVS writes via single blob (putBytes).
// R10H-2: Journal size increased to 64 entries.
// R10H-3: Commit-flag pattern — corrupted entries rejected on load.
// =============================================================================
#include "TransactionJournal.h"
#include "Config.h"
#include "LogService.h"
#include <Preferences.h>
#include <string.h>

namespace Services {

TransactionJournal journal;

// R10H-1: Single blob key per entry — atomic write.
// Key format: tj_entry_0, tj_entry_1, ... tj_entry_63
// Metadata: tj_count, tj_widx (also written atomically via putUChar)
static const char* NVS_KEY_TJ_ENTRY_PREFIX = "tj_entry_";
static const char* NVS_KEY_TJ_COUNT = "tj_count";
static const char* NVS_KEY_TJ_WIDX = "tj_widx";

// R10H-1: Blob format (packed, no padding):
//   [0]     valid flag (1 = committed, 0 = corrupted/in-progress)
//   [1]     requestId length (1 byte, max 64)
//   [2..2+len] requestId data
//   [2+len] commandHash length (1 byte, max 64)
//   [3+len..3+len+hashLen] commandHash data
//   [3+len+hashLen] ackJson length (2 bytes, little-endian, max 1024)
//   [5+len+hashLen..] ackJson data
//
// This is NOT portable across architectures (endianness), but ESP32 is
// little-endian and we only read our own writes, so it's safe here.

void TransactionJournal::begin() {
  _loadFromNVS();
  Serial.printf("[Journal] Loaded %u valid transactions from NVS (capacity %u)\n",
                _journalSize, JOURNAL_SIZE);

  // R10G-2: Queue all stored ACKs for re-delivery after reboot
  for (uint8_t i = 0; i < _journalSize; i++) {
    if (_journalValid[i] && _journalAcks[i].length() > 0) {
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

  _journalSize = 0;
  _journalWriteIdx = prefs.getUChar(NVS_KEY_TJ_WIDX, 0);

  char key[20];
  uint8_t blob[BLOB_SIZE];

  // R10H-3: Load entries with commit-flag validation.
  // An entry is only loaded if valid flag == 1.
  // Entries with valid flag == 0 (power loss during write) are SKIPPED.
  for (uint8_t i = 0; i < JOURNAL_SIZE; i++) {
    snprintf(key, sizeof(key), "%s%u", NVS_KEY_TJ_ENTRY_PREFIX, i);
    size_t len = prefs.getBytesLength(key);

    if (len == 0 || len > BLOB_SIZE) continue;  // empty slot or corrupted

    prefs.getBytes(key, blob, len);

    // R10H-3: Deserialize + validate commit flag
    if (_deserializeEntry(blob, len, i)) {
      _journalSize++;
    } else {
      // Corrupted entry (power loss during write) — mark slot as free
      Serial.printf("[Journal] WARNING: Entry %u corrupted (power loss?) — skipping\n", i);
      _journalValid[i] = false;
      _journalIds[i] = "";
      _journalHashes[i] = "";
      _journalAcks[i] = "";
    }
  }

  prefs.end();
}

// R10H-1: Deserialize blob → fields. Returns true if valid (commit flag set).
bool TransactionJournal::_deserializeEntry(const uint8_t* blob, size_t len, uint8_t idx) {
  if (len < 3) return false;  // minimum: valid(1) + idLen(1) + id(1)

  size_t offset = 0;

  // R10H-3: Check commit flag
  bool valid = blob[offset++] != 0;
  if (!valid) return false;  // not committed — power loss during write

  // requestId
  uint8_t idLen = blob[offset++];
  if (offset + idLen > len || idLen > 64) return false;
  _journalIds[idx] = String((const char*)(blob + offset), idLen);
  offset += idLen;

  // commandHash
  if (offset + 1 > len) return false;
  uint8_t hashLen = blob[offset++];
  if (offset + hashLen > len || hashLen > 64) return false;
  _journalHashes[idx] = String((const char*)(blob + offset), hashLen);
  offset += hashLen;

  // ackJson (2-byte length, little-endian)
  if (offset + 2 > len) return false;
  uint16_t ackLen = blob[offset] | (blob[offset + 1] << 8);
  offset += 2;
  if (offset + ackLen > len || ackLen > 1024) return false;
  _journalAcks[idx] = String((const char*)(blob + offset), ackLen);
  offset += ackLen;

  _journalValid[idx] = true;
  return true;
}

// R10H-1: Atomic blob write — serialize entry to single buffer, write once.
// This ensures power loss during write leaves entry either fully written
// (valid=1) or not written at all (valid=0 from previous state, or empty).
void TransactionJournal::_saveEntryToNVSAtomic(uint8_t idx) {
  if (idx >= JOURNAL_SIZE) return;

  const String& rid = _journalIds[idx];
  const String& hash = _journalHashes[idx];
  const String& ack = _journalAcks[idx];

  uint8_t idLen = min(rid.length(), (unsigned)64);
  uint8_t hashLen = min(hash.length(), (unsigned)64);
  uint16_t ackLen = min(ack.length(), (unsigned)1024);

  // Calculate total blob size
  size_t totalLen = 1 + 1 + idLen + 1 + hashLen + 2 + ackLen;
  if (totalLen > BLOB_SIZE) {
    Serial.printf("[Journal] ERROR: blob too large (%u > %u) — truncating ack\n",
                  totalLen, BLOB_SIZE);
    ackLen = BLOB_SIZE - (1 + 1 + idLen + 1 + hashLen + 2);
    totalLen = BLOB_SIZE;
  }

  // Serialize to buffer
  uint8_t blob[BLOB_SIZE];
  size_t offset = 0;

  // R10H-3: valid flag = 1 (committed)
  blob[offset++] = 1;

  // requestId
  blob[offset++] = idLen;
  memcpy(blob + offset, rid.c_str(), idLen);
  offset += idLen;

  // commandHash
  blob[offset++] = hashLen;
  memcpy(blob + offset, hash.c_str(), hashLen);
  offset += hashLen;

  // ackJson (2-byte length, little-endian)
  blob[offset++] = ackLen & 0xFF;
  blob[offset++] = (ackLen >> 8) & 0xFF;
  memcpy(blob + offset, ack.c_str(), ackLen);
  offset += ackLen;

  // R10H-1: Single atomic NVS write
  Preferences prefs;
  if (!prefs.begin(Core::NVS_NAMESPACE, false)) return;

  char key[20];
  snprintf(key, sizeof(key), "%s%u", NVS_KEY_TJ_ENTRY_PREFIX, idx);
  prefs.putBytes(key, blob, totalLen);

  // Update metadata (these are small uint8 writes, atomic individually)
  prefs.putUChar(NVS_KEY_TJ_COUNT, _journalSize);
  prefs.putUChar(NVS_KEY_TJ_WIDX, _journalWriteIdx);

  prefs.end();
}

int TransactionJournal::_findInJournal(const String& requestId) {
  for (uint8_t i = 0; i < JOURNAL_SIZE; i++) {
    if (_journalValid[i] && _journalIds[i] == requestId) return i;
  }
  return -1;
}

bool TransactionJournal::isProcessed(const String& requestId) {
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
  // Check if already stored (idempotent)
  int existing = _findInJournal(requestId);
  if (existing >= 0) {
    _journalHashes[existing] = commandHash;
    _journalAcks[existing] = ackJson;
    _saveEntryToNVSAtomic(existing);
    queueAck(requestId, ackJson);
    return true;
  }

  // R10H-2: New entry — write to LRU slot (journal size now 64)
  uint8_t idx = _journalWriteIdx;
  _journalIds[idx] = requestId;
  _journalHashes[idx] = commandHash;
  _journalAcks[idx] = ackJson;
  _journalValid[idx] = true;  // R10H-3: mark valid in RAM

  _journalWriteIdx = (_journalWriteIdx + 1) % JOURNAL_SIZE;
  if (_journalSize < JOURNAL_SIZE) _journalSize++;

  // R10H-1: Atomic NVS write
  _saveEntryToNVSAtomic(idx);

  queueAck(requestId, ackJson);

  Serial.printf("[Journal] Stored: rid=%s (slot %u, size %u/%u)\n",
                requestId.c_str(), idx, _journalSize, JOURNAL_SIZE);
  return true;
}

void TransactionJournal::queueAck(const String& requestId, const String& ackJson) {
  for (uint8_t i = 0; i < _pendingAckCount; i++) {
    if (_pendingAcks[i].requestId == requestId) {
      _pendingAcks[i].ackJson = ackJson;
      _pendingAcks[i].retryCount = 0;
      _pendingAcks[i].lastAttemptMs = 0;
      return;
    }
  }

  if (_pendingAckCount >= MAX_PENDING_ACKS) {
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

static std::function<bool(const char* topic, const uint8_t* payload, size_t len)> _publishCallback;

void setPublishCallback(std::function<bool(const char* topic, const uint8_t* payload, size_t len)> cb) {
  _publishCallback = cb;
}

uint8_t TransactionJournal::processPendingAcks() {
  if (_pendingAckCount == 0 || !_publishCallback) return 0;

  unsigned long now = millis();
  uint8_t published = 0;

  for (int i = _pendingAckCount - 1; i >= 0; i--) {
    if (now - _pendingAcks[i].lastAttemptMs < ACK_RETRY_INTERVAL_MS) continue;

    bool success = _publishCallback(
      "ack",
      (const uint8_t*)_pendingAcks[i].ackJson.c_str(),
      _pendingAcks[i].ackJson.length()
    );

    _pendingAcks[i].lastAttemptMs = now;
    _pendingAcks[i].retryCount++;

    if (success) {
      Serial.printf("[Journal] ACK delivered: rid=%s (attempt %u)\n",
                    _pendingAcks[i].requestId.c_str(), _pendingAcks[i].retryCount);
      for (int j = i; j < _pendingAckCount - 1; j++) {
        _pendingAcks[j] = _pendingAcks[j + 1];
      }
      _pendingAckCount--;
      published++;
    } else if (_pendingAcks[i].retryCount >= MAX_ACK_RETRIES) {
      Serial.printf("[Journal] ACK give up after %u retries: rid=%s\n",
                    MAX_ACK_RETRIES, _pendingAcks[i].requestId.c_str());
      Services::Log.append(Core::LogType::Error,
        "ACK delivery failed after " + String(MAX_ACK_RETRIES) + " retries: " + _pendingAcks[i].requestId, 0);
      for (int j = i; j < _pendingAckCount - 1; j++) {
        _pendingAcks[j] = _pendingAcks[j + 1];
      }
      _pendingAckCount--;
    }
  }

  return published;
}

} // namespace Services
