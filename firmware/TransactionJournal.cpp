// =============================================================================
// Services/TransactionJournal.cpp — NVS-persisted transaction journal
// =============================================================================
// CYCLE-7 (auditor #7): Intent-first journaling.
//   storeIntent() writes PENDING entry BEFORE execute.
//   commitTransaction() flips commit=1 AFTER execute + ACK ready.
//   isProcessed() returns true for BOTH PENDING and COMMITTED.
//
// This closes the execute→store gap (F-001) and ensures persistence
// failure is surfaced to caller (F-002).
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
static const char* NVS_KEY_TJ_ENTRY_PREFIX = "tj_entry_";  // tj_entry_0 .. tj_entry_63 (blob data)
static const char* NVS_KEY_TJ_COMMIT_PREFIX = "tj_commit_"; // tj_commit_0 .. tj_commit_63 (1 byte each)
static const char* NVS_KEY_TJ_COUNT = "tj_count";
static const char* NVS_KEY_TJ_WIDX = "tj_widx";

// ============================================================================
// R10I-1: CRC32 computation using ESP-IDF's crc32_le (hardware-accelerated)
// ============================================================================
uint32_t TransactionJournal::_computeCRC(const uint8_t* data, size_t len) {
  return esp_crc32_le(0, data, len);
}

// ============================================================================
// R10I-1: Deserialize blob + verify integrity (magic + version + CRC)
// ============================================================================
bool TransactionJournal::_deserializeEntry(const uint8_t* blob, size_t len, uint8_t idx) {
  if (len < BLOB_HEADER_SIZE) {
    Serial.printf("[Journal] Entry %u: blob too short (%u < %u)\n", idx, len, BLOB_HEADER_SIZE);
    return false;
  }

  if (blob[0] != BLOB_MAGIC1 || blob[1] != BLOB_MAGIC2) {
    Serial.printf("[Journal] Entry %u: bad magic (0x%02X 0x%02X)\n", idx, blob[0], blob[1]);
    return false;
  }

  if (blob[2] != BLOB_VERSION) {
    Serial.printf("[Journal] Entry %u: unsupported version %u\n", idx, blob[2]);
    return false;
  }

  (void)blob[3];  // reserved byte (commit flag is in separate key)

  uint32_t storedCRC = blob[4] | (blob[5] << 8) | (blob[6] << 16) | ((uint32_t)blob[7] << 24);

  const uint8_t* payload = blob + BLOB_HEADER_SIZE;
  size_t payloadLen = len - BLOB_HEADER_SIZE;
  uint32_t computedCRC = _computeCRC(payload, payloadLen);

  if (storedCRC != computedCRC) {
    Serial.printf("[Journal] Entry %u: CRC mismatch (stored=0x%08X computed=0x%08X) — CORRUPT\n",
                  idx, storedCRC, computedCRC);
    return false;
  }

  size_t offset = 0;

  if (offset >= payloadLen) return false;
  uint8_t idLen = payload[offset++];
  if (offset + idLen > payloadLen || idLen > 64 || idLen == 0) return false;
  _journalIds[idx] = String((const char*)(payload + offset), idLen);
  offset += idLen;

  if (offset >= payloadLen) return false;
  uint8_t hashLen = payload[offset++];
  if (offset + hashLen > payloadLen || hashLen > 64 || hashLen == 0) return false;
  _journalHashes[idx] = String((const char*)(payload + offset), hashLen);
  offset += hashLen;

  // ackJson (2-byte length LE) — for PENDING entries, this may be empty
  if (offset + 2 > payloadLen) return false;
  uint16_t ackLen = payload[offset] | (payload[offset + 1] << 8);
  offset += 2;
  if (ackLen > 1024) return false;
  if (ackLen > 0) {
    if (offset + ackLen > payloadLen) return false;
    _journalAcks[idx] = String((const char*)(payload + offset), ackLen);
  } else {
    _journalAcks[idx] = "";
  }
  offset += ackLen;

  _journalValid[idx] = true;
  return true;
}

