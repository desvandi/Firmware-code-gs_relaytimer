// TelemetrySpool.cpp — Bounded telemetry store-and-forward with CRC + critical-event preservation
#include "TelemetrySpool.h"
#include <cstring>
#include <cstdio>

namespace Services {

TelemetrySpool telemetrySpool;

// CRC-16/CCITT — simple, fast, sufficient for integrity checking
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

void TelemetrySpool::begin() {
  _head = 0; _count = 0;
  _criticalHead = 0; _criticalCount = 0;
  _dropCount = 0; _replayCount = 0;
  _lastReplayMs = 0; _replayIdx = 0;
  _lastSpooledSeq = 0;
  for (uint8_t i = 0; i < SPOOL_CAPACITY; i++) _records[i] = {};
  for (uint8_t i = 0; i < CRITICAL_SPOOL_CAPACITY; i++) _criticalRecords[i] = {};
  Serial.println("[SPOOL] init — RAM ring buffer + critical-event buffer (no NVS yet)");
}

uint16_t TelemetrySpool::_computeCRC(const TelemetryRecord& r) const {
  // CRC over: sequence(4) + timestamp(4) + payloadLen(2) + recordType(1) + payload(payloadLen)
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

  _writeRecord(_records[_head], sequence, timestamp, payload, len, 0);  // type=0 telemetry
  _head = (_head + 1) % SPOOL_CAPACITY;
  if (_count < SPOOL_CAPACITY) _count++;
  else _dropCount++;  // DROP_OLDEST
  return true;
}

bool TelemetrySpool::spoolCritical(uint32_t sequence, uint32_t timestamp,
                                     const char* payload, uint16_t len) {
  if (!payload || len == 0 || len > MAX_PAYLOAD_LEN) return false;
  // Critical events get their own ring buffer — NEVER evicted by regular telemetry
  _writeRecord(_criticalRecords[_criticalHead], sequence, timestamp, payload, len, 1);
  _criticalHead = (_criticalHead + 1) % CRITICAL_SPOOL_CAPACITY;
  if (_criticalCount < CRITICAL_SPOOL_CAPACITY) _criticalCount++;
  // If critical buffer full: DROP_OLDEST critical (but regular telemetry can't push them out)
  return true;
}

uint8_t TelemetrySpool::replay() {
  if (_count == 0 && _criticalCount == 0) return 0;
  unsigned long now = millis();
  if (now - _lastReplayMs < (1000UL / MAX_REPLAY_PER_SEC)) return 0;
  _lastReplayMs = now;

  // Replay critical events FIRST (higher priority — safety > telemetry)
  if (_criticalCount > 0) {
    uint8_t oldest = (_criticalHead + CRITICAL_SPOOL_CAPACITY - _criticalCount) % CRITICAL_SPOOL_CAPACITY;
    if (verifyRecord(_criticalRecords[oldest])) {
      _criticalCount--;
      _replayCount++;
      return 1;
    } else {
      // CRC mismatch — corrupted record, skip
      _criticalCount--;
    }
  }
  if (_count > 0) {
    uint8_t oldest = (_head + SPOOL_CAPACITY - _count) % SPOOL_CAPACITY;
    if (verifyRecord(_records[oldest])) {
      _count--;
      _replayCount++;
      return 1;
    } else {
      _count--;  // skip corrupted
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
}

} // namespace Services
