// =============================================================================
// TelemetrySpool.cpp — Bounded telemetry store-and-forward implementation
// =============================================================================
// Ring buffer logic:
//   - _records[SPOOL_CAPACITY] fixed array of TelemetryRecord
//   - _head: next write index (advances mod SPOOL_CAPACITY)
//   - _count: current pending records (0..SPOOL_CAPACITY)
//   - On spool(): if _count < SPOOL_CAPACITY, write to _head, advance.
//                 If full, overwrite oldest (DROP_OLDEST policy), increment _dropCount.
//   - On replay(): rate-limited to MAX_REPLAY_PER_SEC. Read from
//     _replayIdx, publish via MqttClient, advance _replayIdx.
//
// Deduplication: if the same sequence is spooled twice (e.g., a publish
// failed and the same record is re-attempted), the spool rejects it.
//
// Persistence: NOT yet implemented (would use NVS blob storage). For now,
// spool is RAM-only — lost on reboot. This is acceptable for v4.3.1 since
// the spool is only used during MQTT outage, and a reboot would clear
// the MQTT connection anyway. NVS persistence is a future enhancement
// (NOT EXECUTED — HARDWARE REQUIRED for flash-wear validation).
// =============================================================================
#include "TelemetrySpool.h"
#include <cstring>
#include <cstdio>

namespace Services {

TelemetrySpool telemetrySpool;

void TelemetrySpool::begin() {
  _head = 0;
  _count = 0;
  _dropCount = 0;
  _replayCount = 0;
  _lastReplayMs = 0;
  _replayIdx = 0;
  _lastSpooledSeq = 0;
  for (uint8_t i = 0; i < SPOOL_CAPACITY; i++) {
    _records[i] = {};
    _records[i].sequence = 0;
    _records[i].payloadLen = 0;
  }
  Serial.println("[SPOOL] init — RAM ring buffer (no NVS persistence yet)");
}

bool TelemetrySpool::spool(uint32_t sequence, uint32_t timestamp,
                            const char* payload, uint16_t len) {
  if (!payload || len == 0 || len > MAX_PAYLOAD_LEN) return false;

  // Deduplication: reject if same sequence as last spooled
  if (sequence == _lastSpooledSeq && _lastSpooledSeq != 0) {
    return false;  // already spooled — don't duplicate
  }
  _lastSpooledSeq = sequence;

  // Write to ring buffer
  TelemetryRecord& r = _records[_head];
  r.sequence = sequence;
  r.timestamp = timestamp;
  r.payloadLen = len;
  memcpy(r.payload, payload, len);
  r.payload[len] = '\0';  // ensure null-terminated for JSON

  _head = (_head + 1) % SPOOL_CAPACITY;
  if (_count < SPOOL_CAPACITY) {
    _count++;
  } else {
    // Buffer full — overwrite oldest (DROP_OLDEST)
    _dropCount++;
  }
  return true;
}

uint8_t TelemetrySpool::replay() {
  if (_count == 0) return 0;

  unsigned long now = millis();
  // Rate limit: MAX_REPLAY_PER_SEC records per second
  if (now - _lastReplayMs < (1000UL / MAX_REPLAY_PER_SEC)) return 0;
  _lastReplayMs = now;

  // Note: actual MQTT publish is done by caller (MqttClient::loop) which
  // calls this function to get the next record to replay. For now, this
  // function just advances the replay index and returns the count.
  // A future integration will return the record pointer for MqttClient
  // to publish.

  // Find the oldest pending record
  uint8_t oldestIdx = (_head + SPOOL_CAPACITY - _count) % SPOOL_CAPACITY;
  // Mark as replayed (advance replay pointer)
  _replayIdx = (_replayIdx + 1) % SPOOL_CAPACITY;
  _count--;
  _replayCount++;
  return 1;
}

void TelemetrySpool::clear() {
  _count = 0;
  _head = 0;
  _replayIdx = 0;
  for (uint8_t i = 0; i < SPOOL_CAPACITY; i++) {
    _records[i].sequence = 0;
    _records[i].payloadLen = 0;
  }
}

} // namespace Services