// ============================================================================
// _saveEntryToNVSAtomic — write entry blob + persist writeIdx + optionally commit
//
// CYCLE-7 changes:
//   - Added `commitToCommitted` parameter.
//   - When false (storeIntent): writes blob with ackJson="" + commit=0.
//   - When true  (legacy storeTransaction): writes blob with ackJson + commit=1.
//   - Updates existing entry's data WITHOUT advancing writeIdx (F-005 fix).
// ============================================================================
bool TransactionJournal::_saveEntryToNVSAtomic(uint8_t idx, bool commitToCommitted) {
  if (idx >= JOURNAL_SIZE) return false;

  const String& rid = _journalIds[idx];
  const String& hash = _journalHashes[idx];
  const String& ack = _journalAcks[idx];

  uint8_t idLen = min((unsigned)rid.length(), (unsigned)64);
  uint8_t hashLen = min((unsigned)hash.length(), (unsigned)64);
  uint16_t ackLen = min((unsigned)ack.length(), (unsigned)1024);

  size_t payloadLen = 1 + idLen + 1 + hashLen + 2 + ackLen;
  size_t totalLen = BLOB_HEADER_SIZE + payloadLen;

  if (totalLen > BLOB_SIZE) {
    Serial.printf("[Journal] ERROR: blob too large (%u > %u)\n", totalLen, BLOB_SIZE);
    return false;
  }

  uint8_t blob[BLOB_SIZE];
  memset(blob, 0, BLOB_SIZE);

  blob[0] = BLOB_MAGIC1;   // 'T'
  blob[1] = BLOB_MAGIC2;   // 'J'
  blob[2] = BLOB_VERSION;  // 1
  blob[3] = 0;             // reserved

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
  if (ackLen > 0) {
    memcpy(payload + offset, ack.c_str(), ackLen);
    offset += ackLen;
  }

  uint32_t crc = _computeCRC(payload, payloadLen);
  blob[4] = crc & 0xFF;
  blob[5] = (crc >> 8) & 0xFF;
  blob[6] = (crc >> 16) & 0xFF;
  blob[7] = (crc >> 24) & 0xFF;

  Preferences prefs;
  if (!prefs.begin(Core::NVS_NAMESPACE, false)) {
    Serial.println("[Journal] FATAL: Cannot open NVS for write");
    return false;
  }

  char entryKey[20];
  char commitKey[20];
  snprintf(entryKey, sizeof(entryKey), "%s%u", NVS_KEY_TJ_ENTRY_PREFIX, idx);
  snprintf(commitKey, sizeof(commitKey), "%s%u", NVS_KEY_TJ_COMMIT_PREFIX, idx);

  // --- Phase 0: Clear commit flag (invalidate old entry) ---
  prefs.putUChar(commitKey, 0);

  // --- Phase 1: Write blob data ---
  size_t written = prefs.putBytes(entryKey, blob, totalLen);
  if (written != totalLen) {
    Serial.printf("[Journal] Phase 1 write FAILED (wrote %u/%u bytes)\n", written, totalLen);
    prefs.end();
    _journalValid[idx] = false;
    return false;
  }

  // --- Phase 1b: Persist writeIdx (only for NEW entries, not updates) ---
  // CYCLE-7 fix for F-005: updates to existing entries must NOT advance writeIdx.
  // _journalValid[idx] is true only if this is a NEW slot being written for the
  // first time. If it's already true in RAM, the entry existed before this call —
  // meaning we're updating an existing entry, and writeIdx should NOT advance.
  bool isNewSlot = !_journalValid[idx];
  if (isNewSlot) {
    uint8_t nextWriteIdx = (_journalWriteIdx + 1) % JOURNAL_SIZE;
    size_t widxWritten = prefs.putUChar(NVS_KEY_TJ_WIDX, nextWriteIdx);
    if (widxWritten != 1) {
      Serial.printf("[Journal] Phase 1b writeIdx persist FAILED — entry left uncommitted\n");
      prefs.end();
      _journalValid[idx] = false;
      return false;
    }
    // Keep _journalWriteIdx advanced in RAM (matches NVS).
    _journalWriteIdx = nextWriteIdx;
    if (_journalSize < JOURNAL_SIZE) _journalSize++;
  }

  // --- Phase 2: Set commit flag if requested ---
  if (commitToCommitted) {
    size_t commitWritten = prefs.putUChar(commitKey, 1);
    if (commitWritten != 1) {
      Serial.printf("[Journal] Phase 2 commit FAILED (idx=%u) — entry left PENDING\n", idx);
      prefs.end();
      _journalValid[idx] = true;       // entry data is valid (CRC ok) but PENDING
      _journalCommitted[idx] = false;  // not committed
      return false;
    }
    _journalCommitted[idx] = true;
  } else {
    // PENDING — commit flag already cleared in Phase 0
    _journalCommitted[idx] = false;
  }

  // Update count (metadata)
  prefs.putUChar(NVS_KEY_TJ_COUNT, _journalSize < JOURNAL_SIZE ? _journalSize : JOURNAL_SIZE);

  prefs.end();
  _journalValid[idx] = true;
  return true;
}

