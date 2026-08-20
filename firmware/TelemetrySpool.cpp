// TelemetrySpool.cpp — Phase D: NVS persistence for critical events.
#include "TelemetrySpool.h"
#include <cstring>
#include <cstdio>
#include <Preferences.h>

namespace Services {

TelemetrySpool telemetrySpool;

static constexpr const char* NVS_NAMESPACE = "t12_spool";
static constexpr const char* NVS_KEY_VERSION = "ver";
static constexpr const char* NVS_KEY_CRIT_HEAD = "crit_head";
static constexpr const char* NVS_KEY_CRIT_COUNT = "crit_count";

static uint16_t crc16(const uint8_t* data, size_t len) {
  uint16_t crc = 0xFFFF;
  for (size_t i = 0; i < len; i++) {
    crc ^= (uint16_t)data[i] << 8;
    for (uint8_t b = 0; b < 8; b++) {
      if (crc & 0x8000) crc = (crc << 1) ^ 0x1021;
      else               crc <<= 1;
    }
  }
  return crc;
}

const char* TelemetrySpool::_nvsKeyForIndex(uint8_t idx, char* buf, size_t bufLen) {
  snprintf(buf, bufLen, "rec_%u", idx);
  return buf;
}

void TelemetrySpool::begin() {
  _head = 0; _count = 0;
  _dropCount = 0; _replayCount = 0;
  _lastReplayMs = 0; _replayIdx = 0;
  _lastSpooledSeq = 0;
  for (uint8_t i = 0; i < SPOOL_CAPACITY; i++) _records[i] = {};
  for (uint8_t i = 0; i < CRITICAL_SPOOL_CAPACITY; i++) _criticalRecords[i] = {};
  _criticalHead = 0;
  _criticalCount = 0;
  _nvsLoaded = false;
  _nvsLoadFailures = 0;
  _nvsWriteFailures = 0;

  _loadCriticalFromNvs();

  Serial.printf("[SPOOL] init — RAM ring (%d) + NVS-persisted critical (%d/%d)\n",
                SPOOL_CAPACITY, _criticalCount, CRITICAL_SPOOL_CAPACITY);
}

void TelemetrySpool::_loadCriticalFromNvs() {
  Preferences prefs;
  if (!prefs.begin(NVS_NAMESPACE, true)) {
    Serial.println("[SPOOL] NVS open failed (read)");
    _nvsLoadFailures++;
    return;
  }

  uint8_t storedVer = prefs.getUChar(NVS_KEY_VERSION, 0);
  if (storedVer != SPOOL_SCHEMA_VERSION) {
    if (storedVer == 0) {
      Serial.println("[SPOOL] NVS spool empty (first boot)");
    } else {
      Serial.printf("[SPOOL] NVS schema mismatch (stored=%u, expected=%u) — clearing\n",
                    storedVer, SPOOL_SCHEMA_VERSION);
    }
    prefs.end();
    _clearNvsSpool();
    if (prefs.begin(NVS_NAMESPACE, false)) {
      prefs.putUChar(NVS_KEY_VERSION, SPOOL_SCHEMA_VERSION);
      prefs.end();
      _nvsLoaded = true;
    }
    return;
  }

  _criticalHead = prefs.getUChar(NVS_KEY_CRIT_HEAD, 0);
  _criticalCount = prefs.getUChar(NVS_KEY_CRIT_COUNT, 0);
  if (_criticalCount > CRITICAL_SPOOL_CAPACITY) {
    Serial.println("[SPOOL] NVS count corrupted — clearing");
    prefs.end();
    _clearNvsSpool();
    _criticalCount = 0;
    _criticalHead = 0;
    return;
  }

  uint8_t validCount = 0;
  uint8_t firstInvalid = 0xFF;
  for (uint8_t i = 0; i < CRITICAL_SPOOL_CAPACITY; i++) {
    char keyBuf[8];
    const char* key = _nvsKeyForIndex(i, keyBuf, sizeof(keyBuf));
    size_t needed = prefs.getBytesLength(key);
    if (needed != sizeof(TelemetryRecord)) {
      continue;
    }
    TelemetryRecord tmp;
    size_t got = prefs.getBytes(key, &tmp, sizeof(TelemetryRecord));
    if (got != sizeof(TelemetryRecord)) {
      continue;
    }
    // Phase D: distinguish empty slot from corrupted record
    if (tmp.sequence == 0 && tmp.payloadLen == 0) {
      continue;
    }
    if (!verifyRecord(tmp)) {
      Serial.printf("[SPOOL] NVS rec %u CRC mismatch — skipping\n", i);
      if (firstInvalid == 0xFF) firstInvalid = i;
      continue;
    }
    _criticalRecords[i] = tmp;
    validCount++;
  }

  if (firstInvalid != 0xFF && validCount < _criticalCount) {
    Serial.printf("[SPOOL] Compacting: %u valid, %u corrupted\n",
                  validCount, _criticalCount - validCount);
    TelemetryRecord compacted[CRITICAL_SPOOL_CAPACITY] = {};
    uint8_t ci = 0;
    for (uint8_t i = 0; i < CRITICAL_SPOOL_CAPACITY; i++) {
      if (verifyRecord(_criticalRecords[i])) {
        compacted[ci++] = _criticalRecords[i];
      }
    }
    for (uint8_t i = 0; i < CRITICAL_SPOOL_CAPACITY; i++) {
      _criticalRecords[i] = compacted[i];
    }
    _criticalHead = ci % CRITICAL_SPOOL_CAPACITY;
    _criticalCount = ci;
    prefs.end();
    _persistCriticalToNvs();
  } else {
    prefs.end();
  }

  _nvsLoaded = true;
  Serial.printf("[SPOOL] Loaded %u critical events from NVS\n", _criticalCount);
}

void TelemetrySpool::_persistCriticalToNvs() {
  Preferences prefs;
  if (!prefs.begin(NVS_NAMESPACE, false)) {
    Serial.println("[SPOOL] NVS open failed (write)");
    _nvsWriteFailures++;
    return;
  }

  prefs.putUChar(NVS_KEY_VERSION, SPOOL_SCHEMA_VERSION);
  prefs.putUChar(NVS_KEY_CRIT_HEAD, _criticalHead);
  prefs.putUChar(NVS_KEY_CRIT_COUNT, _criticalCount);

  for (uint8_t i = 0; i < CRITICAL_SPOOL_CAPACITY; i++) {
    char keyBuf[8];
    const char* key = _nvsKeyForIndex(i, keyBuf, sizeof(keyBuf));
    prefs.putBytes(key, &_criticalRecords[i], sizeof(TelemetryRecord));
  }

  prefs.end();
}

void TelemetrySpool::_clearNvsSpool() {
  Preferences prefs;
  if (!prefs.begin(NVS_NAMESPACE, false)) {
    Serial.println("[SPOOL] NVS open failed (clear)");
    _nvsWriteFailures++;
    return;
  }
  prefs.clear();
  prefs.end();
}

uint16_t TelemetrySpool::_computeCRC(const TelemetryRecord& r) const {
  uint8_t buf[512 + 11];
  size_t off = 0;
  memcpy(buf + off, &r.sequence, 4); off += 4;
  memcpy(buf + off, &r.timestamp, 4); off += 4;
  memcpy(buf + off, &r.payloadLen, 2); off += 2;
  memcpy(buf + off, &r.recordType, 1); off += 1;
  if (r.payloadLen > 0 && r.payloadLen <= MAX_PAYLOAD_LEN) {
    memcpy(buf + off, r.payload, r.payloadLen);
    off += r.payloadLen;
  }
  return crc16(buf, off);
}

void TelemetrySpool::_writeRecord(TelemetryRecord& dst, uint32_t seq, uint32_t ts,
                                    const char* payload, uint16_t len, uint8_t type) {
  dst.sequence = seq;
  dst.timestamp = ts;
  dst.payloadLen = len;
  dst.recordType = type;
  if (payload && len > 0) {
    memcpy(dst.payload, payload, len < MAX_PAYLOAD_LEN ? len : MAX_PAYLOAD_LEN);
    dst.payload[len < MAX_PAYLOAD_LEN ? len : MAX_PAYLOAD_LEN - 1] = '\0';
  }
  dst.crc = _computeCRC(dst);
}

bool TelemetrySpool::spool(uint32_t sequence, uint32_t timestamp,
                            const char* payload, uint16_t len) {
  if (!payload || len == 0 || len > MAX_PAYLOAD_LEN) return false;
  if (sequence == _lastSpooledSeq && _lastSpooledSeq != 0) return false;
  _lastSpooledSeq = sequence;

  _writeRecord(_records[_head], sequence, timestamp, payload, len, 0);
  _head = (_head + 1) % SPOOL_CAPACITY;
  if (_count < SPOOL_CAPACITY) _count++;
  else _dropCount++;
  return true;
}

bool TelemetrySpool::spoolCritical(uint32_t sequence, uint32_t timestamp,
                                     const char* payload, uint16_t len) {
  if (!payload || len == 0 || len > MAX_PAYLOAD_LEN) return false;
  _writeRecord(_criticalRecords[_criticalHead], sequence, timestamp, payload, len, 1);
  _criticalHead = (_criticalHead + 1) % CRITICAL_SPOOL_CAPACITY;
  if (_criticalCount < CRITICAL_SPOOL_CAPACITY) _criticalCount++;

  _persistCriticalToNvs();
  return true;
}

uint8_t TelemetrySpool::replay() {
  if (_count == 0 && _criticalCount == 0) return 0;
  unsigned long now = millis();
  if (now - _lastReplayMs < (1000UL / MAX_REPLAY_PER_SEC)) return 0;
  _lastReplayMs = now;

  if (_criticalCount > 0) {
    uint8_t oldest = (_criticalHead + CRITICAL_SPOOL_CAPACITY - _criticalCount) % CRITICAL_SPOOL_CAPACITY;
    if (verifyRecord(_criticalRecords[oldest])) {
      _criticalCount--;
      _replayCount++;
      _persistCriticalToNvs();
      return 1;
    } else {
      _criticalCount--;
      _persistCriticalToNvs();
    }
  }
  if (_count > 0) {
    uint8_t oldest = (_head + SPOOL_CAPACITY - _count) % SPOOL_CAPACITY;
    if (verifyRecord(_records[oldest])) {
      _count--;
      _replayCount++;
      return 1;
    } else {
      _count--;
    }
  }
  return 0;
}

bool TelemetrySpool::verifyRecord(const TelemetryRecord& r) const {
  uint16_t expected = _computeCRC(r);
  return r.crc == expected;
}

void TelemetrySpool::clear() {
  _count = 0; _head = 0; _criticalCount = 0; _criticalHead = 0; _replayIdx = 0;
  for (uint8_t i = 0; i < SPOOL_CAPACITY; i++) _records[i] = {};
  for (uint8_t i = 0; i < CRITICAL_SPOOL_CAPACITY; i++) _criticalRecords[i] = {};
  _clearNvsSpool();
}

} // namespace Services
