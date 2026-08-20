// =============================================================================
// TelemetrySpool.h — Bounded telemetry store-and-forward with NVS persistence
// Timer Digital Relay v4.3.8
// -----------------------------------------------------------------------------
// Phase D: NVS persistence for critical events. RAM ring buffer for regular
// telemetry (5-15s interval would wear out flash — see §11 flash wear calc).
// Critical events (boot, alarm, safety, fault) are rare and MUST survive
// power loss — these are persisted to NVS.
//
// Flash wear calculation (directive §11):
//   - Regular telemetry: every 10s → 3.15M writes/year → sector death in ~12 days
//   - Critical events: ~10/day → 3,650/year → 27 YEARS per sector
//   - Decision: REGULAR = RAM ONLY, CRITICAL = NVS PERSISTED.
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
  uint8_t  recordType;      // 0=telemetry, 1=critical_event
  uint16_t crc;
  char     payload[512];
};

static constexpr uint8_t SPOOL_SCHEMA_VERSION = 1;

class TelemetrySpool {
public:
  static constexpr uint8_t  SPOOL_CAPACITY = 16;
  static constexpr uint8_t  CRITICAL_SPOOL_CAPACITY = 8;
  static constexpr uint16_t MAX_REPLAY_PER_SEC = 2;
  static constexpr uint16_t MAX_PAYLOAD_LEN = 512;

  void begin();
  bool spool(uint32_t sequence, uint32_t timestamp, const char* payload, uint16_t len);
  bool spoolCritical(uint32_t sequence, uint32_t timestamp, const char* payload, uint16_t len);
  uint8_t replay();
  uint8_t pendingCount() const { return _count; }
  uint8_t criticalPendingCount() const { return _criticalCount; }
  uint32_t dropCount() const { return _dropCount; }
  uint32_t replayCount() const { return _replayCount; }
  bool isEmpty() const { return _count == 0 && _criticalCount == 0; }
  void clear();
  bool verifyRecord(const TelemetryRecord& r) const;

  // Phase D: persistence status
  bool isNvsLoaded() const { return _nvsLoaded; }
  uint32_t nvsLoadFailures() const { return _nvsLoadFailures; }
  uint32_t nvsWriteFailures() const { return _nvsWriteFailures; }

private:
  TelemetryRecord _records[SPOOL_CAPACITY] = {};
  TelemetryRecord _criticalRecords[CRITICAL_SPOOL_CAPACITY] = {};
  uint8_t  _head = 0;
  uint8_t  _count = 0;
  uint8_t  _criticalHead = 0;
  uint8_t  _criticalCount = 0;
  uint32_t _dropCount = 0;
  uint32_t _replayCount = 0;
  unsigned long _lastReplayMs = 0;
  uint8_t  _replayIdx = 0;
  uint32_t _lastSpooledSeq = 0;

  bool     _nvsLoaded = false;
  uint32_t _nvsLoadFailures = 0;
  uint32_t _nvsWriteFailures = 0;

  uint16_t _computeCRC(const TelemetryRecord& r) const;
  void _writeRecord(TelemetryRecord& dst, uint32_t seq, uint32_t ts,
                     const char* payload, uint16_t len, uint8_t type);

  void _persistCriticalToNvs();
  void _loadCriticalFromNvs();
  void _clearNvsSpool();
  static const char* _nvsKeyForIndex(uint8_t idx, char* buf, size_t bufLen);
};

extern TelemetrySpool telemetrySpool;

} // namespace Services

#endif
