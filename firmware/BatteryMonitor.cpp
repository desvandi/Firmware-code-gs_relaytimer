// =============================================================================
// BatteryMonitor.cpp — battery calculation layer
// =============================================================================
// Cell calculation (brief §14):
//   Cell1 = C1
//   Cell2 = C2 - C1
//   ...
//   Cell8 = B+ - C7
// where Cn is the calibrated node voltage (ADS1115 reading) and B+ is the
// top-of-pack node (= ADS1115 #2 AIN3, also used by packVoltage driver).
//
// Energy integration (brief §23):
//   Wh += P_w * dt_hours
//   Ah += I_a * dt_hours (split by sign: chargedAh for I<0, dischargedAh for I>0)
//   Guards: dt > 0, dt < 60 s (anti-rollover), |P| < ENERGY_SPIKE_REJECT_W,
//   |I| < CURRENT_SPIKE_REJECT_A, all sensors valid.
//
// SOC (brief §24): coulomb counting with optional voltage synchronization:
//   If BATTERY_CAPACITY_AH == 0 → SOC = UNAVAILABLE (null in JSON)
//   Else: SOC% += (I*dt_hours / capacity_ah) * 100
//         When packVoltage ≥ SOC_FULL_VOLTAGE - hysteresis → SOC = 100%
//         When packVoltage ≤ SOC_EMPTY_VOLTAGE + hysteresis → SOC = 0%
// =============================================================================
// BATTERY_MONITORING_ENABLED guard — file compiles to nothing when battery
// monitoring is disabled (saves flash for relay-only installations).
#include "BatteryConfig.h"
#if !BATTERY_ENABLED
// File intentionally empty when battery monitoring is disabled.
#else
#include "BatteryMonitor.h"
#include "Config.h"
#include "LogService.h"
#include <Preferences.h>
#include <cmath>

// v4.3.6 D-004 FIX: bring Drivers:: types into scope for this translation unit.
// BatteryMonitor.cpp references Ina219Reading, AdsStatus, Ina219Status without
// Drivers:: qualification. Without these using declarations, the file does NOT
// compile (30+ errors). This is the root cause of the P0 build failure.
using Drivers::Ina219Reading;
using Drivers::Ina219Status;
using Drivers::AdsStatus;

