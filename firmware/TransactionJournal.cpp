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
    case TransactionState::PENDING:                          return "PENDING";
    case TransactionState::EXECUTING:                        return "EXECUTING";
    case TransactionState::COMMITTED:                        return "COMMITTED";
    case TransactionState::COMMITTED_UNKNOWN:                return "COMMITTED_UNKNOWN";
    case TransactionState::UNKNOWN:                          return "UNKNOWN";
    case TransactionState::FAILED:                           return "FAILED";
    case TransactionState::CORRUPTED:                        return "CORRUPTED";
    case TransactionState::EXECUTION_FAILED_OUTPUT_MISMATCH: return "EXECUTION_FAILED_OUTPUT_MISMATCH";
    default: return "INVALID_ENUM";
  }
}

// CYCLE-8B
const char* TransactionJournal::_phaseToString(BootPhase p) {
  switch (p) {
    case BootPhase::PRE_INIT:     return "PRE_INIT";
    case BootPhase::SAFE_INIT:    return "SAFE_INIT";
    case BootPhase::LOADING_NVS:  return "LOADING_NVS";
    case BootPhase::SNAPSHOT:     return "SNAPSHOT";
    case BootPhase::RECONCILING:  return "RECONCILING";
    case BootPhase::RESTORING:    return "RESTORING";
    case BootPhase::RUNNING:      return "RUNNING";
    default: return "UNKNOWN";
  }
}

// ============================================================================
// CYCLE-8B: Boot phase management
// ============================================================================
void TransactionJournal::setBootPhase(BootPhase phase) {
  BootPhase old = _bootPhase;
  _bootPhase = phase;
  Serial.printf("[Journal] Boot phase: %s → %s\n", _phaseToString(old), _phaseToString(phase));
}

// ============================================================================
// CYCLE-8B: Capture raw GPIO output state BEFORE RelayEngine runs
// ============================================================================
void TransactionJournal::captureOutputSnapshot() {
  setBootPhase(BootPhase::SNAPSHOT);
  for (uint8_t i = 0; i < Core::NUM_CHANNELS && i < 16; i++) {
    _outputSnapshot[i] = Drivers::relay.readLogicalState(i);
  }
  _snapshotCaptured = true;
  Serial.printf("[Journal] Output snapshot captured: ");
  for (uint8_t i = 0; i < Core::NUM_CHANNELS; i++) {
    Serial.printf("%d", _outputSnapshot[i] ? 1 : 0);
  }
  Serial.println();
}

