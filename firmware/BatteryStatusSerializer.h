// =============================================================================
// BatteryStatusSerializer.h — JSON serializer for v4.1 battery telemetry
// Timer Digital Relay v4.1 — REST + MQTT shared serializer
// -----------------------------------------------------------------------------
// Per brief §31, §32, §33: the same canonical telemetry must be available
// through REST /api/status and MQTT timer12/<mac>/status. This shared inline
// helper ensures both transports emit IDENTICAL semantic data — the only
// allowed difference is the transport envelope.
//
// Used by:
//   Web/Handlers/StatusHandlers.h::handleStatus()   → REST /api/status
//   Services/MqttClient::publishStatus()             → MQTT status topic
//
// Existing SystemStatus fields remain untouched (backward compat — brief §55).
// All new fields are added under nested "battery", "powerFlow", "environment"
// objects so old PWA versions still parse the legacy fields without breaking.
// =============================================================================
#pragma once
#ifndef TIMER12_BATTERY_STATUS_SERIALIZER_H
#define TIMER12_BATTERY_STATUS_SERIALIZER_H

#include <Arduino.h>
#include <ArduinoJson.h>
#include "BatteryConfig.h"
#include "BatteryMonitor.h"
#include "BatteryDiagnostics.h"
#include "ResistanceEstimator.h"
#include "Sht31Driver.h"
#include "Ina219Driver.h"
#include "Ads1115Driver.h"
#include "BatteryVoltageDriver.h"
#include "BatteryTypes.h"

