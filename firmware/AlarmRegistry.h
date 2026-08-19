// =============================================================================
// AlarmRegistry.h — Central alarm engine (brief §60)
// Timer Digital Relay v4.2 — Industrial-Grade Hardening
// -----------------------------------------------------------------------------
// All alarms have: alarmId, source, severity, condition, timestamp, active,
// acknowledged, cleared. Severity levels: INFO / WARNING / CRITICAL.
//
// Per brief §60 minimum alarm set:
//   device offline, MQTT failure, RTC invalid, PZEM failure, PIR failure,
//   overvoltage, undervoltage, overcurrent, overpower, storage failure,
//   OTA failure, authentication failure, repeated reboot, watchdog,
//   brownout, state drift, interlock violation
// =============================================================================
#pragma once
#ifndef TIMER12_ALARM_REGISTRY_H
#define TIMER12_ALARM_REGISTRY_H

#include <Arduino.h>
#include <cstdint>

namespace Services {

enum class AlarmSeverity : uint8_t {
  Info     = 0,
  Warning  = 1,
  Critical = 2,
};

inline const char* alarmSeverityStr(AlarmSeverity s) {
  switch (s) {
    case AlarmSeverity::Info:     return "INFO";
    case AlarmSeverity::Warning:  return "WARNING";
    case AlarmSeverity::Critical:  return "CRITICAL";
  }
  return "INFO";
}

struct Alarm {
  static constexpr uint8_t CODE_LEN = 32;
  char     code[CODE_LEN];        // e.g., "OVER_VOLTAGE", "RTC_INVALID"
  AlarmSeverity severity;
  bool     active;
  bool     acknowledged;
  uint32_t raisedAt;              // millis() when alarm first raised
  uint32_t clearedAt;             // millis() when alarm cleared (0 if still active)
  uint32_t lastUpdatedAt;
  char     message[64];           // human-readable detail
};

class AlarmRegistry {
public:
  static constexpr uint8_t MAX_ALARMS = 24;

  void begin();
  // Raise an alarm by code. If already active, refreshes lastUpdatedAt +
  // message. Idempotent — same code raised twice doesn't duplicate.
  void raise(const char* code, AlarmSeverity sev, const char* message = "");
  // Clear an alarm by code. No-op if not active.
  void clear(const char* code);
  // Mark an active alarm as acknowledged (operator has seen it).
  void acknowledge(const char* code);
  // Get alarm count (active only, or all)
  uint8_t countActive() const;
  uint8_t countAll() const;
  // Get alarm by index (for serialization). Returns nullptr if out of range.
  const Alarm* getAlarm(uint8_t idx) const;
  // Find an alarm by code. Returns nullptr if not found.
  const Alarm* find(const char* code) const;
  // Highest severity among active alarms (Info if none active).
  AlarmSeverity highestActiveSeverity() const;
  // Acknowledge all active alarms.
  void acknowledgeAll();

private:
  Alarm     _alarms[MAX_ALARMS] = {};
  uint8_t   _count = 0;
  uint8_t   _findIdx(const char* code) const;
};

extern AlarmRegistry alarms;

} // namespace Services

#endif // TIMER12_ALARM_REGISTRY_H
