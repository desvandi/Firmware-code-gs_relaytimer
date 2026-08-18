// =============================================================================
// ResistanceEstimator.h — DC dynamic internal resistance estimate (ΔV / ΔI)
// Timer Digital Relay v4.1 — Battery Resistance Diagnostics
// -----------------------------------------------------------------------------
// Brief §25-29: R = ΔV / ΔI is described as a DC/dynamic resistance estimate,
// NOT laboratory ESR and NOT AC impedance. Treated as a diagnostic estimate.
//
// Passive estimation: continuously detect current transitions of sufficient
// magnitude, capture (V0, I0) and (V1, I1) around the step, compute:
//   ΔV = V0 - V1,  ΔI = I1 - I0,  R = |ΔV / ΔI|
//
// Only compute when:
//   |ΔI| ≥ RESISTANCE_MIN_DELTA_I_A
//   |ΔV| ≥ RESISTANCE_MIN_DELTA_V_V (reject tiny noise)
//   samples are valid (non-NaN, within plausible bounds)
//   no simultaneous unstable transition (load switching artifact)
//
// Optional test load (brief §27) — DISABLED by default. Owner must explicitly
// configure testLoadEnabled, testLoadResistance, testLoadMaxCurrent, and
// testLoadRelayChannel in BatteryConfig.h. If config is missing/unsafe, the
// test refuses to run. NO automatic energizing of unknown relay/load.
// =============================================================================
#pragma once
#ifndef TIMER12_RESISTANCE_ESTIMATOR_H
#define TIMER12_RESISTANCE_ESTIMATOR_H

#include <Arduino.h>
#include <cstdint>
#include "BatteryConfig.h"
#include "BatteryMonitor.h"

namespace Services {

enum class ResistanceQuality : uint8_t {
  INVALID = 0,
  LOW_DELTA_I,
  UNSTABLE,
  VALID,
  HIGH_CONFIDENCE,
};

struct PackResistanceResult {
  float    resistanceOhms;
  float    deltaVoltage;
  float    deltaCurrent;
  uint32_t sampleWindowMs;
  ResistanceQuality quality;
  bool     valid;
  uint32_t timestamp;
};

struct CellResistanceResult {
  float    resistanceOhms;
  ResistanceQuality quality;
  bool     valid;
  uint32_t timestamp;
};

class ResistanceEstimator {
public:
  void begin();
  void tick();

  PackResistanceResult   getPackResistance() const { return _packRes; }
  CellResistanceResult   getCellResistance(uint8_t idx) const {
    return (idx < Battery::NUM_CELLS) ? _cellRes[idx] : CellResistanceResult{};
  }

  // Optional diagnostic test (brief §27). Returns false if config is unsafe.
  bool runLoadStepTest();
  bool isTestRunning() const { return _testRunning; }

private:
  PackResistanceResult _packRes = {};
  CellResistanceResult _cellRes[Battery::NUM_CELLS] = {};
  bool _initialized = false;

  // Rolling buffer for passive step detection
  static constexpr uint8_t RING_LEN = 16;
  float _vRing[RING_LEN];
  float _iRing[RING_LEN];
  uint32_t _tsRing[RING_LEN];
  uint8_t  _ringIdx = 0;
  bool     _ringFilled = false;

  // Test load state
  bool     _testRunning = false;
  uint32_t _testStartMs = 0;

  void     _evaluatePassive();
  void     _evaluateCells(const float vPre[Battery::NUM_CELLS],
                           const float vPost[Battery::NUM_CELLS],
                           float dI, uint32_t windowMs);
  const char* _qualityStr(ResistanceQuality q) const;
};

extern ResistanceEstimator resistance;

} // namespace Services

#endif // TIMER12_RESISTANCE_ESTIMATOR_H
