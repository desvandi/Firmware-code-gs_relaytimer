// =============================================================================
// Services/TransactionJournal.cpp — NVS-persisted transaction journal
// =============================================================================
// CYCLE-8A (AUDIT-7-001): Transaction Recovery State Machine + Boot Reconciliation
//
//   Closes the crash-window ambiguity identified by auditor AUDIT-7-001.
//   On boot, PENDING/EXECUTING entries are reconciled with actual GPIO state.
//   FAILED entries allow retry. COMMITTED_UNKNOWN entries replay ACK with disclaimer.
// =============================================================================
#include "TransactionJournal.h"
#include "Config.h"
#include "LogService.h"
#include "RelayDriver.h"
#include "RtcDriver.h"
#include <Preferences.h>
#include <string.h>
#include <esp_crc.h>

namespace Services {

TransactionJournal journal;

// NVS keys
static const char* NVS_KEY_TJ_ENTRY_PREFIX = "tj_entry_";
static const char* NVS_KEY_TJ_COMMIT_PREFIX = "tj_commit_";
static const char* NVS_KEY_TJ_STATE_PREFIX = "tj_state_";   // CYCLE-8A: transaction state
static const char* NVS_KEY_TJ_COUNT = "tj_count";
static const char* NVS_KEY_TJ_WIDX = "tj_widx";

// ============================================================================
uint32_t TransactionJournal::_computeCRC(const uint8_t* data, size_t len) {
  return esp_crc32_le(0, data, len);
}

const char* TransactionJournal::_stateToString(TransactionState s) {
  switch (s) {
    case TransactionState::PENDING:           return "PENDING";
    case TransactionState::EXECUTING:         return "EXECUTING";
    case TransactionState::COMMITTED:         return "COMMITTED";
    case TransactionState::COMMITTED_UNKNOWN: return "COMMITTED_UNKNOWN";
    case TransactionState::FAILED:            return "FAILED";
    default: return "UNKNOWN";
  }
}

// ============================================================================
// Deserialize blob (version 2) + verify integrity
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

  // CYCLE-8A: version check. Version 1 entries (from Cycle 7) are incompatible
  // with version 2 schema. They are treated as corrupt and cleared.
  if (blob[2] != BLOB_VERSION) {
    Serial.printf("[Journal] Entry %u: version %u incompatible (expected %u) — clearing slot\n",
                  idx, blob[2], BLOB_VERSION);
    return false;
  }

  (void)blob[3];

  uint32_t storedCRC = blob[4] | (blob[5] << 8) | (blob[6] << 16) | ((uint32_t)blob[7] << 24);

  const uint8_t* payload = blob + BLOB_HEADER_SIZE;
  size_t payloadLen = len - BLOB_HEADER_SIZE;
  uint32_t computedCRC = _computeCRC(payload, payloadLen);

  if (storedCRC != computedCRC) {
    Serial.printf("[Journal] Entry %u: CRC mismatch — CORRUPT\n", idx);
    return false;
  }

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

  // CYCLE-8A: channelId (1 byte)
  if (offset >= payloadLen) return false;
  _journalChannelId[idx] = payload[offset++];

  // CYCLE-8A: desiredState (1 byte)
  if (offset >= payloadLen) return false;
  uint8_t dsRaw = payload[offset++];
  _journalDesiredState[idx] = (dsRaw == 1);

  // CYCLE-8A: previousKnownState (1 byte)
  if (offset >= payloadLen) return false;
  uint8_t pksRaw = payload[offset++];
  _journalPreviousKnownState[idx] = (pksRaw == 1);

  // CYCLE-8A: attempt (1 byte)
  if (offset >= payloadLen) return false;
  _journalAttempt[idx] = payload[offset++];

  // CYCLE-8A: timestamp (4 bytes LE)
  if (offset + 4 > payloadLen) return false;
  _journalTimestamp[idx] = payload[offset] | (payload[offset+1] << 8) |
                           (payload[offset+2] << 16) | ((uint32_t)payload[offset+3] << 24);
  offset += 4;

