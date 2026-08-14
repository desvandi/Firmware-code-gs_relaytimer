// =============================================================================
// Services/TransactionJournal.cpp — NVS-persisted transaction journal
// =============================================================================
// R10I (audit round 10I): CRC32 + magic + version + two-phase commit.
//
// Full source provided for engineer baris-demi-baris audit.
// =============================================================================
#include "TransactionJournal.h"
#include "Config.h"
#include "LogService.h"
#include <Preferences.h>
#include <string.h>
#include <esp_crc.h>  // ESP-IDF CRC32 (hardware-accelerated on ESP32)

namespace Services {

TransactionJournal journal;

// NVS keys
static const char* NVS_KEY_TJ_ENTRY_PREFIX = "tj_entry_";  // tj_entry_0 .. tj_entry_63
static const char* NVS_KEY_TJ_COUNT = "tj_count";
static const char* NVS_KEY_TJ_WIDX = "tj_widx";

// ============================================================================
// R10I-1: CRC32 computation using ESP-IDF's crc32_le (hardware-accelerated)
// ============================================================================
uint32_t TransactionJournal::_computeCRC(const uint8_t* data, size_t len) {
  // ESP-IDF provides esp_crc32_le() which is CRC-32 (same polynomial as zlib)
  return esp_crc32_le(0, data, len);
}

// ============================================================================
// R10I-1: Deserialize blob + verify integrity (magic + version + CRC)
// ============================================================================
bool TransactionJournal::_deserializeEntry(const uint8_t* blob, size_t len, uint8_t idx) {
  // --- Bounds check: minimum header size ---
  if (len < BLOB_HEADER_SIZE) {
    Serial.printf("[Journal] Entry %u: blob too short (%u < %u)\n", idx, len, BLOB_HEADER_SIZE);
    return false;
  }

  // --- R10I-1: Verify magic ---
  if (blob[0] != BLOB_MAGIC1 || blob[1] != BLOB_MAGIC2) {
    Serial.printf("[Journal] Entry %u: bad magic (0x%02X 0x%02X)\n", idx, blob[0], blob[1]);
    return false;
  }

  // --- R10I-1: Verify version ---
  if (blob[2] != BLOB_VERSION) {
    Serial.printf("[Journal] Entry %u: unsupported version %u\n", idx, blob[2]);
    return false;
  }

  // --- R10I-2: Check valid flag (commit marker) ---
  bool valid = blob[3] != 0;
  if (!valid) {
    Serial.printf("[Journal] Entry %u: not committed (valid=0)\n", idx);
    return false;  // power loss during Phase 1 or Phase 2
  }

  // --- R10I-1: Extract stored CRC ---
  uint32_t storedCRC = blob[4] | (blob[5] << 8) | (blob[6] << 16) | ((uint32_t)blob[7] << 24);

  // --- R10I-1: Compute CRC over payload (everything after CRC field) ---
  const uint8_t* payload = blob + BLOB_HEADER_SIZE;
  size_t payloadLen = len - BLOB_HEADER_SIZE;
  uint32_t computedCRC = _computeCRC(payload, payloadLen);

  if (storedCRC != computedCRC) {
    Serial.printf("[Journal] Entry %u: CRC mismatch (stored=0x%08X computed=0x%08X) — CORRUPT\n",
                  idx, storedCRC, computedCRC);
    return false;  // data corrupted by power loss
  }

  // --- CRC valid → deserialize payload ---
  size_t offset = 0;

  // requestId
  if (offset >= payloadLen) return false;
  uint8_t idLen = payload[offset++];
  if (offset + idLen > payloadLen || idLen > 64 || idLen == 0) return false;
  _journalIds[idx] = String((const char*)(payload + offset), idLen);
  offset += idLen;

  // commandHash
  if (offset >= payloadLen) return false;
  uint8_t hashLen = payload[offset++];
  if (offset + hashLen > payloadLen || hashLen > 64 || hashLen == 0) return false;
  _journalHashes[idx] = String((const char*)(payload + offset), hashLen);
  offset += hashLen;

  // ackJson (2-byte length LE)
  if (offset + 2 > payloadLen) return false;
  uint16_t ackLen = payload[offset] | (payload[offset + 1] << 8);
  offset += 2;
  if (offset + ackLen > payloadLen || ackLen > 1024 || ackLen == 0) return false;
  _journalAcks[idx] = String((const char*)(payload + offset), ackLen);
  offset += ackLen;

  _journalValid[idx] = true;
  return true;
}

// ============================================================================
// R10I-2 + R10I-3 + R10I-4: Two-phase atomic commit
// ============================================================================
bool TransactionJournal::_saveEntryToNVSAtomic(uint8_t idx) {
  if (idx >= JOURNAL_SIZE) return false;

  const String& rid = _journalIds[idx];
  const String& hash = _journalHashes[idx];
  const String& ack = _journalAcks[idx];

  // --- Truncate fields to max lengths ---
  uint8_t idLen = min((unsigned)rid.length(), (unsigned)64);
  uint8_t hashLen = min((unsigned)hash.length(), (unsigned)64);
  uint16_t ackLen = min((unsigned)ack.length(), (unsigned)1024);

  // --- Calculate total blob size ---
  size_t payloadLen = 1 + idLen + 1 + hashLen + 2 + ackLen;
  size_t totalLen = BLOB_HEADER_SIZE + payloadLen;

  if (totalLen > BLOB_SIZE) {
    Serial.printf("[Journal] ERROR: blob too large (%u > %u)\n", totalLen, BLOB_SIZE);
    return false;
  }

  // --- Serialize to buffer ---
  uint8_t blob[BLOB_SIZE];
  memset(blob, 0, BLOB_SIZE);

  // Header
  blob[0] = BLOB_MAGIC1;   // 'T'
  blob[1] = BLOB_MAGIC2;   // 'J'
  blob[2] = BLOB_VERSION;   // 1
  blob[3] = 0;              // R10I-2: valid=0 (INCOMPLETE) for Phase 1
  // CRC filled after payload

  // Payload
  uint8_t* payload = blob + BLOB_HEADER_SIZE;
  size_t offset = 0;

  payload[offset++] = idLen;
  memcpy(payload + offset, rid.c_str(), idLen);
  offset += idLen;

  payload[offset++] = hashLen;
  memcpy(payload + offset, hash.c_str(), hashLen);
  offset += hashLen;

  payload[offset++] = ackLen & 0xFF;
  payload[offset++] = (ackLen >> 8) & 0xFF;
  memcpy(payload + offset, ack.c_str(), ackLen);
  offset += ackLen;

  // R10I-1: Compute CRC over payload
  uint32_t crc = _computeCRC(payload, payloadLen);
  blob[4] = crc & 0xFF;
  blob[5] = (crc >> 8) & 0xFF;
  blob[6] = (crc >> 16) & 0xFF;
  blob[7] = (crc >> 24) & 0xFF;

  // --- Open NVS ---
  Preferences prefs;
  if (!prefs.begin(Core::NVS_NAMESPACE, false)) {
    Serial.println("[Journal] FATAL: Cannot open NVS for write");
    return false;
  }

  char key[20];
  snprintf(key, sizeof(key), "%s%u", NVS_KEY_TJ_ENTRY_PREFIX, idx);

  // --- R10I-4: Phase 0 — Clear old entry (write valid=0 first) ---
  // This prevents "double valid" if new write is interrupted.
  uint8_t clearBlob[BLOB_HEADER_SIZE];
  clearBlob[0] = BLOB_MAGIC1;
  clearBlob[1] = BLOB_MAGIC2;
  clearBlob[2] = BLOB_VERSION;
  clearBlob[3] = 0;  // valid=0 (clearing)
  clearBlob[4] = 0; clearBlob[5] = 0; clearBlob[6] = 0; clearBlob[7] = 0;  // CRC=0
  prefs.putBytes(key, clearBlob, BLOB_HEADER_SIZE);

  // --- R10I-2: Phase 1 — Write full blob with valid=0 ---
  size_t written = prefs.putBytes(key, blob, totalLen);
  if (written != totalLen) {
    // R10I-3: Write failed — mark slot invalid
    Serial.printf("[Journal] Phase 1 write FAILED (wrote %u/%u bytes)\n", written, totalLen);
    prefs.putBytes(key, clearBlob, BLOB_HEADER_SIZE);  // ensure valid=0
    prefs.end();
    _journalValid[idx] = false;
    return false;
  }

  // --- R10I-2: Phase 2 — Flip valid byte to 1 (commit) ---
  blob[3] = 1;  // valid=1 (COMMITTED)
  written = prefs.putBytes(key, blob, totalLen);
  if (written != totalLen) {
    // R10I-3: Phase 2 write failed — entry remains valid=0 (uncommitted)
    Serial.printf("[Journal] Phase 2 write FAILED — entry uncommitted\n");
    prefs.end();
    _journalValid[idx] = false;
    return false;
  }

  // Update metadata
  prefs.putUChar(NVS_KEY_TJ_COUNT, _journalSize);
  prefs.putUChar(NVS_KEY_TJ_WIDX, _journalWriteIdx);

  prefs.end();
  _journalValid[idx] = true;
  return true;
}

// ============================================================================
// R10I-4: Clear slot — write valid=0 to prevent double-valid on overwrite
// ============================================================================
void TransactionJournal::_clearSlotNVS(uint8_t idx) {
  Preferences prefs;
  if (!prefs.begin(Core::NVS_NAMESPACE, false)) return;

  char key[20];
  snprintf(key, sizeof(key), "%s%u", NVS_KEY_TJ_ENTRY_PREFIX, idx);

  uint8_t clearBlob[BLOB_HEADER_SIZE] = {BLOB_MAGIC1, BLOB_MAGIC2, BLOB_VERSION, 0, 0, 0, 0, 0};
  prefs.putBytes(key, clearBlob, BLOB_HEADER_SIZE);
  prefs.end();
}

// ============================================================================
// Boot: Load all valid entries from NVS
// ============================================================================
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

