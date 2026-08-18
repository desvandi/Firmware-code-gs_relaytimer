// =============================================================================
// BatteryDiagnostics.h — derived fault / health state
// Timer Digital Relay v4.1
// -----------------------------------------------------------------------------
// Consumes BatteryTelemetry from BatteryMonitor and produces:
//   • per-sensor fault flags (brief §30)
//   • cell imbalance / over-voltage / under-voltage / tap-fault summary
//   • high pack resistance (from ResistanceEstimator)
//   • power-flow inconsistency persistence/debounce (brief §22)
//   • consolidated AlarmState per category (brief §59: NORMAL/WARNING/FAULT/UNAVAILABLE)
//
// All thresholds live in BatteryConfig.h — no magic constants in this module.
// =============================================================================
#pragma once
#ifndef TIMER12_BATTERY_DIAGNOSTICS_H
#define TIMER12_BATTERY_DIAGNOSTICS_H

#include <Arduino.h>
#include <cstdint>
#include "BatteryConfig.h"
#include "BatteryMonitor.h"

namespace Services {

struct BatteryDiagnosticsState {
  // Per-sensor faults (brief §30)
  bool batteryVoltageFault;
  bool batteryCurrentSensorFault;
  bool inverterCurrentSensorFault;
  bool cellMeasurementFault;       // ADS1115 unavailable / cells invalid
  bool cellTapFault;               // C[n+1] < C[n] (impossible ordering)
  bool cellOverVoltage;           // any cell > CELL_OVERVOLTAGE_WARN
  bool cellUnderVoltage;          // any cell < CELL_UNDERVOLTAGE_WARN
  bool cellImbalance;              // cellDelta > CELL_IMBALANCE_WARN
  bool highPackResistance;        // pack R > threshold (set by ResistanceEstimator)
  bool highCellResistance;         // any cell R > threshold (set by ResistanceEstimator)
  bool powerFlowInconsistency;    // brief §22 debounced
  bool sht31Fault;
  bool adsFault;                  // both ADS unavailable
  bool inaFault;                  // both INA unavailable

  // Aggregated severity (brief §59)
  AlarmState overall;

  uint32_t timestamp;
};

class BatteryDiagnostics {
public:
  void begin();
  void tick();   // calls BatteryMonitor::getTelemetry() and recomputes faults

  BatteryDiagnosticsState getState() const { return _state; }

  // Allows ResistanceEstimator to update resistance-related flags
  void setPackResistanceAlarm(bool high, float ohms);
  void setCellResistanceAlarm(bool anyHigh);

private:
  BatteryDiagnosticsState _state = {};
  unsigned long _pfInconsistentSince = 0;  // debounce start time (brief §22)
  bool         _pfStateActive = false;
};

extern BatteryDiagnostics batteryDiagnostics;

} // namespace Services

#endif // TIMER12_BATTERY_DIAGNOSTICS_H
