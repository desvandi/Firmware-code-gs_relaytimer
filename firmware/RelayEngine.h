// =============================================================================
// Services/RelayEngine.h — Unified relay state machine (priority order)
// =============================================================================
// v4.3.1 audit fix (D-001): exposed applyChannelState() as the SINGLE
// authoritative GPIO mutation path. ResistanceEstimator and any other
// subsystem that needs to drive a relay MUST go through this function.
// No other code may call Drivers::relay.setChannel() except:
//   - RelayEngine::applyChannelState() (normal + safety + boot paths)
//   - RelayDriver::allOff() (factory reset only — documented exception)
// =============================================================================
#pragma once
#ifndef TIMER12_SERVICES_RELAY_ENGINE_H
#define TIMER12_SERVICES_RELAY_ENGINE_H

#include <Arduino.h>
#include "Types.h"

namespace Services {

class RelayEngine {
public:
  void tick();                       // Recompute all 12 channels
  void forceRefresh();               // Force recomputation after config change
  void setManual(uint8_t idx, bool on);
  void setMode(uint8_t idx, bool autoMode);
  void toggle(uint8_t idx);

  // v4.3.1 D-001: SINGLE authoritative GPIO mutation path.
  // Public so ResistanceEstimator::runLoadStepTest() and other subsystems
  // can apply a relay state WITHOUT bypassing safety/interlock gates.
  // This function:
  //   1. Calls Drivers::relay.setChannel() (the ONLY place this is called)
  //   2. Updates Core::relayState[]
  //   3. Records the transition in SafetySupervisor + InterlockEngine
  //   4. Updates state sequence + timestamp + fault tracking
  void applyChannelState(uint8_t idx, bool newState);

  // v4.3.1 D-001: For test-load / commissioning paths that need to force
  // a relay state WITHOUT going through arbitration. Used by
  // ResistanceEstimator::runLoadStepTest(). Routes through the unified GPIO
  // path (no bypass of actuator) but skips safety/interlock evaluation
  // (caller assumes responsibility). Safety lockout (maxOnTimeForced)
  // still wins — cannot force ON while locked out.
  bool forceChannelState(uint8_t idx, bool newState);

private:
  bool _computeChannel(uint8_t idx, uint16_t currentMin, int weekdayIdx,
                       Core::RelaySource& outSource);
};

extern RelayEngine relayEngine;

} // namespace Services

#endif // TIMER12_SERVICES_RELAY_ENGINE_H