// ============================================================================
// CYCLE-7: _commitSlotNVS — flip commit flag 0 → 1 + update ackJson for PENDING entry
// ============================================================================
bool TransactionJournal::_commitSlotNVS(uint8_t idx, const String& ackJson) {
  if (idx >= JOURNAL_SIZE) return false;
  if (!_journalValid[idx]) {
    Serial.printf("[Journal] _commitSlotNVS: slot %u not valid (no PENDING entry)\n", idx);
    return false;
  }
  if (_journalCommitted[idx]) {
    // Already committed — update ackJson in place (idempotent).
    Serial.printf("[Journal] _commitSlotNVS: slot %u already COMMITTED — updating ackJson\n", idx);
  }

  // Update ackJson in RAM
  _journalAcks[idx] = ackJson;

  // Rewrite blob with new ackJson, then flip commit=1.
  // This reuses _saveEntryToNVSAtomic with commitToCommitted=true.
  // Note: _journalValid[idx] is already true, so _saveEntryToNVSAtomic
  // will treat this as an UPDATE (not advancing writeIdx) — exactly what we want.
  return _saveEntryToNVSAtomic(idx, true);
}

// ============================================================================
// R10J: Clear slot — clear commit flag (separate key)
// ============================================================================
void TransactionJournal::_clearSlotNVS(uint8_t idx) {
  Preferences prefs;
  if (!prefs.begin(Core::NVS_NAMESPACE, false)) return;

  char commitKey[20];
  snprintf(commitKey, sizeof(commitKey), "%s%u", NVS_KEY_TJ_COMMIT_PREFIX, idx);
  prefs.putUChar(commitKey, 0);  // clear commit flag
  prefs.end();
}