namespace Services {

// Serialize float that may be NaN → emit null (ArduinoJson: setNaN/null)
// To preserve backward-compat with PWA's optional fields, we omit NaN keys.
inline void setOptionalFloat(JsonObject& obj, const char* key, float value) {
  if (!std::isnan(value) && !std::isinf(value)) {
    obj[key] = value;
  } else {
    obj[key] = nullptr;  // emit null so PWA type `number | null` matches
  }
}

// Convert internal CellSensorState → API string (brief §17 — public contract)
inline const char* cellStateToStr(CellSensorState s) {
  switch (s) {
    case CellSensorState::OK:           return "ok";
    case CellSensorState::I2C_ERROR:     return "i2c_error";
    case CellSensorState::TAP_FAULT:    return "tap_fault";
    case CellSensorState::INVALID_VALUE: return "invalid_value";
    case CellSensorState::RANGE_FAULT:  return "range_fault";
    case CellSensorState::STALE:        return "stale";
  }
  return "stale";
}

inline const char* resistanceQualityStr(ResistanceQuality q) {
  switch (q) {
    case ResistanceQuality::INVALID:        return "INVALID";
    case ResistanceQuality::LOW_DELTA_I:    return "LOW_DELTA_I";
    case ResistanceQuality::UNSTABLE:        return "UNSTABLE";
    case ResistanceQuality::VALID:          return "VALID";
    case ResistanceQuality::HIGH_CONFIDENCE: return "HIGH_CONFIDENCE";
  }
  return "INVALID";
}

// Serialize the full battery / powerFlow / environment blocks into the given
// `data` JsonObject. Called from REST handler and MQTT publisher.
inline void serializeBatteryTelemetry(JsonObject& data) {
  if (!Battery::ENABLED) return;

  BatteryTelemetry t = battery.getTelemetry();
  BatteryDiagnosticsState d = batteryDiagnostics.getState();

  // -------- BATTERY block (brief §31) --------
  JsonObject battery = data.createNestedObject("battery");
  setOptionalFloat(battery, "packVoltage", t.packVoltage);
  battery["packVoltageValid"] = t.packVoltageValid;
  battery["packVoltageSource"] = t.packVoltageSource;
  setOptionalFloat(battery, "current", t.batteryCurrent);
  setOptionalFloat(battery, "power", t.batteryPower);
  setOptionalFloat(battery, "chargePower", t.chargePower);
  setOptionalFloat(battery, "dischargePower", t.dischargePower);

  // SOC (brief §24 — null if capacity not configured)
  battery["socAvailable"] = t.soc.available;
  if (t.soc.available) {
    setOptionalFloat(battery, "soc", t.soc.socPercent);
    battery["socSynchronized"] = t.soc.synchronized;
  } else {
    battery["soc"] = nullptr;
    battery["socSynchronized"] = false;
  }

  setOptionalFloat(battery, "chargedAh", t.energy.chargedAh);
  setOptionalFloat(battery, "dischargedAh", t.energy.dischargedAh);
  setOptionalFloat(battery, "chargedWh", t.energy.batteryChargedWh);
  setOptionalFloat(battery, "dischargedWh", t.energy.batteryDischargedWh);

  // Cells array — individual cell voltages (brief §14, §17)
  JsonArray cellsArr = battery.createNestedArray("cells");
  for (uint8_t i = 0; i < Battery::NUM_CELLS; i++) {
    JsonObject c = cellsArr.createNestedObject();
    c["index"] = i + 1;
    if (t.cells[i].state == CellSensorState::OK && !std::isnan(t.cells[i].voltageV)) {
      c["voltage"] = t.cells[i].voltageV;
    } else {
      c["voltage"] = nullptr;  // brief §17: do NOT expose invalid as 0
    }
    c["state"] = cellStateToStr(t.cells[i].state);
  }

  // Cell metrics
  if (t.cellMetrics.valid) {
    JsonObject cm = battery.createNestedObject("cellMetrics");
    setOptionalFloat(cm, "min", t.cellMetrics.cellMin);
    setOptionalFloat(cm, "max", t.cellMetrics.cellMax);
    setOptionalFloat(cm, "average", t.cellMetrics.cellAverage);
    setOptionalFloat(cm, "delta", t.cellMetrics.cellDelta);
    cm["minIndex"] = t.cellMetrics.minCellIndex + 1;  // 1-based for API
    cm["maxIndex"] = t.cellMetrics.maxCellIndex + 1;
  }

  // Resistance (pack + per-cell — brief §25-29)
  PackResistanceResult pr = resistance.getPackResistance();
  JsonObject prObj = battery.createNestedObject("packResistance");
  if (pr.valid) {
    setOptionalFloat(prObj, "ohms", pr.resistanceOhms);
    setOptionalFloat(prObj, "deltaV", pr.deltaVoltage);
    setOptionalFloat(prObj, "deltaI", pr.deltaCurrent);
    prObj["sampleWindowMs"] = pr.sampleWindowMs;
    prObj["quality"] = resistanceQualityStr(pr.quality);
  } else {
    prObj["ohms"] = nullptr;
    prObj["quality"] = resistanceQualityStr(pr.quality);
  }

  JsonArray cellResArr = battery.createNestedArray("cellResistance");
  for (uint8_t i = 0; i < Battery::NUM_CELLS; i++) {
    CellResistanceResult cr = resistance.getCellResistance(i);
    JsonObject crObj = cellResArr.createNestedObject();
    crObj["index"] = i + 1;
    if (cr.valid) {
      setOptionalFloat(crObj, "ohms", cr.resistanceOhms);
      crObj["quality"] = resistanceQualityStr(cr.quality);
    } else {
      crObj["ohms"] = nullptr;
      crObj["quality"] = resistanceQualityStr(cr.quality);
    }
  }

  battery["valid"] = t.valid;

  // Diagnostics sub-object (brief §30)
  JsonObject diag = battery.createNestedObject("diagnostics");
  diag["batteryVoltageFault"]       = d.batteryVoltageFault;
  diag["batteryCurrentSensorFault"] = d.batteryCurrentSensorFault;
  diag["inverterCurrentSensorFault"] = d.inverterCurrentSensorFault;
  diag["cellMeasurementFault"]      = d.cellMeasurementFault;
  diag["cellTapFault"]              = d.cellTapFault;
  diag["cellOverVoltage"]           = d.cellOverVoltage;
  diag["cellUnderVoltage"]          = d.cellUnderVoltage;
  diag["cellImbalance"]              = d.cellImbalance;
  diag["highPackResistance"]        = d.highPackResistance;
  diag["highCellResistance"]       = d.highCellResistance;
  diag["powerFlowInconsistency"]    = d.powerFlowInconsistency;
  diag["sht31Fault"]                = d.sht31Fault;
  diag["adsFault"]                   = d.adsFault;
  diag["inaFault"]                   = d.inaFault;
  diag["overall"]                    = alarmStateStr(d.overall);

  // -------- POWER FLOW block (brief §31, §22) --------
  JsonObject pf = data.createNestedObject("powerFlow");
  setOptionalFloat(pf, "mpptCurrent", t.powerFlow.mpptCurrent);
  setOptionalFloat(pf, "mpptPower",   t.powerFlow.mpptPower);
  setOptionalFloat(pf, "batteryCurrent", t.powerFlow.batteryCurrent);
  setOptionalFloat(pf, "batteryPower",   t.powerFlow.batteryPower);
  setOptionalFloat(pf, "inverterCurrent", t.powerFlow.inverterCurrent);
  setOptionalFloat(pf, "inverterDcPower", t.powerFlow.inverterDcPower);
  setOptionalFloat(pf, "consistencyError", t.powerFlow.consistencyError);
  pf["consistency"]   = alarmStateStr(t.powerFlow.consistency);
  pf["batteryCurrentValid"] = t.powerFlow.batteryCurrentValid;
  pf["inverterCurrentValid"] = t.powerFlow.inverterCurrentValid;
  pf["valid"] = t.powerFlow.valid;

  // -------- ENVIRONMENT block (brief §20, §39) --------
  Drivers::Sht31Reading env = Drivers::sht31.getReading();
  JsonObject environment = data.createNestedObject("environment");
  if (Drivers::sht31.isAvailable() && env.status == Drivers::Sht31Status::Ok) {
    setOptionalFloat(environment, "temperature", env.temperatureC);
    setOptionalFloat(environment, "humidity", env.humidityRH);
    environment["valid"] = true;
  } else {
    environment["temperature"] = nullptr;
    environment["humidity"] = nullptr;
    environment["valid"] = false;
  }
  environment["label"] = "ambient";  // brief §20: do NOT label as battery T

  // -------- ENERGY counters top-level (brief §23) --------
  JsonObject energy = data.createNestedObject("energy");
  setOptionalFloat(energy, "pvWh",           t.energy.pvEnergyWh);
  setOptionalFloat(energy, "batteryChargedWh", t.energy.batteryChargedWh);
  setOptionalFloat(energy, "batteryDischargedWh", t.energy.batteryDischargedWh);
  setOptionalFloat(energy, "inverterDcWh",   t.energy.inverterDcEnergyWh);
  setOptionalFloat(energy, "chargedAh",      t.energy.chargedAh);
  setOptionalFloat(energy, "dischargedAh",   t.energy.dischargedAh);
  energy["valid"] = t.energy.valid;
}

} // namespace Services

#endif // TIMER12_BATTERY_STATUS_SERIALIZER_H
