// =============================================================================
// BatteryDiagnostics.cpp — derived fault / health state
// =============================================================================
// Logic:
//   - Per-sensor fault set when sensor status != OK OR data invalid (NaN)
//   - cellTapFault = any cell with TAP_FAULT state
//   - cellOverVoltage / underVoltage / imbalance based on cellMetrics
//   - powerFlowInconsistency requires sustained POWER_FLOW_PERSIST_MS (brief §22)
//   - overall severity: UNAVAILABLE if all sensors fault, else FAULT if any fault
//     flag set, WARNING if cellImbalance/warning only, else NORMAL
// =============================================================================
// BATTERY_MONITORING_ENABLED guard — file compiles to nothing when battery
// monitoring is disabled (saves flash for relay-only installations).
#include "BatteryConfig.h"
#if !BATTERY_ENABLED
// File intentionally empty when battery monitoring is disabled.
#else
#include "BatteryDiagnostics.h"
#include "Ina219Driver.h"
#include "Ads1115Driver.h"
#include "Sht31Driver.h"
#include "BatteryVoltageDriver.h"
#include <cmath>

namespace Services {

BatteryDiagnostics batteryDiagnostics;

static inline bool isF(float v) { return !std::isnan(v) && !std::isinf(v); }

void BatteryDiagnostics::begin() {
  _state = {};
  _state.timestamp = millis();
  _pfInconsistentSince = 0;
  _pfStateActive = false;
  Serial.println("[BATDIAG] init");
}

void BatteryDiagnostics::tick() {
  BatteryTelemetry t = battery.getTelemetry();
  _state.timestamp = millis();

  // ----- per-sensor fault -----
  _state.batteryVoltageFault       = !t.packVoltageValid;
  _state.batteryCurrentSensorFault = !t.powerFlow.batteryCurrentValid;
  _state.inverterCurrentSensorFault = !t.powerFlow.inverterCurrentValid;

  // ADS fault (cell measurement) — covers both devices
  bool anyAdsFault = !Drivers::adsCell1.isAvailable() && !Drivers::adsCell2.isAvailable();
  _state.adsFault = anyAdsFault;
  _state.cellMeasurementFault = anyAdsFault || !t.cellMetrics.valid;

  // Per-cell faults
  bool tap = false, ov = false, uv = false;
  for (uint8_t i = 0; i < Battery::NUM_CELLS; i++) {
    if (t.cells[i].state == CellSensorState::TAP_FAULT) tap = true;
    if (t.cells[i].state == CellSensorState::OK && isF(t.cells[i].voltageV)) {
      if (t.cells[i].voltageV > Battery::CELL_OVERVOLTAGE_WARN)  ov = true;
      if (t.cells[i].voltageV < Battery::CELL_UNDERVOLTAGE_WARN) uv = true;
    }
  }
  _state.cellTapFault = tap;
  _state.cellOverVoltage = ov;
  _state.cellUnderVoltage = uv;
  _state.cellImbalance =
      t.cellMetrics.valid && isF(t.cellMetrics.cellDelta) &&
      (t.cellMetrics.cellDelta > Battery::CELL_IMBALANCE_WARN);

  // SHT31 fault
  _state.sht31Fault = !Drivers::sht31.isAvailable();

  // INA fault (both unavailable)
  _state.inaFault = !Drivers::ina219Battery.isAvailable() &&
                    !Drivers::ina219Inverter.isAvailable();

  // Power-flow inconsistency (debounced — brief §22)
  if (t.powerFlow.valid && isF(t.powerFlow.consistencyError)) {
    float tol = std::max(Battery::POWER_FLOW_TOLERANCE_W,
                          std::fabs(t.powerFlow.inverterDcPower) *
                          Battery::POWER_FLOW_TOLERANCE_PCT);
    bool inconsistentNow = std::fabs(t.powerFlow.consistencyError) > tol;
    if (inconsistentNow) {
      if (_pfInconsistentSince == 0) _pfInconsistentSince = millis();
      if (millis() - _pfInconsistentSince > Battery::POWER_FLOW_PERSIST_MS) {
        _pfStateActive = true;
      }
    } else {
      _pfInconsistentSince = 0;
      _pfStateActive = false;
    }
  } else {
    _pfInconsistentSince = 0;
    _pfStateActive = false;  // sensor unavailable → no alarm (brief §46)
  }
  _state.powerFlowInconsistency = _pfStateActive;

  // Overall severity (brief §59)
  bool anyFault =
      _state.batteryVoltageFault ||
      _state.batteryCurrentSensorFault ||
      _state.inverterCurrentSensorFault ||
      _state.cellMeasurementFault ||
      _state.cellTapFault ||
      _state.cellOverVoltage ||
      _state.cellUnderVoltage ||
      _state.highPackResistance ||
      _state.highCellResistance ||
      _state.powerFlowInconsistency;

  bool allUnavailable = _state.batteryVoltageFault &&
                          _state.batteryCurrentSensorFault &&
                          _state.inverterCurrentSensorFault &&
                          _state.cellMeasurementFault;
  if (allUnavailable) {
    _state.overall = AlarmState::UNAVAILABLE;
  } else if (anyFault) {
    _state.overall = AlarmState::FAULT;
  } else if (_state.cellImbalance || _state.sht31Fault || _state.inaFault) {
    _state.overall = AlarmState::WARNING;
  } else {
    _state.overall = AlarmState::NORMAL;
  }
}

void BatteryDiagnostics::setPackResistanceAlarm(bool high, float ohms) {
  _state.highPackResistance = high && isF(ohms) && ohms > 0;
}

void BatteryDiagnostics::setCellResistanceAlarm(bool anyHigh) {
  _state.highCellResistance = anyHigh;
}

} // namespace Services

#endif // BATTERY_ENABLED
