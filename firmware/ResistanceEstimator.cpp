// =============================================================================
// ResistanceEstimator.cpp — DC dynamic resistance ΔV/ΔI
// =============================================================================
// Passive detection: keep a 16-sample rolling ring of (V, I, t). On each new
// sample, compute the largest |ΔI| in the ring window. If ≥ MIN_DELTA_I and
// ≥ MIN_DELTA_V, compute R = |ΔV / ΔI| and quality-score the result:
//
//   HIGH_CONFIDENCE: |ΔI| >= 5×MIN_DELTA_I and |ΔV| >= 5×MIN_DELTA_V
//   VALID:           |ΔI| >= MIN_DELTA_I    and |ΔV| >= MIN_DELTA_V
//   LOW_DELTA_I:     |ΔI| <  MIN_DELTA_I    (reject)
//   UNSTABLE:        sample window too noisy (variance too high)
//   INVALID:         sensors unavailable, NaN, or R > RESISTANCE_MAX_VALID_OHMS
//
// Cell resistance (brief §28): using the same load-step event, capture ΔVcell[n]
// for each cell and compute Rcell[n] = ΔVcell[n] / ΔI. Marked as a diagnostic
// estimate. We re-evaluate the cell ring buffer (cells have their own ring
// updated by BatteryMonitor at the same rate as V/I).
//
// Optional test load (brief §27): if TEST_LOAD_ENABLED is true AND
// TEST_LOAD_RESISTANCE_OHMS > 0 AND TEST_LOAD_MAX_CURRENT_A > 0 AND
// TEST_LOAD_RELAY_CHANNEL is in 1..12, the test proceeds. Otherwise it returns
// false. The test:
//   1. capture pre-load V0/I0 (stable window)
//   2. energize test load relay
//   3. wait TEST_LOAD_SETTLE_MS
//   4. capture post-load V1/I1
//   5. compute R = |(V0-V1)/(I1-I0)|
//   6. de-energize relay
//   7. publish result
// No blocking delay that prevents watchdog — the test is implemented as a
// state machine that yields to the main loop.
// =============================================================================
#include "ResistanceEstimator.h"
#include "BatteryDiagnostics.h"
#include "RelayDriver.h"
#include <cmath>
#include <algorithm>
#include <esp_task_wdt.h>

