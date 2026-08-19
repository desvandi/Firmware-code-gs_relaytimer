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
  uint32_t sequence;          // monotonic sequence number (matches telemetrySequence)
  uint32_t timestamp;         // Unix epoch (from RTC) or millis() if RTC invalid
  uint16_t payloadLen;        // actual bytes used in payload[]
  char     payload[512];      // serialized JSON telemetry snapshot
};

class TelemetrySpool {
public:
  static constexpr uint8_t  SPOOL_CAPACITY = 16;     // max records in ring buffer
  static constexpr uint16_t MAX_REPLAY_PER_SEC = 2; // rate-limited replay on reconnect
  static constexpr uint16_t MAX_PAYLOAD_LEN = 512;

  void begin();

  // Spool a telemetry snapshot. Called when MQTT publish fails or MQTT
  // is disconnected. Returns true if spooled, false if dropped (overflow).
  bool spool(uint32_t sequence, uint32_t timestamp, const char* payload, uint16_t len);

  // Replay spooled records to MQTT. Called from MqttClient::loop() when
  // connected. Returns number of records replayed this call (rate-limited).
  uint8_t replay();

  // Query spool state for SystemStatus serialization
  uint8_t pendingCount() const { return _count; }
  uint32_t dropCount() const { return _dropCount; }
  uint32_t replayCount() const { return _replayCount; }
  bool isEmpty() const { return _count == 0; }

  // Clear all spooled records (e.g., on NVS corruption recovery)
  void clear();

private:
  TelemetryRecord _records[SPOOL_CAPACITY] = {};
  uint8_t  _head = 0;          // next write index
  uint8_t  _count = 0;         // current pending count
  uint32_t _dropCount = 0;    // total records dropped due to overflow
  uint32_t _replayCount = 0; // total records successfully replayed
  unsigned long _lastReplayMs = 0;
  uint8_t  _replayIdx = 0;    // next replay index

  // Deduplication: track last replayed sequence to avoid re-spooling duplicates
  uint32_t _lastSpooledSeq = 0;
};

extern TelemetrySpool telemetrySpool;

} // namespace Services

#endif // TIMER12_TELEMETRY_SPOOL_H