namespace Services {

BatteryMonitor battery;

// ---------- HELPERS ----------
static inline bool isF(float v) { return !std::isnan(v) && !std::isinf(v); }
static inline float clampF(float v, float lo, float hi) {
  if (!isF(v)) return NAN;
  if (v < lo) return lo;
  if (v > hi) return hi;
  return v;
}

// ---------- INIT ----------
bool BatteryMonitor::begin() {
  _telemetry = {};
  _telemetry.packVoltageSource = Drivers::packVoltage.getSourceStr();
  _telemetry.packResistanceQuality = "INVALID";
  _telemetry.packResistance = NAN;
  _telemetry.packResistanceValid = false;
  _resetCellMetrics();
  _loadCounters();
  _initialized = true;
  Serial.printf("[BATMON] init: capacityAh=%.2f, socAvailable=%s, source=%s\n",
                (double)Battery::BATTERY_CAPACITY_AH,
                Battery::BATTERY_CAPACITY_AH > 0 ? "true" : "false",
                _telemetry.packVoltageSource);
  return true;
}

void BatteryMonitor::_loadCounters() {
  Preferences p;
  if (!p.begin(Battery::NVS_NAMESPACE_BATTERY, true)) {
    Serial.println("[BATMON] NVS open failed for load — using zero counters");
    _telemetry.energy = {0, 0, 0, 0, 0, 0, true};
    _socPct = 0;
    _socInitialized = false;
    return;
  }
  _telemetry.energy.pvEnergyWh        = p.getFloat(Battery::NVS_KEY_PV_WH, 0.0f);
  _telemetry.energy.batteryChargedWh  = p.getFloat(Battery::NVS_KEY_CHARGED_WH, 0.0f);
  _telemetry.energy.batteryDischargedWh = p.getFloat(Battery::NVS_KEY_DISCHARGED_WH, 0.0f);
  _telemetry.energy.chargedAh         = p.getFloat(Battery::NVS_KEY_CHARGED_AH, 0.0f);
  _telemetry.energy.dischargedAh      = p.getFloat(Battery::NVS_KEY_DISCHARGED_AH, 0.0f);
  _telemetry.energy.inverterDcEnergyWh = p.getFloat(Battery::NVS_KEY_INV_DC_WH, 0.0f);
  _telemetry.energy.valid = true;
  _socPct = p.getFloat(Battery::NVS_KEY_SOC, 0.0f);
  _socInitialized = (_socPct > 0.0f && _socPct <= 100.0f);
  p.end();
}

void BatteryMonitor::_persistCounters() {
  Preferences p;
  if (!p.begin(Battery::NVS_NAMESPACE_BATTERY, false)) return;
  p.putFloat(Battery::NVS_KEY_PV_WH,         _telemetry.energy.pvEnergyWh);
  p.putFloat(Battery::NVS_KEY_CHARGED_WH,    _telemetry.energy.batteryChargedWh);
  p.putFloat(Battery::NVS_KEY_DISCHARGED_WH, _telemetry.energy.batteryDischargedWh);
  p.putFloat(Battery::NVS_KEY_CHARGED_AH,    _telemetry.energy.chargedAh);
  p.putFloat(Battery::NVS_KEY_DISCHARGED_AH, _telemetry.energy.dischargedAh);
  p.putFloat(Battery::NVS_KEY_INV_DC_WH,    _telemetry.energy.inverterDcEnergyWh);
  p.putFloat(Battery::NVS_KEY_SOC,           _socPct);
  p.end();
}

// ---------- PACK VOLTAGE ----------
void BatteryMonitor::_updatePackVoltage() {
  Drivers::PackVoltageStatus s = Drivers::packVoltage.getStatus();
  if (s == Drivers::PackVoltageStatus::Ok) {
    _telemetry.packVoltage = Drivers::packVoltage.getPackVoltageV();
    _telemetry.packVoltageValid = isF(_telemetry.packVoltage);
  } else {
    _telemetry.packVoltageValid = false;
    _telemetry.packVoltage = NAN;
  }
}

// ---------- CURRENTS (per brief §5, §6, §7) ----------
void BatteryMonitor::_updateCurrents() {
  Ina219Reading b = Drivers::ina219Battery.getReading();
  Ina219Reading i = Drivers::ina219Inverter.getReading();
  _telemetry.powerFlow.batteryCurrentValid = (b.status == Drivers::Ina219Status::Ok);
  _telemetry.powerFlow.inverterCurrentValid = (i.status == Drivers::Ina219Status::Ok);

  _telemetry.batteryCurrent  = _telemetry.powerFlow.batteryCurrentValid ? b.currentA : NAN;
  _telemetry.inverterCurrent = _telemetry.powerFlow.inverterCurrentValid ? i.currentA : NAN;

  // Derived MPPT current = Iinverter - Ibattery  (brief §6, with example 60 = 20 - (-40))
  if (_telemetry.powerFlow.batteryCurrentValid &&
      _telemetry.powerFlow.inverterCurrentValid &&
      isF(_telemetry.batteryCurrent) && isF(_telemetry.inverterCurrent)) {
    _telemetry.mpptCurrent = _telemetry.inverterCurrent - _telemetry.batteryCurrent;
    _telemetry.powerFlow.mpptCurrent = _telemetry.mpptCurrent;
    _telemetry.powerFlow.batteryCurrent = _telemetry.batteryCurrent;
    _telemetry.powerFlow.inverterCurrent = _telemetry.inverterCurrent;
  } else {
    _telemetry.mpptCurrent = NAN;
    _telemetry.powerFlow.mpptCurrent = NAN;
  }
}

// ---------- CELLS ----------
void BatteryMonitor::_resetCellMetrics() {
  for (uint8_t i = 0; i < Battery::NUM_CELLS; i++) {
    _telemetry.cells[i].voltageV = NAN;
    _telemetry.cells[i].state = CellSensorState::STALE;
    _telemetry.cells[i].timestamp = 0;
  }
  _telemetry.cellMetrics.valid = false;
}

void BatteryMonitor::_computeCellMetrics() {
  float mn = 1e9f, mx = -1e9f, sum = 0;
  uint8_t mnIdx = 0, mxIdx = 0;
  uint8_t validCount = 0;
  for (uint8_t i = 0; i < Battery::NUM_CELLS; i++) {
    if (_telemetry.cells[i].state != CellSensorState::OK) continue;
    float v = _telemetry.cells[i].voltageV;
    if (!isF(v)) continue;
    validCount++;
    sum += v;
    if (v < mn) { mn = v; mnIdx = i; }
    if (v > mx) { mx = v; mxIdx = i; }
  }
  if (validCount < 4) {  // require at least half the cells for a metric
    _telemetry.cellMetrics.valid = false;
    return;
  }
  _telemetry.cellMetrics.cellMin = mn;
  _telemetry.cellMetrics.cellMax = mx;
  _telemetry.cellMetrics.cellAverage = sum / validCount;
  _telemetry.cellMetrics.cellDelta = (mx > mn) ? (mx - mn) : 0.0f;
  _telemetry.cellMetrics.minCellIndex = mnIdx;
  _telemetry.cellMetrics.maxCellIndex = mxIdx;
  _telemetry.cellMetrics.valid = true;
}

void BatteryMonitor::_updateCells() {
  _resetCellMetrics();
  // Read cumulative node voltages (brief §13)
  // ADS1115 #1 (0x48): AIN0..AIN3 = C1..C4
  // ADS1115 #2 (0x49): AIN0..AIN3 = C5..C7, B+
  float c[Battery::NUM_CELLS + 1] = {NAN, NAN, NAN, NAN, NAN, NAN, NAN, NAN, NAN};
  AdsStatus s;
  s = Drivers::adsCell1.getChannelStatus(0);  if (s == AdsStatus::Ok) c[0] = Drivers::adsCell1.getChannelV(0);
  s = Drivers::adsCell1.getChannelStatus(1);  if (s == AdsStatus::Ok) c[1] = Drivers::adsCell1.getChannelV(1);
  s = Drivers::adsCell1.getChannelStatus(2);  if (s == AdsStatus::Ok) c[2] = Drivers::adsCell1.getChannelV(2);
  s = Drivers::adsCell1.getChannelStatus(3);  if (s == AdsStatus::Ok) c[3] = Drivers::adsCell1.getChannelV(3);
  s = Drivers::adsCell2.getChannelStatus(0);  if (s == AdsStatus::Ok) c[4] = Drivers::adsCell2.getChannelV(0);
  s = Drivers::adsCell2.getChannelStatus(1);  if (s == AdsStatus::Ok) c[5] = Drivers::adsCell2.getChannelV(1);
  s = Drivers::adsCell2.getChannelStatus(2);  if (s == AdsStatus::Ok) c[6] = Drivers::adsCell2.getChannelV(2);
  s = Drivers::adsCell2.getChannelStatus(3);  if (s == AdsStatus::Ok) c[7] = Drivers::adsCell2.getChannelV(3);
  // c[8] = B+ — already in c[7] slot per §16? No: 8 cells means C1..C7 + B+ = 8 nodes.
  // Brief §16 lists AIN0..AIN3 = C5, C6, C7, B+ → that's only 4 nodes on ADS#2.
  // Combined: 4 + 4 = 8 nodes = C1..C7 + B+. Index 7 is B+.
  // (Cells 1..7 are derived from C1..C7, Cell 8 = B+ - C7.)

  // Detect ADS-level errors first → propagate to cell sensors
  AdsStatus s1[4] = {
    Drivers::adsCell1.getChannelStatus(0),
    Drivers::adsCell1.getChannelStatus(1),
    Drivers::adsCell1.getChannelStatus(2),
    Drivers::adsCell1.getChannelStatus(3),
  };
  AdsStatus s2[4] = {
    Drivers::adsCell2.getChannelStatus(0),
    Drivers::adsCell2.getChannelStatus(1),
    Drivers::adsCell2.getChannelStatus(2),
    Drivers::adsCell2.getChannelStatus(3),
  };

  // Determine overall sensor health
  bool ads1Ok = Drivers::adsCell1.isAvailable();
  bool ads2Ok = Drivers::adsCell2.isAvailable();
  // Brief §18: tap-fault detection — expected C1 < C2 < ... < B+ with tolerance.
  // For each consecutive pair, if c[i+1] < c[i] - tolerance → TAP_FAULT.

  // Cell1 = C1
  if (ads1Ok && s1[0] == AdsStatus::Ok && isF(c[0])) {
    _telemetry.cells[0].voltageV = c[0];
    _telemetry.cells[0].state = (c[0] < Battery::CELL_VALID_MIN_V || c[0] > Battery::CELL_VALID_MAX_V)
                              ? CellSensorState::RANGE_FAULT : CellSensorState::OK;
  } else {
    _telemetry.cells[0].state = ads1Ok ? CellSensorState::I2C_ERROR : CellSensorState::STALE;
  }
  _telemetry.cells[0].timestamp = Drivers::adsCell1.getChannelTime(0);

  // Cell2..Cell7 = C[n] - C[n-1]
  for (uint8_t i = 1; i < 7; i++) {
    bool topOk, botOk;
    AdsStatus topStat, botStat;
    if (i < 4) {
      topStat = s1[i]; botStat = s1[i-1];
      topOk = ads1Ok && (topStat == AdsStatus::Ok) && isF(c[i]);
      botOk = ads1Ok && (botStat == AdsStatus::Ok) && isF(c[i-1]);
    } else {
      topStat = s2[i-4]; botStat = s2[i-4-1];
      topOk = ads2Ok && (topStat == AdsStatus::Ok) && isF(c[i]);
      botOk = ads2Ok && (botStat == AdsStatus::Ok) && isF(c[i-1]);
    }
    if (topOk && botOk) {
      float diff = c[i] - c[i-1];
      // Tap-fault detection (brief §18)
      if (diff < -Battery::NODE_ORDER_TOLERANCE_V) {
        _telemetry.cells[i].voltageV = NAN;
        _telemetry.cells[i].state = CellSensorState::TAP_FAULT;
      } else if (diff < Battery::CELL_VALID_MIN_V) {
        _telemetry.cells[i].voltageV = (diff > 0) ? diff : 0.0f;
        _telemetry.cells[i].state = CellSensorState::INVALID_VALUE;
      } else if (diff > Battery::CELL_VALID_MAX_V) {
        _telemetry.cells[i].voltageV = diff;
        _telemetry.cells[i].state = CellSensorState::RANGE_FAULT;
      } else {
        _telemetry.cells[i].voltageV = diff;
        _telemetry.cells[i].state = CellSensorState::OK;
      }
      _telemetry.cells[i].timestamp = (i < 4)
        ? Drivers::adsCell1.getChannelTime(i)
        : Drivers::adsCell2.getChannelTime(i - 4);
    } else {
      _telemetry.cells[i].state = (ads1Ok && ads2Ok) ? CellSensorState::I2C_ERROR
                                                      : CellSensorState::STALE;
    }
  }

  // Cell8 = B+ - C7  (c[7] = B+, c[6] = C7)
  bool bpOk = ads2Ok && (s2[3] == AdsStatus::Ok) && isF(c[7]);
  bool c7Ok = ads2Ok && (s2[2] == AdsStatus::Ok) && isF(c[6]);
  if (bpOk && c7Ok) {
    float diff = c[7] - c[6];
    if (diff < -Battery::NODE_ORDER_TOLERANCE_V) {
      _telemetry.cells[7].voltageV = NAN;
      _telemetry.cells[7].state = CellSensorState::TAP_FAULT;
    } else if (diff < Battery::CELL_VALID_MIN_V) {
      _telemetry.cells[7].voltageV = (diff > 0) ? diff : 0.0f;
      _telemetry.cells[7].state = CellSensorState::INVALID_VALUE;
    } else if (diff > Battery::CELL_VALID_MAX_V) {
      _telemetry.cells[7].voltageV = diff;
      _telemetry.cells[7].state = CellSensorState::RANGE_FAULT;
    } else {
      _telemetry.cells[7].voltageV = diff;
      _telemetry.cells[7].state = CellSensorState::OK;
    }
    _telemetry.cells[7].timestamp = Drivers::adsCell2.getChannelTime(3);
  } else {
    _telemetry.cells[7].state = ads2Ok ? CellSensorState::I2C_ERROR
                                       : CellSensorState::STALE;
  }
  _computeCellMetrics();
}

// ---------- POWER + ENERGY ----------
void BatteryMonitor::_updatePowerAndEnergy() {
  // Power calculations (brief §21)
  if (_telemetry.packVoltageValid &&
      isF(_telemetry.batteryCurrent) && isF(_telemetry.inverterCurrent)) {
    float V = _telemetry.packVoltage;
    _telemetry.batteryPower    = V * _telemetry.batteryCurrent;     // signed
    _telemetry.inverterDcPower = V * _telemetry.inverterCurrent;   // ≥0
    _telemetry.mpptPower       = V * _telemetry.mpptCurrent;       // ≥0 typically
    _telemetry.chargePower     = (_telemetry.batteryPower < 0) ? -_telemetry.batteryPower : 0.0f;
    _telemetry.dischargePower  = (_telemetry.batteryPower > 0) ?  _telemetry.batteryPower : 0.0f;

    _telemetry.powerFlow.mpptPower       = _telemetry.mpptPower;
    _telemetry.powerFlow.batteryPower     = _telemetry.batteryPower;
    _telemetry.powerFlow.inverterDcPower  = _telemetry.inverterDcPower;

    // Power-flow consistency (brief §22):
    // Unified signed formulation:
    //   Pmppt = Pinverter + |Pbattery_charge| - Pbattery_discharge
    //   equivalently: Pinverter = Pmppt + Pbattery  (Pbattery signed)
    //     - when charging (Pbattery < 0): predicted = Pmppt - |Pb_charge|
    //     - when discharging (Pbattery > 0): predicted = Pmppt + Pb_discharge
    // This single signed formula covers both branches without conditional logic.
    // v4.3.6 D-004 FIX: pRight was used but never declared.
    // pRight = actual Pinverter (right-hand side of consistency equation).
    float pRight = _telemetry.inverterDcPower;
    float predictedInv = _telemetry.mpptPower + _telemetry.batteryPower;
    float err = pRight - predictedInv;
    _telemetry.powerFlow.consistencyError = err;
    float tol = std::max(Battery::POWER_FLOW_TOLERANCE_W,
                          std::fabs(pRight) * Battery::POWER_FLOW_TOLERANCE_PCT);
    _telemetry.powerFlow.consistency =
      (std::fabs(err) <= tol) ? AlarmState::NORMAL : AlarmState::WARNING;
    _telemetry.powerFlow.valid = true;
  } else {
    _telemetry.batteryPower = NAN;
    _telemetry.inverterDcPower = NAN;
    _telemetry.mpptPower = NAN;
    _telemetry.chargePower = NAN;
    _telemetry.dischargePower = NAN;
    _telemetry.powerFlow.consistency = AlarmState::UNAVAILABLE;
    _telemetry.powerFlow.consistencyError = NAN;
    _telemetry.powerFlow.valid = false;
  }

  // Energy integration (brief §23) — 1 Hz tick with spike rejection
  unsigned long now = millis();
  if (_lastEnergyTickMs == 0) {
    _lastEnergyTickMs = now;
    return;
  }
  unsigned long dtMs = now - _lastEnergyTickMs;
  if (dtMs < 100 || dtMs > 60000) {  // 100 ms min, 60 s max (rollover guard)
    _lastEnergyTickMs = now;
    return;
  }
  _lastEnergyTickMs = now;

  // Reject if any sensor invalid OR spike detected (brief §23)
  if (!_telemetry.powerFlow.valid) return;
  if (std::fabs(_telemetry.batteryPower)    > Battery::ENERGY_SPIKE_REJECT_W) return;
  if (std::fabs(_telemetry.inverterDcPower) > Battery::ENERGY_SPIKE_REJECT_W) return;
  if (std::fabs(_telemetry.mpptPower)       > Battery::ENERGY_SPIKE_REJECT_W) return;
  if (std::fabs(_telemetry.batteryCurrent) > Battery::CURRENT_SPIKE_REJECT_A) return;

  float dtH = dtMs / 3600000.0f;
  _telemetry.energy.batteryChargedWh    += _telemetry.chargePower    * dtH;
  _telemetry.energy.batteryDischargedWh += _telemetry.dischargePower * dtH;
  _telemetry.energy.inverterDcEnergyWh  += _telemetry.inverterDcPower * dtH;
  _telemetry.energy.pvEnergyWh          += _telemetry.mpptPower      * dtH;

  // Ah — split by sign of Ibattery
  if (_telemetry.batteryCurrent > 0) {
    _telemetry.energy.dischargedAh += _telemetry.batteryCurrent * dtH;
  } else if (_telemetry.batteryCurrent < 0) {
    _telemetry.energy.chargedAh += -_telemetry.batteryCurrent * dtH;
  }
  _telemetry.energy.valid = true;
  _energyDirty = true;

  // SOC update (brief §24)
  _updateSoc(dtMs / 1000.0f);

  // Persist counters at most every ENERGY_PERSIST_INTERVAL_MS
  if (now - _lastPersistMs > Battery::ENERGY_PERSIST_INTERVAL_MS && _energyDirty) {
    _persistCounters();
    _energyDirty = false;
    _lastPersistMs = now;
  }
}

// ---------- SOC (brief §24 — coulomb counting estimate) ----------
void BatteryMonitor::_updateSoc(float dtSec) {
  if (Battery::BATTERY_CAPACITY_AH <= 0.0f) {
    _telemetry.soc.available = false;
    _telemetry.soc.socPercent = NAN;
    _telemetry.soc.synchronized = false;
    return;
  }
  // Initialize SOC from pack voltage if not initialized
  if (!_socInitialized && _telemetry.packVoltageValid) {
    float V = _telemetry.packVoltage;
    if (V >= Battery::SOC_FULL_VOLTAGE - Battery::SOC_SYNC_HYSTERESIS_V) {
      _socPct = 100.0f;
    } else if (V <= Battery::SOC_EMPTY_VOLTAGE + Battery::SOC_SYNC_HYSTERESIS_V) {
      _socPct = 0.0f;
    } else {
      // Linear interpolation between empty and full (rough estimate)
      _socPct = (V - Battery::SOC_EMPTY_VOLTAGE) /
                (Battery::SOC_FULL_VOLTAGE - Battery::SOC_EMPTY_VOLTAGE) * 100.0f;
    }
    _socInitialized = true;
    _telemetry.soc.synchronized = true;
    _telemetry.soc.lastSyncMs = millis();
  }

  if (_socInitialized && isF(_telemetry.batteryCurrent)) {
    float dtH = dtSec / 3600.0f;
    float dAh = _telemetry.batteryCurrent * dtH;  // signed
    // dAh > 0 = discharge → SOC decreases
    float dSoc = (dAh / Battery::BATTERY_CAPACITY_AH) * 100.0f;
    _socPct -= dSoc;
  }

  // Voltage-based sync (brief §24) — clamp at boundaries
  if (_telemetry.packVoltageValid) {
    float V = _telemetry.packVoltage;
    if (V >= Battery::SOC_FULL_VOLTAGE - Battery::SOC_SYNC_HYSTERESIS_V) {
      _socPct = 100.0f;
      _telemetry.soc.synchronized = true;
      _telemetry.soc.lastSyncMs = millis();
    } else if (V <= Battery::SOC_EMPTY_VOLTAGE + Battery::SOC_SYNC_HYSTERESIS_V) {
      _socPct = 0.0f;
      _telemetry.soc.synchronized = true;
      _telemetry.soc.lastSyncMs = millis();
    }
  }
  _socPct = clampF(_socPct, 0.0f, 100.0f);
  _telemetry.soc.socPercent = _socPct;
  _telemetry.soc.available = _socInitialized;
}

// ---------- TICK ----------
void BatteryMonitor::tick() {
  if (!_initialized) return;
  _updatePackVoltage();
  _updateCurrents();
  _updateCells();
  _updatePowerAndEnergy();

  _telemetry.valid =
      _telemetry.packVoltageValid ||
      _telemetry.powerFlow.batteryCurrentValid ||
      _telemetry.powerFlow.inverterCurrentValid ||
      _telemetry.cellMetrics.valid;
}

// ---------- ACCESSOR for ResistanceEstimator ----------
bool BatteryMonitor::getLatestVi(float& v, float& i, uint32_t& ts) const {
  if (!_telemetry.packVoltageValid || !isF(_telemetry.batteryCurrent)) return false;
  v = _telemetry.packVoltage;
  i = _telemetry.batteryCurrent;
  ts = millis();
  return true;
}

} // namespace Services

#endif // BATTERY_ENABLED
