// =============================================================================
// TelemetrySpool.h — Bounded telemetry store-and-forward (audit P1-008)
// Timer Digital Relay v4.3.1
// -----------------------------------------------------------------------------
// Per ChatGPT audit (Phase K):
//   "implementasikan software architecture: bounded spool, sequence,
//    persistence, replay, deduplication, overflow policy, corruption recovery"
//
// Software architecture (implementable WITHOUT hardware — hardware needed
// only for flash-wear characterization and power-loss validation).
//
// When MQTT is disconnected, telemetry snapshots are spooled to a bounded
// ring buffer in RAM (with optional NVS persistence). On reconnect, the
// spool is replayed at a controlled rate (MAX_REPLAY_PER_SEC) to avoid
// broker flood.
//
// Overflow policy: DROP_OLDEST (telemetry is time-series, newest is more
// valuable than oldest). Each dropped record increments dropCount for
// observability.
// =============================================================================
#pragma once
#ifndef TIMER12_TELEMETRY_SPOOL_H
#define TIMER12_TELEMETRY_SPOOL_H

#include <Arduino.h>
#include <cstdint>

namespace Services {

struct TelemetryRecord {
  uint32_t sequence;
  uint32_t timestamp;
  uint16_t payloadLen;
  uint8_t  recordType;      // 0=telemetry, 1=critical_event (alarm/safety/boot)
  uint16_t crc;             // CRC-16 over sequence+timestamp+payloadLen+payload
  char     payload[512];
};

class TelemetrySpool {
public:
  static constexpr uint8_t  SPOOL_CAPACITY = 16;
  static constexpr uint8_t  CRITICAL_SPOOL_CAPACITY = 8;  // separate buffer for critical events
  static constexpr uint16_t MAX_REPLAY_PER_SEC = 2;
  static constexpr uint16_t MAX_PAYLOAD_LEN = 512;

  void begin();
  bool spool(uint32_t sequence, uint32_t timestamp, const char* payload, uint16_t len);
  // v4.3.3 B09: Critical events get separate ring buffer — NEVER evicted by
  // regular telemetry overflow. Per ChatGPT: "SAFETY_EVENT, ALARM, FAULT,
  // BOOT_LOOP tidak boleh hilang hanya karena telemetry biasa memenuhi buffer."
  bool spoolCritical(uint32_t sequence, uint32_t timestamp, const char* payload, uint16_t len);
  uint8_t replay();
  uint8_t pendingCount() const { return _count; }
  uint8_t criticalPendingCount() const { return _criticalCount; }
  uint32_t dropCount() const { return _dropCount; }
  uint32_t replayCount() const { return _replayCount; }
  bool isEmpty() const { return _count == 0 && _criticalCount == 0; }
  void clear();

  // v4.3.3 B09: corruption detection — verify CRC of a record
  bool verifyRecord(const TelemetryRecord& r) const;

private:
  TelemetryRecord _records[SPOOL_CAPACITY] = {};
  TelemetryRecord _criticalRecords[CRITICAL_SPOOL_CAPACITY] = {};  // NEVER evicted by regular overflow
  uint8_t  _head = 0;
  uint8_t  _count = 0;
  uint8_t  _criticalHead = 0;
  uint8_t  _criticalCount = 0;
  uint32_t _dropCount = 0;
  uint32_t _replayCount = 0;
  unsigned long _lastReplayMs = 0;
  uint8_t  _replayIdx = 0;
  uint32_t _lastSpooledSeq = 0;

  uint16_t _computeCRC(const TelemetryRecord& r) const;
  void _writeRecord(TelemetryRecord& dst, uint32_t seq, uint32_t ts,
                     const char* payload, uint16_t len, uint8_t type);
};

extern TelemetrySpool telemetrySpool;

} // namespace Services

#endif // TIMER12_TELEMETRY_SPOOL_H