// ============================================================================
// Boot: Load all valid entries (PENDING and COMMITTED) from NVS
// ============================================================================
void TransactionJournal::begin() {
  _loadFromNVS();
  Serial.printf("[Journal] Loaded %u entries from NVS (capacity %u)\n",
                _journalSize, JOURNAL_SIZE);

  // Queue COMMITTED ACKs for re-delivery after reboot
  uint8_t committed = 0, pending = 0;
  for (uint8_t i = 0; i < JOURNAL_SIZE; i++) {
    if (_journalValid[i]) {
      if (_journalCommitted[i]) {
        committed++;
        if (_journalAcks[i].length() > 0) {
          queueAck(_journalIds[i], _journalAcks[i]);
        }
      } else {
        pending++;
        // CYCLE-7: PENDING entries after reboot = crash during execute.
        // Log warning — these will block re-execution until LRU evicted.
        Serial.printf("[Journal] WARN: PENDING entry found at boot: rid=%s (crash during execute?)\n",
                      _journalIds[i].c_str());
        Services::Log.append(Core::LogType::Error,
          "PENDING transaction found at boot (crash during execute): " + _journalIds[i], 0);
      }
    }
  }
  Serial.printf("[Journal] %u committed, %u pending\n", committed, pending);
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

  char entryKey[20];
  char commitKey[20];
  uint8_t blob[BLOB_SIZE];

  for (uint8_t i = 0; i < JOURNAL_SIZE; i++) {
    snprintf(entryKey, sizeof(entryKey), "%s%u", NVS_KEY_TJ_ENTRY_PREFIX, i);
    snprintf(commitKey, sizeof(commitKey), "%s%u", NVS_KEY_TJ_COMMIT_PREFIX, i);

    uint8_t committed = prefs.getUChar(commitKey, 0);

    // Load blob regardless of commit state — we need to know if entry exists.
    size_t len = prefs.getBytesLength(entryKey);
    if (len == 0 || len > BLOB_SIZE) {
      _journalValid[i] = false;
      _journalCommitted[i] = false;
      // If commit=1 but blob missing → corrupted, clear commit
      if (committed == 1) {
        Serial.printf("[Journal] Entry %u: commit=1 but blob missing — clearing commit\n", i);
        // Reopen in RW mode to clear
        prefs.end();
        Preferences rw;
        rw.begin(Core::NVS_NAMESPACE, false);
        rw.putUChar(commitKey, 0);
        rw.end();
        prefs.begin(Core::NVS_NAMESPACE, true);
      }
      continue;
    }

    prefs.getBytes(entryKey, blob, len);

    if (_deserializeEntry(blob, len, i)) {
      _journalCommitted[i] = (committed == 1);
      _journalSize++;
      if (_journalCommitted[i]) {
        Serial.printf("[Journal] Entry %u: COMMITTED rid=%s\n", i, _journalIds[i].c_str());
      } else {
        Serial.printf("[Journal] Entry %u: PENDING rid=%s (crash during execute)\n",
                      i, _journalIds[i].c_str());
      }
    } else {
      Serial.printf("[Journal] Entry %u: blob CORRUPT — slot freed\n", i);
      _journalValid[i] = false;
      _journalCommitted[i] = false;
      _journalIds[i] = "";
      _journalHashes[i] = "";
      _journalAcks[i] = "";
      // Clear commit flag for corrupt slot
      prefs.end();
      Preferences rw;
      rw.begin(Core::NVS_NAMESPACE, false);
      rw.putUChar(commitKey, 0);
      rw.end();
      prefs.begin(Core::NVS_NAMESPACE, true);
    }
  }

  prefs.end();
}

// ============================================================================
// Lookup helpers — check BOTH PENDING and COMMITTED (CYCLE-7)
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

bool TransactionJournal::isCommitted(const String& requestId) {
  int idx = _findInJournal(requestId);
  if (idx < 0) return false;
  return _journalCommitted[idx];
}

String TransactionJournal::getCommandHash(const String& requestId) {
  int idx = _findInJournal(requestId);
  if (idx >= 0) return _journalHashes[idx];
  return "";
}

String TransactionJournal::getAckJson(const String& requestId) {
  int idx = _findInJournal(requestId);
  if (idx >= 0 && _journalCommitted[idx]) return _journalAcks[idx];
  return "";
}

// ============================================================================
// CYCLE-7: storeIntent — durable PENDING entry BEFORE execute
// ============================================================================
bool TransactionJournal::storeIntent(const String& requestId, const String& commandHash) {
  if (requestId.length() == 0 || requestId.length() > 64) {
    Serial.println("[Journal] storeIntent: invalid requestId");
    return false;
  }
  if (commandHash.length() == 0 || commandHash.length() > 64) {
    Serial.println("[Journal] storeIntent: invalid commandHash");
    return false;
  }

  // Check if already in journal (PENDING or COMMITTED) — caller should handle
  // via isProcessed() before calling storeIntent().
  int existing = _findInJournal(requestId);
  if (existing >= 0) {
    Serial.printf("[Journal] storeIntent: requestId %s already exists (idx=%u, committed=%d)\n",
                  requestId.c_str(), existing, _journalCommitted[existing] ? 1 : 0);
    return false;
  }

  // Write PENDING entry (commit=0)
  uint8_t idx = _journalWriteIdx;
  _journalIds[idx] = requestId;
  _journalHashes[idx] = commandHash;
  _journalAcks[idx] = "";  // ackJson empty until commit

  if (!_saveEntryToNVSAtomic(idx, false /* commitToCommitted */)) {
    Serial.printf("[Journal] storeIntent FAILED for rid=%s — execute MUST NOT proceed\n",
                  requestId.c_str());
    // Clear RAM state so slot can be reused
    _journalIds[idx] = "";
    _journalHashes[idx] = "";
    _journalAcks[idx] = "";
    _journalValid[idx] = false;
    _journalCommitted[idx] = false;
    return false;
  }

  Serial.printf("[Journal] Intent stored: rid=%s (slot %u, PENDING)\n",
                requestId.c_str(), idx);
  return true;
}