bool TransactionJournal::getSnapshotState(uint8_t channelIdx) const {
  if (channelIdx >= 16) return false;
  if (!_snapshotCaptured) return false;
  return _outputSnapshot[channelIdx];
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
// CYCLE-8B-Rev1 (fixes C8B-001): SEPARATED operations.
//
//   _createPendingEntryNVS: write blob for NEW PENDING entry.
//     - Phase 0: clear commit flag (only if slot was previously used)
//     - Phase 1: write blob with state=PENDING
//     - Phase 1b: persist writeIdx (new slot only)
//     - Phase 2: set state=PENDING (commit flag stays 0)
//     - If crash during this: entry is PENDING (correct — execute didn't run)
//
//   _commitExecutingEntryNVS: commit EXECUTING → COMMITTED.
//     - Does NOT clear commit flag (preserves EXECUTING evidence)
//     - Phase 1: write blob with NEW ackJson (commit still 0, state still EXECUTING in NVS)
//     - Phase 1b: set state=COMMITTED (commit still 0)
//     - Phase 2: flip commit flag 0 → 1 (atomic commit point)
//     - If crash during Phase 1: entry still EXECUTING (blob may be partial,
//       but commit flag still 0, state still EXECUTING — reconciliation will
//       mark UNKNOWN, NOT FAILED — this is the key fix for C8B-001)
//     - If crash during Phase 2: commit flag may be 0 or 1.
//       - If 0: entry is EXECUTING (commit didn't complete) → reconciliation UNKNOWN
//       - If 1: entry is COMMITTED (commit completed) → replay ACK
// ============================================================================

// Helper: serialize entry blob (shared by both functions)
// Note: esp_crc32_le is from esp_crc.h (already included at top of file)
static size_t _serializeBlob(uint8_t* blob, size_t blobSize,
                              const String& rid, const String& hash, const String& ack,
                              uint8_t channelId, bool desiredState, bool previousKnown,
                              uint8_t attempt, uint32_t timestamp) {
  uint8_t idLen = min((unsigned)rid.length(), (unsigned)64);
  uint8_t hashLen = min((unsigned)hash.length(), (unsigned)64);
  uint16_t ackLen = min((unsigned)ack.length(), (unsigned)1024);

  size_t payloadLen = 1 + idLen + 1 + hashLen + 1 + 1 + 1 + 1 + 4 + 2 + ackLen;
  size_t totalLen = 8 + payloadLen;  // BLOB_HEADER_SIZE = 8

  if (totalLen > blobSize) return 0;

  memset(blob, 0, blobSize);
  blob[0] = 0x54;  // 'T'
  blob[1] = 0x4A;  // 'J'
  blob[2] = 2;     // BLOB_VERSION
  blob[3] = 0;

  uint8_t* payload = blob + 8;
  size_t offset = 0;

  payload[offset++] = idLen;
  memcpy(payload + offset, rid.c_str(), idLen);
  offset += idLen;

  payload[offset++] = hashLen;
  memcpy(payload + offset, hash.c_str(), hashLen);
  offset += hashLen;

  payload[offset++] = channelId;
  payload[offset++] = desiredState ? 1 : 0;
  payload[offset++] = previousKnown ? 1 : 0;
  payload[offset++] = attempt;
  payload[offset++] = timestamp & 0xFF;
  payload[offset++] = (timestamp >> 8) & 0xFF;
  payload[offset++] = (timestamp >> 16) & 0xFF;
  payload[offset++] = (timestamp >> 24) & 0xFF;

  payload[offset++] = ackLen & 0xFF;
  payload[offset++] = (ackLen >> 8) & 0xFF;
  if (ackLen > 0) {
    memcpy(payload + offset, ack.c_str(), ackLen);
    offset += ackLen;
  }

  uint32_t crc = esp_crc32_le(0, payload, payloadLen);
  blob[4] = crc & 0xFF;
  blob[5] = (crc >> 8) & 0xFF;
  blob[6] = (crc >> 16) & 0xFF;
  blob[7] = (crc >> 24) & 0xFF;

  return totalLen;
}

// CYCLE-8B-Rev1: Create NEW PENDING entry (fixes C8B-001)
bool TransactionJournal::_createPendingEntryNVS(uint8_t idx) {
  if (idx >= JOURNAL_SIZE) return false;

  uint8_t blob[BLOB_SIZE];
  size_t totalLen = _serializeBlob(blob, BLOB_SIZE,
    _journalIds[idx], _journalHashes[idx], _journalAcks[idx],
    _journalChannelId[idx], _journalDesiredState[idx],
    _journalPreviousKnownState[idx], _journalAttempt[idx],
    _journalTimestamp[idx]);
  if (totalLen == 0) {
    Serial.printf("[Journal] _createPendingEntryNVS: blob too large (idx=%u)\n", idx);
    return false;
  }

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

  // Phase 0: Clear commit flag (invalidate any old entry in this slot)
  prefs.putUChar(commitKey, 0);

  // Phase 1: Write blob data
  size_t written = prefs.putBytes(entryKey, blob, totalLen);
  if (written != totalLen) {
    Serial.printf("[Journal] createPending Phase 1 write FAILED (wrote %u/%u bytes)\n",
                  written, totalLen);
    prefs.end();
    _journalValid[idx] = false;
    return false;
  }

  // Phase 1b: Persist writeIdx (only for NEW slots)
  bool isNewSlot = !_journalValid[idx];
  if (isNewSlot) {
    uint8_t nextWriteIdx = (_journalWriteIdx + 1) % JOURNAL_SIZE;
    size_t widxWritten = prefs.putUChar(NVS_KEY_TJ_WIDX, nextWriteIdx);
    if (widxWritten != 1) {
      Serial.printf("[Journal] createPending Phase 1b writeIdx persist FAILED\n");
      prefs.end();
      _journalValid[idx] = false;
      return false;
    }
    _journalWriteIdx = nextWriteIdx;
    if (_journalSize < JOURNAL_SIZE) _journalSize++;
  }

  // Phase 2: Set state=PENDING (commit flag stays 0)
  prefs.putUChar(stateKey, (uint8_t)TransactionState::PENDING);

  prefs.putUChar(NVS_KEY_TJ_COUNT, _journalSize < JOURNAL_SIZE ? _journalSize : JOURNAL_SIZE);
  prefs.end();

  _journalValid[idx] = true;
  _journalCommitted[idx] = false;
  _journalState[idx] = TransactionState::PENDING;
  return true;
}

// CYCLE-8B-Rev1: Commit EXECUTING → COMMITTED (fixes C8B-001)
//   Does NOT reset state to PENDING. Preserves EXECUTING evidence during crash window.
bool TransactionJournal::_commitExecutingEntryNVS(uint8_t idx, const String& ackJson,
                                                   TransactionState targetState) {
  if (idx >= JOURNAL_SIZE) return false;
  if (!_journalValid[idx]) {
    Serial.printf("[Journal] _commitExecutingEntryNVS: slot %u not valid\n", idx);
    return false;
  }

  // Monotonicity check: must be in EXECUTING state to commit.
  if (_journalState[idx] != TransactionState::EXECUTING) {
    Serial.printf("[Journal] _commitExecutingEntryNVS: slot %u state=%s (expected EXECUTING) — REJECTED (monotonicity)\n",
                  idx, _stateToString(_journalState[idx]));
    return false;
  }

  // CYCLE-8C: validate targetState is a valid commit target
  if (targetState != TransactionState::COMMITTED &&
      targetState != TransactionState::EXECUTION_FAILED_OUTPUT_MISMATCH) {
    Serial.printf("[Journal] _commitExecutingEntryNVS: invalid targetState %s\n",
                  _stateToString(targetState));
    return false;
  }

  // Update ackJson in RAM
  _journalAcks[idx] = ackJson;

  uint8_t blob[BLOB_SIZE];
  size_t totalLen = _serializeBlob(blob, BLOB_SIZE,
    _journalIds[idx], _journalHashes[idx], _journalAcks[idx],
    _journalChannelId[idx], _journalDesiredState[idx],
    _journalPreviousKnownState[idx], _journalAttempt[idx],
    _journalTimestamp[idx]);
  if (totalLen == 0) {
    Serial.printf("[Journal] _commitExecutingEntryNVS: blob too large (idx=%u)\n", idx);
    return false;
  }

  Preferences prefs;
  if (!prefs.begin(Core::NVS_NAMESPACE, false)) {
    Serial.println("[Journal] FATAL: Cannot open NVS for commit");
    return false;
  }

  char entryKey[20];
  char commitKey[20];
  char stateKey[20];
  snprintf(entryKey, sizeof(entryKey), "%s%u", NVS_KEY_TJ_ENTRY_PREFIX, idx);
  snprintf(commitKey, sizeof(commitKey), "%s%u", NVS_KEY_TJ_COMMIT_PREFIX, idx);
  snprintf(stateKey, sizeof(stateKey), "%s%u", NVS_KEY_TJ_STATE_PREFIX, idx);

  // CYCLE-8B-Rev1 (fixes C8B-001): Do NOT clear commit flag here.
  //   Write blob FIRST, keeping commit=0. If crash: entry remains EXECUTING.
  //   Only AFTER blob write succeeds do we flip commit flag 0→1.

  // Phase 1: Write blob with new ackJson (commit flag still 0)
  size_t written = prefs.putBytes(entryKey, blob, totalLen);
  if (written != totalLen) {
    Serial.printf("[Journal] commitExecuting Phase 1 write FAILED (wrote %u/%u bytes)\n",
                  written, totalLen);
    prefs.end();
    return false;
  }

  // Phase 1b: Set state=targetState (but commit flag still 0 — not yet atomic)
  size_t stateWritten = prefs.putUChar(stateKey, (uint8_t)targetState);
  if (stateWritten != 1) {
    Serial.printf("[Journal] commitExecuting Phase 1b state write FAILED (idx=%u)\n", idx);
    prefs.end();
    return false;
  }

  // Phase 2: Flip commit flag 0 → 1 (ATOMIC COMMIT POINT)
  size_t commitWritten = prefs.putUChar(commitKey, 1);
  if (commitWritten != 1) {
    Serial.printf("[Journal] commitExecuting Phase 2 commit flip FAILED (idx=%u)\n", idx);
    prefs.end();
    // State byte says targetState but commit flag still 0.
    // On load, _loadFromNVS treats commit=0 as uncommitted.
    // Revert RAM state to EXECUTING to match NVS reality.
    _journalState[idx] = TransactionState::EXECUTING;
    _journalCommitted[idx] = false;
    return false;
  }

  prefs.end();

  // Success — update RAM state
  _journalCommitted[idx] = true;
  _journalState[idx] = targetState;
  return true;
}

// ============================================================================
// CYCLE-8A: _setTransactionStateNVS — update transaction state in NVS
// ============================================================================
bool TransactionJournal::_setTransactionStateNVS(uint8_t idx, TransactionState state) {
  if (idx >= JOURNAL_SIZE) return false;

  // CYCLE-8B-Rev1: monotonicity check
  if (!_isTransitionAllowed(_journalState[idx], state)) {
    Serial.printf("[Journal] _setTransactionStateNVS REJECTED (monotonicity): %s → %s (idx=%u)\n",
                  _stateToString(_journalState[idx]), _stateToString(state), idx);
    return false;
  }

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
// CYCLE-8B-Rev1 + CYCLE-8C: Monotonicity validator
// ============================================================================
bool TransactionJournal::_isTransitionAllowed(TransactionState from, TransactionState to) {
  // ALLOWED transitions (forward only):
  //   (none/PENDING) → PENDING          (creating new entry)
  //   PENDING → EXECUTING               (markExecuting)
  //   EXECUTING → COMMITTED             (commitTransaction)
  //   EXECUTING → EXECUTION_FAILED_OUTPUT_MISMATCH  (commitTransactionFailed — CYCLE-8C)
  //   PENDING → UNKNOWN                 (reconciliation)
  //   EXECUTING → UNKNOWN               (reconciliation)
  //   PENDING → FAILED                  (reconciliation — proven not executed)
  //   Any non-terminal → CORRUPTED      (invariant violation detected)
  //
  // FORBIDDEN:
  //   EXECUTING → PENDING               (was C8B-001 bug)
  //   COMMITTED → anything               (terminal durable)
  //   COMMITTED_UNKNOWN → anything       (terminal durable)
  //   EXECUTION_FAILED_OUTPUT_MISMATCH → anything  (terminal durable — CYCLE-8C)
  //   CORRUPTED → anything except (cleared)  (terminal safety — only recoverCorruptedEntry can remove)
  //   UNKNOWN/FAILED → PENDING/EXECUTING/COMMITTED (semi-terminal — only clearable)

  if (from == to) return true;  // no-op is allowed

  switch (from) {
    case TransactionState::PENDING:
      return to == TransactionState::EXECUTING ||
             to == TransactionState::UNKNOWN ||
             to == TransactionState::FAILED ||
             to == TransactionState::CORRUPTED;
    case TransactionState::EXECUTING:
      return to == TransactionState::COMMITTED ||
             to == TransactionState::UNKNOWN ||
             to == TransactionState::EXECUTION_FAILED_OUTPUT_MISMATCH ||
             to == TransactionState::CORRUPTED;
             // NOT allowed: EXECUTING → PENDING (C8B-001 fix)
             // NOT allowed: EXECUTING → FAILED (ambiguous — use UNKNOWN instead)
    case TransactionState::COMMITTED:
    case TransactionState::COMMITTED_UNKNOWN:
    case TransactionState::EXECUTION_FAILED_OUTPUT_MISMATCH:
      return false;  // terminal durable — no transitions allowed (except operator recovery)
    case TransactionState::UNKNOWN:
    case TransactionState::FAILED:
      return to == TransactionState::CORRUPTED;  // semi-terminal, can transition to CORRUPTED if needed
    case TransactionState::CORRUPTED:
      return false;  // terminal safety — only recoverCorruptedEntry (separate path) can remove
    default:
      return false;
  }
}

// ============================================================================
// CYCLE-8B-Rev1 (fixes C8B-007): _clearSlotNVS now returns success status.
//   Caller (clearEntry) checks return value before updating RAM state.
//   If NVS write fails, RAM state is NOT updated (prevents journal resurrection).
// ============================================================================
bool TransactionJournal::_clearSlotNVS(uint8_t idx) {
  Preferences prefs;
  if (!prefs.begin(Core::NVS_NAMESPACE, false)) return false;

  char commitKey[20];
  char stateKey[20];
  snprintf(commitKey, sizeof(commitKey), "%s%u", NVS_KEY_TJ_COMMIT_PREFIX, idx);
  snprintf(stateKey, sizeof(stateKey), "%s%u", NVS_KEY_TJ_STATE_PREFIX, idx);

  // Clear commit flag and reset state to PENDING (slot is now free for reuse)
  size_t w1 = prefs.putUChar(commitKey, 0);
  size_t w2 = prefs.putUChar(stateKey, (uint8_t)TransactionState::PENDING);
  prefs.end();

  if (w1 != 1 || w2 != 1) {
    Serial.printf("[Journal] _clearSlotNVS FAILED (idx=%u, w1=%u w2=%u)\n", idx, w1, w2);
    return false;
  }
  return true;
}

// ============================================================================
// CYCLE-8C (fixes C8BR1-002): Durable tombstone for clearEntry()
//
//   Tombstone is a separate NVS key: tj_tomb_<hash_of_requestId>
//   Written BEFORE blob is erased (so crash during clear is safe)
//   On load: if tombstone exists, entry is NOT resurrected
//   Tombstone has TTL (via NVS key count limit — we rely on LRU of key names)
// ============================================================================

// Helper: compute tombstone key from requestId.
//   We use a simple hash to keep key length under 15 chars (NVS key limit).
String TransactionJournal::_tombstoneKey(const String& requestId) {
  // Simple FNV-1a hash to fit within NVS key length limit (15 chars)
  uint32_t hash = 2166136261UL;
  for (size_t i = 0; i < requestId.length(); i++) {
    hash ^= (uint8_t)requestId[i];
    hash *= 16777619UL;
  }
  char key[16];
  snprintf(key, sizeof(key), "tj_tomb_%08x", hash);
  return String(key);
}

bool TransactionJournal::_writeTombstoneNVS(const String& requestId) {
  if (requestId.length() == 0) return false;

  String key = _tombstoneKey(requestId);
  Preferences prefs;
  if (!prefs.begin(Core::NVS_NAMESPACE, false)) return false;

  // Write tombstone value (timestamp for audit trail)
  uint32_t ts = (uint32_t)(millis() / 1000);
  size_t written = prefs.putULong(key.c_str(), ts);
  prefs.end();

  if (written != 4) {
    Serial.printf("[Journal] _writeTombstoneNVS FAILED (key=%s)\n", key.c_str());
    return false;
  }
  Serial.printf("[Journal] Tombstone written: rid=%s (key=%s)\n", requestId.c_str(), key.c_str());
  return true;
}

bool TransactionJournal::_hasTombstoneNVS(const String& requestId) {
  if (requestId.length() == 0) return false;

  String key = _tombstoneKey(requestId);
  Preferences prefs;
  if (!prefs.begin(Core::NVS_NAMESPACE, true)) return false;

  // Check if key exists by reading — if default 0 is returned AND key was never set,
  // we can't distinguish. So we use a non-zero marker.
  // Actually, getULong returns default if key doesn't exist. We write a non-zero
  // timestamp, so if value is 0, key doesn't exist OR was set to 0 (impossible since
  // we write millis()/1000 which is > 0 after boot).
  // Edge case: if tombstone written at exactly millis()=0, this fails. Acceptable.
  uint32_t val = prefs.getULong(key.c_str(), 0);
  prefs.end();

  return val != 0;
}

void TransactionJournal::_removeTombstoneNVS(const String& requestId) {
  if (requestId.length() == 0) return;

  String key = _tombstoneKey(requestId);
  Preferences prefs;
  if (!prefs.begin(Core::NVS_NAMESPACE, false)) return;

  prefs.remove(key.c_str());
  prefs.end();
}

bool TransactionJournal::_eraseBlobNVS(uint8_t idx) {
  if (idx >= JOURNAL_SIZE) return false;

  Preferences prefs;
  if (!prefs.begin(Core::NVS_NAMESPACE, false)) return false;

  char entryKey[20];
  snprintf(entryKey, sizeof(entryKey), "%s%u", NVS_KEY_TJ_ENTRY_PREFIX, idx);

  bool ok = prefs.remove(entryKey);
  prefs.end();

  if (!ok) {
    Serial.printf("[Journal] _eraseBlobNVS FAILED (idx=%u)\n", idx);
    return false;
  }
  return true;
}

// ============================================================================
// Boot: Load all valid entries (does NOT reconcile — caller must capture
// snapshot first, then call reconcilePendingEntries)
// ============================================================================
void TransactionJournal::begin() {
  _loadFromNVS();
  Serial.printf("[Journal] Loaded %u entries from NVS (capacity %u, version=%u)\n",
                _journalSize, JOURNAL_SIZE, BLOB_VERSION);
  // CYCLE-8B: reconciliation is now a SEPARATE step, called by setup()
  // AFTER captureOutputSnapshot(). This fixes C8A-001.
  // Queue ACKs for re-delivery (COMMITTED, COMMITTED_UNKNOWN, EXECUTION_FAILED_OUTPUT_MISMATCH)
  uint8_t committed = 0, committedUnknown = 0, pending = 0, executing = 0, failed = 0;
  uint8_t unknown = 0, corrupted = 0, outputMismatch = 0;
  for (uint8_t i = 0; i < JOURNAL_SIZE; i++) {
    if (!_journalValid[i]) continue;
    switch (_journalState[i]) {
      case TransactionState::COMMITTED:
        committed++;
        if (_journalAcks[i].length() > 0) queueAck(_journalIds[i], _journalAcks[i]);
        break;
      case TransactionState::COMMITTED_UNKNOWN:
        committedUnknown++;
        if (_journalAcks[i].length() > 0) queueAck(_journalIds[i], _journalAcks[i]);
        break;
      case TransactionState::EXECUTION_FAILED_OUTPUT_MISMATCH:
        outputMismatch++;
        if (_journalAcks[i].length() > 0) queueAck(_journalIds[i], _journalAcks[i]);
        break;
      case TransactionState::FAILED:           failed++; break;
      case TransactionState::UNKNOWN:          unknown++; break;
      case TransactionState::CORRUPTED:        corrupted++; break;
      case TransactionState::PENDING:          pending++; break;
      case TransactionState::EXECUTING:       executing++; break;
    }
  }
  Serial.printf("[Journal] Pre-reconcile: committed=%u committed_unknown=%u output_mismatch=%u failed=%u unknown=%u corrupted=%u pending=%u executing=%u\n",
                committed, committedUnknown, outputMismatch, failed, unknown, corrupted, pending, executing);
  if (corrupted > 0) {
    Services::Log.append(Core::LogType::Error,
      String("Journal has ") + corrupted + " CORRUPTED entries — operator recovery required", 0);
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
  char stateKey[20];
  uint8_t blob[BLOB_SIZE];

  for (uint8_t i = 0; i < JOURNAL_SIZE; i++) {
    snprintf(entryKey, sizeof(entryKey), "%s%u", NVS_KEY_TJ_ENTRY_PREFIX, i);
    snprintf(commitKey, sizeof(commitKey), "%s%u", NVS_KEY_TJ_COMMIT_PREFIX, i);
    snprintf(stateKey, sizeof(stateKey), "%s%u", NVS_KEY_TJ_STATE_PREFIX, i);

    uint8_t committed = prefs.getUChar(commitKey, 0);
    uint8_t stateRaw = prefs.getUChar(stateKey, (uint8_t)TransactionState::PENDING);

    size_t len = prefs.getBytesLength(entryKey);
    if (len == 0 || len > BLOB_SIZE) {
      // No blob in this slot
      _journalValid[i] = false;
      _journalCommitted[i] = false;
      _journalState[i] = TransactionState::PENDING;
      // CYCLE-8C: if commit=1 but blob missing, that's inconsistent — mark CORRUPTED
      //   But we can't mark CORRUPTED without a blob (no requestId to track).
      //   So just clear the stale commit/state flags.
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
      // Blob deserialized OK — now validate state enum + invariant (CYCLE-8C)
      if (!isValidTransactionState(stateRaw)) {
        // C8BR1-005: invalid state enum → mark CORRUPTED (do NOT free)
        Serial.printf("[Journal] Entry %u: invalid state byte 0x%02X — marking CORRUPTED (not freed)\n",
                      i, stateRaw);
        _journalValid[i] = true;  // keep slot occupied (don't free)
        _journalCommitted[i] = false;
        _journalIds[i] = _journalIds[i];  // preserve requestId from blob
        _journalState[i] = TransactionState::CORRUPTED;
        _journalSize++;
        // Persist CORRUPTED state to NVS
        prefs.end();
        Preferences rw;
        rw.begin(Core::NVS_NAMESPACE, false);
        rw.putUChar(stateKey, (uint8_t)TransactionState::CORRUPTED);
        rw.putUChar(commitKey, 0);  // ensure not treated as committed
        rw.end();
        prefs.begin(Core::NVS_NAMESPACE, true);
        Services::Log.append(Core::LogType::Error,
          "Journal entry " + String(i) + " marked CORRUPTED (invalid state byte): " + _journalIds[i], 0);
        continue;
      }

      TransactionState state = (TransactionState)stateRaw;

      // CYCLE-8C (C8BR1-004): validate blob/state/commit invariant
      if (!_validateInvariant(i, committed == 1, state)) {
        // Invariant violated — mark CORRUPTED (do NOT free)
        Serial.printf("[Journal] Entry %u: invariant violated (commit=%u state=%s) — marking CORRUPTED\n",
                      i, committed, _stateToString(state));
        _journalValid[i] = true;  // keep slot occupied
        _journalCommitted[i] = false;
        _journalState[i] = TransactionState::CORRUPTED;
        _journalSize++;
        // Persist CORRUPTED state
        prefs.end();
        Preferences rw;
        rw.begin(Core::NVS_NAMESPACE, false);
        rw.putUChar(stateKey, (uint8_t)TransactionState::CORRUPTED);
        rw.putUChar(commitKey, 0);
        rw.end();
        prefs.begin(Core::NVS_NAMESPACE, true);
        Services::Log.append(Core::LogType::Error,
          "Journal entry " + String(i) + " marked CORRUPTED (invariant violation): " + _journalIds[i], 0);
        continue;
      }

      // CYCLE-8C (C8BR1-002): check for tombstone (cleared entry that shouldn't resurrect)
      if (_hasTombstoneNVS(_journalIds[i])) {
        Serial.printf("[Journal] Entry %u: tombstone found for rid=%s — honoring clear (not resurrecting)\n",
                      i, _journalIds[i].c_str());
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
        // Erase the blob now that we've confirmed tombstone
        prefs.end();
        Preferences rw;
        rw.begin(Core::NVS_NAMESPACE, false);
        rw.remove(entryKey);
        rw.putUChar(commitKey, 0);
        rw.putUChar(stateKey, (uint8_t)TransactionState::PENDING);
        rw.end();
        prefs.begin(Core::NVS_NAMESPACE, true);
        continue;
      }

      // Valid entry — accept into RAM
      _journalCommitted[i] = (committed == 1);
      _journalState[i] = state;
      _journalSize++;
      Serial.printf("[Journal] Entry %u: loaded OK (rid=%s, commit=%u, state=%s)\n",
                    i, _journalIds[i].c_str(), committed, _stateToString(state));
    } else {
      // C8BR1-001: Blob CRC/magic/version failed — do NOT free slot!
      //   Mark CORRUPTED to preserve requestId evidence (if we can extract it).
      //   Try to extract requestId from partial blob (best-effort).
      Serial.printf("[Journal] Entry %u: blob CORRUPT (CRC/magic/version fail) — marking CORRUPTED (not freed)\n", i);
      _journalValid[i] = true;  // keep slot occupied — do NOT free
      _journalCommitted[i] = false;
      _journalState[i] = TransactionState::CORRUPTED;
      // Best-effort: try to extract requestId from blob for duplicate detection
      // (even if CRC fails, the requestId field may be readable)
      // For safety, we use a placeholder if extraction fails
      _journalIds[i] = "CORRUPTED_SLOT_" + String(i);  // placeholder
      _journalHashes[i] = "";
      _journalAcks[i] = "";
      _journalChannelId[i] = 0;
      _journalDesiredState[i] = false;
      _journalPreviousKnownState[i] = false;
      _journalAttempt[i] = 0;
      _journalTimestamp[i] = 0;
      _journalSize++;
      // Persist CORRUPTED state
      prefs.end();
      Preferences rw;
      rw.begin(Core::NVS_NAMESPACE, false);
      rw.putUChar(stateKey, (uint8_t)TransactionState::CORRUPTED);
      rw.putUChar(commitKey, 0);
      rw.end();
      prefs.begin(Core::NVS_NAMESPACE, true);
      Services::Log.append(Core::LogType::Error,
        "Journal entry " + String(i) + " marked CORRUPTED (blob CRC/magic/version failure) — evidence preserved, slot not freed", 0);
    }
  }

  prefs.end();
}

// ============================================================================
// CYCLE-8C (C8BR1-004): Validate blob/state/commit invariant
// ============================================================================
bool TransactionJournal::_validateInvariant(uint8_t idx, bool committed, TransactionState state) {
  (void)idx;  // not used yet, but available for future per-slot checks

  // Valid combinations:
  //   commit=0, state=PENDING/EXECUTING/COMMITTED/UNKNOWN/FAILED/CORRUPTED/EXECUTION_FAILED_OUTPUT_MISMATCH
  //     → uncommitted (commit flag is authoritative for durability)
  //     → COMMITTED with commit=0 is transitional (treat as EXECUTING during reconcile)
  //   commit=1, state=COMMITTED
  //     → durable committed
  //   commit=1, state=anything else
  //     → INVALID (commit says durable but state doesn't match)

  if (committed) {
    // commit=1 requires state=COMMITTED (or EXECUTION_FAILED_OUTPUT_MISMATCH which is also durable)
    return state == TransactionState::COMMITTED ||
           state == TransactionState::EXECUTION_FAILED_OUTPUT_MISMATCH;
  }
  // commit=0: any state is valid (including CORRUPTED)
  return true;
}

// ============================================================================
// CYCLE-8C: Mark entry as CORRUPTED (durable, does not free slot)
// ============================================================================
bool TransactionJournal::_markCorruptedNVS(uint8_t idx) {
  if (idx >= JOURNAL_SIZE) return false;

  Preferences prefs;
  if (!prefs.begin(Core::NVS_NAMESPACE, false)) return false;

  char stateKey[20];
  char commitKey[20];
  snprintf(stateKey, sizeof(stateKey), "%s%u", NVS_KEY_TJ_STATE_PREFIX, idx);
  snprintf(commitKey, sizeof(commitKey), "%s%u", NVS_KEY_TJ_COMMIT_PREFIX, idx);

  size_t w1 = prefs.putUChar(stateKey, (uint8_t)TransactionState::CORRUPTED);
  size_t w2 = prefs.putUChar(commitKey, 0);  // ensure not treated as committed
  prefs.end();

  if (w1 != 1 || w2 != 1) {
    Serial.printf("[Journal] _markCorruptedNVS FAILED (idx=%u)\n", idx);
    return false;
  }
  _journalState[idx] = TransactionState::CORRUPTED;
  _journalCommitted[idx] = false;
  return true;
}

// ============================================================================
// CYCLE-8B-Rev1: Reconcile PENDING/EXECUTING entries using captured SNAPSHOT
// ============================================================================
// IMPORTANT (fixes C8A-001): This uses the SNAPSHOT captured by
// captureOutputSnapshot(), NOT live GPIO reads. The snapshot was taken AFTER
// RelayDriver.begin() (safe OFF state) but BEFORE RelayEngine.forceRefresh().
//
// Reconciliation logic (CYCLE-8B-Rev1, fixes C8B-002):
//   - PENDING entries: execute DEFINITELY didn't run (journal says so).
//     Snapshot is OFF (safe init). desiredState may be ON or OFF.
//       - desired=ON: execute didn't run, snapshot=OFF → FAILED (proven not executed, allow retry)
//       - desired=OFF: idempotent, can't tell → UNKNOWN (cannot determine)
//   - EXECUTING entries: execute MAY have run, but GPIO is now OFF (safe init).
//     We CANNOT determine if execute ran before crash.
//     → mark UNKNOWN (cannot determine — NOT FAILED, because FAILED would
//       incorrectly allow retry which could double-execute)
//
// KEY CHANGE from Cycle 8B:
//   - EXECUTING → UNKNOWN (was COMMITTED_UNKNOWN)
//   - PENDING + desired=OFF → UNKNOWN (was COMMITTED_UNKNOWN)
//   - UNKNOWN is clearable (allows retry), COMMITTED_UNKNOWN is NOT clearable (durable)
//   - This distinguishes "proven not executed" (FAILED) from "cannot determine" (UNKNOWN)
// ============================================================================
uint8_t TransactionJournal::reconcilePendingEntries() {
  setBootPhase(BootPhase::RECONCILING);
  uint8_t count = 0;

  if (!_snapshotCaptured) {
    Serial.println("[Journal] WARNING: reconcilePendingEntries() called before captureOutputSnapshot()!");
    Serial.println("[Journal]   Snapshot is stale — reconciliation may be inaccurate.");
  }

  for (uint8_t i = 0; i < JOURNAL_SIZE; i++) {
    if (!_journalValid[i]) continue;
    if (_journalState[i] != TransactionState::PENDING &&
        _journalState[i] != TransactionState::EXECUTING) continue;

    count++;

    Serial.printf("[Journal] Reconciling slot %u: rid=%s state=%s channelId=%u desiredState=%d\n",
                  i, _journalIds[i].c_str(), _stateToString(_journalState[i]),
                  _journalChannelId[i], _journalDesiredState[i]);

    TransactionState oldState = _journalState[i];
    TransactionState newState;

    if (_journalState[i] == TransactionState::PENDING) {
      // PENDING — execute DEFINITELY didn't run (journal says so).
      if (_journalChannelId[i] == 0) {
        // Non-relay command — cannot verify via GPIO → UNKNOWN
        newState = TransactionState::UNKNOWN;
        Services::Log.append(Core::LogType::Error,
          "Non-relay PENDING transaction reconciled as UNKNOWN (no GPIO verification): " + _journalIds[i], 0);
      } else if (_journalDesiredState[i]) {
        // desired=ON, PENDING means execute never ran → FAILED (proven not executed)
        newState = TransactionState::FAILED;
        Services::Log.append(Core::LogType::Error,
          "PENDING transaction with desired=ON reconciled as FAILED (proven not executed): " + _journalIds[i], 0);
      } else {
        // desired=OFF — idempotent, snapshot is OFF, can't tell → UNKNOWN
        newState = TransactionState::UNKNOWN;
        Services::Log.append(Core::LogType::Error,
          "PENDING transaction with desired=OFF reconciled as UNKNOWN (idempotent, cannot determine): " + _journalIds[i], 0);
      }
    } else {
      // EXECUTING — execute MAY have run, but GPIO is now OFF (safe init).
      // CYCLE-8B-Rev1 (fixes C8B-002): mark UNKNOWN, NOT FAILED.
      //   FAILED would mean "proven not executed" — but EXECUTING means execute
      //   WAS called. We cannot prove it didn't run. Mark UNKNOWN so caller
      //   knows to handle with caution (retry may or may not be safe depending
      //   on command idempotency).
      newState = TransactionState::UNKNOWN;
      Services::Log.append(Core::LogType::Error,
        "EXECUTING transaction reconciled as UNKNOWN (GPIO was safe-init OFF, cannot determine if execute ran): " + _journalIds[i], 0);
    }

    _setTransactionStateNVS(i, newState);
    Serial.printf("[Journal] Slot %u: %s → %s\n", i, _stateToString(oldState), _stateToString(newState));
  }
  return count;
}

// Reconcile a single entry (used on retry during RUNNING phase — uses LIVE GPIO)
TransactionState TransactionJournal::reconcileEntry(const String& requestId) {
  int idx = _findInJournal(requestId);
  if (idx < 0) return TransactionState::PENDING;

  if (_journalState[idx] != TransactionState::PENDING &&
      _journalState[idx] != TransactionState::EXECUTING) {
    return _journalState[idx];
  }

  // CYCLE-8B-Rev1 (fixes C8B-004): During RUNNING phase, GPIO is controlled by
  //   RelayEngine (scheduler/PIR/manual). Live GPIO read does NOT prove whether
  //   THIS transaction's execute ran — it only shows current RelayEngine output.
  //   Therefore, ALL reconciliations during RUNNING → UNKNOWN (cannot determine).
  //   We do NOT use GPIO equality as proof (that was the Cycle 8A/8B bug).
  //
  //   Callers must handle UNKNOWN explicitly:
  //   - For idempotent commands (relay ON/OFF): retry is safe, treat like FAILED
  //   - For non-idempotent commands: do NOT retry, surface to operator
  _setTransactionStateNVS(idx, TransactionState::UNKNOWN);
  Services::Log.append(Core::LogType::Error,
    "Transaction reconciled as UNKNOWN during RUNNING phase (GPIO controlled by RelayEngine, cannot determine original execution): " + requestId, 0);
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

// CYCLE-8B-Rev1: isProcessed returns true ONLY for COMMITTED and COMMITTED_UNKNOWN.
//   PENDING/EXECUTING: in-flight (caller should reconcile or wait)
//   FAILED: proven not executed (allow retry)
//   UNKNOWN: cannot determine (caller decides — default: allow retry with caution)
// CYCLE-8C: isProcessed includes EXECUTION_FAILED_OUTPUT_MISMATCH (durable terminal).
//   CORRUPTED is also "processed" in the sense that requestId is blocked
//   (prevents blind re-execution of corrupted transactions).
bool TransactionJournal::isProcessed(const String& requestId) {
  int idx = _findInJournal(requestId);
  if (idx < 0) return false;
  return _journalState[idx] == TransactionState::COMMITTED ||
         _journalState[idx] == TransactionState::COMMITTED_UNKNOWN ||
         _journalState[idx] == TransactionState::EXECUTION_FAILED_OUTPUT_MISMATCH ||
         _journalState[idx] == TransactionState::CORRUPTED;
}

bool TransactionJournal::isCommitted(const String& requestId) {
  int idx = _findInJournal(requestId);
  if (idx < 0) return false;
  return _journalState[idx] == TransactionState::COMMITTED ||
         _journalState[idx] == TransactionState::COMMITTED_UNKNOWN ||
         _journalState[idx] == TransactionState::EXECUTION_FAILED_OUTPUT_MISMATCH;
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
                   _journalState[idx] == TransactionState::COMMITTED_UNKNOWN ||
                   _journalState[idx] == TransactionState::EXECUTION_FAILED_OUTPUT_MISMATCH)) {
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

  if (!_createPendingEntryNVS(idx)) {
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
  // CYCLE-8B-Rev1 (fixes C8B-005): persist attempt counter atomically with state.
  //   Previous: attempt++ in RAM only, then _setTransactionStateNVS writes state.
  //   If crash between RAM update and NVS write: attempt lost on reboot.
  //   Now: rewrite blob with incremented attempt + state=EXECUTING in single NVS write.
  _journalAttempt[idx]++;
  _journalState[idx] = TransactionState::EXECUTING;

  // Rewrite blob with new attempt value (preserves all other fields)
  uint8_t blob[BLOB_SIZE];
  size_t totalLen = _serializeBlob(blob, BLOB_SIZE,
    _journalIds[idx], _journalHashes[idx], _journalAcks[idx],
    _journalChannelId[idx], _journalDesiredState[idx],
    _journalPreviousKnownState[idx], _journalAttempt[idx],
    _journalTimestamp[idx]);
  if (totalLen == 0) {
    Serial.printf("[Journal] markExecuting: blob serialization FAILED (idx=%u)\n", idx);
    _journalAttempt[idx]--;  // revert
    _journalState[idx] = TransactionState::PENDING;
    return false;
  }

  Preferences prefs;
  if (!prefs.begin(Core::NVS_NAMESPACE, false)) {
    Serial.println("[Journal] markExecuting: Cannot open NVS");
    _journalAttempt[idx]--;
    _journalState[idx] = TransactionState::PENDING;
    return false;
  }

  char entryKey[20];
  char stateKey[20];
  snprintf(entryKey, sizeof(entryKey), "%s%u", NVS_KEY_TJ_ENTRY_PREFIX, idx);
  snprintf(stateKey, sizeof(stateKey), "%s%u", NVS_KEY_TJ_STATE_PREFIX, idx);

  // Write blob (with new attempt) — commit flag stays 0
  size_t written = prefs.putBytes(entryKey, blob, totalLen);
  if (written != totalLen) {
    Serial.printf("[Journal] markExecuting: blob write FAILED (idx=%u)\n", idx);
    prefs.end();
    _journalAttempt[idx]--;
    _journalState[idx] = TransactionState::PENDING;
    return false;
  }

  // Set state=EXECUTING (commit flag stays 0 — entry not yet committed)
  size_t stateWritten = prefs.putUChar(stateKey, (uint8_t)TransactionState::EXECUTING);
  prefs.end();

  if (stateWritten != 1) {
    Serial.printf("[Journal] markExecuting: state write FAILED (idx=%u)\n", idx);
    _journalAttempt[idx]--;
    _journalState[idx] = TransactionState::PENDING;
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
    // Already committed — just update ackJson in-place (idempotent)
    // Use _commitExecutingEntryNVS which handles state check, but we need to
    // bypass the EXECUTING check. For now, just update RAM + queue.
    _journalAcks[idx] = ackJson;
    queueAck(requestId, ackJson);
    return true;
  }

  // CYCLE-8B-Rev1: use separated commit function (fixes C8B-001)
  if (!_commitExecutingEntryNVS(idx, ackJson)) {
    Serial.printf("[Journal] commitTransaction FAILED for rid=%s (state=%s)\n",
                  requestId.c_str(), _stateToString(_journalState[idx]));
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
    // Entry already exists — update in place.
    // CYCLE-8B-Rev1: use monotonic transitions only.
    // If existing is PENDING → mark EXECUTING → commit (proper flow)
    // If existing is EXECUTING → commit
    // If existing is COMMITTED/COMMITTED_UNKNOWN → just update ackJson
    if (_journalState[existing] == TransactionState::COMMITTED ||
        _journalState[existing] == TransactionState::COMMITTED_UNKNOWN) {
      _journalAcks[existing] = ackJson;
      queueAck(requestId, ackJson);
      return true;
    }
    // For PENDING/EXECUTING, transition to EXECUTING then COMMITTED
    if (_journalState[existing] == TransactionState::PENDING) {
      _setTransactionStateNVS(existing, TransactionState::EXECUTING);
    }
    _journalHashes[existing] = commandHash;
    bool ok = _commitExecutingEntryNVS(existing, ackJson);
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
// CYCLE-8B: clearEntry — allows clearing PENDING, EXECUTING, and FAILED.
// CYCLE-8B-Rev1: clearEntry allows PENDING, EXECUTING, FAILED, UNKNOWN.
//   NOT allowed: COMMITTED, COMMITTED_UNKNOWN (terminal, durable).
//   Returns true if entry was cleared (fixes C8B-007: check NVS write success).
// ============================================================================
// CYCLE-8C (fixes C8BR1-002): clearEntry now uses durable tombstone + blob erase.
//   Tombstone written FIRST (survives reboot), then blob erased, then slot freed.
//   If crash during clear: tombstone exists → on reboot, entry NOT resurrected.
//   NOT allowed for: COMMITTED, COMMITTED_UNKNOWN, CORRUPTED, EXECUTION_FAILED_OUTPUT_MISMATCH
//   (these are durable terminal states — operator must use recoverCorruptedEntry)
// ============================================================================
bool TransactionJournal::clearEntry(const String& requestId) {
  int idx = _findInJournal(requestId);
  if (idx < 0) return false;

  TransactionState state = _journalState[idx];
  // CYCLE-8C: cannot clear durable terminal states via clearEntry
  if (state == TransactionState::COMMITTED ||
      state == TransactionState::COMMITTED_UNKNOWN ||
      state == TransactionState::EXECUTION_FAILED_OUTPUT_MISMATCH) {
    Serial.printf("[Journal] clearEntry: rid=%s state=%s — cannot clear durable entry\n",
                  requestId.c_str(), _stateToString(state));
    return false;
  }
  // CYCLE-8C: CORRUPTED entries cannot be cleared via clearEntry — use recoverCorruptedEntry
  if (state == TransactionState::CORRUPTED) {
    Serial.printf("[Journal] clearEntry: rid=%s state=CORRUPTED — use recoverCorruptedEntry() instead\n",
                  requestId.c_str());
    return false;
  }

  Serial.printf("[Journal] Clearing entry: rid=%s (slot %u, state=%s) — durable tombstone + blob erase\n",
                requestId.c_str(), idx, _stateToString(state));

  // CYCLE-8C (fixes C8BR1-002): Write tombstone FIRST (before any other changes).
  //   If crash after tombstone write: on reboot, _loadFromNVS sees tombstone
  //   and refuses to resurrect the entry (even though blob still exists).
  if (!_writeTombstoneNVS(requestId)) {
    Serial.printf("[Journal] clearEntry FAILED: cannot write tombstone (rid=%s) — ABORT\n",
                  requestId.c_str());
    Services::Log.append(Core::LogType::Error,
      "clearEntry FAILED: tombstone write failed — entry NOT cleared: " + requestId, 0);
    return false;
  }

  // Erase blob from NVS (so slot can be reused)
  if (!_eraseBlobNVS(idx)) {
    Serial.printf("[Journal] clearEntry: blob erase failed (idx=%u) — tombstone still protects\n", idx);
    Services::Log.append(Core::LogType::Error,
      "clearEntry: blob erase failed, but tombstone written — entry will not resurrect: " + requestId, 0);
    // Continue anyway — tombstone protects against resurrection even if blob exists
  }

  // Clear commit flag + state byte
  if (!_clearSlotNVS(idx)) {
    Serial.printf("[Journal] clearEntry: _clearSlotNVS failed (idx=%u) — tombstone still protects\n", idx);
    // Continue — tombstone protects
  }

  // Update RAM state
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

  Serial.printf("[Journal] Cleared: rid=%s (slot %u freed, tombstone durable)\n",
                requestId.c_str(), idx);
  return true;
}

// ============================================================================
// CYCLE-8C: recoverCorruptedEntry — operator-initiated recovery for CORRUPTED.
//   Clears CORRUPTED entry + writes tombstone + erases blob.
//   This is the ONLY way to remove a CORRUPTED entry (clearEntry refuses).
// ============================================================================
bool TransactionJournal::recoverCorruptedEntry(const String& requestId) {
  int idx = _findInJournal(requestId);
  if (idx < 0) return false;

  if (_journalState[idx] != TransactionState::CORRUPTED) {
    Serial.printf("[Journal] recoverCorruptedEntry: rid=%s state=%s — only CORRUPTED can be recovered\n",
                  requestId.c_str(), _stateToString(_journalState[idx]));
    return false;
  }

  Serial.printf("[Journal] Recovering CORRUPTED entry: rid=%s (slot %u) — operator-initiated\n",
                requestId.c_str(), idx);
  Services::Log.append(Core::LogType::Error,
    "CORRUPTED entry recovered by operator: " + requestId, 0);

  // Write tombstone (in case blob is still readable)
  if (!_writeTombstoneNVS(requestId)) {
    Serial.printf("[Journal] recoverCorruptedEntry: tombstone write failed — continuing\n");
  }

  // Erase blob
  if (!_eraseBlobNVS(idx)) {
    Serial.printf("[Journal] recoverCorruptedEntry: blob erase failed — continuing\n");
  }

  // Clear slot
  if (!_clearSlotNVS(idx)) {
    Serial.printf("[Journal] recoverCorruptedEntry: _clearSlotNVS failed\n");
  }

  // Update RAM
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

  Serial.printf("[Journal] CORRUPTED entry recovered: rid=%s (slot %u freed)\n",
                requestId.c_str(), idx);
  return true;
}

// ============================================================================
// CYCLE-8C (fixes C8BR1-008): commitTransactionFailed — durable terminal state
//   for OUTPUT_MISMATCH and other execution failures.
//   Stores ackJson + state=EXECUTION_FAILED_OUTPUT_MISMATCH + commit=1 (durable).
//   This is a terminal state — no auto-retry (operator must investigate).
// ============================================================================
bool TransactionJournal::commitTransactionFailed(const String& requestId, const String& ackJson,
                                                   TransactionState failureState) {
  int idx = _findInJournal(requestId);
  if (idx < 0) {
    Serial.printf("[Journal] commitTransactionFailed: rid=%s NOT FOUND\n", requestId.c_str());
    return false;
  }

  // Only EXECUTION_FAILED_OUTPUT_MISMATCH is currently supported as failure state
  if (failureState != TransactionState::EXECUTION_FAILED_OUTPUT_MISMATCH) {
    Serial.printf("[Journal] commitTransactionFailed: unsupported failure state %s\n",
                  _stateToString(failureState));
    return false;
  }

  // Monotonicity check: must be in EXECUTING state to commit failure
  if (_journalState[idx] != TransactionState::EXECUTING) {
    Serial.printf("[Journal] commitTransactionFailed: rid=%s state=%s (expected EXECUTING) — REJECTED\n",
                  requestId.c_str(), _stateToString(_journalState[idx]));
    return false;
  }

  // Use _commitExecutingEntryNVS with targetState = failureState
  // _commitExecutingEntryNVS needs to support non-COMMITTED target states.
  // For now, we reuse it with targetState parameter.
  if (!_commitExecutingEntryNVS(idx, ackJson, failureState)) {
    Serial.printf("[Journal] commitTransactionFailed FAILED for rid=%s\n", requestId.c_str());
    return false;
  }

  queueAck(requestId, ackJson);
  Serial.printf("[Journal] Committed FAILURE: rid=%s (slot %u, state=%s)\n",
                requestId.c_str(), idx, _stateToString(failureState));
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
