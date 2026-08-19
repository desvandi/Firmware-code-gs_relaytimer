// =============================================================================
// BatteryMonitor.h — Convert raw sensor data → battery measurements
// Timer Digital Relay v4.1 — Battery Calculation Layer
// -----------------------------------------------------------------------------
// Responsibilities (brief §53):
//   • pack voltage — from BatteryVoltageDriver
//   • 8 individual cell voltages — from cumulative ADS1115 nodes (brief §13/14)
//   • battery current — from INA219 #1 (battery branch, sign-corrected)
//   • inverter current — from INA219 #2 (inverter branch)
//   • derived MPPT current = Iinverter - Ibattery  (brief §6)
//   • battery power, inverter DC power, MPPT power  (brief §21)
//   • energy integration (Wh/Ah) with spike rejection  (brief §23)
//   • SOC framework (coulomb-counting + voltage sync, never invents capacity)
//   • publish data to consumers (BatteryDiagnostics, ResistanceEstimator,
//     SystemStatus serializer)
//
// Sign contract (brief §5, §6, §21):
//   Ibattery > 0 = DISCHARGE;  Ibattery < 0 = CHARGE
//   Iinverter > 0 = CONSUME
//   Imppt = Iinverter - Ibattery
//   Pbattery  > 0 = DISCHARGE;  Pbattery  < 0 = CHARGE
//
// All sensors optional: if any reading is invalid, the corresponding field
// is exposed as NaN / unavailable. System never fabricates data. (Brief §46.)
// =============================================================================
#pragma once
#ifndef TIMER12_BATTERY_MONITOR_H
#define TIMER12_BATTERY_MONITOR_H

#include <Arduino.h>
#include <cstdint>
#include "BatteryConfig.h"
#include "Ina219Driver.h"
#include "Ads1115Driver.h"
#include "Sht31Driver.h"
#include "BatteryVoltageDriver.h"

namespace Services {

// ---------- DIAGNOSTIC ENUMS (brief §17, §30) ----------
enum class CellSensorState : uint8_t {
  OK = 0,
  I2C_ERROR,
  TAP_FAULT,
  INVALID_VALUE,
  RANGE_FAULT,
  STALE,
};
enum class AlarmState : uint8_t {
  NORMAL = 0,
  WARNING,
  FAULT,
  UNAVAILABLE,
};

// ---------- CELL MEASUREMENT (brief §14, §17-19) ----------
struct CellMeasurement {
  float    voltageV;                 // individual cell voltage (C[n+1]-C[n])
  CellSensorState state;
  uint32_t timestamp;
};
struct CellMetrics {
  float    cellMin;
  float    cellMax;
  float    cellAverage;
  float    cellDelta;               // cellMax - cellMin
  uint8_t  minCellIndex;            // 0..7
  uint8_t  maxCellIndex;
  bool     valid;
};

// ---------- POWER FLOW (brief §21, §22) ----------
struct PowerFlow {
  float    mpptCurrent;              // = Iinverter - Ibattery
  float    mpptPower;                // = Vbattery × Imppt
  float    batteryCurrent;           // raw Ibattery (sign per contract)
  float    batteryPower;             // = Vbattery × Ibattery  (signed)
  float    inverterCurrent;          // Iinverter
  float    inverterDcPower;          // = Vbattery × Iinverter
  bool     batteryCurrentValid;
  bool     inverterCurrentValid;
  AlarmState consistency;            // NORMAL/WARNING/FAULT/UNAVAILABLE
  float    consistencyError;         // W
  bool     valid;
};

// ---------- ENERGY INTEGRATION (brief §23, §24) ----------
struct EnergyCounters {
  float    pvEnergyWh;               // MPPT-derived PV energy
  float    batteryChargedWh;        // abs(negative Ibattery) integrated
  float    batteryDischargedWh;     // positive Ibattery integrated
  float    inverterDcEnergyWh;      // inverter DC consumption
  float    chargedAh;
  float    dischargedAh;
  bool     valid;
};

// ---------- SOC (brief §24 — coulomb counting estimate) ----------
struct SocState {
  float    socPercent;              // 0..100 (NaN if unavailable)
  bool     available;               // false if BATTERY_CAPACITY_AH == 0
  bool     synchronized;           // true if last sync was via voltage
  uint32_t lastSyncMs;
};

// ---------- PUBLIC STRUCT: BatteryTelemetry (consumed by SystemStatus) ----------
struct BatteryTelemetry {
  // Pack
  float    packVoltage;
  bool     packVoltageValid;
  const char* packVoltageSource;    // static string

  // Currents (per contract)
  float    batteryCurrent;
  float    inverterCurrent;
  float    mpptCurrent;

  // Powers
  float    batteryPower;            // signed
  float    inverterDcPower;
  float    mpptPower;
  float    chargePower;             // abs(negative batteryPower)
  float    dischargePower;          // positive batteryPower only

  // Cells
  CellMeasurement cells[Battery::NUM_CELLS];
  CellMetrics     cellMetrics;

  // SOC
  SocState soc;

  // Energy
  EnergyCounters energy;

  // Resistance (populated by ResistanceEstimator)
  float    packResistance;          // Ω (NaN if unavailable)
  bool     packResistanceValid;
  const char* packResistanceQuality;  // static string

  // Power flow
  PowerFlow powerFlow;

  // Overall validity
  bool     valid;
};

class BatteryMonitor {
public:
  bool begin();
  void tick();
  BatteryTelemetry getTelemetry() const { return _telemetry; }

  // Direct accessors for diagnostics modules
  float getPackVoltage() const { return _telemetry.packVoltage; }
  float getBatteryCurrent() const { return _telemetry.batteryCurrent; }
  float getInverterCurrent() const { return _telemetry.inverterCurrent; }
  float getMpptCurrent() const { return _telemetry.mpptCurrent; }
  const CellMeasurement* getCells() const { return _telemetry.cells; }

  // For ResistanceEstimator — snapshot of latest valid (V,I) pair
  bool   getLatestVi(float& v, float& i, uint32_t& ts) const;

private:
  BatteryTelemetry _telemetry = {};
  unsigned long _lastEnergyTickMs = 0;
  unsigned long _lastPersistMs = 0;
  bool          _energyDirty = false;
  bool          _initialized = false;

  // SPIKE-REJECT ACCUMULATORS (brief §23)
  float         _prevBatteryCurrentA = 0.0f;
  float         _prevInverterCurrentA = 0.0f;

  // SOC state (loaded from NVS at boot)
  float         _socPct = 0.0f;
  bool          _socInitialized = false;
  uint32_t      _socLastSyncMs = 0;

  void          _updatePackVoltage();
  void          _updateCurrents();
  void          _updateCells();
  void          _updatePowerAndEnergy();
  void          _updateSoc(float dtSec);
  void          _loadCounters();
  void          _persistCounters();
  void          _resetCellMetrics();
  void          _computeCellMetrics();
};

extern BatteryMonitor battery;

} // namespace Services

#endif // TIMER12_BATTERY_MONITOR_H