  // R10I-1: Load + verify each entry (magic + version + valid + CRC)
  for (uint8_t i = 0; i < JOURNAL_SIZE; i++) {
    snprintf(key, sizeof(key), "%s%u", NVS_KEY_TJ_ENTRY_PREFIX, i);
    size_t len = prefs.getBytesLength(key);

    if (len == 0 || len > BLOB_SIZE) {
      // Empty slot — skip
      _journalValid[i] = false;
      continue;
    }

    prefs.getBytes(key, blob, len);

    if (_deserializeEntry(blob, len, i)) {
      _journalSize++;
    } else {
      // R10I-1: Corrupted/uncommitted entry — mark as invalid
      Serial.printf("[Journal] Entry %u: INVALID (corrupt/uncommitted) — slot freed\n", i);
      _journalValid[i] = false;
      _journalIds[i] = "";
      _journalHashes[i] = "";
      _journalAcks[i] = "";
    }
  }

  prefs.end();
}

// ============================================================================
// Lookup helpers
// ============================================================================
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

// ============================================================================
// Store transaction (called from ACK publishers after successful execute)
// ============================================================================
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

  // New entry — write to LRU slot
  uint8_t idx = _journalWriteIdx;
  _journalIds[idx] = requestId;
  _journalHashes[idx] = commandHash;
  _journalAcks[idx] = ackJson;

  // R10I-2: Atomic two-phase commit (valid=0 → write → valid=1)
  if (!_saveEntryToNVSAtomic(idx)) {
    // Write failed — don't advance pointer, don't queue ACK
    // PWA will retry, ESP32 will re-execute (safe for idempotent commands)
    Serial.printf("[Journal] storeTransaction FAILED for rid=%s — will retry on next command\n",
                  requestId.c_str());
    return false;
  }

  // Advance LRU pointer
  _journalWriteIdx = (_journalWriteIdx + 1) % JOURNAL_SIZE;
  if (_journalSize < JOURNAL_SIZE) _journalSize++;

  // Queue ACK for delivery
  queueAck(requestId, ackJson);

  Serial.printf("[Journal] Stored: rid=%s (slot %u, size %u/%u)\n",
                requestId.c_str(), idx, _journalSize, JOURNAL_SIZE);
  return true;
}

// ============================================================================
// ACK retry queue
// ============================================================================
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

// R10G-2: Publish callback (injected by MqttClient)
static std::function<bool(const char* topic, const uint8_t* payload, size_t len)> _publishCallback;

void setPublishCallback(std::function<bool(const char* topic, const uint8_t* payload, size_t len)> cb) {
  _publishCallback = cb;
}

// ============================================================================
// R10G-2: Process pending ACK queue — called from loop()
// Only publishes ACKs, NEVER executes commands.
// ============================================================================
uint8_t TransactionJournal::processPendingAcks() {
  if (_pendingAckCount == 0 || !_publishCallback) return 0;

  unsigned long now = millis();
  uint8_t published = 0;

  for (int i = _pendingAckCount - 1; i >= 0; i--) {
    // R10I: Only retry if interval has elapsed (prevents tight-loop writes)
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