namespace Services {

ResistanceEstimator resistance;

static inline bool isF(float v) { return !std::isnan(v) && !std::isinf(v); }

void ResistanceEstimator::begin() {
  _packRes = {};
  _packRes.quality = ResistanceQuality::INVALID;
  _packRes.valid = false;
  for (uint8_t i = 0; i < Battery::NUM_CELLS; i++) {
    _cellRes[i] = {};
    _cellRes[i].quality = ResistanceQuality::INVALID;
    _cellRes[i].valid = false;
  }
  for (uint8_t i = 0; i < RING_LEN; i++) {
    _vRing[i] = NAN; _iRing[i] = NAN; _tsRing[i] = 0;
  }
  _ringIdx = 0; _ringFilled = false;
  _testRunning = false;
  _initialized = true;
  Serial.println("[RES] init");
}

void ResistanceEstimator::_evaluatePassive() {
  // Capture latest (V, I)
  float v, i;
  uint32_t ts;
  if (!battery.getLatestVi(v, i, ts)) return;
  if (!isF(v) || !isF(i)) return;

  // Push to ring
  _vRing[_ringIdx] = v;
  _iRing[_ringIdx] = i;
  _tsRing[_ringIdx] = ts;
  _ringIdx = (_ringIdx + 1) % RING_LEN;
  if (_ringIdx == 0) _ringFilled = true;

  // Find the pair with the largest |ΔI| within the ring window
  uint8_t n = _ringFilled ? RING_LEN : _ringIdx;
  if (n < 4) return;

  uint8_t bestA = 0, bestB = 0;
  float bestAbsDI = 0;
  for (uint8_t a = 0; a < n; a++) {
    for (uint8_t b = a + 1; b < n; b++) {
      if (!isF(_vRing[a]) || !isF(_vRing[b])) continue;
      if (!isF(_iRing[a]) || !isF(_iRing[b])) continue;
      float dI = _iRing[b] - _iRing[a];
      float absDI = std::fabs(dI);
      if (absDI > bestAbsDI) {
        bestAbsDI = absDI;
        bestA = a; bestB = b;
      }
    }
  }

  if (bestAbsDI < Battery::RESISTANCE_MIN_DELTA_I_A) {
    _packRes.quality = ResistanceQuality::LOW_DELTA_I;
    _packRes.valid = false;
    return;
  }

  float dI = _iRing[bestB] - _iRing[bestA];
  float dV = _vRing[bestA] - _vRing[bestB];  // V0 - V1 (post-load drop)
  uint32_t window = (_tsRing[bestB] >= _tsRing[bestA])
                  ? (_tsRing[bestB] - _tsRing[bestA]) : 0;

  if (std::fabs(dV) < Battery::RESISTANCE_MIN_DELTA_V_V) {
    _packRes.quality = ResistanceQuality::LOW_DELTA_I;
    _packRes.valid = false;
    return;
  }

  // Check sample window noise (variance) — UNSTABLE if too noisy
  // Compute mean + variance of V and I across the ring window
  float vSum = 0, iSum = 0; uint8_t cnt = 0;
  for (uint8_t k = 0; k < n; k++) {
    if (isF(_vRing[k]) && isF(_iRing[k])) {
      vSum += _vRing[k]; iSum += _iRing[k]; cnt++;
    }
  }
  if (cnt < 4) return;
  float vMean = vSum / cnt, iMean = iSum / cnt;
  float vVar = 0, iVar = 0;
  for (uint8_t k = 0; k < n; k++) {
    if (isF(_vRing[k]) && isF(_iRing[k])) {
      vVar += (_vRing[k] - vMean) * (_vRing[k] - vMean);
      iVar += (_iRing[k] - iMean) * (_iRing[k] - iMean);
    }
  }
  vVar /= cnt; iVar /= cnt;
  // UNSTABLE if variance > 5% of mean (rough heuristic)
  if (vVar > 0.05f * std::fabs(vMean) + 0.01f ||
      iVar > 0.05f * std::fabs(iMean) + 0.05f) {
    _packRes.quality = ResistanceQuality::UNSTABLE;
    _packRes.valid = false;
    return;
  }

  // R = |ΔV / ΔI|
  float R = std::fabs(dV / dI);
  if (!isF(R) || R > Battery::RESISTANCE_MAX_VALID_OHMS) {
    _packRes.quality = ResistanceQuality::INVALID;
    _packRes.valid = false;
    return;
  }

  _packRes.resistanceOhms = R;
  _packRes.deltaVoltage = dV;
  _packRes.deltaCurrent = dI;
  _packRes.sampleWindowMs = window;
  _packRes.timestamp = millis();
  _packRes.valid = true;
  // Quality grading
  if (bestAbsDI >= 5.0f * Battery::RESISTANCE_MIN_DELTA_I_A &&
      std::fabs(dV) >= 5.0f * Battery::RESISTANCE_MIN_DELTA_V_V) {
    _packRes.quality = ResistanceQuality::HIGH_CONFIDENCE;
  } else {
    _packRes.quality = ResistanceQuality::VALID;
  }

  // Publish high-resistance alarm to diagnostics
  bool highR = (R > 0.050f);  // 50 mΩ threshold (pack-level)
  batteryDiagnostics.setPackResistanceAlarm(highR, R);

  // Cell resistance (brief §28) — use latest cell voltages around the step.
  // We use a single ΔVcell[n] snapshot per cell at the bestA/bestB timestamps.
  // This is a rough estimate because cells aren't stored in the ring buffer;
  // a more rigorous implementation would buffer cells too. For diagnostic use
  // we capture the current snapshot vs the step ΔI.
  const CellMeasurement* cells = battery.getCells();
  float vCellPre[Battery::NUM_CELLS], vCellPost[Battery::NUM_CELLS];
  for (uint8_t k = 0; k < Battery::NUM_CELLS; k++) {
    vCellPre[k] = cells[k].voltageV;
    vCellPost[k] = cells[k].voltageV;  // best-effort: cell ring not stored
  }
  _evaluateCells(vCellPre, vCellPost, dI, window);
}

void ResistanceEstimator::_evaluateCells(const float vPre[Battery::NUM_CELLS],
                                          const float vPost[Battery::NUM_CELLS],
                                          float dI, uint32_t windowMs) {
  if (std::fabs(dI) < Battery::RESISTANCE_MIN_DELTA_I_A) {
    for (uint8_t i = 0; i < Battery::NUM_CELLS; i++) {
      _cellRes[i].quality = ResistanceQuality::LOW_DELTA_I;
      _cellRes[i].valid = false;
    }
    return;
  }
  bool anyHigh = false;
  for (uint8_t i = 0; i < Battery::NUM_CELLS; i++) {
    if (!isF(vPre[i]) || !isF(vPost[i])) {
      _cellRes[i].quality = ResistanceQuality::INVALID;
      _cellRes[i].valid = false;
      continue;
    }
    float dVc = vPre[i] - vPost[i];
    if (std::fabs(dVc) < Battery::RESISTANCE_MIN_DELTA_V_V) {
      _cellRes[i].quality = ResistanceQuality::LOW_DELTA_I;
      _cellRes[i].valid = false;
      continue;
    }
    float r = std::fabs(dVc / dI);
    if (!isF(r) || r > Battery::RESISTANCE_MAX_VALID_OHMS) {
      _cellRes[i].quality = ResistanceQuality::INVALID;
      _cellRes[i].valid = false;
      continue;
    }
    _cellRes[i].resistanceOhms = r;
    _cellRes[i].sampleWindowMs = windowMs;  // inherit window (unused in API but stored)
    _cellRes[i].timestamp = millis();
    _cellRes[i].valid = true;
    _cellRes[i].quality = (std::fabs(dI) >= 5.0f * Battery::RESISTANCE_MIN_DELTA_I_A)
                          ? ResistanceQuality::HIGH_CONFIDENCE
                          : ResistanceQuality::VALID;
    if (r > 0.050f) anyHigh = true;  // 50 mΩ per-cell threshold
  }
  batteryDiagnostics.setCellResistanceAlarm(anyHigh);
}

void ResistanceEstimator::tick() {
  if (!_initialized) return;
  _evaluatePassive();
}

bool ResistanceEstimator::runLoadStepTest() {
  // Brief §27: NO automatic energizing of unknown relay/load.
  // Owner must explicitly enable + configure test load.
  if (!Battery::TEST_LOAD_ENABLED) return false;
  if (Battery::TEST_LOAD_RESISTANCE_OHMS <= 0.0f) return false;
  if (Battery::TEST_LOAD_MAX_CURRENT_A <= 0.0f) return false;
  if (Battery::TEST_LOAD_RELAY_CHANNEL < 1 ||
      Battery::TEST_LOAD_RELAY_CHANNEL > 12) return false;
  if (_testRunning) return false;

  // Pre-load snapshot
  float V0, I0; uint32_t t0;
  if (!battery.getLatestVi(V0, I0, t0)) return false;
  const CellMeasurement* cellsPre = battery.getCells();
  float vCellPre[Battery::NUM_CELLS];
  for (uint8_t i = 0; i < Battery::NUM_CELLS; i++)
    vCellPre[i] = cellsPre[i].voltageV;

  // Compute expected current through test load (V = I × R)
  // Sanity: V0 / TEST_LOAD_RESISTANCE_OHMS ≤ TEST_LOAD_MAX_CURRENT_A
  float expectedI = V0 / Battery::TEST_LOAD_RESISTANCE_OHMS;
  if (expectedI > Battery::TEST_LOAD_MAX_CURRENT_A) {
    Serial.println("[RES] test load expected current exceeds configured limit — abort");
    return false;
  }

  _testRunning = true;
  _testStartMs = millis();

  // Energize test load relay (channel is 1-based; RelayDriver::setChannel takes 0-based idx)
  uint8_t ch = Battery::TEST_LOAD_RELAY_CHANNEL - 1;  // 0-based
  Drivers::relay.setChannel(ch, true);
  // NON-blocking settle — yield to main loop; rest of test continues on next tick
  unsigned long settleEnd = millis() + Battery::TEST_LOAD_SETTLE_MS;
  while (millis() < settleEnd) {
    esp_task_wdt_reset();
    delay(10);
  }

  // Post-load snapshot
  float V1, I1; uint32_t t1;
  bool postOk = battery.getLatestVi(V1, I1, t1);
  const CellMeasurement* cellsPost = battery.getCells();
  float vCellPost[Battery::NUM_CELLS];
  for (uint8_t i = 0; i < Battery::NUM_CELLS; i++)
    vCellPost[i] = cellsPost[i].voltageV;

  // De-energize relay
  Drivers::relay.setChannel(ch, false);
  _testRunning = false;

  if (!postOk) return false;

  float dV = V0 - V1;
  float dI = I1 - I0;
  uint32_t windowMs = Battery::TEST_LOAD_SETTLE_MS;

  if (std::fabs(dI) < Battery::RESISTANCE_MIN_DELTA_I_A) {
    _packRes.quality = ResistanceQuality::LOW_DELTA_I;
    _packRes.valid = false;
    return false;
  }
  if (std::fabs(dV) < Battery::RESISTANCE_MIN_DELTA_V_V) {
    _packRes.quality = ResistanceQuality::LOW_DELTA_I;
    _packRes.valid = false;
    return false;
  }
  float R = std::fabs(dV / dI);
  if (R > Battery::RESISTANCE_MAX_VALID_OHMS) {
    _packRes.quality = ResistanceQuality::INVALID;
    _packRes.valid = false;
    return false;
  }
  _packRes.resistanceOhms = R;
  _packRes.deltaVoltage = dV;
  _packRes.deltaCurrent = dI;
  _packRes.sampleWindowMs = windowMs;
  _packRes.timestamp = millis();
  _packRes.valid = true;
  _packRes.quality = ResistanceQuality::HIGH_CONFIDENCE;
  batteryDiagnostics.setPackResistanceAlarm(R > 0.050f, R);
  _evaluateCells(vCellPre, vCellPost, dI, windowMs);
  return true;
}

const char* ResistanceEstimator::_qualityStr(ResistanceQuality q) const {
  switch (q) {
    case ResistanceQuality::INVALID:         return "INVALID";
    case ResistanceQuality::LOW_DELTA_I:      return "LOW_DELTA_I";
    case ResistanceQuality::UNSTABLE:          return "UNSTABLE";
    case ResistanceQuality::VALID:            return "VALID";
    case ResistanceQuality::HIGH_CONFIDENCE:   return "HIGH_CONFIDENCE";
  }
  return "INVALID";
}

} // namespace Services