// ============================================================================
// CYCLE-7: commitTransaction — flip PENDING → COMMITTED, store ackJson
// ============================================================================
bool TransactionJournal::commitTransaction(const String& requestId, const String& ackJson) {
  int idx = _findInJournal(requestId);
  if (idx < 0) {
    Serial.printf("[Journal] commitTransaction: rid=%s NOT FOUND — was intent stored?\n",
                  requestId.c_str());
    return false;
  }

  if (_journalCommitted[idx]) {
    // Already committed — update ackJson (idempotent replay).
    Serial.printf("[Journal] commitTransaction: rid=%s already COMMITTED — updating ackJson\n",
                  requestId.c_str());
  }

  if (!_commitSlotNVS(idx, ackJson)) {
    Serial.printf("[Journal] commitTransaction FAILED for rid=%s — ACK NOT durable\n",
                  requestId.c_str());
    Services::Log.append(Core::LogType::Error,
      "Transaction commit FAILED (NVS write error): " + requestId, 0);
    return false;
  }

  // Queue ACK for delivery (also handles re-delivery on duplicate command)
  queueAck(requestId, ackJson);

  Serial.printf("[Journal] Committed: rid=%s (slot %u, COMMITTED)\n",
                requestId.c_str(), idx);
  return true;
}

// ============================================================================
// Legacy storeTransaction — kept for backward compat with code paths that
// haven't been migrated to intent-first pattern (e.g., schedule upsert from
// REST API). Internally does storeIntent + commitTransaction in one call.
// ============================================================================
bool TransactionJournal::storeTransaction(const String& requestId,
                                           const String& commandHash,
                                           const String& ackJson) {
  // If already exists, just update (commit if not yet committed)
  int existing = _findInJournal(requestId);
  if (existing >= 0) {
    _journalHashes[existing] = commandHash;
    _journalAcks[existing] = ackJson;
    // Use _saveEntryToNVSAtomic with commit=true (updates in place, commits)
    bool ok = _saveEntryToNVSAtomic(existing, true);
    if (ok) {
      queueAck(requestId, ackJson);
    }
    return ok;
  }

  // New entry — store intent first (PENDING), then commit
  if (!storeIntent(requestId, commandHash)) {
    return false;
  }
  return commitTransaction(requestId, ackJson);
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

// CYCLE-7 fix for F-006: dequeue ACK after immediate publish succeeded
void TransactionJournal::dequeueAck(const String& requestId) {
  for (uint8_t i = 0; i < _pendingAckCount; i++) {
    if (_pendingAcks[i].requestId == requestId) {
      // Shift remaining entries left
      for (uint8_t j = i; j < _pendingAckCount - 1; j++) {
        _pendingAcks[j] = _pendingAcks[j + 1];
      }
      _pendingAckCount--;
      return;
    }
  }
}

// R10G-2: Publish callback (injected by MqttClient)
static std::function<bool(const char* topic, const uint8_t* payload, size_t len)> _publishCallback;

void setPublishCallback(std::function<bool(const char* topic, const uint8_t* payload, size_t len)> cb) {
  _publishCallback = cb;
}

// ============================================================================
// R10G-2: Process pending ACK queue — called from loop()
// ============================================================================
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
