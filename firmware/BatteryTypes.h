// =============================================================================
// BatteryTypes.h — Public telemetry types for DC energy & battery monitoring
// Timer Digital Relay v4.1 — API contract additions
// -----------------------------------------------------------------------------
// These structs are consumed by SystemStatus serializers (REST + MQTT) and
// mirrored 1:1 in the PWA's src/lib/types.ts (Phase 6).
// All structs use optional `valid` flags + NaN-safe floats (brief §17, §46).
// Field names follow the project's existing camelCase convention.
// =============================================================================
#pragma once
#ifndef TIMER12_BATTERY_TYPES_H
#define TIMER12_BATTERY_TYPES_H

#include <Arduino.h>
#include <cstdint>
#include "BatteryConfig.h"

namespace Services {

// ---------- CELL SENSOR STATE (brief §17) ----------
enum class CellSensorStateApi : uint8_t {
  Ok = 0,
  I2cError,
  TapFault,
  InvalidValue,
  RangeFault,
  Stale,
};

inline const char* cellSensorStateStr(CellSensorStateApi s) {
  switch (s) {
    case CellSensorStateApi::Ok:           return "ok";
    case CellSensorStateApi::I2cError:      return "i2c_error";
    case CellSensorStateApi::TapFault:      return "tap_fault";
    case CellSensorStateApi::InvalidValue: return "invalid_value";
    case CellSensorStateApi::RangeFault:   return "range_fault";
    case CellSensorStateApi::Stale:         return "stale";
  }
  return "stale";
}

// ---------- ALARM STATE (brief §59) ----------
inline const char* alarmStateStr(AlarmState s) {
  switch (s) {
    case AlarmState::NORMAL:      return "NORMAL";
    case AlarmState::WARNING:     return "WARNING";
    case AlarmState::FAULT:       return "FAULT";
    case AlarmState::UNAVAILABLE: return "UNAVAILABLE";
  }
  return "UNAVAILABLE";
}

// ---------- RESISTANCE QUALITY (brief §29) ----------
enum class ResistanceQualityApi : uint8_t {
  Invalid = 0,
  LowDeltaI,
  Unstable,
  Valid,
  HighConfidence,
};

inline const char* resistanceQualityStr(ResistanceQualityApi q) {
  switch (q) {
    case ResistanceQualityApi::Invalid:       return "INVALID";
    case ResistanceQualityApi::LowDeltaI:     return "LOW_DELTA_I";
    case ResistanceQualityApi::Unstable:      return "UNSTABLE";
    case ResistanceQualityApi::Valid:         return "VALID";
    case ResistanceQualityApi::HighConfidence: return "HIGH_CONFIDENCE";
  }
  return "INVALID";
}

} // namespace Services

#endif // TIMER12_BATTERY_TYPES_H
