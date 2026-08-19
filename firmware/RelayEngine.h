// =============================================================================
// Services/RelayEngine.h — Unified relay state machine (priority order)
// =============================================================================
// v4.3.2 BLOCKER-01: forceChannelState() REMOVED (was bypass vector).
// Per ChatGPT audit: "forceChannelState() berpotensi menjadi bypass terselubung
//   terhadap CommandArbiter/SafetySupervisor/InterlockEngine."
//
// ALL relay mutations now go through the FULL arbitration pipeline:
//   setManual()/setMode() → tick() → CommandArbiter::arbitrate() →
//   InterlockEngine::evaluateTransition() → SafetySupervisor::evaluateTransition() →
//   applyChannelState() → RelayDriver::setChannel() → GPIO
//
// No subsystem (including ResistanceEstimator) may bypass this pipeline.
// ResistanceEstimator uses setManual() which goes through the full pipeline.
// =============================================================================
#pragma once
#ifndef TIMER12_SERVICES_RELAY_ENGINE_H
#define TIMER12_SERVICES_RELAY_ENGINE_H

#include <Arduino.h>
#include "Types.h"

namespace Services {

class RelayEngine {
public:
  void tick();
  void forceRefresh();
  void setManual(uint8_t idx, bool on);
  void setMode(uint8_t idx, bool autoMode);
  void toggle(uint8_t idx);

  // v4.3.2 BLOCKER-01: applyChannelState is the SINGLE GPIO mutation point.
  // PUBLIC for RelayEngine::tick() to call, but it is NOT a bypass —
  // tick() calls it ONLY after CommandArbiter + Safety + Interlock have
  // evaluated the transition. No external subsystem should call this.
  // (Kept public for RelayEngine::tick() access, but documented as internal.)
  void applyChannelState(uint8_t idx, bool newState);

private:
  // v4.3.4: _computeChannel removed — logic moved to CommandArbiter::arbitrate()
};

extern RelayEngine relayEngine;

} // namespace Services

#endif // TIMER12_SERVICES_RELAY_ENGINE_H