  // ackJson (2-byte length LE)
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
// ============================================================================
bool TransactionJournal::_saveEntryToNVSAtomic(uint8_t idx, bool commitToCommitted) {
  if (idx >= JOURNAL_SIZE) return false;

  const String& rid = _journalIds[idx];
  const String& hash = _journalHashes[idx];
  const String& ack = _journalAcks[idx];

  uint8_t idLen = min((unsigned)rid.length(), (unsigned)64);
  uint8_t hashLen = min((unsigned)hash.length(), (unsigned)64);
  uint16_t ackLen = min((unsigned)ack.length(), (unsigned)1024);

  // CYCLE-8A: payload now includes channelId(1) + desiredState(1) +
  // previousKnownState(1) + attempt(1) + timestamp(4) = 8 extra bytes
  size_t payloadLen = 1 + idLen + 1 + hashLen + 1 + 1 + 1 + 1 + 4 + 2 + ackLen;
  size_t totalLen = BLOB_HEADER_SIZE + payloadLen;

  if (totalLen > BLOB_SIZE) {
    Serial.printf("[Journal] ERROR: blob too large (%u > %u)\n", totalLen, BLOB_SIZE);
    return false;
  }

  uint8_t blob[BLOB_SIZE];
  memset(blob, 0, BLOB_SIZE);

  blob[0] = BLOB_MAGIC1;
  blob[1] = BLOB_MAGIC2;
  blob[2] = BLOB_VERSION;  // CYCLE-8A: version 2
  blob[3] = 0;

  uint8_t* payload = blob + BLOB_HEADER_SIZE;
  size_t offset = 0;

  payload[offset++] = idLen;
  memcpy(payload + offset, rid.c_str(), idLen);
  offset += idLen;

  payload[offset++] = hashLen;
  memcpy(payload + offset, hash.c_str(), hashLen);
  offset += hashLen;

  // CYCLE-8A: new fields
  payload[offset++] = _journalChannelId[idx];
  payload[offset++] = _journalDesiredState[idx] ? 1 : 0;
  payload[offset++] = _journalPreviousKnownState[idx] ? 1 : 0;
  payload[offset++] = _journalAttempt[idx];
  uint32_t ts = _journalTimestamp[idx];
  payload[offset++] = ts & 0xFF;
  payload[offset++] = (ts >> 8) & 0xFF;
  payload[offset++] = (ts >> 16) & 0xFF;
  payload[offset++] = (ts >> 24) & 0xFF;

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
  char stateKey[20];
  snprintf(entryKey, sizeof(entryKey), "%s%u", NVS_KEY_TJ_ENTRY_PREFIX, idx);
  snprintf(commitKey, sizeof(commitKey), "%s%u", NVS_KEY_TJ_COMMIT_PREFIX, idx);
  snprintf(stateKey, sizeof(stateKey), "%s%u", NVS_KEY_TJ_STATE_PREFIX, idx);

  // Phase 0: Clear commit flag + set state to PENDING
  prefs.putUChar(commitKey, 0);
  prefs.putUChar(stateKey, (uint8_t)TransactionState::PENDING);

  // Phase 1: Write blob data
  size_t written = prefs.putBytes(entryKey, blob, totalLen);
  if (written != totalLen) {
    Serial.printf("[Journal] Phase 1 write FAILED (wrote %u/%u bytes)\n", written, totalLen);
    prefs.end();
    _journalValid[idx] = false;
    return false;
  }

  // Phase 1b: Persist writeIdx (only for NEW entries)
  bool isNewSlot = !_journalValid[idx];
  if (isNewSlot) {
    uint8_t nextWriteIdx = (_journalWriteIdx + 1) % JOURNAL_SIZE;
    size_t widxWritten = prefs.putUChar(NVS_KEY_TJ_WIDX, nextWriteIdx);
    if (widxWritten != 1) {
      Serial.printf("[Journal] Phase 1b writeIdx persist FAILED\n");
      prefs.end();
      _journalValid[idx] = false;
      return false;
    }
    _journalWriteIdx = nextWriteIdx;
    if (_journalSize < JOURNAL_SIZE) _journalSize++;
  }

  // Phase 2: Set commit flag if requested
  if (commitToCommitted) {
    size_t commitWritten = prefs.putUChar(commitKey, 1);
    if (commitWritten != 1) {
      Serial.printf("[Journal] Phase 2 commit FAILED (idx=%u)\n", idx);
      prefs.end();
      _journalValid[idx] = true;
      _journalCommitted[idx] = false;
      _journalState[idx] = TransactionState::EXECUTING;  // stuck in EXECUTING
      return false;
    }
    _journalCommitted[idx] = true;
    _journalState[idx] = TransactionState::COMMITTED;
    prefs.putUChar(stateKey, (uint8_t)TransactionState::COMMITTED);
  } else {
    _journalCommitted[idx] = false;
    _journalState[idx] = TransactionState::PENDING;
    // state key already set to PENDING in Phase 0
  }

  prefs.putUChar(NVS_KEY_TJ_COUNT, _journalSize < JOURNAL_SIZE ? _journalSize : JOURNAL_SIZE);

  prefs.end();
  _journalValid[idx] = true;
  return true;
}

// ============================================================================
// CYCLE-8A: _setTransactionStateNVS — update transaction state in NVS
// ============================================================================
bool TransactionJournal::_setTransactionStateNVS(uint8_t idx, TransactionState state) {
  if (idx >= JOURNAL_SIZE) return false;

  Preferences prefs;
  if (!prefs.begin(Core::NVS_NAMESPACE, false)) return false;

  char stateKey[20];
  snprintf(stateKey, sizeof(stateKey), "%s%u", NVS_KEY_TJ_STATE_PREFIX, idx);
  size_t written = prefs.putUChar(stateKey, (uint8_t)state);
  prefs.end();

  if (written != 1) {
    Serial.printf("[Journal] _setTransactionStateNVS FAILED (idx=%u, state=%s)\n",
                  idx, _stateToString(state));
    return false;
  }

  _journalState[idx] = state;
  return true;
}

// ============================================================================
// _commitSlotNVS — flip commit flag 0 → 1 + update ackJson
// ============================================================================
bool TransactionJournal::_commitSlotNVS(uint8_t idx, const String& ackJson) {
  if (idx >= JOURNAL_SIZE) return false;
  if (!_journalValid[idx]) {
    Serial.printf("[Journal] _commitSlotNVS: slot %u not valid\n", idx);
    return false;
  }

  _journalAcks[idx] = ackJson;
  return _saveEntryToNVSAtomic(idx, true);
}

// ============================================================================
void TransactionJournal::_clearSlotNVS(uint8_t idx) {
  Preferences prefs;
  if (!prefs.begin(Core::NVS_NAMESPACE, false)) return;

  char commitKey[20];
  char stateKey[20];
  snprintf(commitKey, sizeof(commitKey), "%s%u", NVS_KEY_TJ_COMMIT_PREFIX, idx);
  snprintf(stateKey, sizeof(stateKey), "%s%u", NVS_KEY_TJ_STATE_PREFIX, idx);
  prefs.putUChar(commitKey, 0);
  prefs.putUChar(stateKey, (uint8_t)TransactionState::PENDING);
  prefs.end();
}

// ============================================================================
// Boot: Load all valid entries + reconcile
// ============================================================================
void TransactionJournal::begin() {
  _loadFromNVS();
  Serial.printf("[Journal] Loaded %u entries from NVS (capacity %u, version=%u)\n",
                _journalSize, JOURNAL_SIZE, BLOB_VERSION);

  // CYCLE-8A: Reconcile PENDING/EXECUTING entries with actual GPIO state.
  uint8_t reconciled = reconcilePendingEntries();

  // Queue COMMITTED and COMMITTED_UNKNOWN ACKs for re-delivery
  uint8_t committed = 0, committedUnknown = 0, pending = 0, failed = 0;
  for (uint8_t i = 0; i < JOURNAL_SIZE; i++) {
    if (_journalValid[i]) {
      switch (_journalState[i]) {
        case TransactionState::COMMITTED:
          committed++;
          if (_journalAcks[i].length() > 0) {
            queueAck(_journalIds[i], _journalAcks[i]);
          }
          break;
        case TransactionState::COMMITTED_UNKNOWN:
          committedUnknown++;
          if (_journalAcks[i].length() > 0) {
            queueAck(_journalIds[i], _journalAcks[i]);
          }
          break;
        case TransactionState::FAILED:
          failed++;
          break;
        case TransactionState::PENDING:
        case TransactionState::EXECUTING:
          pending++;
          break;
      }
    }
  }
  Serial.printf("[Journal] Boot summary: %u committed, %u committed_unknown, %u failed, %u pending (reconciled %u)\n",
                committed, committedUnknown, failed, pending, reconciled);
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
  char stateKey[20];
  uint8_t blob[BLOB_SIZE];

  for (uint8_t i = 0; i < JOURNAL_SIZE; i++) {
    snprintf(entryKey, sizeof(entryKey), "%s%u", NVS_KEY_TJ_ENTRY_PREFIX, i);
    snprintf(commitKey, sizeof(commitKey), "%s%u", NVS_KEY_TJ_COMMIT_PREFIX, i);
    snprintf(stateKey, sizeof(stateKey), "%s%u", NVS_KEY_TJ_STATE_PREFIX, i);

    uint8_t committed = prefs.getUChar(commitKey, 0);
    // CYCLE-8A: read transaction state (defaults to PENDING for old entries)
    uint8_t stateRaw = prefs.getUChar(stateKey, (uint8_t)TransactionState::PENDING);

    size_t len = prefs.getBytesLength(entryKey);
    if (len == 0 || len > BLOB_SIZE) {
      _journalValid[i] = false;
      _journalCommitted[i] = false;
      _journalState[i] = TransactionState::PENDING;
      // Clear stale commit/state if blob missing
      if (committed == 1 || stateRaw != (uint8_t)TransactionState::PENDING) {
        prefs.end();
        Preferences rw;
        rw.begin(Core::NVS_NAMESPACE, false);
        rw.putUChar(commitKey, 0);
        rw.putUChar(stateKey, (uint8_t)TransactionState::PENDING);
        rw.end();
        prefs.begin(Core::NVS_NAMESPACE, true);
      }
      continue;
    }

    prefs.getBytes(entryKey, blob, len);

    if (_deserializeEntry(blob, len, i)) {
      _journalCommitted[i] = (committed == 1);
      _journalState[i] = (TransactionState)stateRaw;
      _journalSize++;
    } else {
      Serial.printf("[Journal] Entry %u: blob CORRUPT or version mismatch — slot freed\n", i);
      _journalValid[i] = false;
      _journalCommitted[i] = false;
      _journalState[i] = TransactionState::PENDING;
      _journalIds[i] = "";
      _journalHashes[i] = "";
      _journalAcks[i] = "";
      _journalChannelId[i] = 0;
      _journalDesiredState[i] = false;
      _journalPreviousKnownState[i] = false;
      _journalAttempt[i] = 0;
      _journalTimestamp[i] = 0;
      // Clear NVS keys
      prefs.end();
      Preferences rw;
      rw.begin(Core::NVS_NAMESPACE, false);
      rw.putUChar(commitKey, 0);
      rw.putUChar(stateKey, (uint8_t)TransactionState::PENDING);
      rw.end();
      prefs.begin(Core::NVS_NAMESPACE, true);
    }
  }

  prefs.end();
}

// ============================================================================
// CYCLE-8A: Reconcile PENDING/EXECUTING entries with actual GPIO state
// ============================================================================
uint8_t TransactionJournal::reconcilePendingEntries() {
  uint8_t count = 0;
  for (uint8_t i = 0; i < JOURNAL_SIZE; i++) {
    if (!_journalValid[i]) continue;
    if (_journalState[i] != TransactionState::PENDING &&
        _journalState[i] != TransactionState::EXECUTING) continue;

    count++;

    Serial.printf("[Journal] Reconciling slot %u: rid=%s state=%s channelId=%u desiredState=%d\n",
                  i, _journalIds[i].c_str(), _stateToString(_journalState[i]),
                  _journalChannelId[i], _journalDesiredState[i]);

    // Non-relay commands (channelId == 0): can't reconcile via GPIO.
    // Leave as PENDING/EXECUTING — these are schedule/config commands that
    // either completed idempotently or need manual review.
    if (_journalChannelId[i] == 0) {
      Serial.printf("[Journal] Slot %u: non-relay command, cannot reconcile via GPIO — marking COMMITTED_UNKNOWN (best-effort)\n", i);
      Services::Log.append(Core::LogType::Error,
        "Non-relay transaction reconciled as COMMITTED_UNKNOWN (no GPIO verification possible): " + _journalIds[i], 0);
      _setTransactionStateNVS(i, TransactionState::COMMITTED_UNKNOWN);
      continue;
    }

    // Relay command: read actual GPIO output state
    uint8_t chIdx = _journalChannelId[i] - 1;  // channelId is 1-based, array is 0-based
    if (chIdx >= Core::NUM_CHANNELS) {
      Serial.printf("[Journal] Slot %u: invalid channelId %u — marking FAILED\n", i, _journalChannelId[i]);
      _setTransactionStateNVS(i, TransactionState::FAILED);
      continue;
    }

    bool actualGpioState = Drivers::relay.readLogicalState(chIdx);
    bool desired = _journalDesiredState[i];

    if (actualGpioState == desired) {
      // GPIO matches desired — likely succeeded (or was already in desired state)
      Serial.printf("[Journal] Slot %u: GPIO matches desired (actual=%d desired=%d) → COMMITTED_UNKNOWN\n",
                    i, actualGpioState, desired);
      Services::Log.append(Core::LogType::Error,
        "Transaction reconciled as COMMITTED_UNKNOWN (GPIO matches but no durable proof of execution): " + _journalIds[i], 0);
      _setTransactionStateNVS(i, TransactionState::COMMITTED_UNKNOWN);
    } else {
      // GPIO doesn't match desired — execute didn't happen or failed
      Serial.printf("[Journal] Slot %u: GPIO mismatch (actual=%d desired=%d) → FAILED (will allow retry)\n",
                    i, actualGpioState, desired);
      Services::Log.append(Core::LogType::Error,
        "Transaction reconciled as FAILED (GPIO doesn't match desired state): " + _journalIds[i], 0);
      _setTransactionStateNVS(i, TransactionState::FAILED);
    }
  }
  return count;
}

// Reconcile a single entry (used on retry during normal operation)
TransactionState TransactionJournal::reconcileEntry(const String& requestId) {
  int idx = _findInJournal(requestId);
  if (idx < 0) return TransactionState::PENDING;

  if (_journalState[idx] != TransactionState::PENDING &&
      _journalState[idx] != TransactionState::EXECUTING) {
    return _journalState[idx];  // already resolved
  }

  if (_journalChannelId[idx] == 0) {
    // Non-relay command — can't reconcile via GPIO, mark as COMMITTED_UNKNOWN
    _setTransactionStateNVS(idx, TransactionState::COMMITTED_UNKNOWN);
    return _journalState[idx];
  }

  uint8_t chIdx = _journalChannelId[idx] - 1;
  if (chIdx >= Core::NUM_CHANNELS) {
    _setTransactionStateNVS(idx, TransactionState::FAILED);
    return _journalState[idx];
  }

  bool actualGpioState = Drivers::relay.readLogicalState(chIdx);
  bool desired = _journalDesiredState[idx];

  if (actualGpioState == desired) {
    _setTransactionStateNVS(idx, TransactionState::COMMITTED_UNKNOWN);
  } else {
    _setTransactionStateNVS(idx, TransactionState::FAILED);
  }
  return _journalState[idx];
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

// CYCLE-8A: isProcessed now EXCLUDES FAILED entries.
//   FAILED entries allow retry with same requestId.
bool TransactionJournal::isProcessed(const String& requestId) {
  int idx = _findInJournal(requestId);
  if (idx < 0) return false;
  return _journalState[idx] != TransactionState::FAILED;
}

bool TransactionJournal::isCommitted(const String& requestId) {
  int idx = _findInJournal(requestId);
  if (idx < 0) return false;
  return _journalState[idx] == TransactionState::COMMITTED ||
         _journalState[idx] == TransactionState::COMMITTED_UNKNOWN;
}

TransactionState TransactionJournal::getTransactionState(const String& requestId) {
  int idx = _findInJournal(requestId);
  if (idx < 0) return TransactionState::PENDING;
  return _journalState[idx];
}

String TransactionJournal::getCommandHash(const String& requestId) {
  int idx = _findInJournal(requestId);
  if (idx >= 0) return _journalHashes[idx];
  return "";
}

String TransactionJournal::getAckJson(const String& requestId) {
  int idx = _findInJournal(requestId);
  if (idx >= 0 && (_journalState[idx] == TransactionState::COMMITTED ||
                   _journalState[idx] == TransactionState::COMMITTED_UNKNOWN)) {
    return _journalAcks[idx];
  }
  return "";
}

uint8_t TransactionJournal::getChannelId(const String& requestId) {
  int idx = _findInJournal(requestId);
  if (idx >= 0) return _journalChannelId[idx];
  return 0;
}

bool TransactionJournal::getDesiredState(const String& requestId) {
  int idx = _findInJournal(requestId);
  if (idx >= 0) return _journalDesiredState[idx];
  return false;
}

// ============================================================================
// storeIntent — durable PENDING entry BEFORE execute
// ============================================================================
bool TransactionJournal::storeIntent(const String& requestId, const String& commandHash,
                                     uint8_t channelId, bool desiredState,
                                     bool previousKnownState) {
  if (requestId.length() == 0 || requestId.length() > 64) return false;
  if (commandHash.length() == 0 || commandHash.length() > 64) return false;

  int existing = _findInJournal(requestId);
  if (existing >= 0) {
    Serial.printf("[Journal] storeIntent: requestId %s already exists (state=%s)\n",
                  requestId.c_str(), _stateToString(_journalState[existing]));
    return false;
  }

  uint8_t idx = _journalWriteIdx;
  _journalIds[idx] = requestId;
  _journalHashes[idx] = commandHash;
  _journalAcks[idx] = "";
  _journalChannelId[idx] = channelId;
  _journalDesiredState[idx] = desiredState;
  _journalPreviousKnownState[idx] = previousKnownState;
  _journalAttempt[idx] = 0;
  _journalTimestamp[idx] = (uint32_t)Drivers::rtc.getUnixTime();
  _journalState[idx] = TransactionState::PENDING;

  if (!_saveEntryToNVSAtomic(idx, false)) {
    Serial.printf("[Journal] storeIntent FAILED for rid=%s — execute MUST NOT proceed\n",
                  requestId.c_str());
    _journalIds[idx] = "";
    _journalHashes[idx] = "";
    _journalAcks[idx] = "";
    _journalValid[idx] = false;
    _journalState[idx] = TransactionState::PENDING;
    return false;
  }

  Serial.printf("[Journal] Intent stored: rid=%s (slot %u, PENDING, ch=%u, desired=%d)\n",
                requestId.c_str(), idx, channelId, desiredState);
  return true;
}

// ============================================================================
// CYCLE-8A: markExecuting — narrow the crash window
// ============================================================================
bool TransactionJournal::markExecuting(const String& requestId) {
  int idx = _findInJournal(requestId);
  if (idx < 0) {
    Serial.printf("[Journal] markExecuting: rid=%s NOT FOUND\n", requestId.c_str());
    return false;
  }
  if (_journalState[idx] != TransactionState::PENDING) {
    Serial.printf("[Journal] markExecuting: rid=%s state=%s (expected PENDING)\n",
                  requestId.c_str(), _stateToString(_journalState[idx]));
    return false;
  }
  _journalAttempt[idx]++;
  if (!_setTransactionStateNVS(idx, TransactionState::EXECUTING)) {
    return false;
  }
  Serial.printf("[Journal] Marked EXECUTING: rid=%s (attempt %u)\n",
                requestId.c_str(), _journalAttempt[idx]);
  return true;
}

// ============================================================================
// commitTransaction — flip PENDING/EXECUTING → COMMITTED, store ackJson
// ============================================================================
bool TransactionJournal::commitTransaction(const String& requestId, const String& ackJson) {
  int idx = _findInJournal(requestId);
  if (idx < 0) {
    Serial.printf("[Journal] commitTransaction: rid=%s NOT FOUND\n", requestId.c_str());
    return false;
  }

  if (_journalState[idx] == TransactionState::COMMITTED) {
    Serial.printf("[Journal] commitTransaction: rid=%s already COMMITTED — updating ackJson\n",
                  requestId.c_str());
  }

  if (!_commitSlotNVS(idx, ackJson)) {
    Serial.printf("[Journal] commitTransaction FAILED for rid=%s\n", requestId.c_str());
    Services::Log.append(Core::LogType::Error,
      "Transaction commit FAILED (NVS write error): " + requestId, 0);
    return false;
  }

  queueAck(requestId, ackJson);
  Serial.printf("[Journal] Committed: rid=%s (slot %u, COMMITTED)\n", requestId.c_str(), idx);
  return true;
}

// ============================================================================
// Legacy storeTransaction — kept for backward compat
// ============================================================================
bool TransactionJournal::storeTransaction(const String& requestId,
                                           const String& commandHash,
                                           const String& ackJson) {
  int existing = _findInJournal(requestId);
  if (existing >= 0) {
    _journalHashes[existing] = commandHash;
    _journalAcks[existing] = ackJson;
    bool ok = _saveEntryToNVSAtomic(existing, true);
    if (ok) {
      queueAck(requestId, ackJson);
    }
    return ok;
  }

  if (!storeIntent(requestId, commandHash)) {
    return false;
  }
  return commitTransaction(requestId, ackJson);
}

// ============================================================================
// CYCLE-8A: clearEntry — remove a FAILED entry (allows retry with same requestId)
// ============================================================================
void TransactionJournal::clearEntry(const String& requestId) {
  int idx = _findInJournal(requestId);
  if (idx < 0) return;

  if (_journalState[idx] != TransactionState::FAILED) {
    Serial.printf("[Journal] clearEntry: rid=%s state=%s (only FAILED can be cleared)\n",
                  requestId.c_str(), _stateToString(_journalState[idx]));
    return;
  }

  Serial.printf("[Journal] Clearing FAILED entry: rid=%s (slot %u) — retry allowed\n",
                requestId.c_str(), idx);
  _clearSlotNVS(idx);
  _journalValid[idx] = false;
  _journalCommitted[idx] = false;
  _journalState[idx] = TransactionState::PENDING;
  _journalIds[idx] = "";
  _journalHashes[idx] = "";
  _journalAcks[idx] = "";
  _journalChannelId[idx] = 0;
  _journalDesiredState[idx] = false;
  _journalPreviousKnownState[idx] = false;
  _journalAttempt[idx] = 0;
  _journalTimestamp[idx] = 0;
  if (_journalSize > 0) _journalSize--;
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

void TransactionJournal::dequeueAck(const String& requestId) {
  for (uint8_t i = 0; i < _pendingAckCount; i++) {
    if (_pendingAcks[i].requestId == requestId) {
      for (uint8_t j = i; j < _pendingAckCount - 1; j++) {
        _pendingAcks[j] = _pendingAcks[j + 1];
      }
      _pendingAckCount--;
      return;
    }
  }
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
